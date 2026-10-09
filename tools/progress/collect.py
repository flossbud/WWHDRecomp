"""The progress page's data (tools/progress/index.html), as JSON on stdout.

Usage (on the editing machine, from the repo): python3 tools/progress/collect.py NAMES.tsv [OBJECT_NAMES.tsv] > progress.json
NAMES.tsv is the worker's /wwhd/data/ghidra-out/actor_names.tsv (process number, GameCube name);
OBJECT_NAMES.tsv, the worker's /wwhd/data/ghidra-out/object_names.tsv (process number, the stage names
l_objectName gives it: tools/stage_actors.py object_names), names the types the decomp's profiles don't:
tools/progress/publish.sh fetches it. Everything else comes from the repo: the default conversions
(src/overrides/sixty.cpp), the tick rules and the processes they name, the overrides, the names in
symbols.csv, the milestones and groups (tools/progress/plan.json) and the branch's commits.
"""
import csv
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(path):
    with open(os.path.join(ROOT, path)) as f:
        return f.read()


def converted_ids(text):
    # the list is one string literal per session, from its declaration to the `;`
    m = re.search(r'kConvertedByDefault =([^;]*);', text)
    joined = "".join(re.findall(r'"([0-9,]*)"', m.group(1))) if m else ""
    return sorted({int(x) for x in joined.split(",") if x})   # a process listed twice counts once


def effort(items):
    """Session-hours per unit of work, measured from WW-4's commits: each commit is credited the time since its
    session's previous commit (at most 90 min: longer is a break), as a bug's (names B<n>, or session qa), an
    actor's (the converted list grew: hours per type converted), a queue item's (tagged "(ITEM; session ...)"
    with one of ITEMS, the queue's ids, but converting nothing: the scans, tests and captures) or nothing (tools, research, handoff)."""
    log = subprocess.run(["git", "-C", ROOT, "log", "--reverse", "--format=%H\x1f%ct\x1f%s", "HEAD"],
                         capture_output=True, text=True).stdout.splitlines()
    log = [l.split("\x1f") for l in log]
    log = [(h, int(t), s) for h, t, s in log if s.startswith("WW-4")]
    if not log:
        return None
    blobs = subprocess.run(["git", "-C", ROOT, "cat-file", "--batch"], capture_output=True,
                           input="".join(f"{h}:src/overrides/sixty.cpp\n" for h, _, _ in log).encode()).stdout
    counts, at = [], 0
    for _ in log:   # "<sha> blob <size>\n<content>\n", or "<name> missing\n"
        nl = blobs.index(b"\n", at)
        head = blobs[at:nl].split()
        if head[-1] == b"missing":
            counts.append(0); at = nl + 1; continue
        size = int(head[2])
        counts.append(len(converted_ids(blobs[nl + 1:nl + 1 + size].decode(errors="replace"))))
        at = nl + 1 + size + 1
    hours, units, last, prev = {"actor": 0.0, "queue": 0.0, "bug": 0.0}, {"actor": 0, "queue": set(), "bug": set()}, {}, 0
    for (h, t, s), c in zip(log, counts):
        m = re.search(r"session (\w+)", s)
        sess = m.group(1) if m else "main"
        gap = min(t - last[sess], 5400) if sess in last else 1800
        last[sess] = t
        bugs = re.findall(r"\bB\d+\b", s)
        item = re.search(r"\(([\w-]+); session", s)
        if bugs or sess == "qa":
            hours["bug"] += gap / 3600; units["bug"].update(bugs)
        elif c > prev:
            hours["actor"] += gap / 3600; units["actor"] += c - prev
        elif item and item.group(1) in items:
            hours["queue"] += gap / 3600; units["queue"].add(item.group(1))
        prev = c
    n = {k: v if isinstance(v, int) else len(v) for k, v in units.items()}
    return {k: round(hours[k] / n[k], 3) for k in hours if n[k]}


def main():
    plan = json.loads(read("tools/progress/plan.json"))
    names = {}
    if len(sys.argv) > 1 and os.path.exists(sys.argv[1]):
        for line in open(sys.argv[1]):
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split("\t")
            if p and p[0].isdigit():
                names[int(p[0])] = p[1] if len(p) > 1 and p[1] != "?" else ""
    # a type the GameCube profiles leave unnamed takes its stage names (the first, and how many more)
    if len(sys.argv) > 2 and os.path.exists(sys.argv[2]):
        for line in open(sys.argv[2]):
            p = line.rstrip("\n").split("\t")
            if len(p) == 2 and p[0].isdigit() and int(p[0]) in names and not names[int(p[0])]:   # a known type, unnamed
                stage = p[1].split(",")
                names[int(p[0])] = stage[0] + (f" +{len(stage) - 1}" if len(stage) > 1 else "")
    converted = converted_ids(read("src/overrides/sixty.cpp"))
    rules_text = read("config/US_v0/tick_rules.txt")
    extra = os.path.join(ROOT, "config/US_v0/tick_rules")
    if os.path.isdir(extra):
        for f in sorted(os.listdir(extra)):
            if f.endswith(".txt"):
                rules_text += "\n" + read("config/US_v0/tick_rules/" + f)
    rules = [l for l in rules_text.splitlines() if l.strip() and not l.startswith("#")]
    with_rules = {int(x) for x in re.findall(r"(?i)process (\d+)", rules_text)}
    overrides = [l for l in read("config/US_v0/overrides.txt").splitlines() if re.match(r"[0-9A-F]{8}\s", l)]
    symbols = sum(1 for r in csv.reader(read("config/US_v0/symbols.csv").splitlines()) if r and re.fullmatch(r"[0-9A-Fa-f]{8}", r[0]))
    functions = sum(1 for l in read("config/US_v0/functions.csv").splitlines() if re.match(r"[0-9A-F]{8},", l))

    # plan.json "still": types checked and found to need nothing at 60 (they never change state), with the reason;
    # counted as done like a conversion. A type converted later is converted.
    still = {int(k): v for k, v in plan.get("still", {}).items()}

    def status(p):
        return "converted" if p in converted else "still" if p in still else "rules" if p in with_rules else "todo"

    labels = plan.get("labels", {})

    def actor(p):
        a = {"id": p, "name": labels.get(str(p), names.get(p, "")), "code": names.get(p, ""), "status": status(p)}
        if a["status"] == "still":
            a["reason"] = still[p]
        return a

    groups = []
    grouped = set()
    for g in plan["groups"]:
        groups.append({"name": g["name"], "actors": [actor(p) for p in g["ids"]]})
        grouped |= set(g["ids"])
    every = sorted(set(names) | set(converted))
    groups.append({"name": "World, objects & NPCs", "actors": [actor(p) for p in every if p not in grouped]})

    log = subprocess.run(["git", "-C", ROOT, "log", "-25", "--format=%h\x1f%ct\x1f%s", "HEAD"],
                         capture_output=True, text=True).stdout.splitlines()
    commits = [dict(zip(("hash", "time", "subject"), l.split("\x1f"))) for l in log]
    for c in commits:
        c["time"] = int(c["time"])
    out = {
        "generated": int(time.time()),
        "title": plan["title"],
        "ticket": plan["ticket"],
        "milestones": plan["milestones"],
        # an item with "ids" (the actor types it covers) gets their count done (converted, or needing nothing:
        # "still") for its bar, and the still ones' count
        "queue": [dict(q, converted=sum(1 for i in q["ids"] if i in converted or i in still),
                       still=sum(1 for i in q["ids"] if i in still and i not in converted)) if q.get("ids") else q
                  for q in plan.get("queue", [])],
        "queue_done": plan.get("queue_done", []),
        "roadmap": plan.get("roadmap", []),
        "known_issues": plan["known_issues"],
        "groups": groups,
        "counts": {
            "types": len(every),
            "converted": len(converted),
            "with_rules": len(with_rules - set(converted) - set(still)),
            "still": len(set(still) & set(every) - set(converted)),
            "tick_rules": len(rules),
            "overrides": len(overrides),
            "named_functions": symbols,
            "functions": functions,
        },
        "commits": commits,
        "effort": effort({q["id"] for q in plan.get("queue", [])} | set(plan.get("queue_done", []))),
    }
    json.dump(out, sys.stdout, separators=(",", ":"))


if __name__ == "__main__":
    main()
