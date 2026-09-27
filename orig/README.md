# orig/ — your game files go here (never committed)

This project ships **no game data**. Supply your own dump of
*The Legend of Zelda: The Wind Waker HD* (USA, `WUP-P-BCZE`, title ID `0005000010143500`, v0).

From a Cemu `.wua` archive:

```sh
uv run tools/wua_extract.py "/path/to/The Legend of Zelda - The Wind Waker HD.wua" orig/ code/ meta/meta.xml
```

This produces `orig/0005000010143500_v0/code/cking.rpx`. The extractor verifies it against:

| file | sha256 |
|---|---|
| `code/cking.rpx` (US v0) | `c4f0ab300542e0bfc462696850534e71db2ad02288a7eb55e5a4cd4062f16153` |

The hash matches `cemu-project/title-checksums` for `0005000010143500_v0`. Other regions or
versions have different addresses; everything in this repo assumes US v0 (the same build the
TWWHD Randomizer targets).

Everything in `orig/` except this file is gitignored.
