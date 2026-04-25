# j2me

A small CLDC/MIDP-2.0 emulator written in C++ with SDL2 for graphics, audio,
input, and persistent storage. Plays a handful of feature-phone Java games
(Solitaire, Age of Empires II, 365 Puzzle Club, Doom RPG, etc.) on Linux.

This repo has three pieces:

| Directory  | What it is |
| ---------- | ---------- |
| `runtime/` | Native C++/SDL2 J2ME VM — loads a JAR, interprets MIDP bytecode, renders the game. Also builds to WebAssembly via Emscripten. |
| `launcher/`| Compose Multiplatform desktop UI — picks a JAR from a folder, spawns the runtime. Kotlin/JVM. |
| `games/`   | Local JAR library (gitignored). |

## Build & run

Native runtime:

```sh
cd runtime/build
cmake --build .
ASAN_OPTIONS=detect_leaks=0 ./j2me <jar> <MIDletClass> [WxH]
```

Optional `WxH` overrides the screen resolution (default 240×320, otherwise
read from `boxal.inf` if present in the jar).

Launcher (opens a Material-styled window over the runtime):

```sh
cd launcher
./gradlew run
```

Web / WebAssembly build (Emscripten; see `memory/project_j2me_wasm_build.md`
for full detail):

```sh
cd runtime/build-web
source ~/emsdk/emsdk_env.sh
emcmake cmake .. -DJ2ME_WEB_GAME=/abs/path/to/game.jar -DJ2ME_WEB_MIDLET=pkg.MIDlet
emmake make -j
python3 -m http.server 8765   # open http://localhost:8765/j2me.html
```

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
- **On-screen keypad**: dedicated strip below the game with a d-pad
  (with diagonals), A/B/Start and a phone numpad layout. Hide/show
  toggle + layout cycle. JSON-defined layouts with PNG button art under
  `runtime/assets/keypad/`. See "On-screen keypad" below.
- **Audio**: SDL2_mixer plays MIDI (`.mid`) and WAV via `Player`.
- **RecordStore (RMS)**: persisted to `~/.j2me/<jar-stem>/rms/<name>.rms`;
  `enumerateRecords` (filter/comparator ignored) supported. Records written
  by the current runtime round-trip cleanly with games like 365 Solitaire.
- **VServ ad SDK bypass**: see "Special handling" below.

## Tested games

| Game | Status |
| --- | --- |
| 365 Solitaire (Connect2Media) | Playable end-to-end with audio + save |
| Age of Empires II Mobile | Menu, gameplay, soft-key touch all work; minor bitmap-font scroll bleed |
| 365 Puzzle Club | Launches into language menu via VServ bypass; cosmetic highlight-bar narrower than item width |
| Bejeweled (JAMDAT 2007) | Fully playable. Wipe `~/.j2me/bejeweled*/rms/` if it boots to a white screen |
| Doom RPG (JAMDAT 2009) | Textured in-game world; playable |
| Spore Origins | Playable. Minor clipping around background swirls |
| Lemonade Tycoon | In-game. Audio off by default in-game (not a runtime issue) |
| Bejeweled 3 (EA/JAMDAT) | Reaches the Enable-Sound dialog; soft-key mapping + M3G gem grid both unresolved |
| 3D Bomberman Atomic | Needs JSR-184 (M3G) — not implemented, crashes on 3D init |
| Lumines Mobile | Window opens but stays mostly black — needs more debugging (Nokia FullCanvas) |

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

### JAMDAT custom PNG format (Bejeweled 2007, Tetris Pop, …)

Mid-2000s JAMDAT / EA Mobile titles pack their sprite sheets as PNGs that
libpng rejects outright with `invalid chunk type: [00][0D]IH`. It looks
like a PNG — the 8-byte signature is standard — but the immediate layout
diverges:

| Bytes | Standard PNG | JAMDAT |
| ----- | ------------ | ------ |
| 0x08–0x0B | IHDR length (`00 00 00 0D`) | Fixed marker `00 C0 00 80` |
| 0x0C–… | IHDR type + data | **2-byte** length + 4-byte type + data + **2-byte** truncated CRC (top 2 bytes of the real CRC32 only) |
| subsequent chunks | standard length/type/data/CRC | standard PNG, but with a **variable trailer** — either 4 bytes (real CRC) or 6 bytes (CRC + 2 bytes padding, seen between IDAT → IEND) |
| tail | 12-byte IEND chunk | No proper IEND; file ends with "JAMDAT marker + IEND type", no CRC |

`load_png_from_bytes` in `runtime/src/midp/graphics_natives.cpp` detects
the format by the 12-byte header signature and runs `repair_jamdat_png`
before handing bytes to libpng. The repair:

1. Rebuilds IHDR from its 13 data bytes with a real CRC32 (via zlib).
2. Scans the rest for known chunk types (PLTE, tRNS, IDAT, IEND, gAMA, …);
   for each, picks trailer size 4 or 6 by whichever makes the stored CRC
   verify — that tells us where the data ends.
3. Appends a fresh standard IEND.

Standard PNGs take a zero-cost early-out (signature check misses at byte
0x08). Heads-up: if you encounter a JAMDAT title that still fails, the
marker may differ — dump the bytes with `J2ME_DUMP_PNGS=<dir>` and compare
the first 12 against `{0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A, 0x00,0xC0,0x00,0x80}`.

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

## Per-game config

Every game gets a JSON file at `~/.j2me/<jar-stem>/config.json` — same
directory as its RMS saves. Missing on first run, auto-created with the
defaults below:

```json
{
  "resolution":     "auto",
  "keypad_layout":  "minimal",
  "keypad_visible": true
}
```

Fields:

| Field             | Values                         | Effect |
| ----------------- | ------------------------------ | ------ |
| `resolution`      | `"WxH"` (e.g. `"176x208"`) or `"auto"` | Overrides the screen resolution. `"auto"` lets the runtime pick from `boxal.inf` / manifest / 240×320 default. A CLI `WxH` positional still wins over the config value. |
| `keypad_layout`   | Folder name under `runtime/assets/keypad/layouts/` (e.g. `"minimal"`, `"numpad"`) | Which layout to start on. User can still tap the ↻ button to cycle. |
| `keypad_visible`  | `true` / `false`               | Whether the keypad starts expanded or collapsed to the thin handle strip. |

Edit the JSON with any text editor — changes apply on the next launch.
Unknown fields and malformed files are ignored (defaults win, with a
one-line log).

## On-screen keypad

A virtual keypad renders below the game in a dedicated strip. Taps on
buttons dispatch MIDP key events identical to a physical keyboard press
(`keyPressed` / `keyReleased` + the `GameCanvas.getKeyStates()` bit is
set while held).

Two surfaces, one window: the game area and the keypad strip are
composed as separate `SDL_RenderCopy` rects each frame — they have
independent coord systems, so manual window resizes and the strip's
own hide/show collapse never interfere with the game's touch mapping.

### Controls (top-centre of the strip)

| Glyph | Action |
| ----- | ------ |
| ▲     | Hide the keypad (collapses the strip to a thin band with just the toggle). |
| ▼     | Show the keypad. Shown instead of ▲ while hidden. Tap anywhere in the thin band also works. |
| ↻     | Cycle to the next layout. Wraps. |

`Tab` (keyboard) toggles hide/show as well. `Esc` quits the runtime.

### Built-in layouts

Ship with two layouts under `runtime/assets/keypad/layouts/`:

- **minimal** — d-pad (4 cardinals + 4 diagonals) on the left, A / B /
  Start on the right.
- **numpad** — phone-style 3×4 dialpad (1–9, *, 0, #) covering the
  whole strip.

### Adding a custom layout

1. Create `runtime/assets/keypad/layouts/<name>/` with a `layout.json`.
2. Drop any override PNGs into the same folder. Missing images fall
   back to `runtime/assets/keypad/default/<slug>.png`; missing there
   too falls back to live TTF render using the image slug as a label.
3. Re-run. The runtime scans all layout folders lexicographically and
   adds them to the cycle.

### Layout JSON schema

All fields are optional except `buttons`. Minimal valid layout:

```json
{ "buttons": [ { "code": -5, "image": "start", "x": 0, "y": 0, "w": 176, "h": 89 } ] }
```

Full shape:

```jsonc
{
  "name":                "Minimal",     // debug label
  "description":         "…",           // free-form, ignored by runtime
  "strip_width":         176,           // design-time reference width  (default 176)
  "strip_height":        89,            // design-time reference height (default 89)
  "strip_height_px":     100,           // optional: absolute runtime strip height
  "strip_aspect":        "3:2",         // optional: w:h ratio, runtime h = game_w / ratio
  "collapsed_height_px": 24,            // optional: runtime hidden-state strip height
  "buttons": [
    // single-key button:
    { "code":  -1, "image": "arrow-up", "x": 30, "y": 2, "w": 28, "h": 28 },
    // multi-key button (j2me-loader-style diagonal — fires UP + RIGHT together):
    { "codes": [-1, -4], "image": "arrow-up-right", "x": 58, "y": 2, "w": 28, "h": 28 }
  ]
}
```

- `x`, `y`, `w`, `h` are in the layout's own reference coord space
  (`strip_width × strip_height`). Runtime scales to the actual strip
  size, so the same layout works at any game resolution.
- `code` and `codes` are alternatives — use `codes` when a single
  button should press more than one MIDP key simultaneously
  (d-pad diagonals, combo shortcuts).
- Strip-height precedence: `strip_height_px` > `strip_aspect` >
  built-in formula.

Standard MIDP key codes:

| Key | Code |
| --- | ---- |
| UP / DOWN / LEFT / RIGHT | -1 / -2 / -3 / -4 |
| FIRE (OK / Select)       | -5 |
| SOFT1 / SOFT2 (A / B)    | -6 / -7 |
| 0–9                      | 48–57 (ASCII) |
| `*` / `#`                | 42 / 35 |

### Button image slugs

Image lookup: `<layout_dir>/<slug>.png` first, then
`runtime/assets/keypad/default/<slug>.png`. Default PNGs live at 128×128
but any source size works — `SDL_BlitScaled` stretches to whichever rect
the layout puts the button in.

Default slug set (regenerable — see below):

```
arrow-up         arrow-down         arrow-left         arrow-right
arrow-up-left    arrow-up-right     arrow-down-left    arrow-down-right
soft-left        soft-right         start
num-0 .. num-9   asterisk           pound
toggle-hide      toggle-show        cycle               (controls)
```

### Regenerating the defaults

The default PNGs are built by a Python script using DejaVu Sans Bold +
Font Awesome Free Solid (same TTFs the runtime loads live):

```sh
python3 runtime/tools/export_keypad_sprites.py
```

Requires Pillow. Output goes to `runtime/assets/keypad/default/`.

### Env-var overrides

| Env var                  | Effect |
| ------------------------ | ------ |
| `J2ME_OVERLAY_PLACEMENT` | `below` (default), `overlay` (translucent on top), or `off` (hide entirely). |
| `J2ME_OVERLAY_LAYOUT`    | `numpad` to start on the numpad layout instead of minimal. |
| `J2ME_TRACE_MOUSE`       | Verbose mouse-event trace (window → logical coord mapping). |

## Known limitations

- **JSR-184 (M3G)**: implemented end-to-end via the vendored Khronos core
  (`runtime/third_party/m3g/`). Asphalt 3D / Galaxy on Fire / Bomberman 3D
  load and render. The GL-output → SDL-surface compositing pass is still
  open (results show as ghosting on titles that mix 2D HUD with 3D scenes).
- **Networking**: no real sockets/HTTP. `Connector.open` returns the fake
  HTTP-200 connection described above. `HttpConnection.setRequestMethod`,
  `setRequestProperty`, `close` and `StreamConnectionNotifier.acceptAndOpen`
  remain no-op stubs.
- **JSR-120 (SMS)**: 4 stubs (`MessageConnection.close/setMessageListener`,
  `Message.setAddress`, `TextMessage.setPayloadText`).
- **VideoControl** (JSR-135 video): no decoder; 7 stubs, games proceed but
  see no video.
- **`OutputStream.write([B)V` / `write([BII)V`**: no-ops on the base class.
  Anything writing logs/saves through raw `OutputStream` loses data.
- **Custom bitmap fonts**: games that compute scroll/layout offsets assuming
  a specific pixel-width bitmap font (AoE tutorial scroll, 365 Puzzle Club
  selection bar) render with minor edge bleed since we use a TTF fallback.

## Outstanding code-review findings

A pass over the runtime turned up these latent issues. The high-impact ones
(array-bounds checks, `MULTIANEWARRAY` stale-pointer, byte-array windows in
`Image.createImage` / `InputStream.read` / `readFully`, RAII surface lock)
have been fixed; the rest are documented here so they don't get forgotten.

- **`graphics_natives.cpp:749` — `(int)len` cast in `SDL_RWFromConstMem`.**
  Truncates if `len > INT_MAX`. Vanishingly rare for J2ME PNGs (single asset
  > 2 GB) but defensive: clamp or reject before cast.
- **Global `ObjRef`-keyed maps (`g_string_buffers`, `g_vectors`, `g_alerts`,
  `g_players`, `g_dg_wrap`, `g_images`, `g_gfx_surf`, `g_m3g_handles` …) —
  no GC integration.** Today the heap never sweeps so it's fine; the moment
  a real GC lands these become dangling-key zombies and any native lookup
  by `ObjRef` post-collect is a use-after-free. Fix: either weak-ref +
  finalizer hooks per map, or a sweep callback that enumerates them all.
  This is the single biggest item blocking the heap-rewrite work.
- **`stub_registry.cpp:54-62` — `wrap_stub` lambda captures raw `Entry*`.**
  Currently safe (deque keeps element addresses stable, `Entry::hits` is
  atomic). Fragile: if the container ever changes or a non-atomic field is
  added, races appear. Document the invariant or move to an arena.
- **`class_loader.cpp:146-149` — `find_or_stub` mutates `m_classes`.** Today
  every load happens before `VM::run()` so it's race-free, but no assertion
  enforces that. If concurrent class loading ever ships, this is a data race.
- **`interpreter.cpp:700-780` — `BALOAD`/`CALOAD` dispatch alias style.**
  Both opcodes alias to the same dispatch label but the handlers differ in
  fall-through behaviour; the dispatch-table-vs-label mismatch is easy to
  break on refactor. Either explicit per-opcode entries or a shared handler
  with a type tag.

## Performance notes & possible enhancements

Current performance is comfortable on a modern machine — these CLDC games
are tiny by today's standards. If a future game needs more headroom, here's
the rough order of work, cheapest first.

### Render-side wins (likely biggest ROI)

The hot path on a modern CPU isn't bytecode dispatch, it's our SDL2 work
and TTF text rendering. Free wins:

- **Glyph cache**: `TTF_RenderUTF8_Blended` is allocated/freed every
  `drawString` call. Cache rendered glyph surfaces by `(font_size, char,
  color)` and blit per-glyph instead of per-string.
- **Image surface cache**: convert PNGs to display format once at load and
  hold them; avoid re-decoding `Image` instances.
- **SDL_Texture instead of SDL_Surface**: keep the framebuffer on the GPU
  and present via `SDL_RenderCopy`, skip the per-frame `SDL_UpdateTexture`.

These probably give a 2–3× speedup on text-heavy or sprite-heavy frames
with very little code.

### Interpreter wins (~2–3× over current `switch`)

- **Direct-threaded dispatch**: replace the giant `switch (op)` with a
  computed-goto table (GCC `&&label` extension). Each opcode handler
  ends with `goto *dispatch_table[*pc++]` instead of falling out to the
  switch jump. Removes the dispatch indirection — usually 1.3–2× on
  bytecode interpreters.
- **Pre-decoded bytecode**: walk a method's `code` array once and emit a
  flat array of `{handler_addr, operand}` pairs. The dispatch loop
  becomes `goto *cur->h;` with no per-instruction operand parsing.
- **Hoist `pc`/`sp`/`code` into register locals**: encourage the compiler
  to keep them in CPU registers across opcodes instead of reloading
  through the `Frame` struct on every step.
- **Inline-cache for `invokevirtual`**: most call sites resolve to the
  same `(klass, method)` every call. A single-entry cache per call site
  skips `resolve_virtual` after the first hit.
- **Fast-path frequent natives**: inline `String.length`,
  `Math.min/max/abs`, `Integer.parseInt`, `System.currentTimeMillis`,
  `Object.<init>` directly in the dispatcher instead of going through
  the `std::function` lookup.
- **Smaller `Slot`**: if the tagged-union `Slot` can become a plain
  `int32_t`, push/pop/copy gets cheaper. Floats and refs already fit.
- **Heap fast-path**: bump alloc is cheap; the per-deref handle table
  lookup is the lingering cost. A non-handle heap (raw pointers + write
  barrier for GC) removes one indirection per `getfield`/`getstatic`.

### JIT / AOT (5–10× over current interpreter)

If interpreter tweaks aren't enough:

- **Template JIT**: emit a fixed sequence of x86_64 instructions per
  opcode into a code buffer at method-compile time. No optimization, but
  zero dispatch overhead. ~3–5× over a switch interpreter; this is what
  early HotSpot and the original Sun KVM did. Maybe 2–3k LOC.
- **Tracing JIT (LuaJIT-style)**: profile hot loops, record one trace
  through, compile straight-line specialized native code. CLDC games are
  dominated by tight per-frame paint loops which trace beautifully.
  Highest perf-per-effort if you only care about hot paths.
- **AOT to C**: walk every loaded class once, emit a `<game>.c` with one
  C function per Java method, compile with `gcc -O2`, link against the
  existing native runtime library. Build time per game is slower (30s–2
  min); runtime should be near-native. Closed-world assumption holds for
  these games (no reflection / dynamic class loading), and our existing
  C++ heap + natives are directly callable from generated C.

#### AOT-to-C feasibility for *this* codebase

Tractable. The translator would walk our existing parsed `ClassFile`s
and emit straight-line C using the same operations the interpreter
already implements. Rough sketch:

```c
// ICONST_3; ISTORE_1
locals[1] = 3;

// GETSTATIC f.b; IFNE L60
if (Cf_b != 0) goto L60;

// INVOKEVIRTUAL j.f()V (resolves to k.f at this site)
vt_k[K_F_IDX](this);
```

Stack slots become numbered C locals (`s0`, `s1`, …) sized from the
method's `max_stack` so the C compiler keeps them in registers. Object
refs stay as `int32_t` heap handles. Native bindings become direct C
function calls into the existing runtime.

The thorny parts:

- **Exceptions**: `athrow` + per-method exception tables. Either map
  to C++ `try`/`catch` or `setjmp`/`longjmp`. Need to preserve a
  notion of "current PC" so the exception table can find the right
  handler.
- **Virtual dispatch**: precompute vtables per class at translation
  time; `invokevirtual` becomes `vt[idx](args)`.
- **GC roots**: C locals aren't introspectable. Either restrict GC to
  safepoints (method entry / loop back-edge), or skip GC entirely for
  short-running games (J2ME workloads are tiny).
- **`<clinit>` ordering**: keep the lazy first-touch check at every
  static field access, or hoist init eagerly at jar load.

Estimated effort: 1500–2500 LOC of translator, 1–2 weeks of focused
work for a working prototype. Probably overkill for these games on
modern hardware — the render-side wins alone should be enough.

### Recommended order

1. Glyph + image cache, SDL_Texture framebuffer (a few hundred LOC,
   ~2–3× total).
2. Direct-threaded dispatch + invokevirtual inline cache (~1 day,
   ~2× more on top of #1).
3. Profile. If still not enough, template JIT or AOT-to-C.

For the games we care about you'd hit "renders faster than a real
phone" well before getting to step 3.

## Layout

```
runtime/
  CMakeLists.txt                ← native + emscripten build
  src/
    main.cpp                    ← CLI entry, VServ bypass setup
    util/jar.cpp                ← ZIP/jar reader
    classfile/                  ← .class file parser
    vm/
      class_loader.cpp          ← parse, link, layout, native binding
      class_def.cpp             ← virtual dispatch
      heap.cpp                  ← bump alloc + mark/sweep
      interpreter.cpp           ← bytecode dispatch loop
      scheduler.cpp             ← green-thread scheduler (ucontext / wasm fibers)
      vm.cpp                    ← run(), invoke(), <clinit>
    midp/
      natives.cpp               ← non-graphics MIDP natives (RMS, Player,
                                  Connector, Hashtable, …)
      graphics_natives.cpp      ← Graphics, Image, Canvas, Font, Display,
                                  Timer, Thread.start
      audio_eas.cpp             ← Sonivox EAS wrapper (not yet wired to JSR-135)
      lcdui_natives.cpp         ← Form, TextField, Alert, List, etc.
      m3g_natives.cpp           ← JSR-184 native bridge (native build)
      m3g_backend.cpp           ← SDL2+GLES1 context shim for M3G
      m3g_natives_stub.cpp      ← no-op replacement for wasm (no GL)
    backend/
      display.cpp               ← SDL2 window, event polling, per-surface
                                  compositing (game + keypad strip)
      overlay.cpp               ← on-screen keypad: layouts, hit test,
                                  sprite bake, JSON loader
  assets/
    keypad/
      default/                  ← canonical 128×128 button PNGs
      layouts/<name>/           ← one folder per layout; layout.json + PNGs
  tools/
    export_keypad_sprites.py    ← regenerate default/ PNGs with Pillow
  third_party/
    fontawesome/                ← Font Awesome Free Solid TTF (OFL-1.1)
    json/                       ← nlohmann/json single-header
    m3g/                        ← Khronos M3G reference (EPL-1.0)
    sonivox/                    ← Sonivox EAS synthesizer (Apache-2.0)
  build/                        ← native Debug (ASan + UBSan)
  build-release/                ← native Release
  build-web/                    ← emscripten output (.html/.js/.wasm/.data)
launcher/
  build.gradle.kts              ← Kotlin/Compose Multiplatform
  src/main/kotlin/…             ← Material list UI, JAR metadata, subprocess launch
games/                          ← .jars (gitignored)
```
