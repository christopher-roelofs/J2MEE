# j2me

A small CLDC/MIDP-2.0 emulator written in C++ with SDL2 for graphics, audio,
input, and persistent storage. Plays a handful of feature-phone Java games
(Solitaire, Age of Empires II, 365 Puzzle Club, etc.) on Linux.

## Build & run

```sh
cd runtime/build
cmake --build .
ASAN_OPTIONS=detect_leaks=0 ./j2me <jar> <MIDletClass> [WxH]
```

Optional `WxH` overrides the screen resolution (default 240×320, otherwise
read from `boxal.inf` if present in the jar).

## What works

- **Class loading**: full JAR-walk, hierarchy linking, field layout, method
  resolution; `<clinit>` runs lazily on first access.
- **Interpreter**: all common opcodes, including int/long/float/double
  arithmetic, array ops, exception throw/catch, `tableswitch`/`lookupswitch`,
  reference compares, multi-dim array allocation.
- **Heap**: 32 MB bump allocator with mark/sweep GC (not auto-triggered yet).
- **Threads**: `Thread.start()` deferred to the main loop after `startApp()`
  returns; repeating `Timer.schedule` runs in a synthetic loop with
  `Display.flush()` polled between ticks.
- **Graphics**: SDL2 software surface for the screen, ARGB blits with clip,
  translate, image transforms (rotate/mirror), `drawRegion`, `drawImage`,
  `drawString`/`drawSubstring`, fill/draw rect/line, `setColor`,
  `setClip`/`clipRect`, magenta color-key transparency for opaque PNGs.
- **Fonts**: SDL2_ttf (DejaVu Sans + DejaVu Sans Mono); size table mirrors
  freej2me-plus per-screen-bucket sizes, `FACE_MONOSPACE` honored, CJK
  fallback font when string contains Chinese/Japanese/Korean codepoints.
- **Touch input**: SDL mouse → `Canvas.pointerPressed`/`pointerReleased`/
  `pointerDragged` in logical screen coordinates; `hasPointerEvents` and
  `hasPointerMotionEvents` return true. Compositor-scale aware so clicks
  map correctly under HiDPI Wayland.
- **Audio**: SDL2_mixer plays MIDI (`.mid`) and WAV via `Player`.
- **RecordStore (RMS)**: persisted to `~/.j2me/<jar-stem>/rms/<name>.rms`;
  `enumerateRecords` (filter/comparator ignored) supported. Records written
  by the current runtime round-trip cleanly with games like 365 Solitaire.
- **VServ ad SDK bypass**: see "Special handling" below.

## Tested games

| Game | Status |
| --- | --- |
| 365 Solitaire (Connect2Media) | Playable end-to-end with audio + save |
| Age of Empires II Mobile | Menu, gameplay, soft-key touch all work; some bitmap-font scroll text shows minor edge bleed (game expects pixel-exact 6px font) |
| 365 Puzzle Club | Launches into language menu via VServ bypass; cosmetic highlight-bar narrower than item width |
| 3D Bomberman Atomic | Reaches 3D init then crashes — game uses M3G (JSR-184), not implemented |
| Lumines Mobile | Window opens but stays mostly black — needs more debugging (uses Nokia FullCanvas) |

## Special handling

### VServ ad SDK (`VservManager.class`)

Several Connect2Media / Sun Microsystems games bundle the VServ ad SDK,
which spins up a dedicated thread on launch to fetch banner ads from
`http://a.vserv.mobi`. Without working network the thread sits forever in a
"Fetching Data..." UI and the game never starts.

The bypass lives in `runtime/src/main.cpp` and triggers when the JAR ships
`VservManager.class`. For the **first** instantiation of `VservManager` we:

1. Set the `VservManager.startMainApp` static flag to `true` so the MIDlet's
   own `startApp()` knows the ad path is "complete".
2. Drive the BoxAL framework's two-phase init manually:
   `MIDlet.constructorMainApp()` (creates the game canvas + reads config),
   then `MIDlet.startMainApp()` (kicks off the game thread).

For **subsequent** `new VservManager(...)` calls (mid-game / between-level
ads), the constructor becomes a no-op. Since we never swap the canvas to the
VServ instance, the game stays in control and just keeps drawing — the only
visible difference is that the user doesn't see an ad.

`HttpConnection` is also stubbed to return HTTP 200 with `Location: vserv:`
and `X-VSERV-CONTEXT: asd` (mirroring freej2me-plus), so VServ classes that
do reach the network code path see a "successful no-ad" response instead of
an `IOException`.

To re-enable the override flag mechanism: this is keyed on the presence of
`VservManager.class` in the jar — no per-game configuration needed.

### Stale RMS save files

If a game appears to hang on a splash forever, it may be tripping over a
RecordStore save written by an older buggy version of this runtime. The
canonical case was 365 Solitaire's `Solitaire365save.rms`: the game's
loader (`j.o`) requires the first record to start with the byte sequence
`H o a l H` — older runtime builds didn't write that header, so when the
game tried to read slot 1 of the (null) save header it threw
`NullPointerException`. The catch handler at the top of `f.run()` swallowed
the exception and looped forever.

Fix: `rm -rf ~/.j2me/<game-name>/rms/` once. Saves written by the current
runtime are valid and round-trip correctly.

### HiDPI / Wayland

GNOME Wayland with fractional scaling reports SDL window size in physical
pixels but mouse events arrive in compositor-logical coordinates. We detect
`WAYLAND_DISPLAY` and assume a 2× compositor scale, mapping clicks against
the original window size captured at startup so resizing doesn't break
input. Override with `J2ME_SCALE=<n>` (e.g. `J2ME_SCALE=1` to disable on
non-scaled X11).

## Known limitations

- **JSR-184 (M3G)**: not implemented. Games using mobile 3D graphics
  (3D Bomberman, etc.) will crash when they touch `Graphics3D` or related
  classes.
- **Networking**: no real sockets/HTTP. `Connector.open` returns the fake
  HTTP-200 connection described above.
- **JSR-120 (SMS)**: not implemented.
- **Custom bitmap fonts**: games that compute scroll/layout offsets assuming
  a specific pixel-width bitmap font (AoE tutorial scroll, 365 Puzzle Club
  selection bar) render with minor edge bleed since we use a TTF fallback.

## Layout

```
runtime/
  src/
    main.cpp                    ← CLI entry, VServ bypass setup
    util/jar.cpp                ← ZIP/jar reader
    classfile/                  ← .class file parser
    vm/
      class_loader.cpp          ← parse, link, layout, native binding
      class_def.cpp             ← virtual dispatch
      heap.cpp                  ← bump alloc + mark/sweep
      interpreter.cpp           ← bytecode dispatch loop
      vm.cpp                    ← run(), invoke(), <clinit>
    midp/
      natives.cpp               ← non-graphics MIDP natives (RMS, Player,
                                  Connector, Hashtable, …)
      graphics_natives.cpp      ← Graphics, Image, Canvas, Font, Display,
                                  Timer, Thread.start
    backend/
      display.cpp               ← SDL2 window, mouse/key event polling,
                                  HiDPI-aware coordinate mapping
games/                          ← .jars (gitignored)
```
