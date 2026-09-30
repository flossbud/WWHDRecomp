"""Summarise a wwhd-null CPU profile (src/runtime/profile.cpp, WWHD_PROFILE=path).

Usage:
    python3 tools/profile_report.py PROFILE [--top N]

Symbolizes every sampled address with llvm-symbolizer (the program and every shared object it
loaded) and prints: CPU time per thread (from /proc, exact), then per thread group the share of
samples by category (the leaf: where the time is spent) and by the categories on the stack (what
the time is spent under), and the top functions, self and inclusive. Recompiled guest functions
(f_XXXXXXXX) are named from config/US_v0/symbols.csv where it has a name. Run it on the worker.
"""
import collections
import csv
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def load(path):
    info, threads, segments, samples = {}, {}, [], []
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == 's':
            samples.append((int(parts[1]), [int(x, 16) for x in parts[2:]]))
        elif parts[0] == 'thread':
            threads[int(parts[1])] = (' '.join(parts[4:]), int(parts[2]), int(parts[3]))
        elif parts[0] == 'segment':
            segments.append((int(parts[1], 16), int(parts[2], 16), int(parts[3], 16), ' '.join(parts[4:])))
        else:
            info[parts[0]] = ' '.join(parts[1:])
    return info, threads, segments, samples


def symbolize(info, segments, addresses):
    """address -> function name, via llvm-symbolizer per object."""
    by_object = collections.defaultdict(list)
    names = {}
    for a in addresses:
        for start, end, base, obj in segments:
            if start <= a < end:
                by_object[(obj, base)].append(a)
                break
        else:
            names[a] = '?'
    for (obj, base), addrs in by_object.items():
        path = info['exe'] if obj == 'exe' else obj
        # return addresses point after the call: look up the byte before, so the call's line counts
        query = '\n'.join(hex(a - base - 1) for a in addrs) + '\n'
        out = subprocess.run(['llvm-symbolizer', '--obj=' + path, '--functions=linkage', '--no-inlines', '--demangle',
                              '--output-style=GNU'], input=query, capture_output=True, text=True).stdout.split('\n')
        funcs = out[0::2]
        short = os.path.basename(path)
        for a, fn in zip(addrs, funcs):
            names[a] = fn if fn and fn != '??' else f'?{short}'
    return names


def guest_names():
    names = {}
    path = os.path.join(ROOT, 'config/US_v0/symbols.csv')
    if os.path.exists(path):
        for row in csv.DictReader(open(path)):
            addr = (row.get('address') or row.get('addr') or '').lower().replace('0x', '')
            if addr and row.get('name'):
                names[f'f_{int(addr, 16):08X}'] = row['name']
    return names


# (category, pattern), first match wins; a sample counts for the first category found walking up its
# stack from the interrupted function, so time in libc goes to whoever called it
CATEGORIES = [
    ('guest code', re.compile(r'^f_[0-9A-F]{8}\b')),
    ('guest code helpers', re.compile(r'^(fcmpu|fcmpo|psq_|frsqrte|fres|rt_div|rt_mul|ps_)|_espresso\(')),
    ('trace writing', re.compile(r'HLETrace|ZSTD_|zstd', re.I)),
    ('message queues', re.compile(r'OSSendMessage|OSReceiveMessage|OSJamMessage|MessageQueue')),
    ('mutexes and events', re.compile(r'OSLockMutex|OSUnlockMutex|OSTryLockMutex|OSSignalEvent|OSWaitEvent|OSResetEvent|'
                                      r'Semaphore|OSFastMutex|Spinlock|OSUninterruptible|OSWaitCond|OSSignalCond')),
    ('thread switches', re.compile(r'__OSThreadSwitch|swapcontext|makecontext|Fiber::|__OSSwapContext|__OSQueueThread|'
                                   r'coreinit::OSSleepThread|coreinit::OSWakeupThread|__OSScheduleThread|__OSResumeThread|'
                                   r'coreinit::__OSCheckSuspend')),
    ('scheduler (other)', re.compile(r'__OSLockScheduler|__OSUnlockScheduler|OSThread|PPCCore_|PPCTimer_|Alarm|'
                                     r'coreinit::__OS|coreinit::OSYield|coreinit::OSSleepTicks|PPCScheduler|'
                                     r'OSSchedulerCoreEmulationThread')),
    ('gx2', re.compile(r'GX2|gx2')),
    ('snd_core', re.compile(r'snd_core|AXIst|AXOut|AXMix|AXVoice|AXPB')),
    ('GPU (null GPU, renderer)', re.compile(r'wwhd::gpu|LatteGPUState|TCL::|processBuffer|packet\(|streamhash|Latte|'
                                            r'handleTimedVsync|\?libvulkan|lvp_|llvmpipe|\?libLLVM', re.I)),
    ('OS (other)', re.compile(r'coreinit|wwhd::os|nn::|padscore|vpad|FSC|iosu|IOSU|nsys|proc_ui|swkbd|erreula|OSDynLoad')),
    ('HLE dispatch', re.compile(r'PPCInterpreter_virtualHLE|cafeExportCallWrapper|osLib_|rt_import')),
    ('runtime', re.compile(r'^rt_|wwhd::rt::|g_ppcExecuteHook')),
]
UNIVERSAL = re.compile(r'wwhd::rt::Execute|__OSFiberThreadEntry|^clone$|threadEntry|\?lib')


def category(stack_names):
    for fn in stack_names:
        for name, rx in CATEGORIES:
            if rx.search(fn) and not (name == 'runtime' and UNIVERSAL.search(fn)):
                return name
    return 'libc/other'


def group(thread_name):
    """Threads by role, from Cemu's thread names."""
    n = thread_name.lower()
    if 'cpu' in n or 'core' in n or n.startswith('ppc'):
        return 'CPU cores'
    if 'gpu' in n or 'latte' in n or 'vk' in n or 'lvp' in n or 'llvmpipe' in n:
        return 'GPU'
    return thread_name


def main():
    path = sys.argv[1]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 25
    info, threads, segments, samples = load(path)
    tick = int(info.get('ticks_per_second', 100))
    names = symbolize(info, segments, {a for _, st in samples for a in st})
    gnames = guest_names()

    def label(a):
        fn = names.get(a, '?')
        return f'{fn} ({gnames[fn]})' if fn in gnames else fn

    print(f"{info.get('samples')} samples at {info.get('hz')} Hz of CPU time from {info.get('exe')}\n")
    print('CPU time by thread (from /proc):')
    total = sum(u + s for _, u, s in threads.values()) or 1
    by_name = collections.Counter()
    for name, u, s in threads.values():
        by_name[name] += u + s
    for name, t in by_name.most_common():
        if t:
            print(f'  {t / tick:9.1f} s  {t / total * 100:5.1f}%  {name}')

    groups = collections.defaultdict(list)
    for tid, stack in samples:
        name = threads.get(tid, (f'tid {tid}', 0, 0))[0]
        groups[group(name)].append(stack)
    for g, stacks in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        n = len(stacks)
        print(f'\n== {g}: {n} samples')
        attributed = collections.Counter(category([names.get(a, '?') for a in st]) for st in stacks)
        under_import = sum(1 for st in stacks if any(names.get(a, '?').startswith('rt_import') for a in st))
        print(f'  by category (libc time counted for its caller); {under_import / n * 100:.1f}% of samples inside an OS call:')
        for c, k in attributed.most_common():
            print(f'  {k / n * 100:5.1f}%  {c}')
        self_fn = collections.Counter(label(st[0]) for st in stacks)
        print(f'  top {top}, self:')
        for fn, k in self_fn.most_common(top):
            print(f'    {k / n * 100:5.1f}%  {fn[:110]}')
        incl = collections.Counter()
        for st in stacks:
            for fn in {label(a) for a in st}:
                incl[fn] += 1
        print(f'  top {top}, inclusive:')
        for fn, k in incl.most_common(top):
            print(f'    {k / n * 100:5.1f}%  {fn[:110]}')


if __name__ == '__main__':
    main()
