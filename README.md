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
