"""Create the forks src/forks.txt lists that don't exist yet (design D18).

Usage:
    python3 tools/cemu_fork.py

A fork is a copy of one of Cemu's source files ($CEMU_SRC, default ~/opt/cemu-src: the pinned commit
with tools/reference/cemu-patches) under Cemu's names, with a header saying where it comes from
(MPL-2.0). The build (src/CMakeLists.txt) compiles it into Cemu's target in place of Cemu's file. Existing forks are ours
and never overwritten.
"""
import os
import textwrap

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
cemu = os.path.join(os.environ.get('CEMU_SRC', os.path.expanduser('~/opt/cemu-src')), 'src')
for line in open(os.path.join(root, 'src/forks.txt')):
    if not line.strip() or line.startswith('#'):
        continue
    ours, theirs, what = line.split(None, 2)
    path = os.path.join(root, 'src', ours)
    if os.path.exists(path):
        continue
    source = open(os.path.join(cemu, theirs), encoding='utf-8-sig').read()
    head = (f'{os.path.basename(ours)} (docs/recompiler-design.md D18): {what.strip()}. Cemu\'s src/{theirs} '
            '(Mozilla Public License 2.0), forked: wwhd-null links this in place of Cemu\'s object, under '
            'Cemu\'s names (so Cemu\'s code that calls it reaches ours). Route traces, GPU commands and sound '
            'must stay as Cemu\'s (tools/reference/stream_check.sh).')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'w').write(''.join(f'// {l}\n' for l in textwrap.wrap(head, 97)) + source)
    print(f'forked src/{theirs} -> src/{ours}')
