#!/usr/bin/env python3
"""tls_across_calls.py DISASSEMBLY - functions that reuse a thread-pointer value across a call.

With three host threads (WWHD_CORES=3) a guest thread's fiber can resume on another host thread
(WWHD_THREAD_STATS's "moves"; docs/research/threads.md). A function that loads the thread pointer
(`mov %fs:0x0,%reg`), keeps it in a callee-saved register across a call (which may switch fibers) and
then addresses thread-local data through it reads the old host thread's copy. Input: `objdump -d
--no-show-raw-insn` of the binary (streamed). Out: one line per function that does it (uses after a
call, the function), most first, then the totals. A crude register tracker: any write to the register
ends its life; a conditional path that leaves it intact is not followed (an over-count, never an
under-count on a straight line).
"""
import re
import sys

SAVED = ('rbx', 'rbp', 'r12', 'r13', 'r14', 'r15')
fn_re = re.compile(r'^[0-9a-f]+ <(.*)>:$')
load_re = re.compile(r'\bmov\s+%fs:0x0,%(\w+)$')
call_re = re.compile(r'\bcall')
counts = {}
spills = {}               # a thread-pointer value stored to the stack (not followed)
fns = 0
with open(sys.argv[1], errors='replace') as f:
    fn = None
    live = {}                        # reg -> crossed a call since its load
    for line in f:
        line = line.rstrip('\n')
        m = fn_re.match(line)
        if m:
            fn, live = m.group(1), {}
            fns += 1
            continue
        if fn is None or ':\t' not in line:
            continue
        ins = line.split(':\t', 1)[1].split('#')[0].strip()
        m = load_re.search(ins)
        if m:
            live[m.group(1)] = False
            continue
        if call_re.match(ins):
            for r in list(live):
                if r in SAVED:
                    live[r] = True
                else:
                    del live[r]          # a caller-saved register doesn't survive the call
            continue
        if not live:
            continue
        parts = ins.split(None, 1)
        ops = parts[1] if len(parts) > 1 else ''
        dst = re.search(r',%(\w+)$', ops)
        dst = dst.group(1) if dst else None
        src = ops.rsplit(',', 1)[0] if dst else ops
        crossed = None
        for r in list(live):
            if '%' + r in src:
                if live[r] and ('(%' + r in src or ',%' + r + ')' in src or parts[0] in ('mov', 'lea')):
                    counts[fn] = counts.get(fn, 0) + 1
                if parts[0] in ('lea', 'mov') and dst and not src.startswith('('):
                    crossed = live[r]         # a pointer derived from it (lea N(%r),%d; mov %r,%d)
                if parts[0] == 'mov' and not dst and '(%rsp)' in ops or '(%rbp)' in ops and parts[0] == 'mov' and src == '%' + r:
                    spills[fn] = spills.get(fn, 0) + 1
        if dst:
            live.pop(dst, None)
            if crossed is not None:
                live[dst] = crossed
        if parts[0].startswith('pop'):
            live.pop(ops.lstrip('%'), None)
for fn, n in sorted(counts.items(), key=lambda kv: -kv[1]):
    print(n, fn)
for fn, n in sorted(spills.items(), key=lambda kv: -kv[1]):
    print('spill', n, fn)
print(f'# {len(counts)} of {fns} functions use a thread pointer across a call, {sum(counts.values())} uses; {len(spills)} store one to the stack')
