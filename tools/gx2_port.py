"""Port Cemu's gx2 source files into src/os/gx2 (design D18: our gx2).

Usage:
    python3 tools/gx2_port.py

Reads $CEMU_SRC/src/Cafe/OS/libs/gx2/ (default ~/opt/cemu-src: the pinned commit with
tools/reference/cemu-patches) and, for every file in PORTS, writes src/os/gx2/<name>.cpp: the same
code (MPL-2.0, with a header saying where it comes from) in namespace wwhd::gx2 instead of GX2, and
its exports registered through our OS layer (src/os/os.h) instead of Cemu's registration function
(which must do nothing else; functions GX2.cpp registers for other files are registered by theirs).
Every call to a ported function, qualified GX2:: or not, goes to ours (argument-dependent lookup
would also find Cemu's), and src/os/gx2/ported.h declares those called across files. The ports are
then ours to edit; tools/reference/stream_check.sh checks that they send Cemu's commands. Running
this again overwrites them.
"""
import os
import re

CEMU = os.path.join(os.environ.get('CEMU_SRC', os.path.expanduser('~/opt/cemu-src')), 'src/Cafe/OS/libs/gx2/')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../src/os/gx2/')

# Cemu's file, ours, what it is
PORTS = [
    ('GX2_Texture.cpp', 'texture', 'textures and samplers'),
    ('GX2_State.cpp', 'state', 'blend, depth, stencil, raster, viewport and scissor state'),
    ('GX2_Shader.cpp', 'shader', 'fetch shaders, shader registers, uniforms'),
    ('GX2_Surface.cpp', 'surface', 'surface sizes and layouts'),
    ('GX2_Draw.cpp', 'draw', 'draws'),
    ('GX2_Blit.cpp', 'blit', 'clears'),
    ('GX2_Streamout.cpp', 'streamout', 'stream-out'),
    ('GX2_Query.cpp', 'query', 'occlusion queries'),
    ('GX2_Surface_Copy.cpp', 'surface_copy', 'surface copies'),
    ('GX2_ContextState.cpp', 'context_state', 'context states (register shadowing) and the default state'),
    ('GX2_RenderTarget.cpp', 'render_target', 'color and depth buffers'),
    ('GX2_shader_legacy.cpp', 'shader_legacy', 'shader and uniform-block setters'),
    ('GX2_TilingAperture.cpp', 'tiling_aperture', 'tiling apertures'),
    ('GX2_Resource.cpp', 'resource', 'GX2R buffers and surfaces'),
    ('GX2_Memory.cpp', 'memory', 'the default allocator'),
]
# lines a registration function may also have: they reset state that ours starts with anyway
ALLOWED_IN_REGISTRATION = {'GX2RAllocateFunc = MPTR_NULL;', 'GX2RFreeFunc = MPTR_NULL;'}
# ported functions that hand-ported files (src/os/gx2/misc.cpp) call
EXTRA_DECLARED = ['GX2CalcSurfaceSizeAndAlignment']

DEF = re.compile(r'^\s*(?:static\s+|inline\s+)*[\w:<>]+[\s\*&]+(\w+)\(')
NOT_DEF = ('return', 'else', 'if', 'case', 'while', 'for', 'switch', 'new', 'delete')


def is_def(line):
    return DEF.match(line) and not line.strip().startswith(NOT_DEF)


def registration_functions(lines):
    """Remove the functions that only register exports; return (remaining lines, registration text)."""
    out, regs, i = [], [], 0
    while i < len(lines):
        m = re.match(r'^(\t*)void (\w+)\(\)\s*$', lines[i])
        if m and i + 1 < len(lines) and lines[i + 1] == m.group(1) + '{':
            j = i + 2
            while lines[j] != m.group(1) + '}':
                j += 1
            body = lines[i + 2:j]
            if any('cafeExportRegister' in l or 'osLib_addFunction' in l for l in body):
                rest = [l.strip() for l in body if l.strip() and not re.match(r'(cafeExportRegister|osLib_addFunction|//)', l.strip())
                        and l.strip() not in ALLOWED_IN_REGISTRATION]
                if rest:
                    raise SystemExit(f'{m.group(2)}() does more than register: {rest}')
                regs.extend(body)
                i = j + 1
                continue
        out.append(lines[i])
        i += 1
    return out, '\n'.join(regs)


def exports(text):
    """(export name, function, typed) for every registration in text."""
    found = []
    for m in re.finditer(r'cafeExportRegister\("gx2",\s*(\w+),', text):
        found.append((m.group(1), m.group(1), True))
    for m in re.finditer(r'cafeExportRegisterFunc\(([\w:]+),\s*"gx2",\s*"(\w+)"', text):
        found.append((m.group(2), m.group(1).replace('GX2::', ''), True))
    for m in re.finditer(r'osLib_addFunction\("gx2",\s*"(\w+)",\s*([\w:]+)\)', text):
        found.append((m.group(1), m.group(2).replace('GX2::', ''), False))
    return found


def dissolve_namespaces(body):
    """Drop `namespace GX2 {` and its closer, the first column-0 brace after it (Cemu's style)."""
    res, k = [], 0
    while k < len(body):
        opener = re.match(r'^namespace GX2\s*(\{)?\s*$', body[k])
        if opener:
            k += 1 if opener.group(1) else 2
            end = k
            while not re.match(r'^\};?\s*$', body[end]):
                end += 1
            res.extend(body[k:end])
            k = end + 1
            continue
        res.append(body[k])
        k += 1
    return res


# GX2.cpp registers functions of other files
gx2cpp = open(CEMU + 'GX2.cpp', encoding='utf-8-sig').read()
module_exports = exports(gx2cpp[gx2cpp.index('void RPLMapped() override'):])

files = []
for cemu_name, ours, what in PORTS:
    lines = open(CEMU + cemu_name, encoding='utf-8-sig').read().split('\n')
    lines, reg_text = registration_functions(lines)
    last_include = max(k for k, l in enumerate(lines) if l.startswith('#include'))
    includes = [re.sub(r'#include "(GX2[\w]*\.h)"', r'#include "Cafe/OS/libs/gx2/\1"', l) for l in lines[:last_include + 1]]
    body = dissolve_namespaces(lines[last_include + 1:])
    defined = {}
    for k, l in enumerate(body):
        if is_def(l) and not l.strip().startswith('static'):
            signature, j = l.strip(), k
            while not signature.endswith(')'):          # parameters over several lines
                j += 1
                signature += ' ' + body[j].strip()
            defined[DEF.match(l).group(1)] = signature
    local = {DEF.match(l).group(1) for l in body if is_def(l) and not l.rstrip().endswith(';')}
    # declarations of functions defined elsewhere (Latte's, Cemu's) stay in their namespace
    hoisted = [l for l in body if is_def(l) and l.rstrip().endswith(';') and DEF.match(l).group(1) not in local]
    body = [l for l in body if l not in hoisted]
    includes += hoisted
    regs = exports(reg_text) + [e for e in module_exports if e[1] in local]
    files.append(dict(cemu=cemu_name, ours=ours, what=what, includes=includes, body=body, defined=defined, local=local, regs=regs))

ported = set().union(*(f['local'] for f in files))
signatures = {}
for f in files:
    signatures.update(f['defined'])


def qualify(line, used):
    if is_def(line):
        return line
    for n in ported:
        if n + '(' not in line:
            continue
        new = re.sub(r'(?<![\w.>:])(?:::)?(?:GX2::)?' + n + r'\(', 'wwhd::gx2::' + n + '(', line)
        new = new.replace('wwhd::gx2::wwhd::gx2::', 'wwhd::gx2::')
        if new != line:
            used.add(n)
            line = new
    return line


cross = set()
for f in files:
    used = set()
    f['body'] = [qualify(l, used) for l in f['body']]
    cross |= {n for n in used if n not in f['local']}

cross |= set(EXTRA_DECLARED)
header = '''// gx2 functions called across our gx2 files (tools/gx2_port.py writes this).
#pragma once
''' + ''.join(f'#include "Cafe/OS/libs/gx2/{h}"\n' for h in sorted(f for f in os.listdir(CEMU) if f.endswith('.h'))) + '''
namespace wwhd::gx2
{
	using namespace ::GX2;
'''
for n in sorted(cross):
    header += '\t' + signatures[n] + ';\n'
header += '}\n'
open(OUT + 'ported.h', 'w').write(header)

for f in files:
    text = f'''// gx2: {f['what']} (docs/recompiler-design.md D18). Derived from Cemu's
// src/Cafe/OS/libs/gx2/{f['cemu']} (Mozilla Public License 2.0) by tools/gx2_port.py: our namespace
// and registration. Commands go through gx2's command pipe (GX2_Command.h) and must stay byte for byte
// what Cemu's sent (WWHD_GPU_STREAM, tools/reference/stream_check.sh).
'''
    text += '\n'.join(f['includes']) + '\n#include "Cafe/OS/libs/gx2/GX2_Command.h"\n#include "ported.h"\n#include "../os.h"\n\n'
    text += 'namespace wwhd::gx2\n{\n\tusing namespace ::GX2;\n' + '\n'.join(f['body']).rstrip() + '\n}\n\n'
    for export, fn, typed in f['regs']:
        if typed:
            text += f'WWHD_OS_EXPORT(gx2, {export}, wwhd::gx2::{fn});\n'
        else:
            text += f'static ::wwhd::os::Registration wwhd_os_reg_gx2_{export}("gx2", "{export}", wwhd::gx2::{fn});\n'
    open(OUT + f['ours'] + '.cpp', 'w').write(text)
    print(f"{f['cemu']} -> {f['ours']}.cpp: {len(f['regs'])} exports")
print(f'ported.h: {len(cross)} functions called across files')
