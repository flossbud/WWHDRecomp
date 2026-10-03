"""The progress page's data (tools/progress/index.html), as JSON on stdout.

Usage (on the editing machine, from the repo): python3 tools/progress/collect.py NAMES.tsv > progress.json
NAMES.tsv is the worker's /wwhd/data/ghidra-out/actor_names.tsv (process number, GameCube name):
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
    m = re.search(r'kConvertedByDefault = "([0-9,]*)"', read("src/overrides/sixty.cpp"))
    converted = [int(x) for x in m.group(1).split(",") if x] if m else []
    rules_text = read("config/US_v0/tick_rules.txt")
    rules = [l for l in rules_text.splitlines() if l.strip() and not l.startswith("#")]
    with_rules = {int(x) for x in re.findall(r"process (\d+)", rules_text)}
    overrides = [l for l in read("config/US_v0/overrides.txt").splitlines() if re.match(r"[0-9A-F]{8}\s", l)]
    symbols = sum(1 for r in csv.reader(read("config/US_v0/symbols.csv").splitlines()) if r and re.fullmatch(r"[0-9A-Fa-f]{8}", r[0]))
    functions = sum(1 for l in read("config/US_v0/functions.csv").splitlines() if re.match(r"[0-9A-F]{8},", l))

    def status(p):
        return "converted" if p in converted else "rules" if p in with_rules else "todo"

    def actor(p):
        return {"id": p, "name": names.get(p, ""), "status": status(p)}

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
        "known_issues": plan["known_issues"],
        "groups": groups,
        "counts": {
            "types": len(every),
            "converted": len(converted),
            "with_rules": len(with_rules - set(converted)),
            "tick_rules": len(rules),
            "overrides": len(overrides),
            "named_functions": symbols,
            "functions": functions,
        },
        "commits": commits,
    }
    json.dump(out, sys.stdout, separators=(",", ":"))


if __name__ == "__main__":
    main()
