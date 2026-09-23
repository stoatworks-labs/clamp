# clamp

An AC-coupled video amplifier whose DC restoration has failed, as an FFGL **effect**
for Resolume Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll`. MIT.

Read `AGENTS.md` before changing the timeline (`Model.*`), the host-frame schedule in
`Clamp.cpp`, the block sums or the fill.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships, and what `verify.sh` builds): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel 4`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/cltest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Clamp Health=0" --set "Coupling=0.6" --set "Standard=1"`
  (0..1 for sliders, the element index for options)
- List parameters, kinds, defaults and ranges: `./build/cltest --list`
- The exact GLSL the plugin compiles: `./build/cltest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps, default 60; the
  raster runs in real time, so this matters) and an optional `--script` of
  `frame Parameter Name value` cues, linearly interpolated between a name's cues and
  held before the first and after the last — an option index interpolated passes
  through the options between, so key a cut two cues a frame apart. A cue naming no
  parameter exits 2 before any frame; a partial frame at the end of stdin ends the
  stream with exit 0; a failed render or a closed stdout exits 1 (SIGPIPE is
  ignored so a closed stdout is a failed write, not a 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/cltest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the offline checks +
  every rendered check at 320x180 AND 1280x720 + the --pipe contract + the sweep + the
  bundle, ~35 s)
- After a burst, black falls back on exp(-t/tau), blanking counted: `./build/cltest --droop`
- A flat field tilts 1 - exp(-T_active/tau) per line: `./build/cltest --tilt`
- Black shifts with APL by the closed form; primed = settled: `./build/cltest --apl`
- The clamp's residual per porch; black settles at the reference: `./build/cltest --clamp`
- Every pixel against a serial double run: `./build/cltest --reference`
- Six host rates, one trajectory: `./build/cltest --continuity`
- The supply's attack and recovery: `./build/cltest --sag`
- A perfect clamp is the identity: `./build/cltest --identity`
- A resize mid-run carries the state: `./build/cltest --resize`
- The checks can fail: `./build/cltest --negative`; one perturbation verbosely:
  `./build/cltest --droop --perturb 1` (bits in `Model.h`)
- No GL (what CI runs): `./build/cltest --offline` = `--model --clock --names` and their
  negative controls
- Every rendered check takes `--size WxH`; CI runs them at 320x180 with `--allow-no-gl`
- Shaders through glslc: `tools/check-shaders.sh build/cltest`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/cltest --bench` (best of three; the GPU is shared, so run it twice)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Clamp.bundle`
- The demo's shaders are still the plugin's: `python3 demo/tools/check_shaders.py`

## Notes
- **The CPU decides every number, the GPU every sample.** `Model.cpp` holds the
  standards' timing, the timeline's coefficients and the walk (one affine step per
  16-sample block and per blanking interval, in double); `Clamp.cpp` decides which
  fields a host frame runs. A wrong level is almost always a C++ fix.
- **The state is u = s + r**, the capacitor's voltage with the reference taken out: a
  low-pass of x toward blanking, inside [0, 1]. The display adds r back. The harness
  states the dynamics in s with r inside them, on purpose.
- **Nothing on the GPU outlives a frame.** The state across frames is six doubles
  (`nextState`, `shownState`) and the supply's two; a resize needs no carrying.
- **Every host frame reads the block sums back** (`glReadPixels` of blocks × lines
  RGBA32F): the walk needs them. It is a stall and it is the design.
- **Fields due since the last frame all run on this frame's picture**; a frame with no
  new field re-scans the one on show from its own start. `model::FieldAt` decides the
  field, with 1e-6 of a field of slack for a host frame that lands on a field start.
- **The first frame is primed** to the picture's periodic steady state
  (`B / (1 - A)`), not started from a discharged capacitor. `SetPrimeForTest(false)`
  turns that off for `--apl`'s settle check.
- **The harness never re-types the model.** Timing, control laws and the serial
  reference are stated in `cltest` from the documents and definitions; `--model` holds
  the plugin to them. The one plugin constant it reads is `kBlock`, for the bound.
- **`Perturb` bits are test hooks**, always 0 in the plugin.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1; `SetParamInfof` reads its default
  out of `params[]`, so fill `params[]` first. Options are mapped by index in
  `Controls.cpp` (an option's range reads back 0..1).
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `clamp_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- `FFGLShader::Set` has no array overload: the three 16-entry weight tables go in with
  `glUniform1fv` under the pass's own shader binding.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `CL01`, display name `SW Clamp`.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured offline on
  macOS, plus an `oxbow` load. On Windows, v0.1.0's CI build met Arena 7.27.1 in the
  fleet gate on win-lab (software rendering); see the README's status.
- Never seen on footage, only on synthetic cards.
- The triode stage and Show Blanking are checked only for being alive.
- No OpenFX port, no factory presets.
- **The browser demo's CPU half is a port**: the timeline, field walk, supply, triode constants and clock are a hand port of `Model.cpp`, `Controls.cpp`, `Clock.cpp` and `ProcessOpenGL`, and nothing checks it. Change any of those and change `demo/plugin.js` by hand.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated by stoatworks-backend's
  sync-about.py and sync-attributions.py; edit them there.

## Browser demo

`demo/` is the page at **clamp-demo.stoatworks-labs.com**, deployed from
`wrangler.toml` with `cf-run npx wrangler deploy` — no build step; what is
committed is what is served. `demo/vendor/` is copied in by
`~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh clamp` and is not
a place to edit. The shaders in `demo/plugin.js` must stay the plugin's:
`python3 demo/tools/check_shaders.py` (run by `tools/verify.sh`). Serve it locally
with `python3 -m http.server` in `demo/`. See AGENTS.md, *The browser demo*.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/clamp/clamp.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\clamp\logs\clamp.YYYY-MM-DD.log   (Windows)
