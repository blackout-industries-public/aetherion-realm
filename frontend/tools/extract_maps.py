"""Build continent map images from a WoW 3.3.5a client you own.

Nothing is downloaded. The art lives in the client's own MPQ archives as a 4x3 grid
of BLP tiles per continent; this reads them, decodes, stitches and writes one JPEG
per map id for the dashboard to render behind the dots.

Run through tools/extract-maps.sh, which supplies the container and mounts.
"""
from __future__ import annotations

import io
import sys
from pathlib import Path

from mpyq import MPQArchive
from PIL import Image

# Client folder name -> AzerothCore map id. These names come from WorldMapArea.dbc.
CONTINENTS = {
    "Azeroth": 0,
    "Kalimdor": 1,
    "Expansion01": 530,
    "Northrend": 571,
}

# Highest priority first: later patches override earlier archives. The world map art
# lives in the LOCALE archives, not the base ones - the base MPQs contain no
# Interface\WorldMap entries at all.
ARCHIVE_ORDER = [
    "enUS/patch-enUS-3.MPQ", "enUS/patch-enUS-2.MPQ", "enUS/patch-enUS.MPQ",
    "enUS/lichking-locale-enUS.MPQ", "enUS/expansion-locale-enUS.MPQ",
    "enUS/locale-enUS.MPQ", "enUS/base-enUS.MPQ",
    "patch-3.MPQ", "patch-2.MPQ", "patch.MPQ",
    "lichking.MPQ", "expansion.MPQ", "common-2.MPQ", "common.MPQ",
]

TILE_COLS, TILE_ROWS = 4, 3

# The stitched grid is 1024x768, but only the top-left 1002x668 is map content - the
# rest is padding to reach power-of-two tiles. That 1002x668 area is exactly 3:2,
# which matches the aspect of every continent region in WorldMapArea.dbc, so cropping
# is what makes the art line up with the projected coordinates.
CONTENT_W, CONTENT_H = 1002, 668


def build_index(data_dir: Path):
    """Map lowercased path -> (archive, real name).

    MPQ paths are case-insensitive but mpyq's lookup is not, and the client is
    inconsistent about it: base archives use "Interface\\WorldMap" while the locale
    patches use "Interface\\WORLDMAP". Indexing once avoids guessing.
    First archive to claim a path wins, so ARCHIVE_ORDER decides overrides.
    """
    index: dict[str, tuple[MPQArchive, str]] = {}
    opened = []
    for name in ARCHIVE_ORDER:
        path = data_dir / name
        if not path.exists():
            continue
        try:
            archive = MPQArchive(str(path))
            files = archive.files or []
        except Exception as exc:                      # noqa: BLE001 - report and continue
            print(f"  ! could not open {name}: {exc}", file=sys.stderr)
            continue
        opened.append(name)
        for raw in files:
            key = raw.decode("latin-1").lower()
            index.setdefault(key, (archive, raw.decode("latin-1")))
    return index, opened


def read_tile(index, continent: str, i: int) -> bytes | None:
    key = f"interface\\worldmap\\{continent}\\{continent}{i}.blp".lower()
    entry = index.get(key)
    if not entry:
        return None
    archive, real = entry
    try:
        return archive.read_file(real)
    except Exception:                                 # noqa: BLE001 - unreadable tile
        return None


def build(continent: str, map_id: int, index, out_dir: Path) -> bool:
    tiles: list[Image.Image] = []
    for i in range(1, TILE_COLS * TILE_ROWS + 1):
        raw = read_tile(index, continent, i)
        if raw is None:
            print(f"  {continent}: tile {i} missing - skipping continent")
            return False
        tiles.append(Image.open(io.BytesIO(raw)).convert("RGB"))

    tw, th = tiles[0].size
    canvas = Image.new("RGB", (tw * TILE_COLS, th * TILE_ROWS))
    for idx, tile in enumerate(tiles):
        canvas.paste(tile, ((idx % TILE_COLS) * tw, (idx // TILE_COLS) * th))

    canvas = canvas.crop((0, 0, CONTENT_W, CONTENT_H))

    out = out_dir / f"{map_id}.jpg"
    canvas.save(out, "JPEG", quality=88, optimize=True)
    print(f"  {continent:12} -> {out.name}  {canvas.size[0]}x{canvas.size[1]}  "
          f"{out.stat().st_size // 1024} KB")
    return True


def main() -> int:
    data_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    index, opened = build_index(data_dir)
    if not index:
        print("no MPQ archives found", file=sys.stderr)
        return 1
    print(f"indexed {len(index)} paths from {len(opened)} archives")

    built = sum(build(c, m, index, out_dir) for c, m in CONTINENTS.items())
    print(f"built {built}/{len(CONTINENTS)} continent maps")
    return 0 if built else 1


if __name__ == "__main__":
    raise SystemExit(main())
