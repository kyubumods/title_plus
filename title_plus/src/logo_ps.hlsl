// The pixel shader title_plus draws the logo with while it turns red, and after. It does what the engine's own
// pixel shader does (the vertex colour, an optional texture with its colour controls, an optional mask), then turns
// the result red where a noise pattern, fixed on the screen, is below a threshold that the mod raises over time.
// compile_shader.py compiles it to logo_ps.h.
bool textureEnabled : register(b0);      // b0 and c0 to c4, s0 and s1 are set by the game, as for its own shader
float filter : register(c0);             // 0: no mask, 1: the mask's colour over the shape, 2: times the mask, other: the mask's alpha
float textureOpacity : register(c1);
float textureBrightness : register(c2);
float textureContrast : register(c3);
float textureSaturation : register(c4);
sampler2D maskSampler : register(s0);
sampler2D textureSampler : register(s1);
float4 spread : register(c30);           // set by the mod. x: threshold, y: softness of the front, z: 1 / blotch size in pixels
float4 shine_line : register(c28);       // set by the mod. xy: unit normal of the shine's line on the screen, z: where it is along that
                                         // normal in pixels, w: strength (0: no shine)
float4 shine_shape : register(c29);      // x: half width of the line in pixels, y: half width of the glow around it, z: its strength

// How much white the shine adds at a screen position: a line with a crisp edge (a pixel of softness) and a soft glow.
float shine_amount(float2 pixel) {
    float d = abs(dot(pixel, shine_line.xy) - shine_line.z);
    float line_edge = 1 - smoothstep(shine_shape.x - 1.0, shine_shape.x + 1.0, d);
    float glow = shine_shape.z * (1 - smoothstep(0, shine_shape.y, d));
    return saturate(max(line_edge, glow)) * shine_line.w;
}

float hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

float noise(float2 p) {
    float2 cell = floor(p), f = p - cell;
    f = f * f * (3 - 2 * f);
    return lerp(lerp(hash(cell), hash(cell + float2(1, 0)), f.x),
                lerp(hash(cell + float2(0, 1)), hash(cell + float2(1, 1)), f.x), f.y);
}

float4 shape_colour(float4 colour, float2 maskUv, float2 textureUv) {
    float4 c;
    if (textureEnabled) {
        float4 t = tex2D(textureSampler, textureUv);
        float3 rgb = t.rgb;
        if (textureSaturation != 1) rgb = lerp(rgb, dot(float3(0.3, 0.59, 0.11), rgb), 1 - textureSaturation);
        if (textureBrightness != 1) rgb *= textureBrightness;
        if (textureContrast != 1) rgb = lerp(rgb, 0.5, 1 - textureContrast);
        c = lerp(colour, colour * float4(min(rgb, 1), t.a), textureOpacity);
    } else {
        c = textureContrast * colour;
    }
    if (filter == 0) return c;
    float4 m = tex2D(maskSampler, maskUv);
    if (m.a > 0) m.rgb /= m.a;
    if (filter == 1) return float4(lerp(c.rgb, m.rgb, m.a), c.a);
    if (filter == 2) return c * m;
    return float4(c.rgb, c.a * m.a);
}

// The logo's art is shaded along one ramp of blues: a dark navy for the sides, a vivid blue for the line round each
// face, an azure for the face (lighter at the top), and towards white where the bevel catches the light. The red
// follows the same ramp, so every part keeps its tone: that is what keeps the face, its line and its side apart.
// The knots are a reference picture's colours at each position on the blue ramp, fitted offline on the two pictures
// laid over each other. y is the luminance of the blue part of the colour.
float3 red_ramp(float y) {
    float3 c = float3(0, 0, 0);
    c = lerp(c, float3(0.1392, 0.0167, 0.0235), saturate((y - 0.0000) / 0.0941));
    c = lerp(c, float3(0.2069, 0.0265, 0.0382), saturate((y - 0.0941) / 0.0235));
    c = lerp(c, float3(0.4265, 0.0676, 0.0873), saturate((y - 0.1176) / 0.1647));
    c = lerp(c, float3(0.5196, 0.0931, 0.1118), saturate((y - 0.2824) / 0.0471));
    c = lerp(c, float3(0.7941, 0.1941, 0.2020), saturate((y - 0.3294) / 0.1647));
    c = lerp(c, float3(0.8206, 0.2059, 0.2147), saturate((y - 0.4941) / 0.0235));
    c = lerp(c, float3(0.8588, 0.2314, 0.2471), saturate((y - 0.5176) / 0.0706));
    return c;
}

// How far a colour with this much grey in it (the white of a highlight) goes to the pink-white of the reference.
float pale_amount(float grey) {
    float p = 0;
    p = lerp(p, 0.07, saturate((grey - 0.0000) / 0.0275));
    p = lerp(p, 0.18, saturate((grey - 0.0275) / 0.0431));
    p = lerp(p, 0.27, saturate((grey - 0.0706) / 0.0549));
    p = lerp(p, 0.49, saturate((grey - 0.1255) / 0.0549));
    p = lerp(p, 0.65, saturate((grey - 0.1804) / 0.0980));
    p = lerp(p, 0.79, saturate((grey - 0.2784) / 0.0980));
    p = lerp(p, 0.89, saturate((grey - 0.3765) / 0.0902));
    p = lerp(p, 1.00, saturate((grey - 0.4667) / 0.0745));
    return p;
}

float4 main(float4 colour : COLOR0, float2 maskUv : TEXCOORD0, float2 textureUv : TEXCOORD1, float2 pixel : VPOS) : COLOR0 {
    float4 c = shape_colour(colour, maskUv, textureUv);
    // The noise is between 0 and 1: once the threshold is above it (the mod sets 4 when the red is all over) every
    // pixel is red, and the noise, 12 sines, is skipped.
    float red = 1;
    [branch] if (spread.x < 2) {
        float2 p = pixel * spread.z;
        float n = 0.55 * noise(p) + 0.30 * noise(p * 2.3 + 17.0) + 0.15 * noise(p * 5.1 + 43.0);
        red = 1 - smoothstep(spread.x - spread.y, spread.x + spread.y, n);
    }
    // Only what is blue changes: black stays black (the outline), white stays white, and a colour that is red
    // already (a shape drawn with this shader before) is not changed twice. The grey part of a colour is its
    // smallest channel; it is what a highlight adds to the blue.
    float grey = min(c.r, min(c.g, c.b));
    float3 blue = c.rgb - grey;
    float3 crimson = lerp(red_ramp(dot(blue, float3(0.2126, 0.7152, 0.0722))), float3(0.8941, 0.7137, 0.7373), pale_amount(grey));
    c.rgb = lerp(c.rgb, crimson, red * saturate((c.b - c.r) * 8));
    c.rgb = lerp(c.rgb, 1, shine_amount(pixel));
    return c;
}

// The same shine over a shape the mod draws itself (the plus): white, as opaque as the line and its glow, times the
// shape's own opacity. Compiled as ps_2_0 so that it can go with the fixed-function vertices; the screen position
// comes in the texture coordinate.
float4 plus_shine(float4 colour : COLOR0, float2 pixel : TEXCOORD0) : COLOR0 {
    return float4(1, 1, 1, colour.a * shine_amount(pixel));
}
