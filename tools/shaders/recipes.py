"""Count the pipelines in the renderer's recipe file (portable/shaderCache/wwhd/pipelines.bin, D20).

Usage:
    python3 tools/shaders/recipes.py PIPELINES_BIN      # '-' reads stdin (e.g. through ssh)

Prints how many distinct pipelines the recipes need today, and how many they would need if more of
their state were dynamic in Vulkan: stencil reference and masks (core 1.0); then extended dynamic
state 1 and 2 as Vulkan 1.3 has them (cull mode, front face, topology within its class, primitive
restart, rasterizer discard, depth-bias enable, depth test/write/compare, stencil test and ops,
vertex strides); then extended dynamic state 3 (blend enables and equations, write masks, logic op,
depth clip). The floor is the number of distinct shader pairs. The recipes hold shader keys and
register values, not game content, but they come from the game: read them where they are.

The layout is PipelineDesc::Fields in src/gpu/vk/draw.cpp (little-endian), records framed as
shader_cache.cpp writes them: magic, version, kind, then length, bytes, checksum.
"""
import struct
import sys

TOPOLOGY_CLASS = {0: "points", 1: "lines", 2: "lines", 3: "triangles", 4: "triangles", 5: "triangles"}
STAGES = ["today", "+ stencil reference and masks (core 1.0)", "+ extended dynamic state 1 and 2 (core 1.3)",
          "+ extended dynamic state 3 (blend, write masks, logic op, depth clip)"]


def records(data):
    if data[:8] != b"WWHDSCAC":
        sys.exit("not a shader cache file")
    pos = 16
    while pos + 4 <= len(data):
        n = struct.unpack_from("<I", data, pos)[0]
        if pos + 4 + n + 8 > len(data):
            break                                   # a torn record at the end
        yield data[pos + 4:pos + 4 + n]
        pos += 4 + n + 8


def parse(r):
    at = 16
    d = dict(zip(("vs", "ps"), struct.unpack_from("<QQ", r, 0)))

    def words(n):
        nonlocal at
        v = struct.unpack_from(f"<{n}I", r, at)
        at += 4 * n
        return v if n > 1 else v[0]

    d["bindings"] = [words(3) for _ in range(words(1))]
    d["attrs"] = [words(4) for _ in range(words(1))]
    d["topology"], d["restart"], d["discard"], d["depthBias"], d["depthClip"] = words(5)
    d["cull"], d["frontFace"] = words(2)
    colors = words(1)
    d["blends"], d["formats"] = [], []
    for _ in range(colors):
        d["blends"].append(words(8))
        d["formats"].append(words(1))
    d["logicOpEnable"], d["logicOp"] = words(2)
    d["depthTest"], d["depthWrite"], d["stencilTest"], d["depthCompare"] = words(4)
    d["front"], d["back"] = words(7), words(7)
    d["depthFormat"], d["stencilFormat"] = words(2)
    if at != len(r):
        sys.exit(f"a record of {len(r)} bytes doesn't parse (draw.cpp's PipelineDesc changed?)")
    return d


def key(d, stage):
    d = dict(d)
    if stage >= 1:
        d["front"], d["back"] = d["front"][:4], d["back"][:4]      # ops and compare, without ref and masks
    if stage >= 2:
        for f in ("cull", "frontFace", "restart", "discard", "depthBias", "depthTest", "depthWrite", "depthCompare",
                  "stencilTest", "front", "back"):
            d[f] = None
        d["topology"] = TOPOLOGY_CLASS.get(d["topology"], d["topology"])
        d["bindings"] = [(binding, rate) for binding, _stride, rate in d["bindings"]]
    if stage >= 3:
        d["blends"] = d["logicOpEnable"] = d["logicOp"] = d["depthClip"] = None
    return repr(sorted(d.items()))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = sys.stdin.buffer.read() if sys.argv[1] == "-" else open(sys.argv[1], "rb").read()
    recipes = [parse(r) for r in records(data)]
    print(f"{len(recipes)} recipes")
    for stage, what in enumerate(STAGES):
        print(f"{len({key(d, stage) for d in recipes}):6d} pipelines  {what}")
    print(f"{len({(d['vs'], d['ps']) for d in recipes}):6d} shader pairs (the floor)")


if __name__ == "__main__":
    main()
