// title_plus: the title screen of Cube World Alpha Plus.
//
// The game's logo is the "cubeworld" node of start.plx: a tree of vector shapes, one per letter and layer, each
// drawn by a plasma::D3D9Drawing with the engine's pixel shader. A few seconds after the logo is first drawn this
// mod draws those shapes with its own pixel shader (logo_ps.hlsl), in which red spreads over the letters in
// blotches, and then draws a plus sign that falls from the viewer onto the title, beside the D. When the plus has
// landed a tilted white line, a shine, sweeps across the letters and the plus once.
//
// The plus is not drawn through the game's draw call, which takes the mask, the texture and the clip of the node it
// is given. The game also draws the last layer of the letters (the faces) into an offscreen target and lays it over
// the screen afterwards, with a gradient, cut to the box of the letters: anything drawn meanwhile is coloured by the
// gradient and cut to that box. So the mod draws the plus itself, when the frame's last logo node has been drawn, as
// triangles in pixels with the fixed-function pipeline, placed by the transform the engine had at the D of the last
// layer. It lies over the whole D, as in the reference picture, and nothing of the game's state can reach it.
//
// Builds with MinGW (i686) and MSVC (Win32).
#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "kyubu.hpp"
#include "logo_ps.h"

namespace {

// ---- what the mod knows about the game (build cube-0x51EA955E) ----
const uintptr_t kControllerLogoNode = 0x800878;   // cube::GameController: the "cubeworld" node of start.plx

const uintptr_t kNodeName = 0x0C;                 // plasma::Node: std::wstring (MSVC 2012: 16-byte buffer, size, capacity)
const uintptr_t kNodeChildren = 0x2C;             // std::list<Node*>: pointer to the head; a list node is {next, prev, Node*}
const uintptr_t kNodeStyle = 0x3C;                // the node's style: +0x118 an index into the table at +0x144 of 4 floats (RGBA)
const uintptr_t kStyleColourIndex = 0x118;
const uintptr_t kStyleColourTable = 0x144;

const uintptr_t kDrawBounds = 0x110;              // plasma::D3D9Drawing: min x, min y, max x, max y of the shape's points
const uintptr_t kDrawEngine = 0x124;              // plasma::D3D9Engine*
const uintptr_t kEnginePixelShader = 0x198;       // plasma::D3D9Engine: the ps_3_0 shader draw() sets for every shape
const uintptr_t kEngineWorldView = 0x224;         // the current transform, 4x4 floats, a row per axis: x' = x * m[0] + y * m[4] + m[12]
const uintptr_t kEngineDevice = 0x2C0;            // IDirect3DDevice9*
const unsigned kSpreadRegister = 30;              // logo_ps.hlsl's `spread`; the game sets c0 to c4
const unsigned kShineRegister = 28;               // logo_ps.hlsl's `shine_line` and `shine_shape`: two registers
const float kEnginePixelOffset = 0.135f;          // what the engine's vertex shader subtracts from a position in pixels

template <typename T>
T& field(void* object, uintptr_t offset) { return *reinterpret_cast<T*>(static_cast<char*>(object) + offset); }

// ---- state ----
kyubu::Mod* g_mod = nullptr;
uintptr_t g_controller_global = 0;   // address of cube::game_controller
double g_delay = 3.0, g_fade = 2.0;
bool g_plus_enabled = true, g_shine_enabled = true;
float g_shine_seconds = 0.7f;                      // how long the shine takes to cross the logo

std::unordered_set<void*> g_logo_nodes;
void* g_plus_anchor = nullptr;                     // the node the plus is placed by: the D of the first letter layer
void* g_plus_top = nullptr;                        // the D of the last letter layer: the transform the plus is placed with
void* g_plus_trigger = nullptr;                    // the node the plus is drawn after: the last of the logo's nodes drawn in a frame
float g_plus_world[16];                            // the engine's transform at the last letter layer's D
bool g_plus_world_known = false;
std::vector<void*> g_d_nodes;                      // the nodes named D, in drawing order
int g_api_state = 0;                               // the game API's ui functions: 0 not tried, 1 in use, -1 not there
void* g_anchor_drawing = nullptr;                  // the anchor's drawing and colour from its last draw call
float g_anchor_colour[8];
double g_started = -1;                             // when the logo was first drawn, in seconds

IDirect3DPixelShader9* g_logo_ps = nullptr;
bool g_logo_ps_failed = false;
IDirect3DPixelShader9* g_shine_ps = nullptr;       // the shine over the plus
bool g_shine_ps_failed = false;
bool g_warm_logo = false, g_warm_plus = false;     // a draw with each shader is still to come on the logo's first frame

using DrawFn = void(__fastcall*)(void* self, void* edx, void* node, float* colour);
DrawFn g_draw_next = nullptr;

// Seconds since the mod first asked. The counts are subtracted as integers before anything becomes a double: Direct3D 9
// puts the x87 unit in single precision, and the whole counter (days of uptime) divided there came out in steps of a
// sixteenth of a second, so the animation moved 16 times a second.
double now_seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    static LARGE_INTEGER base = [] { LARGE_INTEGER b; QueryPerformanceCounter(&b); return b; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart - base.QuadPart) / double(freq.QuadPart);
}

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ---- finding the logo ----

// True when the node's name is the one character `c`.
bool node_is_named(uintptr_t node, wchar_t c) {
    uint32_t size = 0, capacity = 0;
    if (!g_mod->read(node + kNodeName + 0x10, size) || !g_mod->read(node + kNodeName + 0x14, capacity) || size != 1) return false;
    uintptr_t text = node + kNodeName;
    if (capacity > 7 && !g_mod->read(node + kNodeName, text)) return false;
    wchar_t first = 0;
    return g_mod->read(text, first) && first == c;
}

void collect_logo(uintptr_t node, int depth) {
    if (!node || depth > 16 || g_logo_nodes.size() >= 1024) return;
    g_logo_nodes.insert(reinterpret_cast<void*>(node));
    if (node_is_named(node, L'D')) g_d_nodes.push_back(reinterpret_cast<void*>(node));
    uintptr_t head = 0, item = 0;
    if (!g_mod->read(node + kNodeChildren, head) || !head || !g_mod->read(head, item)) return;
    for (int n = 0; item && item != head && n < 256; ++n) {
        uintptr_t child = 0, next = 0;
        if (!g_mod->read(item + 8, child) || !g_mod->read(item, next)) return;
        collect_logo(child, depth + 1);
        item = next;
    }
}

// The same walk through the game API (ui_node_find and ui_node_child, Kyubu 1.1 with the `ui` primitive).
void collect_logo_api(const KyubuUiNode& info, int depth) {
    if (depth > 16 || g_logo_nodes.size() >= 1024) return;
    g_logo_nodes.insert(reinterpret_cast<void*>(info.node));
    if (std::strcmp(info.name, "D") == 0) g_d_nodes.push_back(reinterpret_cast<void*>(info.node));
    for (uint32_t i = 0; i < info.children && i < 256; ++i) {
        KyubuUiNode child = {};
        child.size = sizeof child;
        if (g_mod->ui_node_child(info.node, i, &child) != KY_OK) return;
        collect_logo_api(child, depth + 1);
    }
}

// True once the logo's nodes are known. The node is found through the game API when the loader has it, and by reading the
// controller's pointer (set only after its constructor has loaded start.plx) when it does not.
bool find_logo() {
    if (!g_logo_nodes.empty()) return true;
    if (g_api_state >= 0) {
        KyubuUiNode logo = {};
        logo.size = sizeof logo;
        const KyubuResult r = g_mod->ui_node_find("cubeworld", 0, &logo);
        if (r == KY_OK) {
            g_api_state = 1;
            collect_logo_api(logo, 0);
        } else if (r == KY_E_UNAVAILABLE || r == KY_E_SIDE || r == KY_E_UNSAFE) {
            g_api_state = -1;   // an older loader, or a build without the primitive: read the game's memory ourselves
        } else {
            return false;       // no logo yet
        }
    }
    if (g_logo_nodes.empty()) {
        uintptr_t controller = 0, logo = 0;
        if (!g_mod->read(g_controller_global, controller) || !controller) return false;
        if (!g_mod->read(controller + kControllerLogoNode, logo) || !logo) return false;
        collect_logo(logo, 0);
    }
    // The logo draws one layer of letters after another (outline, side, ..., face), each with its own D, and the glyph of
    // the alpha is named D too and comes last. The last layer of letters is drawn into an offscreen target that the game
    // lays over the screen afterwards (with the logo's gradient, cut to the box of the letters), so a plus drawn with
    // it would be coloured by the gradient and cut to the box. The plus is drawn when the frame's last logo node, the
    // alpha's glyph, has been drawn, and is placed by the transform of the last layer's D; the first D's shape gives
    // its size and place.
    if (!g_d_nodes.empty()) {
        g_plus_anchor = g_d_nodes.front();
        g_plus_top = g_d_nodes[g_d_nodes.size() >= 2 ? g_d_nodes.size() - 2 : 0];
        g_plus_trigger = g_d_nodes.back();
    }
    g_mod->info("logo found (%s): %u nodes, plus anchor %s", g_api_state > 0 ? "game API" : "memory", (unsigned)g_logo_nodes.size(),
                g_plus_anchor ? "found" : "missing");
    return !g_logo_nodes.empty();
}

// ---- the logo's box on the screen ----

// The box the logo covers, in pixels, from the bounds of every shape drawn in a frame and the transform the engine had
// for it. The shine sweeps across it. A frame's box is complete when its last node has been drawn.
struct Box { float x0, y0, x1, y1; };
const Box kEmptyBox = { 1e30f, 1e30f, -1e30f, -1e30f };
Box g_box_building = kEmptyBox, g_box = kEmptyBox;
unsigned g_box_shapes = 0;

void box_add(float x, float y) {
    if (!(std::fabs(x) < 1e5f && std::fabs(y) < 1e5f)) return;   // not a number, or a shape without bounds
    g_box_building.x0 = std::min(g_box_building.x0, x), g_box_building.x1 = std::max(g_box_building.x1, x);
    g_box_building.y0 = std::min(g_box_building.y0, y), g_box_building.y1 = std::max(g_box_building.y1, y);
}

void box_add_drawing(void* drawing) {
    const float* bounds = &field<float>(drawing, kDrawBounds);
    void* engine = field<void*>(drawing, kDrawEngine);
    if (!engine || !(bounds[2] > bounds[0]) || !(bounds[3] > bounds[1])) return;
    const float* m = reinterpret_cast<const float*>(static_cast<char*>(engine) + kEngineWorldView);
    for (int corner = 0; corner < 4; ++corner) {
        float lx = bounds[corner & 1 ? 2 : 0], ly = bounds[corner & 2 ? 3 : 1];
        box_add(lx * m[0] + ly * m[4] + m[12] - kEnginePixelOffset, lx * m[1] + ly * m[5] + m[13] - kEnginePixelOffset);
    }
    ++g_box_shapes;
}

// ---- the shine ----

// How the plus falls (see fall_pose): its fall and two bounces, then it rests.
const float kFallSeconds = 0.6f, kNear = 0.085f;   // it starts at kNear of its final distance, so 1 / kNear times as big
const float kBounces[2][2] = { { 0.22f, 0.11f }, { 0.13f, 0.035f } };   // seconds, extra size at the top
const float kShineGap = 0.2f;                      // seconds between the plus coming to rest and the shine starting
const float kShineSlope = -0.4f;                   // the line leans like a backslash: 0.4 pixel to the right for each pixel down
const float kShineOpacity = 0.7f;                  // how white the line is where it is brightest

// The two registers of logo_ps.hlsl's shine, `shine_time` seconds after the plus came to rest (the line is off before
// that and after it has crossed the box). The line moves from the box's lower left corner past its upper right one.
void shine_constants(float shine_time, float out[8]) {
    std::memset(out, 0, 8 * sizeof(float));
    if (!g_shine_enabled || shine_time < 0 || shine_time > g_shine_seconds || !(g_box.x1 > g_box.x0) || !(g_box.y1 > g_box.y0)) return;
    float width = g_box.x1 - g_box.x0;
    float length = std::sqrt(1 + kShineSlope * kShineSlope);
    float nx = 1 / length, ny = kShineSlope / length;
    float line_half = 0.010f * width, glow_half = 0.045f * width;
    // Where the line is along its normal when it passes each corner of the box: it starts before the first, ends after the last.
    const float along[4] = { g_box.x0 * nx + g_box.y0 * ny, g_box.x1 * nx + g_box.y0 * ny, g_box.x0 * nx + g_box.y1 * ny, g_box.x1 * nx + g_box.y1 * ny };
    float from = *std::min_element(along, along + 4) - glow_half, to = *std::max_element(along, along + 4) + glow_half;
    float u = clamp01(shine_time / g_shine_seconds), eased = u + (u * u * (3 - 2 * u) - u) * 0.5f;
    out[0] = nx, out[1] = ny, out[2] = from + (to - from) * eased, out[3] = kShineOpacity;
    out[4] = line_half, out[5] = glow_half, out[6] = 0.35f;
}

// ---- the red ----

IDirect3DDevice9* device_of(void* drawing) {
    void* engine = field<void*>(drawing, kDrawEngine);
    return engine ? field<IDirect3DDevice9*>(engine, kEngineDevice) : nullptr;
}

bool logo_shader(IDirect3DDevice9* device) {
    if (g_logo_ps) return true;
    if (g_logo_ps_failed || !device) return false;
    if (FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(kLogoPixelShader), &g_logo_ps)) || !g_logo_ps) {
        g_logo_ps_failed = true;
        g_mod->warn("the logo's pixel shader was refused: the logo stays blue");
        return false;
    }
    return true;
}

bool shine_shader(IDirect3DDevice9* device) {
    if (g_shine_ps) return true;
    if (g_shine_ps_failed || !device) return false;
    if (FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(kShinePixelShader), &g_shine_ps)) || !g_shine_ps) {
        g_shine_ps_failed = true;
        g_mod->warn("the shine's pixel shader was refused: the plus gets no shine");
        return false;
    }
    return true;
}

// Draws a logo shape with the red spread to `progress` (above 0, up to 1) and the shine's constants: the engine's
// pixel shader is swapped for the mod's while the game's draw call runs.
void draw_reddened(void* drawing, void* edx, void* node, float* colour, float progress, const float shine[8]) {
    IDirect3DDevice9* device = device_of(drawing);
    if (!logo_shader(device)) return g_draw_next(drawing, edx, node, colour);

    // The noise is between 0 and 1 and mostly near the middle: the threshold crosses that part at an even pace, and
    // ends above every value.
    const float kSoft = 0.045f, kBlotchPixels = 64.0f;
    // A progress of 0 or less is no red at all (the warm-up draw); 4, above every noise value, is red all over and tells
    // the shader to skip the noise.
    const float threshold = progress <= 0 ? -1.0f : progress >= 1 ? 4.0f : 0.16f - kSoft + progress * (0.68f + 2 * kSoft);
    const float spread[4] = { threshold, kSoft, 1.0f / kBlotchPixels, 0 };

    auto& engine_shader = field<IDirect3DPixelShader9*>(field<void*>(drawing, kDrawEngine), kEnginePixelShader);
    IDirect3DPixelShader9* saved = engine_shader;
    engine_shader = g_logo_ps;
    device->SetPixelShaderConstantF(kSpreadRegister, spread, 1);
    device->SetPixelShaderConstantF(kShineRegister, shine, 2);
    g_draw_next(drawing, edx, node, colour);
    engine_shader = saved;
}

// ---- the plus ----

// How the plus looks `t` seconds after it starts to fall from the viewer onto the title.
struct Pose { float scale, angle, alpha; bool settled; };

Pose fall_pose(float t) {
    if (t < kFallSeconds) {
        float u = std::max(t, 0.0f) / kFallSeconds;
        float distance = kNear + (1 - kNear) * u * u;
        return { 1 / distance, -1.1f * (1 - u) * (1 - u), clamp01(t / 0.1f), false };
    }
    t -= kFallSeconds;
    for (auto& bounce : kBounces) {
        if (t < bounce[0]) {
            float x = 2 * t / bounce[0] - 1;
            return { 1 + bounce[1] * (1 - x * x), 0, 1, false };
        }
        t -= bounce[0];
    }
    return { 1, 0, 1, true };
}

// Seconds from the start of the fall to the plus at rest.
float fall_seconds() { return kFallSeconds + kBounces[0][0] + kBounces[1][0]; }

// A layer of the plus is its outline, 44 points, as a fan around the centre with a fringe ring around it.
const unsigned kPlusLayers = 4, kArcSteps = 4, kOutline = 8 * (kArcSteps + 1) + 4;
const unsigned kLayerVerts = 9 * kOutline, kPlusVerts = kPlusLayers * kLayerVerts;

// A vertex in pixels, as the fixed-function pipeline takes it.
struct PlusVertex {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;   // the same as x and y: the shine's shader finds the pixel's place from them
};
const DWORD kPlusFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// The outline of a plus around (0, 0), clockwise on the screen (y grows downwards): arms `reach` long from the
// centre and `half` thick from their axis, the eight outer corners rounded with `radius`.
void plus_outline(float reach, float half, float radius, float out[][2]) {
    const float corners[12][2] = { { -half, -reach }, { half, -reach }, { half, -half }, { reach, -half }, { reach, half }, { half, half },
                                   { half, reach }, { -half, reach }, { -half, half }, { -reach, half }, { -reach, -half }, { -half, -half } };
    radius = std::min(radius, half);
    unsigned n = 0;
    for (unsigned i = 0; i < 12; ++i) {
        const float* p = corners[i];
        if (i % 3 == 2) {   // an inner corner
            out[n][0] = p[0], out[n][1] = p[1], ++n;
            continue;
        }
        const float* before = corners[(i + 11) % 12];
        const float* after = corners[(i + 1) % 12];
        float in_len = std::hypot(p[0] - before[0], p[1] - before[1]), out_len = std::hypot(after[0] - p[0], after[1] - p[1]);
        float in[2] = { (p[0] - before[0]) / in_len, (p[1] - before[1]) / in_len };
        float onward[2] = { (after[0] - p[0]) / out_len, (after[1] - p[1]) / out_len };
        float centre[2] = { p[0] + (onward[0] - in[0]) * radius, p[1] + (onward[1] - in[1]) * radius };
        for (unsigned k = 0; k <= kArcSteps; ++k) {
            float a = 1.5707963f * float(k) / float(kArcSteps);
            out[n][0] = centre[0] + radius * (in[0] * std::sin(a) - onward[0] * std::cos(a));
            out[n][1] = centre[1] + radius * (in[1] * std::sin(a) - onward[1] * std::cos(a));
            ++n;
        }
    }
}

// Where a point of the plus goes on the screen: the plus's own pose (size and turn about its centre, placed at (x, y)
// in the logo's coordinates), then the engine's transform for the logo, in pixels.
struct Placement {
    float x, y, scale, cos, sin;   // the pose, in the logo's coordinates
    const float* world;            // the engine's 4x4 transform
    float alpha;

    void to_screen(float px, float py, float out[2]) const {
        float lx = x + scale * (px * cos - py * sin), ly = y + scale * (px * sin + py * cos);
        out[0] = lx * world[0] + ly * world[4] + world[12] - kEnginePixelOffset;
        out[1] = lx * world[1] + ly * world[5] + world[13] - kEnginePixelOffset;
    }
};

DWORD pack(const float rgb[3], float alpha) {
    auto byte = [](float v) { return DWORD(clamp01(v) * 255.0f + 0.5f); };
    return (byte(alpha) << 24) | (byte(rgb[0]) << 16) | (byte(rgb[1]) << 8) | byte(rgb[2]);
}

// One layer of the plus as triangles, coloured from `top` through `mid` to `bottom` down the layer and moved by
// (`shift_x`, `shift_y`) before it is placed. Its edge fades out over a pixel, as the game's own shapes do: a ring of
// vertices a pixel further out with no opacity.
PlusVertex* plus_layer(PlusVertex* v, const Placement& at, float reach, float half, float radius, float shift_x, float shift_y,
                       const float top[3], const float mid[3], const float bottom[3]) {
    float outline[kOutline][2];
    plus_outline(reach, half, radius, outline);

    float ring[kOutline][2], fringe[kOutline][2], centre[2];
    DWORD colour[kOutline], centre_colour, clear[kOutline];
    auto shade = [&](float y, float alpha) {
        float down = clamp01((y + reach) / (2 * reach)), rgb[3];
        for (int i = 0; i < 3; ++i)
            rgb[i] = down < 0.5f ? top[i] + (mid[i] - top[i]) * 2 * down : mid[i] + (bottom[i] - mid[i]) * (2 * down - 1);
        return pack(rgb, alpha);
    };
    at.to_screen(shift_x, shift_y, centre);
    centre_colour = shade(0, at.alpha);
    for (unsigned i = 0; i < kOutline; ++i) {
        at.to_screen(outline[i][0] + shift_x, outline[i][1] + shift_y, ring[i]);
        colour[i] = shade(outline[i][1], at.alpha);
        clear[i] = colour[i] & 0x00FFFFFF;
    }
    // The fringe ring: each point moves a pixel outwards along the bisector of its two edges.
    for (unsigned i = 0; i < kOutline; ++i) {
        const float* before = ring[(i + kOutline - 1) % kOutline];
        const float* after = ring[(i + 1) % kOutline];
        float d1[2] = { ring[i][0] - before[0], ring[i][1] - before[1] }, d2[2] = { after[0] - ring[i][0], after[1] - ring[i][1] };
        float l1 = std::max(std::hypot(d1[0], d1[1]), 1e-6f), l2 = std::max(std::hypot(d2[0], d2[1]), 1e-6f);
        float n1[2] = { d1[1] / l1, -d1[0] / l1 }, n2[2] = { d2[1] / l2, -d2[0] / l2 };   // outwards, the outline being clockwise
        float b[2] = { n1[0] + n2[0], n1[1] + n2[1] };
        float bl = std::hypot(b[0], b[1]);
        if (bl < 1e-4f) b[0] = n1[0], b[1] = n1[1], bl = 1;
        b[0] /= bl, b[1] /= bl;
        float reach_out = 1.0f / std::max(0.5f, b[0] * n1[0] + b[1] * n1[1]);
        fringe[i][0] = ring[i][0] + b[0] * reach_out;
        fringe[i][1] = ring[i][1] + b[1] * reach_out;
    }
    auto put = [&](const float p[2], DWORD c) {
        v->x = p[0], v->y = p[1], v->z = 0, v->rhw = 1, v->colour = c, v->u = p[0], v->v = p[1];
        ++v;
    };
    for (unsigned i = 0; i < kOutline; ++i) {
        unsigned j = (i + 1) % kOutline;
        put(centre, centre_colour), put(ring[i], colour[i]), put(ring[j], colour[j]);
        put(ring[i], colour[i]), put(fringe[i], clear[i]), put(fringe[j], clear[j]);
        put(ring[i], colour[i]), put(fringe[j], clear[j]), put(ring[j], colour[j]);
    }
    return v;
}

// The device's states the plus's triangles change, saved and put back.
struct PlusState {
    IDirect3DDevice9* device;
    DWORD render[12];
    DWORD stage[5];
    IDirect3DVertexShader9* vertex_shader = nullptr;
    IDirect3DPixelShader9* pixel_shader = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
    DWORD fvf = 0;

    static const D3DRENDERSTATETYPE* render_types() {
        static const D3DRENDERSTATETYPE types[12] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE,
                                                      D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
                                                      D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE,
                                                      D3DRS_MULTISAMPLEANTIALIAS };
        return types;
    }
    static const D3DTEXTURESTAGESTATETYPE* stage_types() {
        static const D3DTEXTURESTAGESTATETYPE types[4] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1 };
        return types;
    }

    explicit PlusState(IDirect3DDevice9* d) : device(d) {
        for (int i = 0; i < 12; ++i) device->GetRenderState(render_types()[i], &render[i]);
        for (int i = 0; i < 4; ++i) device->GetTextureStageState(0, stage_types()[i], &stage[i]);
        device->GetTextureStageState(1, D3DTSS_COLOROP, &stage[4]);
        device->GetVertexShader(&vertex_shader);
        device->GetPixelShader(&pixel_shader);
        device->GetVertexDeclaration(&declaration);
        device->GetFVF(&fvf);
    }
    ~PlusState() {
        for (int i = 0; i < 12; ++i) device->SetRenderState(render_types()[i], render[i]);
        for (int i = 0; i < 4; ++i) device->SetTextureStageState(0, stage_types()[i], stage[i]);
        device->SetTextureStageState(1, D3DTSS_COLOROP, stage[4]);
        device->SetVertexShader(vertex_shader);
        device->SetPixelShader(pixel_shader);
        if (declaration) device->SetVertexDeclaration(declaration);
        else device->SetFVF(fvf);
        if (vertex_shader) vertex_shader->Release();
        if (pixel_shader) pixel_shader->Release();
        if (declaration) declaration->Release();
    }
};

// The opacity the game draws the node with: its style's colour times the colour its parent passes down.
float node_alpha(void* node, const float* colour) {
    float alpha = colour[3];
    if (void* style = field<void*>(node, kNodeStyle)) {
        const float* table = field<const float*>(style, kStyleColourTable);
        if (table) alpha *= table[field<uint32_t>(style, kStyleColourIndex) * 4 + 3];
    }
    return clamp01(alpha);
}

// Draws the plus over the logo, with the transform captured at the last layer's D, and the shine over it when the
// line is crossing it. `engine_drawing` is the drawing the game's call has just drawn (for the engine and the device);
// `anchor_drawing` is the D's, for the size and place. `warm_up` draws both passes fully transparent, on the logo's
// first frame, so that the driver has prepared them before the plus falls and the shine goes across.
void draw_plus(void* engine_drawing, void* anchor_drawing, float fall_time, float shine_time, bool warm_up = false) {
    IDirect3DDevice9* device = device_of(engine_drawing);
    if (!device) return;
    const float* bounds = &field<float>(anchor_drawing, kDrawBounds);
    float w = bounds[2] - bounds[0], h = bounds[3] - bounds[1];
    if (w <= 0 || h <= 0) return;
    if (!g_plus_world_known) return;
    const float* world = g_plus_world;

    // Sizes in units of the D's height, taken from the reference picture; y grows downwards. While it is big the
    // plus is also further from the middle of the screen, as something that falls along the line of sight is.
    Pose pose = fall_pose(fall_time);
    const float reach = 0.275f * h, half = 0.078f * h, outline = 0.059f * h;
    // Its bevel is lit from the upper right: a wide dark band along the left and bottom edges and a thin one along
    // the top and right edges, with a pale band inside them. The dark layer is the whole plus; the pale layer is
    // smaller and moved up and to the right, most of the way to the top and right edges; the face is smaller again
    // and moved back to the left and down.
    const float inset = 0.015f * h, band = 0.012f * h, lift = 0.6f * inset;
    float alpha = warm_up ? 0.0f : pose.alpha * node_alpha(g_plus_anchor, g_anchor_colour);
    if (alpha <= 0 && !warm_up) return;
    Placement at = { bounds[2] + 0.075f * w - 0.105f * h + (pose.scale - 1) * 0.09f * h,
                     bounds[3] - 0.202f * h + (pose.scale - 1) * 0.05f * h,
                     pose.scale, std::cos(pose.angle), std::sin(pose.angle), world, alpha };
    const float black[3] = { 0, 0, 0 };
    const float dark_top[3] = { 0.36f, 0.08f, 0.10f }, dark_mid[3] = { 0.30f, 0.05f, 0.07f }, dark_bottom[3] = { 0.25f, 0.03f, 0.05f };
    const float pale_top[3] = { 0.80f, 0.37f, 0.42f }, pale_mid[3] = { 0.70f, 0.27f, 0.31f }, pale_bottom[3] = { 0.50f, 0.13f, 0.17f };
    const float face_top[3] = { 0.87f, 0.22f, 0.23f }, face_mid[3] = { 0.70f, 0.12f, 0.145f }, face_bottom[3] = { 0.31f, 0.04f, 0.06f };

    // The plus is part of the logo's box for the shine, a little beyond it at the lower right.
    for (int corner = 0; corner < 4 && !warm_up; ++corner) {
        float edge[2];
        at.to_screen((corner & 1 ? 1 : -1) * (reach + outline), (corner & 2 ? 1 : -1) * (reach + outline), edge);
        box_add(edge[0], edge[1]);
    }

    static PlusVertex verts[kPlusVerts];
    PlusVertex* v = verts;
    v = plus_layer(v, at, reach + outline, half + outline, outline, 0, 0, black, black, black);
    v = plus_layer(v, at, reach, half, 0.2f * half, 0, 0, dark_top, dark_mid, dark_bottom);
    v = plus_layer(v, at, reach - inset, half - inset, 0.2f * half, lift, -lift, pale_top, pale_mid, pale_bottom);
    v = plus_layer(v, at, reach - inset - band, half - inset - band, 0.18f * half, lift - band, band - lift, face_top, face_mid, face_bottom);

    PlusState saved(device);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(kPlusFvf);
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, kPlusVerts / 3, verts, sizeof(PlusVertex));

    // The shine: the plus's whole outline again in white, as opaque as the line is at each pixel.
    float shine[8];
    shine_constants(shine_time, shine);
    if ((shine[3] > 0 || warm_up) && shine_shader(device)) {
        static PlusVertex overlay[kLayerVerts];
        const float white[3] = { 1, 1, 1 };
        plus_layer(overlay, at, reach + outline, half + outline, outline, 0, 0, white, white, white);
        device->SetPixelShader(g_shine_ps);
        device->SetPixelShaderConstantF(kShineRegister, shine, 2);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, kLayerVerts / 3, overlay, sizeof(PlusVertex));
    }
}

// ---- hooks ----

void __fastcall draw_hook(void* self, void* edx, void* node, float* colour) {
    if (!node || !find_logo() || !g_logo_nodes.count(node)) return g_draw_next(self, edx, node, colour);

    double now = now_seconds();
    if (g_started < 0) {
        g_started = now;
        g_mod->info("logo first drawn");
        // Both shaders are made now, not when the red starts or the shine goes across: making one takes a frame.
        IDirect3DDevice9* device = device_of(self);
        logo_shader(device);
        shine_shader(device);
        g_warm_logo = g_warm_plus = true;
    }
    float since = float(now - g_started - g_delay);
    float progress = clamp01(since / float(g_fade));

    // The plus starts to fall once the logo is red all over; the shine goes across when it has come to rest.
    float fall_time = since - float(g_fade);
    float shine[8];
    shine_constants(fall_time - fall_seconds() - kShineGap, shine);

    if (progress > 0) draw_reddened(self, edx, node, colour, progress, shine);
    else if (g_warm_logo) {
        g_warm_logo = false;   // one shape with the mod's shader and no red, so the driver has it ready
        draw_reddened(self, edx, node, colour, 0, shine);
    } else g_draw_next(self, edx, node, colour);
    box_add_drawing(self);
    if (node == g_plus_anchor) {
        g_anchor_drawing = self;
        std::memcpy(g_anchor_colour, colour, sizeof g_anchor_colour);   // the caller's array does not outlive the call
    }
    if (node == g_plus_top) {
        std::memcpy(g_plus_world, static_cast<char*>(field<void*>(self, kDrawEngine)) + kEngineWorldView, sizeof g_plus_world);
        g_plus_world_known = true;
    }
    if (node == g_plus_trigger) {
        if (g_plus_enabled && g_anchor_drawing && fall_time >= 0)
            draw_plus(self, g_anchor_drawing, fall_time, fall_time - fall_seconds() - kShineGap);
        else if (g_plus_enabled && g_anchor_drawing && g_plus_world_known && g_warm_plus) {
            g_warm_plus = false;
            draw_plus(self, g_anchor_drawing, 0, 0, true);
        }
        // The last node of the frame has been drawn: the box is complete (the node may be drawn twice in a frame).
        if (g_box_shapes >= 10) {
            g_box = g_box_building;
            g_box_building = kEmptyBox;
            g_box_shapes = 0;
        }
    }
}

void KYUBU_CALL cmd_replay(const char*, KyubuPrintFn print, void* pctx, void*) {
    g_started = -1;
    print(pctx, "title_plus: the logo starts over the next time it is drawn");
}

#ifdef TITLE_PLUS_FRAME_LOG
// Measurement builds only (-DTITLE_PLUS_FRAME_LOG): the time of every frame from the logo's first drawing on, logged
// once 14 s later as half-second buckets and the longest frames. The release build has none of this.
// The counts are subtracted as integers: Direct3D 9 puts the x87 unit in single precision, so any double made from the
// whole counter is rounded to a sixteenth of a second after a few days of uptime.
struct FrameSample { float at, ms; };
FrameSample g_frames[8192];
unsigned g_frame_count = 0, g_clock_steps = 0;
LONGLONG g_first_count = 0, g_last_count = 0;
double g_last_clock = -1;
bool g_frames_logged = false;

void log_frames() {
    g_mod->info("frames: red at %.2f s, fall at %.2f s, shine at %.2f s", g_delay, g_delay + g_fade,
                g_delay + g_fade + fall_seconds() + kShineGap);
    g_mod->info("frames: the mod's clock moved on %u of %u frames", g_clock_steps, g_frame_count);
    for (int b = 0; b < 28; ++b) {
        unsigned n = 0;
        float sum = 0, worst = 0;
        for (unsigned i = 0; i < g_frame_count; ++i)
            if (g_frames[i].at >= b * 0.5f && g_frames[i].at < (b + 1) * 0.5f) ++n, sum += g_frames[i].ms, worst = std::max(worst, g_frames[i].ms);
        g_mod->info("frames %5.1f-%5.1f s: %3u frames, mean %6.2f ms, max %6.2f ms", b * 0.5f, (b + 1) * 0.5f, n, n ? sum / n : 0, worst);
    }
    std::vector<FrameSample> sorted(g_frames, g_frames + g_frame_count);
    std::sort(sorted.begin(), sorted.end(), [](const FrameSample& a, const FrameSample& b) { return a.ms > b.ms; });
    for (size_t i = 0; i < sorted.size() && i < 12; ++i) g_mod->info("frames: long frame %6.2f ms at %.3f s", sorted[i].ms, sorted[i].at);
}

void frame_sample() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const double clock = now_seconds();
    if (g_started >= 0 && !g_frames_logged) {
        if (!g_first_count) g_first_count = c.QuadPart;
        if (g_last_count) {
            float at = float(double(c.QuadPart - g_first_count) / double(freq.QuadPart));
            float ms = float(double(c.QuadPart - g_last_count) * 1000.0 / double(freq.QuadPart));
            if (g_frame_count < 8192) g_frames[g_frame_count++] = { at, ms };
            if (clock != g_last_clock) ++g_clock_steps;
            if (at > 14) {
                g_frames_logged = true;
                log_frames();
            }
        }
        g_last_count = c.QuadPart;
    }
    g_last_clock = clock;
}
#endif

}  // namespace

struct TitlePlus {
    explicit TitlePlus(kyubu::Mod& m) : mod(m) {
        g_mod = &m;
        g_delay = m.config_float("delay_seconds", 3.0);
        g_fade = std::max(m.config_float("fade_seconds", 2.0), 0.1);
        g_plus_enabled = m.config_bool("plus", true);
        g_shine_enabled = m.config_bool("shine", true);
        g_shine_seconds = std::max(float(m.config_float("shine_seconds", 0.7)), 0.1f);

        g_controller_global = m.symbol("cube::game_controller");
        if (!g_controller_global) {
            m.warn("cube::game_controller is not available on this build: the title screen is left alone");
            return;
        }
        if (!m.hook("title_plus:plasma::D3D9Drawing::draw", &draw_hook, &g_draw_next)) {
            m.warn("plasma::D3D9Drawing::draw is not available on this build: the title screen is left alone");
            return;
        }
        m.api()->register_command(m.handle(), "title_plus:replay", "play the title screen's change to red again", cmd_replay, nullptr);
#ifdef TITLE_PLUS_FRAME_LOG
        m.on("frame.begin", [](KyubuEvent&) { frame_sample(); });
#endif
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("title_plus", KY_SIDE_CLIENT, TitlePlus)
