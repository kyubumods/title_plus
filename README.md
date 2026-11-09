# CWAP title

Cube World Alpha Plus title screen

## Getting it

Players install it in the game: choose **Mods** on the title menu (ModManager, which comes with Kyubu) and pick it
from the catalog.

## Building

This repository builds against a checkout of the Kyubu repository, with the toolchain Kyubu's
`docs/building.md` describes (MinGW-w64 i686 or MSVC Win32):

```bash
cmake -S <kyubu checkout> -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKYUBU_MODS=<this folder>
cmake --build build --target title_plus
```

The staged mod is in `build/gameplay-mods/title_plus/`, ready to copy into the game's `mods/`.

## Licence

MIT, in [LICENSE](LICENSE).
