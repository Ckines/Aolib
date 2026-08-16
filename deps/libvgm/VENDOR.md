# Procedencia de este vendoring

- **Upstream:** https://github.com/ValleyBell/libvgm
- **Commit fijado:** `867223e7c33d63de115d1ab955f784c44f19040a` (19-06-2026)
- **Obtenido:** tarball de `codeload.github.com` para ese commit exacto,
  descomprimido sin modificaciones. Este directorio es una copia pristina
  del árbol upstream en ese commit — **cero parches locales** (a
  diferencia de `deps/aosdk`, ver `deps/patches/README.md`). RFC-F6 §9.2
  fija la política de actualización: commit fijado, adaptador propio
  (`src/engine/libvgm_engine.hpp`), como máximo una o dos actualizaciones
  de upstream al año, y solo con los tests de F6 en verde antes y
  después.
- **No se usa CMake.** La selección de fuentes y defines vive en el
  Makefile raíz del proyecto (`LIBVGM_*`), igual que `aosdk` y
  `game-music-emu`. `Compiling.txt` de este mismo árbol avisa de que sus
  Makefiles no-CMake son de uso interno y están desfasados — tampoco se
  usan.
- **Solo se compila un subconjunto explícito** de `emu/cores/` (chips
  activados por `SNDDEV_*`/`EC_*`, ver Makefile raíz y RFC-F6 §4/§5/§6).
  El resto del árbol (`audio/` con los drivers de dispositivo,
  `emu/cores/` sin activar, las apps de escritorio `player.cpp`/
  `vgm2wav.cpp`/etc.) se vendoriza íntegro por fidelidad al commit fijado
  pero **no se compila, no se enlaza y no se distribuye** — mismo
  criterio que `deps/aosdk/imgui`/`argparse`/`main.c` para aosdk.
