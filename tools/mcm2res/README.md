# mcm2res – resource viewer and extractor

`mcm2res.exe` opens every archive Motocross Madness 2 keeps its data in
(`RES\*.RES`, `RES\*.TEX`, `UI\*.DAT`, `TERAFORM\*\*.ENV`) and writes what is
inside as ordinary files, with an `index.html` to look through it all in a
browser. It only reads the game's files and never changes them.

## Use

1. Put `mcm2res.exe` anywhere and double-click it. It looks for the game (a
   folder with `RES\` and `TERAFORM\`) next to the tool, in a `mcm2` folder
   next to it, or one or two folders up. If it can't find it, drag the game
   folder onto `mcm2res.exe`.
2. To extract only some archives, drag one or more `.ENV`, `.RES`, `.TEX`
   or `.DAT` files onto `mcm2res.exe`: each goes into its own folder,
   `mcm2res_out\<name>\` (e.g. `mcm2res_out\BAJA01\`), with its own
   `index.html`.
3. A full extract writes `mcm2res_out\` next to the tool (all tracks take about 1 GB and
   a few minutes) and says where `index.html` is. Open it in a browser.

Command line:

```
mcm2res [extract] [game folder | archive...] [-o folder] [--only text]... [--obj-step N] [--raw-all]
mcm2res list <archive>...
mcm2res check <folder of PNGs>
```

* `--only baja` handles only archives whose path contains "baja" (repeat it
  for more, e.g. `--only skies --only global`).
* `--obj-step N`: the terrain OBJ uses every Nth height point (default 8).
* `--raw-all` also saves the texture records, terrain and sky files exactly
  as stored, and a raw `.r16` height map.
* `list` prints the files inside archives.
* `check` compares a folder of PNGs, e.g. mcm2fix's `textures_dump\`, with the
  names from the last extract and lists any name mcm2res doesn't produce.

## What you get

| Folder | What |
|---|---|
| `textures\` | Every game texture as PNG (with alpha where it has it), named the way mcm2fix names it. Edit one, keep the file name, and put it in the game's `textures\` folder or `%LOCALAPPDATA%\Motocross Madness 2\textures\` (TextureReplace=1). |
| `textures\_same_name\<archive>\` | Textures that share a name with a different picture: the four stadiums' `texturePanel0..`. The game loads them by that name, so a replacement is used in every stadium. |
| `textures.txt` | Every texture: name, size, format, how the game loads it, archive, member. |
| `images\<archive>\` | Plain TGA pictures (track preview shots, menu art) as PNG. |
| `tracks\<track>\` | Per track: `info.txt` (name, event, sky, races, waypoints with ground height, course points, start and finish lines), `heightmap16.png` (16-bit, range in info.txt), `ground.png` (the terrain texture as laid on the ground, 1 pixel per height point), `relief.png` (shaded heights), `map_raceN.png` (race N's waypoints, white = first, and its course from the `.tdf`: white centre line, black sides, green start line, red finish line; cut down to the course when it is small, as in stadiums), `course_raceN.obj` (the course as lines, to load with the terrain), `trees.png` (every tree and plant from the `.esb`, coloured by kind; the colours are listed in info.txt), `trees.csv` (every tree and plant: kind, model, x, y, z, height, width), `terrain.obj` + `terrain.mtl` (textured with ground.png). |
| `models\<archive>\` | Tree and plant models (`.slt`) as OBJ + MTL, textured from `textures\`. |
| `archives\<archive>\` | Everything else as stored: sounds (`.wav`), scene, waypoint and config text (`.scn`, `.wpt`, `.ini`, `.mcf`, `.pb`, ...) and binary data. |
| `mcm2res.log` | What was found, and anything that could not be read. |

Texture names: a texture stored as `<name>.tga` in a RES (bikes, riders,
objects, trees, the stadiums' panels) is `<name>`; sky tiles and terrain
textures are read from inside a bigger file, so their names are
`<file>_<offset>` (e.g. `terra12_51af51`, `baja01_488689`). These match
mcm2fix's texture dump.

OBJ files are in feet. The game's axes are left-handed, so z is negated in
the OBJ to keep it from being mirrored.

Not yet converted: bike, rider and object models (`.slb`, a binary format),
animations (`.mot`) and collision shapes (`.col`);
they are saved as stored.

## Format notes

See the comment at the top of `mcm2res.cpp`: archive encryption, the RS2
directory, texture records and formats, the LZW coder, the `.trn` terrain
layout, with the Mcm2.exe addresses they were read from.

Build: `i686-w64-mingw32-g++ -O2 -std=c++17 -static -s -o mcm2res.exe mcm2res.cpp`
(or any C++17 compiler; it also builds on Linux). PNG writing is
[stb_image_write](https://github.com/nothings/stb) (public domain), shared
with `tools/mcm2sky/third_party/`.
