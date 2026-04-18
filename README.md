# DirectGlide

A from-scratch **Glide 2.x → Direct3D 11 wrapper**, written to run
**King's Quest 8: Mask of Eternity** in Glide mode on modern Windows.

DirectGlide ships as a drop-in `glide2x.dll` — you replace the game's
existing Glide library (nGlide, dgVoodoo, etc.) with this one and the
game boots into a native D3D11 rendering path.

---

## ⚠️ Important — this is NOT a complete fix for KQ8

DirectGlide only replaces the Glide rendering library. To actually run
KQ8 on modern Windows you **also need the `kq8fix` shim** applied to the
base game. `kq8fix` handles the game-engine-side compatibility problems
(CPU speed detection, DirectDraw fallbacks, window creation, etc.) that
DirectGlide doesn't touch.

**Setup order:**
1. Install KQ8 (GoG / original disc / etc.)
2. Apply `kq8fix` to the base game
3. Drop DirectGlide's `glide2x.dll` into the game folder (replacing any
   existing `glide2x.dll`)
4. Make sure `Options.cs` has `assignGModeName Glide` so the game picks
   the Glide renderer

Without `kq8fix`, the game will likely not launch or will crash before
reaching the 3D pipeline — regardless of what Glide wrapper you use.

---

## Why this exists

KQ8's bundled nGlide 1.04 crashes in Windows' Application Compatibility
layer (`AcGenral.DLL`) when rendering on modern Windows. dgVoodoo had
similar issues in testing. This wrapper was written specifically to
bypass those compatibility shims by implementing Glide directly against
D3D11 with no legacy code paths in between.

The bonus: because we're implementing Glide from scratch, we get to add
niceties that nGlide/dgVoodoo/real Voodoo hardware couldn't provide.

---

## Features

### Rendering
- **2× SSAA** — scene renders at 1280×960 internally, box-filter
  downsampled to 640×480 before final upscale. Fixes tri-diagonal
  seams the original hardware had.
- **8× anisotropic filtering** with **Lanczos-2 CPU-generated
  mipmaps** — sharp detail at oblique angles without shimmer.
- **Sharp-bilinear upscale** from 640×480 to native resolution —
  retains pixel-perfect retro feel while filling modern displays.
- **Clamped-exponential fog** — pre-populated exp fog table for
  KQ8's `grFogMode(GR_FOG_WITH_TABLE)` calls with `grFogTable`
  never invoked by the game (relied on hardware defaults on Voodoo).
- **Full combiner support** — color / alpha / texture combiners
  with factor variants implemented in a single uber-shader.
- **W-buffer depth** for correct sorting in the game's native mode.

### Integration
- **Fullscreen with aspect-preserved letterbox** — 640×480 render
  target, upscaled to fit your monitor with black bars where needed.
- **Mouse coordinate translation** via both WndProc subclassing and
  IAT patching of `GetCursorPos`/`SetCursorPos`/`ClipCursor` in the
  game EXE.
- **HUD persistence hack** — KQ8 draws some static overlays (XP bar,
  decorative icons) only on first appearance; DirectGlide detects
  HUD bar extents from the leftmost pixel column and preserves those
  rows across frames, while dynamic elements (cursor, dialogue) still
  wipe cleanly.
- **Fullscreen intro/outro FMV** — WndProc subclass on the MCIWnd
  child window plus IAT-patches on `msvfw32`'s imports of
  `StretchDIBits`/`SetDIBitsToDevice`/`StretchBlt` intercept the video
  playback thread's draws and redirect them to an aspect-preserved
  letterboxed fullscreen rect. MCIWnd's own `PUT_DEST`/`SETZOOM`
  messages are ignored on modern Windows, so we hook the underlying
  gdi calls instead.
- **Save-file thumbnails** — `grLfbLock(READ_ONLY)` populates the
  returned buffer with a GPU-readback of the current `gameTex` so
  KQ8 can capture a real thumbnail of the current 3D scene when
  saving. `grLfbWriteRegion` blits saved thumbnails back for the
  save/load screen. Pre-fix, both paths silently returned the
  wrapper's canary buffer → every thumbnail slot was colorful static.
- **NVIDIA GPU selection** — enumerates DXGI adapters and picks the
  discrete GPU over integrated when available.

### Performance
- **Chained-hash texture cache** with LRU eviction — no memory thrash
  even in texture-heavy scenes.
- **State object caching** — cached blend/depth/raster states avoid
  redundant D3D11 state object creation.
- **Palette hash caching** for deferred palettized texture decoding
  (OpenGlide-style).

---

## Building

Requires **Visual Studio 2022** (or newer) with the x86 C++ toolchain and
Windows 10 SDK.

```
build.bat
```

Output: `build\glide2x.dll`.

To enable debug logging (writes `DirectGlide.log` next to the game's
EXE), edit `src/log.c` and change `DG_LOG_ENABLED` to `1`, then rebuild.

---

## Installing

1. Locate your KQ8 install folder (typically `C:\Games\KQ8` or
   `C:\Program Files (x86)\GOG Galaxy\Games\Kings Quest 8\`).
2. Back up the existing `glide2x.dll`.
3. Copy `build\glide2x.dll` into the game folder.
4. Launch KQ8. If it crashes in `AcGenral.DLL`, launch via a `.bat`
   that sets `__COMPAT_LAYER=!` to bypass compat shims:

   ```batch
   @echo off
   set __COMPAT_LAYER=!
   start Mask.exe
   ```

---

## Known Limitations

- **KQ8 specific**. DirectGlide implements the subset of Glide 2.x that
  KQ8 actually uses (106 functions; 150 exports for binary compat).
  Other Glide games may work but aren't tested or targeted — features
  KQ8 doesn't call (multi-TMU, LFB read-back, etc.) are stubs.
- **Fog defaults tuned for KQ8**. KQ8 never calls `grFogTable`, so we
  pre-populate a fog table tuned for its specific W range (near=16000,
  far=20000). Other games that do call `grFogTable` will use their
  own tables correctly, but if they also don't call it the defaults
  may look wrong.
- **HUD-detection hack relies on a KQ8 convention**. We probe the
  leftmost pixel column to detect HUD bar extents. If another game
  renders non-HUD content that extends to x=0, the persistence logic
  could misclassify it.
- **No AA draw variants / line drawing**. `grDrawLine`, `grDrawPoint`
  and `grAA*` are routed through the triangle path.
- **Intro/outro FMV codec**. KQ8's intro and ending movies are Indeo 5
  encoded AVIs (`W32opn_1.dll`, `W32ndg_1.dll`, `W32mdl_1.dll`) —
  Windows 10/11 has dropped the Indeo codec. Users can re-encode to
  Cinepak with ffmpeg:
  ```
  ffmpeg -i W32opn_1.bak.dll -c:v cinepak -q:v 5 -c:a pcm_s16le -f avi W32opn_1.dll
  ```
  The "Replay Intro" menu option also requires **two clicks** on
  modern Windows (first click puts the game into a fullscreen /
  mode-switch state, second click plays the video) — that's a KQ8
  engine quirk, not a DirectGlide issue. Once the video does play,
  DirectGlide handles the fullscreen stretch.
- **Pitch-black shadow decals**. KQ8 draws shadow polygons via blend
  `(ZERO, SRC_COLOR)` with a near-black source texture. Because our
  alpha-combiner pipeline emits low alpha for pixels that should be
  fully opaque, we currently map `GR_BLEND_SRC_ALPHA` to
  `D3D11_BLEND_ONE` as a workaround that keeps regular geometry
  looking right. The side-effect is that legitimate multiplicative
  shadow decals render as pure-black rectangles instead of subtle
  darkening. A proper fix requires auditing the alpha combiner to
  match the Glide 2.x spec — see `mapBlend()` in
  `src/d3d11_backend.c`.

---

## Credits

- **OpenGlide** (https://github.com/voyageur/openglide) — reference for
  the `tableIndexToW[]` lookup table and `guFogGenerateExp/Linear`
  formulas. DirectGlide's fog code structure mirrors OpenGlide's.
- **3dfx Interactive / Glide SDK 2.2** — API specification.
- **Sierra On-Line** — King's Quest VIII: Mask of Eternity (1998).

---

## Legal

King's Quest, Mask of Eternity, and related trademarks are the
property of their respective owners. DirectGlide is an independent
interoperability library and is not affiliated with, endorsed by, or
sponsored by Sierra, Activision, or any other rights-holder. It
implements the published Glide 2.x API specification and contains no
code from any Glide SDK implementation.

DirectGlide is released into the public domain under the Unlicense —
see `LICENSE`. Use it however you want, no warranty provided.
