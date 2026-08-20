# Continent map art

Drop images here as `<mapId>.jpg` and the dashboard renders them behind the dots:

| File | Continent |
|---|---|
| `0.jpg`   | Eastern Kingdoms |
| `1.jpg`   | Kalimdor |
| `530.jpg` | Outland |
| `571.jpg` | Northrend |

This folder is bind-mounted into the container, so adding a file takes effect on the
next page refresh - no rebuild, no restart.

## Where the art comes from

These images are Blizzard's, so none are included here. They live inside the game
client you already own, under `Interface\WorldMap\<Continent>\`, split into a 4x3 grid
of twelve `.blp` tiles (e.g. `Northrend1.blp` .. `Northrend12.blp`).

To produce a usable file:

1. Extract that folder from the client MPQs (any MPQ browser will do).
2. Convert the twelve BLP tiles to PNG.
3. Stitch them into one image, 4 wide by 3 tall, reading left to right, top to bottom.
4. Save as `<mapId>.jpg` here.

Keep the aspect ratio as extracted; the SVG stretches the image to the full 1000x1000
viewBox and positions dots using the client's own `WorldMapArea.dbc` bounds, so a
correctly stitched continent map lines up without further calibration.

The `Map art` slider in the sidebar fades the image; below 35% the coordinate grid
comes back, which is useful for checking alignment.
