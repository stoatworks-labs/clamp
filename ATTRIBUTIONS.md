# Attributions

Clamp is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

It is PROVISIONAL: hand-written in the shape the `stoatworks-backend` sync
(`scripts/sync-attributions.py`) generates. Clamp is not yet registered in that
script's lists, so the sync cannot produce this file yet. Once the registration is
finished the sync overwrites this file; edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives through the vcpkg manifest on Windows only. Not fetched on macOS.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls — listed because it is present in the checkout. The harness writes PNGs through the system zlib and not through this.

## Within the fleet

Not third-party, but owed a line. The harness shape, the `--pipe` contract, the
negative-control pattern, `--offline`, `check-shaders.sh`, the verify script and the
host clock (with **readout**'s clock-unit voting) are **standards**'s
(`github.com/stoatworks-labs/standards`, MIT, Stoatworks Labs), by way of **pitch**,
**rebate** and **slowscan**. The idea of computing a serial recurrence on the GPU in
parallel and holding it to the serial one is **compander**'s; keeping the broadcast
standards' numbers in one header is **old-cathode**'s. `PassBuffer` is **tinsel**'s,
by way of standards. The PCG output mix the harness uses for its noise frames is the
well-known `pcg_hash` construction, written out here rather than copied from anyone's
source.

## Science, not code

The model is built from circuit theory and broadcast engineering rather than from
anyone's implementation: the first-order RC high-pass of an AC coupling and the keyed
(back-porch) clamp that restores DC, both solved exactly for a signal held constant
over each sample; the 625-line/50-field timing of ITU-R BT.470-6 and BT.1700 and the
525-line/59.94-field timing of SMPTE 170M (line periods, porch and sync durations,
active lines, field rates); Rec. 601 luma weights; and the Child–Langmuir 3/2-power
law for a triode's plate current, with grid current on the positive side. The float
error bounds are the standard γₙ bounds of rounding-error analysis (Higham, *Accuracy
and Stability of Numerical Algorithms*). No manufacturer's design, coefficients or
name is used.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
