"""Compiles logo_ps.hlsl to logo_ps.h with Windows' d3dcompiler_47.dll: the logo's pixel shader (ps_3_0) and the shine's
shader for the plus (ps_2_0). Run it after changing the shader:
python compile_shader.py"""
import ctypes, pathlib
here = pathlib.Path(__file__).parent
src = (here / 'logo_ps.hlsl').read_bytes()
dc = ctypes.WinDLL('d3dcompiler_47')
def blob(p):
    vt = ctypes.cast(ctypes.cast(p, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(p)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(p)
    return ctypes.string_at(ptr, size)
def compile_shader(entry, profile):
    code, errors = ctypes.c_void_p(), ctypes.c_void_p()
    hr = dc.D3DCompile(src, len(src), b'logo_ps.hlsl', None, None, entry, profile, 0, 0, ctypes.byref(code), ctypes.byref(errors))
    if hr < 0:
        raise SystemExit(blob(errors).decode())
    return blob(code)
lines = ['// Written by compile_shader.py from logo_ps.hlsl. Do not edit by hand.']
for name, entry, profile in (('kLogoPixelShader', b'main', b'ps_3_0'), ('kShinePixelShader', b'plus_shine', b'ps_2_0')):
    data = compile_shader(entry, profile)
    words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
    lines += ['const unsigned long %s[] = {' % name]
    lines += ['    ' + ' '.join('0x%08X,' % w for w in words[i:i + 8]) for i in range(0, len(words), 8)]
    lines += ['};']
    print(name, len(data), 'bytes')
lines += ['']
(here / 'logo_ps.h').write_text('\n'.join(lines), newline='\n')
