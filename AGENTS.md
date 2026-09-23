# AGENTS.md — Clamp

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

An AC-coupled video amplifier whose DC restoration has failed, as an FFGL 2.1 effect
(`CL01`, shown as `SW Clamp`) for Resolume Arena and Avenue. C++17 + GLSL 4.10,
CMake, universal macOS `.bundle` and a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/clamp`.

Built 2026-09-23 in one session from the fleet's templates and `specs/SPEC-clamp.md`
(with `BRIEF.md` and `BRIEF-ADDENDUM.md`): standards for the harness, the verify
script, the `--pipe` contract, the negative-control pattern, `--offline` and the
GL-less CI; compander for the idea of a serial recurrence the GPU computes in
parallel and checks against; old-cathode for the broadcast timing living in one
header; tinsel for `PassBuffer` and the trap list; graticule for the notes.

---

## The one idea

**A capacitor does not pass DC, and the picture's black level is DC.**

A clamp used to put it back every line: a switch that closed in the back porch and
pulled the signal to a reference. With the clamp weak or dead, the coupling
capacitor and the next stage's input resistance are a high-pass filter on the whole
raster, run in time order through every line and every blanking interval. The host
frame is placed as the active area of a real field and that one RC runs through all
of it:

| what the RC does | what comes out |
| --- | --- |
| the average over τ is forced to the reference | **black wanders with content**: a bright scene pushes it down, a dark one lifts it |
| what a burst charged decays on exp(−t/τ), blanking counted | **a bright moment darkens the next one** |
| over one active line, y[n] = (x − u₀) aⁿ | **lines tilt** when τ is near a line, **fields tilt** when it is near a field |
| in each porch the switch pulls toward the reference at 1/τ + 1/τ_c | **a healthy clamp removes it**, less the residual exp(−T_porch/τ_c) |

Two further mechanisms, deliberately separate: the **supply** droops with APL and
recovers (gain = 1 − depth × e, e following APL with an attack and a recovery time
constant), and an optional **triode** stage (3/2 law, hard cutoff, soft grid-current
knee, normalised to unit small-signal gain so Drive reaches into the knees without
changing the level).

### The arithmetic, in one place

    circuit    the grid returns through R to the reference r; the switch joins it to
               the same r through R_s in each back porch
    ODE        ds/dt = ( x - r - s ) ( 1/tau + g(t)/tau_c ),  y = x - s
    in u       u = s + r:  du/dt = ( x - u ) ( 1/tau + g(t)/tau_c ),  y = x - u + r
    sample     u <- a u + ( 1 - a ) x,  a = exp( -D / tau ),  D = T_active / W
    blanking   u <- u exp( -T / tau );  porch: u <- u exp( -( 1/tau + 1/tau_c ) T_porch )
    block      m samples: u <- a^m u + ( 1 - a ) P,  P = sum a^(m-1-j) x[j]
    field      u <- A u + B  (A timing only, B picture only); steady state B / ( 1 - A )
    timeline   per line: sync | porch | W samples | front porch;  A_f lines, then
               floor( L_f - A_f ) blank lines and the half line;  L_f T_line = 1 / rate

Every step is the ODE's exact solution for x held over the step (a zero-order hold of
the host's pixel), so nothing here is an approximation of the physics except the hold
itself.

### What does not fall out, and is the honest limit

- **The field is scanned non-interlaced.** Each field covers the whole picture on 288
  (PAL) or 240 (NTSC) lines, top to bottom, keeping the real field period (the half
  line is kept as time). The capacitor is a low-pass and cannot tell the two line
  orders apart except at τ of about a line, where interlace would put each field's
  lines two frame-lines apart; that difference is not modelled.
- **Lines sample the host picture.** Active line l reads host row
  floor((2l + 1) H / 2A); the output row subtracts the state from its OWN pixel, so the
  host's detail survives and only the state is at the field's line count. A 1080-row
  picture drives the capacitor from 288 of its rows.
- **Sync is at blanking level**, as the spec says. A real sync tip below blanking
  would push the average down a little further; it is drawn at its tip only in Show
  Blanking, for legibility.
- **The picture for a field is the host frame that runs it.** Every field that
  started since the last host frame runs on this frame's picture — the only one there
  is. At a host rate below the field rate the unseen fields see a picture up to a host
  frame early.
- **The triode is a look.** Its law is a textbook shape, not a measured valve, and no
  check reads it beyond the sweep.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Model.{h,cpp}` | The amplifier as arithmetic: the two standards (BT.470 / SMPTE 170M), the timeline's coefficients, the field walk from block sums, the field map and its fixed point, the supply's step, the triode's constants, `FieldAt`, the `Perturb` bits. No GL. |
| `source/Controls.{h,cpp}` | Every 0..1 slider to its physical unit. |
| `source/Clock.{h,cpp}` | standards' clock: unit voting, origin + offset in double, no per-frame clamp. |
| `source/Shaders.{h,cpp}` | Four shaders: vertex, sums, fill, display. |
| `source/Clamp.{h,cpp}` | The plugin: parameters, the schedule of fields per host frame, the passes, the test hooks. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/cltest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/check-shaders.sh` | glslc on the dumped shaders; verify.sh and CI both call it. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |

Per host frame:

1. **sums** — blocks × active lines, RGBA32F: each 16-sample block's weighted sum P of
   R, G and B, and its plain luma sum (for APL). **Read back to the CPU.**
2. **walk** (CPU, double) — the field from u = 0, recording at every block start and
   every blanking edge a scalar A_k and three B_k, so any start state gives
   A_k u₀ + B_k. The field's own A and B fall out.
3. **schedule** (CPU) — the field index is floor(t / T_field). Fields that started
   since the last frame and will never be seen go through u ← A u + B; the last is
   shown, from its start state. A frame with no new field shows the same field again.
   The supply steps once per field. The first frame starts at the steady state.
4. **fill** — W × active lines, RGBA32F: every sample's state from its block's start,
   with a^j and (1 − a) a^i as uniform tables.
5. **display** — the host's framebuffer: y = gain × (x − u + r), the triode, the
   0..1 clamp of the output, the mix; or the whole field with its blanking.

---

## Traps

Roughly in the order they will bite.

### ☠️ A negative control can fail for the wrong reason, and the first droop control did

With the blanking dropped from the timeline, a dead clamp settles a *flat* picture at
y = 0 exactly — the capacitor holds the whole average, because now there is nothing
but picture to average over. The first `--droop` measured on a flat ground, so its
negative control "failed" on a clipped reading at the output's floor, not on the
exponential it exists to check. The verdict said caught; the detail said CLIPPED.
The check now reads a bright stripe down the left edge over a darker ground, which
stays above the floor in either model, and the negative fails on the fit (τ comes
out 64 × 64/51.95 µs: exactly the line period over the active time). Every rendered
check now reports a clipped reading as its own failure, and every negative control's
detail was read (see the table below), not just its verdict.

### ☠️ The first model put the grid's return at blanking, and a dead clamp could only crush

The spec says "the mean over τ is forced to the reference". The first model had the
grid return to blanking (0) and only the clamp pull to the reference, so a dead
clamp forced every picture's average to black and pushed half of it below the floor,
and Clamp Reference did nothing without a working clamp. With both returns at the
same r (as the spec says, and as a single reference rail would be wired), r leaves
the dynamics: u = s + r obeys an r-free equation and y = x − u + r. The plugin runs
in u, which is inside [0, 1]; the harness runs in s with r inside the dynamics — two
arrangements of one statement. The default reference is +0.25 so a failed clamp's
picture sits in view: dark scenes lift, bright ones push black down, which is the
look everybody remembers from a set without DC restoration.

### A stated law must clamp like the control it states

`--reference` failed once with the supply 7e-5 out: the check asked for a 5 ms
attack, which is below the slider's floor of 10^−2.3 = 5.01 ms, and the harness's
statement of the law did not clamp the slider to 0..1 where the plugin's did. The
plugin was right. Every stated law in `cltest` now clamps, and the setting asks for
6 ms.

### Resolume's clock resolves 1.2e-10 s after six days, so a field boundary needs slack

The first field rule had 1e-9 of a field of slack for a host frame landing exactly on
a field start (every sixth frame at 60 fps against PAL). A millisecond clock at
~1e9 ms resolves about 1.2e-10 s, which is 6e-9 of a PAL field, and `--clock` found
100 of 3,000 frames on the wrong field. The slack is 1e-6 of a field (20 ns), in one
place (`model::FieldAt`), which the harness calls on both clocks.

### A 1 − a computed by subtraction is gone at long τ

At τ = 2 s and 1920 samples, D/τ is 1.4e-8 and a rounds to 1.0 in float. (1 − a) is
computed as −expm1(−D/τ) in double and handed over as its own table
((1 − a) aⁱ), never formed on the GPU; a^j likewise. The fill adds the small in-block
sum to a^j u₀ once, at the end, rather than folding u₀ into a running sum j times.

### The readback is the cost

Every frame waits for the sums pass so the walk can run. That is most of the
0.36 ms at 720p. Double-buffering the readback (walking the previous frame's sums)
would hide it and put the capacitor a frame behind the picture; not done.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
restored before the display, with the host's FBO bound explicitly); every
`ffglex::Scoped*` clears to 0 on exit, so every `Ensure()` and texture upload happens
before anything binds a texture; `FFGLFBO::Release()` leaks the colour texture, which
is why `PassBuffer::Destroy()` deletes it first; `SetParamInfo` clamps a STANDARD
default into 0..1 and `SetParamInfof` reads its default out of `params[]` (so
`params[]` is filled first); an option's range reads back 0..1 whatever its element
count; the core is an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS`
for the About block; `FFGLShader::Set` has no array overload (`glUniform1fv`);
Resolume's clock overflows a float, so nothing absolute reaches a shader; the first
frame is primed so the plugin does not open with a settle that belongs to nothing;
state across frames must survive a resize (here it is CPU doubles and cannot not);
`nm | grep -q` fails under pipefail when grep succeeds; a closed stdout must be a
failed write, so `--pipe` ignores SIGPIPE — rebate's `--pipe` does not (standards
measured it at 141), and this one was written from standards', not rebate's.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh`, and at 333×187 and 1920×1080 by hand.

What makes them rasteriser-proof by construction: **every coordinate is an integer
computed in integers** (`gl_FragCoord`, never an interpolated uv); **every read is
`texelFetch`**; **every weight is computed on the CPU in double** and handed over as a
uniform, so no driver `exp` is in any checked path (Show Blanking's diagnostic uses
one, and nothing reads it); and **the walk is in double on the CPU**. The GPU only
multiplies and adds in float32, and the bound below counts every one of those
roundings.

**The bound** (`outputBound` in `cltest`), in u = 2⁻²⁴, for X = 1 (input), U = 1
(state), R = 0.75 (reference), m = 16 (block): the block sums, 1.01(m+2)X — their
errors, carried forward through every later decay, telescope to one block's worth
however long the history; the start state's upload, U; the fill, 4U + 1.01(m+2)X; the
display, 5U + (X+U) + (X+U+R) + 2(X+U+R); with sag, depth × 1.01(m+3)(X+U+R) for the APL
from float sums. Total 56.6 u without sag (3.4e-6), up to 83 u with full sag. It
assumes correctly rounded +, −, × (GLSL 4.10 §4.7.1) and no order of summation: the
γₙ bound holds for any order, so reassociation or fma contraction cannot break it.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--droop` line, field | Δy between a burst run and a plain one on each later line, first sample | each line against the first usable line × exp(−Δt/τ), Δt = lines × T_line: 2ε (two renders) × (1 + decay); ≥ 3 lines must be ten tolerances clear | lines are whatever rows the raster shows; at 320×180 only ~5 lines are clear of the bound at τ of a line, ~20 at 1280×720 |
| `--droop` second | the same across 35 fields at one pixel, host at the field rate | as above with Δt = fields × T_field | none: one pixel, whole-field bursts |
| `--tilt` | 1 − (y[W−1]/y[0])^{W/(W−1)} on every row | the interval the ratio can occupy given ε on each value | the exponent carries W; at τ = 1 s the tilt (5.2e-5) is smaller than that interval (3.4e-5 at either raster), so the check bounds it but cannot separate it from 6.4e-5 |
| `--apl` | slope of the patch's level against the background level, τ = 1 s | 2ε per level against κ from the closed-form convolution over all past fields | the patch is the right eighth; κ is computed for the W given |
| `--apl` primed | the primed frame against 59 settled fields | 2ε + e^{−59} × 1 (the dead clamp's field map is e^{−T_field/τ}) | none |
| `--clamp` | (y[l+2d] − y[l+d]) / (y[l+d] − y[l]) at each line's first sample over four level steps | 2ε on each difference, propagated through the ratio; triples need a first difference > 100ε | which triples exist depends on which lines the raster shows (4 at 320×180, 6 at 1280×720 for the strongest clamp) |
| `--clamp` settle | the last line against the line map's fixed point | ε + what the transient has left (e^{−lines × (T_line/τ + n)}) | none |
| `--reference` | every pixel of every frame | ε (the bound above, with the setting's sag depth) | the serial run is computed at the raster given |
| `--continuity` | nine points a frame against the serial run, at six host rates | ε | none: the material is uniform per line |
| `--sag` | out(depth)/out(0) on a patch against 1 − depth × e(closed form) | ε(depth) and ε(0) propagated through the ratio | none |
| `--identity` | every value against the input | (1 − e^{−T_active/τ}) (the line's drift after the clamp lets go) + e^{−20 − T_porch/τ} + ε | none |
| `--resize` | three rows' first sample across a doubling | ε | the serial run is width-independent for a line-uniform picture (x is constant over the active interval however it is sampled) |
| `--model` | block states, field map, fixed point against the serial walk | 1e-12 (two double arrangements of one recurrence over ~10⁵ steps) | none (no GL) |
| `--clock` | field decisions of a six-day ms clock, a fresh s clock and exact rationals | exact | none (no GL) |

Deliberately NOT relied on: round-to-nearest anywhere (the bound allows a full ULP
per operation); `mix(a, b, 1) == b` (the display returns early at Mix 1);
`exp`, `pow` or any transcendental on the GPU in a checked path; interpolated
varyings; a texture unit's filtering; GLSL integer division of a negative operand
(none occurs).

What might still differ on another rasteriser: a driver that does not round float
+ and × correctly would break the bound; GLSL 4.10 requires it. The software context
CI would fall back to is a different compiler for the same GLSL; `check-shaders.sh`
covers the syntax there and the rendered checks run with `--allow-no-gl` so a runner
without a context skips loudly.

### The negative controls

`cltest --negative` runs nine against rendered checks and `--offline` seven against
the model; `--perturb BITS` runs any check verbosely against one. Each perturbs the
*plugin's* model — a `Perturb` bit the shipped plugin carries at zero — never the
harness's expectation. Every one below was read for what it failed on; none fails on
a clipped reading.

| perturbation | what fails, measured at 320×180 |
| --- | --- |
| blanking dropped from the timeline (the spec's) | `--droop`: fitted τ 78.8 µs against 64 (a line) and 24.6 ms against 20 — both 64/51.95 too long, a line without its blanking — and 1.34 s against 1, which is 20/14.96, a field without its blanking; 59 to 4,700 tolerances out |
| the state reset every host frame (the spec's) | `--continuity`: 0.51 from the continuous run at every rate |
| the sample period from the whole line | `--tilt`: 0.632 against 0.556 at τ of a line, 0.00319 against 0.00259 at τ of a field (18×); not caught at τ = 1 s (see the table above) |
| blanking dropped, on the APL slope | `--apl`: 12,600 to 13,100 tolerances out |
| the switch closed for half the porch | `--clamp`: residual 0.990 against 0.980, 0.905 against 0.819, 0.368 against 0.135 (190 to 1,190 tolerances); black settles 0.002 off |
| the block decay one sample short | `--reference`: 151 to 626,000 u against bounds of 57 to 72 in three settings; the fourth (a strong clamp at τ = 5 ms) passes, correctly: the clamp holds u near 0, and an error of (1 − a) u per block stays under the bound |
| attack and recovery swapped | `--sag`: fitted 0.2 s and 0.039 s against 0.04 and 0.2 |
| the clamp never closes | `--identity`: 0.37 from the input |
| a resize that re-primes the state | `--resize`: 0.056 from the serial run after the resize |
| offline: blanking dropped, whole-line sample period, half porch, short block decay, swap, no clamp | `--model`: the walk, the field map or the supply differs from the statement |
| offline: the clock kept in float | `--clock`: fields differ at a six-day origin |

### The mutation

One character of the shipped GLSL, on a clean committed tree: in the fill,
`InputW[ j - 1 - i ]` → `InputW[ j - 0 - i ]` (each sample's in-block sum weighted one
sample too old — a relative error of 1 − a in the part of the state the block has put
in). Caught at 320×180 by `--reference` (1,389 u against a bound of 56.6, at τ of a
line) and `--tilt` at τ of a line (15.5× and 16.2× tolerance, both standards); at
1280×720, where 1 − a is four times smaller, by `--reference` (89 u against 56.6) and
`--tilt` NTSC only (1.03×). `--droop`, `--clamp` and `--resize` passed, correctly:
they read each line's first sample, where the in-block sum is empty. The others run
at τ long enough that 1 − a is below the bound. Reverted with
`git checkout source/Shaders.cpp`; the tree was clean before and after. The margin
at 1280×720 is thin, and it thins with width: a mutation of this size at 4K would
likely pass. That is a limit of a float bound against an error that scales as 1/W,
stated, not hidden.

---

## Decisions taken without asking

- **The standards**: 625/50 per BT.470-6 (line 64 µs; front porch 1.65, sync 4.7, back
  porch 5.7; active 51.95 µs; 288 active lines a field; 312.5 lines) and 525/59.94 per
  SMPTE 170M (line 1001/15.75 MHz; 1.5, 4.7, 4.7; active 52.656 µs; 240 active lines, the
  digital 480; 262.5 lines; 60000/1001 fields). One header (`Model.cpp`), with sources.
- **Non-interlaced fields** (see the honest limit); **line order in a field**: active
  lines, then the blank lines, then the half line; **line order in a line**: sync,
  back porch, active, front porch. The half line has no clamp.
- **One reference** for the grid's return and the clamp, per the spec's "the mean is
  forced to the reference". Clamp Reference −0.25..+0.75, default +0.25.
- **Clamp Health** is time constants of the switch per back porch,
  n = 20 × 10^(−5(1−v)), exactly 0 at 0 (dead). Per porch, so a setting means the same
  clamp on either standard. **Coupling** 20 µs to 2 s, logarithmic.
- **Luma** is Rec. 601 (0.299, 0.587, 0.114). Per Channel off subtracts the luma of the
  three channel states from every channel — exact by linearity, so the three are
  always run and toggling Per Channel never jumps.
- **The sample rate is the host's width**: W samples across the active line.
- **The output is clamped to 0..1** after the triode, as a display would; every check
  reports a reading on the floor or ceiling as a failure of its own.
- **Supply**: e steps once per field toward that field's APL of the INPUT (luma,
  active area) with the attack while rising and the recovery while falling; the field
  on show carries e as it stood at its start. Depth 0..0.5, times 5 ms to 2 s.
- **Triode**: u = drive (v − 1/2) + bias; plate (1 + u')^{3/2}, 0 below u = −1,
  u' = u / (1 + u/2) above 0; normalised to unit slope at the bias. Bias ±0.6, Drive
  0.5–4.
- **Fields per host frame**: every field started since the last frame runs, on this
  frame's picture; unseen ones through the field map; a frame with no new field
  re-scans the one on show from its own start with this picture. More than 100,000
  due at once (a clock anomaly) jumps to the steady state.
- **Priming**: the first frame (after `InitGL`) starts at the picture's steady state
  and the supply at its APL. A change of Standard re-bases the field count but keeps
  the capacitor's charge.
- **Block size 16**: the fill loops at most 15 samples; the walk is W/16 steps a line.
- **No factory presets, no audio input** — the spec has neither.
- **`--fail-render-at N`** is a harness-only hook so `verify.sh` can prove `--pipe`
  exits 1 on a failed render.
- **Provisional About and attributions** (`StoatworksAbout.h`, `ATTRIBUTIONS.md`) are
  hand copies adapted from standards' with `guide=""`; the button count, and so the
  parameter count, does not change when the fleet's sync regenerates them.
- **Commit trailers name the model that did the work** (`Claude Opus 5.5`), as the
  session's instructions said, not the brief's `Claude Fable 5.1`.
- **The FFGL submodule was dissociated from the reference clone** (`repack -a -d`, the
  alternates file removed) so this repo does not depend on a path in `~/Projects`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-23)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320×180 and 1280×720, with the same checks passing at 333×187 and 1920×1080
by hand.

- **Droop.** At τ of a line, a field and a second, both standards: every measured line
  or field on exp(−t/τ) with the blanking counted, worst 0.013 of tolerance; fitted
  τ 63.998 µs, 20.000 ms, 0.99999 s (PAL).
- **Tilt.** 0.555906 (stated 0.555906) at τ of a line, 0.0025941 at τ of a field, on
  every row; at τ = 1 s within the bound only.
- **APL.** 0.7481 (PAL) and 0.7575 (NTSC) per unit of APL at τ = 1 s, the closed form
  to 0.007 of tolerance; the primed state equals 59 settled fields to the bit.
- **Clamp.** Residual per porch 0.980199, 0.818731, 0.135335 at 0.02, 0.2, 2 time
  constants (e^−n to six places); a healthy clamp settles at the stated level to 8e-9.
- **Reference.** 1.7 M (320×180) and 27.6 M (1280×720) values over four settings, worst
  2.3 u against bounds of 57 to 83 u.
- **Continuity.** Ten runs at host rates from 23.976 to 144 fps, worst 1.3e-7 against
  3.4e-6, including fields no frame showed.
- **Sag.** Fitted attack 0.040 s and recovery 0.200 s against 0.04 and 0.2.
- **Identity.** 1.4e-5 against a bound of 2.9e-5.
- **Resize.** Worst 1.5e-7 across a doubling.
- **Offline.** Timing exact; control laws exact; the blocked walk against the serial
  one to 5.6e-13; the clock on 3,000 frames.
- **Negative controls.** All sixteen fail their check, on the physics.
- **Mutation.** Caught (above), with the margin stated.
- **No dead controls**, all 13, with the four About buttons skipped.
- **Every shader compiles** through `glslc`, as the plugin hands it to the driver.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue
  with 2, and exits 1 on a failed render and on a closed stdout.
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.clamp`, ad-hoc signs, and `oxbow` reports `SW Clamp` / `CL01` /
  `effect` and renders 120 frames through `plugMain`.
- **Render cost**, best of three runs of 60 frames after a warm-up, `glFinish` both
  sides, best of several runs on a shared GPU (single runs moved by up to 2×):

  | | ms/frame | % of a 60fps frame | Per Channel + Triode |
  | --- | --- | --- | --- |
  | 1280×720 | 0.36 | 2.2% | 0.43 |
  | 1920×1080 | 0.41 | 2.4% | 0.41 |
  | 3840×2160 | 0.82 | 4.9% | 0.77 |

### Assumed, or not done

- ☠️ **Never loaded into Resolume**, on either platform. Everything was compiled,
  rendered and measured offline against the real plugin class in a headless CGL
  context, plus an `oxbow` load.
- **Never seen on footage.** Every picture so far is synthetic.
- **The clock-unit voting** is readout's by way of standards, which has met Arena;
  this plugin has not.
- **The triode and Show Blanking** are checked for liveness only.
- **Interlace is not modelled** (see the honest limit).
- **The Windows build is CI-only** and CI cannot run yet.
- **Not verified at 4K**, only benchmarked there; and the mutation margin shrinks with
  width (above).
- **No OpenFX port and no browser demo.** Not required for 0.1.0.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies** with
  `guide=""`; register the project and re-run the syncs before the first release.
- **Nothing has been through a show.**

---

## Open questions

- **Should the fields interlace?** Truthful at τ of a line; invisible above it; it
  would double the passes' bookkeeping (a field of each parity on show).
- **Should sync go to its tip?** A −0.3 sync tip for 4.7 µs a line pulls the average
  down by about 2% of a volt; the spec says blanking level, and that is what is built.
- **Should the readback be double-buffered?** Hides the stall, costs a frame of
  latency in the capacitor.
- **Should the supply be driven by the amplifier's output** (what really draws the
  current) rather than the input's APL? With a dead clamp the output's average is the
  reference, so the sag would stop breathing — which may be more truthful and less
  useful.
- **A tighter bound for `--tilt` at τ = 1 s** would need the error of the two end
  samples to be correlated, which a worst-case bound cannot use.

---

## Siblings

- **standards** — the harness, verify, CI and `--pipe` shapes, the negative controls,
  `--offline`, the clock.
- **compander** — a serial recurrence the GPU does in parallel, checked against the
  serial one.
- **old-cathode** — the broadcast standards in one header.
- **tinsel** — `PassBuffer`, `sweep.py`, and the fleet's trap list.
- **plumbicon** — state that carries picture history (here it is six doubles, so the
  resize trap cannot bite).
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
