# Attributions

Clamp is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Harness shape, --pipe contract and verify — Stoatworks standards

<https://github.com/stoatworks-labs/standards>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract, the negative-control pattern, --offline, check-shaders.sh, the verify script and the host clock (with readout's clock-unit voting) are standards's, by way of pitch, rebate and slowscan.

### A serial recurrence on the GPU — Stoatworks compander

<https://github.com/stoatworks-labs/compander>  
Licence: MIT  
Copyright: Stoatworks Labs

The idea of computing a serial recurrence on the GPU in parallel and holding it to the serial one is compander's.

### Broadcast numbers in one header — Stoatworks old-cathode

<https://github.com/stoatworks-labs/old-cathode>  
Licence: MIT  
Copyright: Stoatworks Labs

Keeping the broadcast standards' numbers in one header is old-cathode's.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The off-screen buffer wrapper is tinsel's, by way of standards.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### AC-coupled video amplifiers and the keyed clamp

The first-order RC high-pass of an AC coupling and the keyed back-porch clamp that restores DC, both solved exactly for a signal held constant over each sample; Rec. 601 luma weights; the Child-Langmuir 3/2-power law for a triode's plate current. Built from circuit theory; no manufacturer's design, coefficients or name is used.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R BT.470-6 and BT.1700; SMPTE 170M** — The 625-line/50-field and 525-line/59.94-field timing: line periods, porch and sync durations, active lines and field rates.
- **Nicholas J. Higham, Accuracy and Stability of Numerical Algorithms** — The standard gamma-n rounding-error bounds the harness's float tolerances are derived from.
- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — The pcg_hash output mix the harness uses for its noise frames, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
