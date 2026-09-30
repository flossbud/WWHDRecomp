"""Port one of Cemu's gx2 source files into src/os/gx2 (design D18: our gx2).

Usage:
    python3 tools/gx2_port.py GX2_State.cpp state "blend, depth, stencil, raster, viewport and scissor state"

Reads $CEMU_SRC/src/Cafe/OS/libs/gx2/<file> (default ~/opt/cemu-src, the pinned commit with
tools/reference/cemu-patches) and writes src/os/gx2/<name>.cpp: the same code (MPL-2.0, with a header
saying where it comes from) in namespace wwhd::gx2 instead of GX2, calls to its own functions
qualified (argument-dependent lookup would also find Cemu's), and its exports registered through our
OS layer (src/os/os.h) instead of Cemu's registration function, which must do nothing else. The port
is then ours to edit; tools/reference/stream_check.sh checks it sends Cemu's commands.
"""
import os
import re
import sys

CEMU = os.path.join(os.environ.get('CEMU_SRC', os.path.expanduser('~/opt/cemu-src')), 'src/Cafe/OS/libs/gx2/')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../src/os/gx2/')

name, out, what = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(CEMU + name, encoding='utf-8-sig').read()
lines = s.split('\n')

regs = []  # (export name, function expression, typed?)
def collect(text):
    for m in re.finditer(r'cafeExportRegister\("gx2",\s*(\w+),', text):
        regs.append((m.group(1), m.group(1), True))
    for m in re.finditer(r'cafeExportRegisterFunc\(([\w:]+),\s*"gx2",\s*"(\w+)"', text):
        regs.append((m.group(2), m.group(1), True))
    for m in re.finditer(r'osLib_addFunction\("gx2",\s*"(\w+)",\s*([\w:]+)\)', text):
        regs.append((m.group(1), m.group(2), False))

# remove functions that only register exports
out_lines, i = [], 0
while i < len(lines):
    m = re.match(r'^(\t*)void (\w+)\(\)\s*$', lines[i])
    if m and i + 1 < len(lines) and lines[i + 1] == m.group(1) + '{':
        j = i + 2
        while lines[j] != m.group(1) + '}':
            j += 1
        body = '\n'.join(lines[i + 2:j])
        if 'cafeExportRegister' in body or 'osLib_addFunction' in body:
            rest = [l.strip() for l in lines[i + 2:j] if l.strip() and not re.match(r'(cafeExportRegister|osLib_addFunction|//)', l.strip())]
            if rest:
                sys.exit(f'{name}: {m.group(2)}() does more than register: {rest}')
            collect(body)
            i = j + 1
            continue
    out_lines.append(lines[i])
    i += 1

# split includes from the rest
last_include = max(k for k, l in enumerate(out_lines) if l.startswith('#include'))
includes = [re.sub(r'#include "(GX2[\w]*\.h)"', r'#include "Cafe/OS/libs/gx2/\1"', l) for l in out_lines[:last_include + 1]]
body = out_lines[last_include + 1:]

# dissolve namespace GX2 blocks: the closer is the first column-0 brace after the opener
res, k = [], 0
while k < len(body):
    if re.match(r'^namespace GX2\s*$', body[k]) and body[k + 1].strip() == '{':
        k += 2
        depth_end = k
        while not re.match(r'^\};?\s*$', body[depth_end]):
            depth_end += 1
        res.extend(body[k:depth_end])
        k = depth_end + 1
        continue
    if re.match(r'^namespace GX2\s*\{\s*$', body[k]):
        k += 1
        depth_end = k
        while not re.match(r'^\};?\s*$', body[depth_end]):
            depth_end += 1
        res.extend(body[k:depth_end])
        k = depth_end + 1
        continue
    res.append(body[k])
    k += 1

# calls to functions defined here go to ours: argument-dependent lookup would also find Cemu's
DEF = re.compile(r'^\s*(?:static\s+|inline\s+)*[\w:<>]+[\s\*&]+(\w+)\(')
defined = {m.group(1) for l in res if (m := DEF.match(l)) and not l.strip().startswith(('return', 'else', 'if', 'case'))}
def qualify(line):
    if DEF.match(line) and not line.strip().startswith(('return', 'else', 'if', 'case')):
        return line
    for n in defined:
        line = re.sub(r'(?<![\w:.>])' + n + r'\(', 'wwhd::gx2::' + n + '(', line)
    return line
res = [qualify(l) for l in res]

header = f'''// gx2: {what} (docs/recompiler-design.md D18). Derived from Cemu's src/Cafe/OS/libs/gx2/{name}
// (Mozilla Public License 2.0), unchanged but for the namespace and registration. Commands go through
// gx2's command pipe (GX2_Command.h) and must stay byte for byte what Cemu's sent (WWHD_GPU_STREAM,
// tools/reference/stream_check.sh).'''
text = header + '\n' + '\n'.join(includes) + '\n#include "Cafe/OS/libs/gx2/GX2_Command.h"\n#include "../os.h"\n\n'
text += 'namespace wwhd::gx2\n{\n\tusing namespace ::GX2;\n' + '\n'.join(res).rstrip() + '\n}\n\n'
for export, fn, typed in regs:
    fn = fn.replace('GX2::', '')
    if typed:
        text += f'WWHD_OS_EXPORT(gx2, {export}, wwhd::gx2::{fn});\n'
    else:
        text += f'static ::wwhd::os::Registration wwhd_os_reg_gx2_{export}("gx2", "{export}", wwhd::gx2::{fn});\n'
open(OUT + out + '.cpp', 'w').write(text)
print(f'{name} -> {out}.cpp: {len(regs)} exports')
