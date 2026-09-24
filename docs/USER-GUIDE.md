# Clamp user guide

Clamp is **a video amplifier whose DC restoration has failed, for [Resolume](https://resolume.com)
Arena and Avenue**, as an FFGL effect. It does not paint a drift or a flicker onto a clip. It
places your picture inside a real 625/50 or 525/59.94 television field — sync, back porch, the
picture, front porch, the blank lines — runs that whole field through one coupling capacitor in
real time, and gives you the clamp that is supposed to put black back every line, with a control
for how badly it is doing its job. Black wanders with the picture, a bright moment darkens the
next one, lines and fields tilt. None of it is drawn; it all falls out of the one circuit.

![A test card through a failing clamp: colour bars, a grey ramp whose dark end is crushed where the bars above pushed black down, and a dark ground that lightens toward the bottom of the field](hero.png)

*The repo's test card through the plugin at its defaults, rendered by the offline harness rather
than captured from Resolume: 625/50, a coupling of 6.3 ms, a clamp that has all but given up, a
little supply sag. The bright bars at the top charge the capacitor and push black down; the dark
end of the ramp is crushed, and the dark ground lightens toward the bottom of the field as the
charge bleeds away.*

> **Before you rely on this:** released at **v0.1.0**, and honestly early. The amplifier is
> measured rather than asserted, by a harness that drives the real plugin class and reads each
> claim back out of the picture it made: after a burst of white, black falls back on the RC's
> exponential with every blanking interval counted — fitted time constants of 63.998 µs,
> 20.000 ms and 0.99999 s against 64 µs, 20 ms and 1 s stated; a flat field tilts 0.555906 across
> each line at a coupling of one line, exactly as stated; black moves 0.7481 per unit of average
> picture level at a one-second coupling, against the closed form; the clamp leaves 0.980199,
> 0.818731 and 0.135335 of the error behind each porch at 0.02, 0.2 and 2 time constants, e⁻ⁿ to
> six places; every pixel matches a serial double-precision run of the same timeline to within
> 2.3 float ULPs; host frame rates from 23.976 to 144 fps all land on one continuous trajectory;
> the supply's attack and recovery fit 0.040 s and 0.200 s against 0.04 and 0.2; and sixteen
> deliberate faults are shown to make those checks fail. All 13 controls are shown to change the
> picture. It has **never been loaded into Resolume on macOS** — the one host it has run in is
> the fleet's own test host, `oxbow`, for 120 frames.
> On Windows, a build of v0.1.0 loads, registers and renders in Resolume Arena 7.27.1, with every control matching what the plugin declares — on software rendering, so that says nothing about a GPU. Sag Attack and Sag Recovery could not be shown moving there, because the gate's picture is a still whose level never changes, so the supply never sags or recovers; the harness measures both.
> Try it on a spare layer before you put it in a show.
>
> This codebase was created with AI assistance, directed and reviewed by a human author.

---

## Installing

Every download carries one effect, **SW Clamp**. Drop it into Resolume's effects folder and
restart Resolume:

```
macOS    ~/Documents/Resolume Arena/Extra Effects/
Windows  %USERPROFILE%\Documents\Resolume Arena\Extra Effects\
```

Avenue uses the same layout under its own folder name. The effect then appears in the effects
browser as **SW Clamp**.

The macOS download is a universal build (Apple silicon and Intel), as a `.dmg` or a `.zip`. It is
**Developer ID-signed and notarised**, so the bundle simply loads. The
Windows download is an x64 installer or a `.zip`. It is not code-signed, so the installer trips
SmartScreen once: **More info** → **Run anyway**.

---

## A capacitor does not pass DC

Analogue video amplifiers were joined to each other by capacitors. A capacitor passes change and
blocks anything steady — and the steadiest thing in a video signal is its **black level**. So
every stage lost it, and every stage had to put it back. The part that did that was the
**clamp**: a switch that closed for a few microseconds at the start of every line, in the **back
porch** just after sync, when the signal is known to be at black, and pulled it back to a
reference. Black was restored fifteen thousand times a second, and nobody watching ever knew.

When the clamp gets weak, or dies, nothing puts black back. The capacitor settles instead to the
**average** of whatever has passed through it recently, and everything is measured from that
average. That is the whole effect:

- **Black wanders with the picture.** A bright scene raises the average, so black is pushed
  down and the shadows crush. A dark scene lowers it, and black lifts to grey.
- **A bright moment darkens the next one.** The capacitor charged during the flash and takes time
  to let go; what follows is darker until it has, and recovers on the capacitor's own curve.
- **Lines and fields tilt.** When the capacitor's time constant is near the length of a line, a
  flat area sags from left to right across each line. Near the length of a field, it sags from
  top to bottom down the picture.
- **A healthy clamp takes all of it out**, line by line, except the little its own switch cannot
  finish in the time the porch gives it.

"Recently" is set by one control, **Coupling**, and it is real time: the capacitor runs through
every line and every blanking interval of the field, fifty or sixty fields a second, in order. The
blanking counts. For about a quarter of every field the signal is at blanking level, not picture,
and the capacitor averages that too — which is why, with a dead clamp, black moves by about three
quarters of the picture's average level rather than all of it.

Two separate things sit on top, as they did in the real equipment: the **power supply sags** when
the picture is bright and recovers when it is not, so the whole picture breathes with content;
and an optional **triode stage** clips one side hard and the other softly.

---

## Start here

Put SW Clamp **on a layer** (not on a clip), with footage that cuts between bright and dark. The
defaults are a coupling of 6.3 ms — about a third of a field — and a clamp that has all but given
up, so out of the box black already wanders and every field tilts: a bright top half darkens the
bottom half below it. A still picture settles and stays settled; **the effect shows on changing
footage**, because most of what it does is what the picture was doing a moment ago.

Why the layer: an effect on a clip belongs to that clip, so the next clip you trigger arrives with
its own copy of the effect, already settled to its own picture. On the layer, the capacitor sees
the cut, and the bright clip before it.

Then:

1. **Coupling → about 0.9, Clamp Health → 0.** A coupling of about 0.6 s and no clamp at all.
   Cut from something bright to something dark: the dark clip arrives crushed and climbs back
   over the next second or so. Cut the other way and it arrives washed out and sinks. A white
   flash leaves a dark hole behind it.
2. **Coupling → about 0.1.** A coupling of about one line. Now each line forgets its own start
   before it reaches the end: flat areas sag toward the reference from left to right, and a
   bright shape leaves a dark trail to its right.
3. **Clamp Health → 1, Clamp Reference → 0.25, Sag Depth → 0.** A working amplifier: black is
   back at black, and the picture is very nearly the clip again. Now bring Clamp Health down
   slowly. That is a failing clamp as a performance control.
4. **Per Channel on**, with the clamp weak. Red, green and blue each get their own capacitor, so
   a strongly coloured scene pushes its own colour down and black takes the opposite tint.
5. **Sag Depth up, Sag Recovery long.** The whole picture dims when it gets bright and takes its
   time to come back: a pump on every cut to white.

**Clamp Reference at 0.25, not 0.** The slider's default, halfway, is a reference of **+0.25**,
not black. That is deliberate: it is where a failed clamp's picture sits in view, with dark scenes
lifting and bright ones pushing black down, which is the look everyone remembers from a set
without DC restoration. But it also means a *healthy* clamp puts black at a quarter grey. For
black at black when the clamp is working, set Clamp Reference to **0.25 on the slider**, a
quarter of the way along, which is a reference of 0.

Every slider is declared to the host as 0 to 1, so the host only knows its position. The value
each position stands for is given with each control below.

---

## Time is real time, not frames

The raster runs on the host's clock, in seconds. A 625/50 field lasts 1/50 s and a 525/59.94
field 1001/60000 s whatever your composition's frame rate is, and every time constant here is in
seconds. So the same settings give the same look at 30 fps and at 60, and the harness checks that
directly: host rates from 23.976 to 144 fps all follow one continuous run to within 1.3e-7.

When the composition runs slower than the field rate, several fields pass between two frames. They
all run, on the one picture there is — the capacitor does not skip them. When it runs faster, some
frames fall inside a field already on show, and that field is shown again with the new picture.
Rendering the same moment twice does not move the capacitor on.

A backwards step in the host's clock, or a gap of more than half a second — a scrub, a loop, a
stall — is not believed: the raster steps on by one sixtieth of a second and carries on from
there, so it never jumps. If the host's clock stops, the raster stops with it.

The first frame after the effect loads starts from the picture's own steady state, as if it had
been running on that picture for ever, so loading it does not open with a settle that belongs to
nothing. A change of resolution changes nothing the capacitor holds.

---

## The Coupling group

The capacitor itself, and the television system it sits in.

**Coupling** — the capacitor's time constant, τ: from **20 µs to 2 s**, on a logarithmic slider
where every fifth of the way is ten times longer. Default **0.5, which is 6.3 ms**. This is how
far back the capacitor remembers, and it decides which of the looks you get:

| Slider | τ | What it looks like with the clamp dead |
| --- | --- | --- |
| 0 | 20 µs, a third of a line | each line keeps only its changes: flat areas sag to the reference within a fraction of the line, and a bright shape's right-hand edge dips dark |
| about 0.1 | about 63 µs, a line | lines tilt: a flat white line sags by more than half from left to right |
| 0.5 | 6.3 ms, a third of a field | the default: the top of the picture sets the level for the bottom |
| 0.6 | 20 ms, a 625/50 field | fields tilt top to bottom, but only gently: 1–7% on Resolume's demo clips; the field before still counts. The clearest field tilt is nearer the default |
| about 0.94 | 1 s | black breathes with the content over a second; a flash darkens what follows |
| 1 | 2 s | slow drift, a hundred fields long |

After a bright moment, the charge it left decays as e^(−t/τ): about a third is left after one τ,
about a seventh after two.

**Standard** — **625/50 (PAL)** or **525/59.94 (NTSC)**; 625/50 by default. The television field
your picture is placed in, with its real line timing: 64 µs lines, 288 picture lines and 50 fields
a second for 625/50; 63.56 µs lines, 240 picture lines and 59.94 fields a second for 525/59.94.
It changes where the blanking falls and how often the clamp gets a chance, and it changes how
many lines the capacitor runs on: 288 or 240, whatever the height of your picture. Your picture's
own detail is kept either way; only the capacitor's state is at the field's line count. Switching
Standard starts the new system's fields from that moment and keeps the capacitor's charge.

**Per Channel** — off by default. Off, the amplifier works on brightness: the capacitor follows
the picture's luma (Rec. 601) and the same offset is taken from red, green and blue, so black
wanders but colours keep their differences. On, red, green and blue are three separate amplifiers
with three capacitors, and each wanders on its own: a mostly red scene pushes red down and the
shadows go cyan. All three are always running underneath, so switching Per Channel never makes
the picture jump.

---

## The Clamp group

**Clamp Health** — how strongly the clamp pulls the signal back in each back porch, as time
constants of the clamp's own switch per porch. **0 is dead**: the switch never closes. Above
that, every fifth of the slider is ten times stronger, up to **20 time constants a porch at 1**,
which leaves e⁻²⁰ of the error behind: a perfect clamp. Default **0.2, which is 0.002 time
constants a porch** — a clamp that has all but given up.

| Slider | per porch | error left after each porch |
| --- | --- | --- |
| 0 | none | all of it: the clamp is dead |
| 0.2 | 0.002 | 99.8% |
| 0.4 | 0.02 | 98.0% |
| 0.6 | 0.2 | 81.9% |
| 0.8 | 2 | 13.5% |
| 1 | 20 | effectively none |

Even a weak clamp gets a porch on every line, fifteen thousand or so a second, so it adds up: at
the default it would put black back over about 32 ms on its own (by arithmetic, on 625/50), which
is why the default coupling, at 6.3 ms, still wins. The strength is per porch, so a setting means
the same clamp on either Standard. Between dead and healthy the clamp and the coupling compete —
the clamp pulling black back every line, the coupling pulling the average there — and this is
the slider to ride live.

**Clamp Reference** — where black is put back, from **−0.25 to +0.75**: 0 is at a quarter of the
slider, and the default, halfway, is **+0.25**. It does two jobs, because in the circuit the next
stage's input returns to the same reference as the clamp:

- **With a healthy clamp** it is simply the black level. At the default, black sits at a quarter
  grey; at 0.25 on the slider it is black. Below that, black is pushed under the floor and the
  shadows crush.
- **With a weak or dead clamp** it is the level the picture's **average** is forced to. Higher,
  and the whole picture rides up, with more room before bright scenes push black off the bottom;
  lower, and dark scenes stay dark and bright ones crush sooner.

See **Start here** for why the default is +0.25 and not 0.

---

## The Supply group

A separate mechanism from the clamp: the power supply sagging under load. The supply follows the
picture's **average brightness** — the luma of the incoming picture over its active area — and
the amplifier's gain drops as it rises. The gain multiplies everything, black level included, so a
bright picture comes out dimmer overall, not just brighter in the highlights.

**Sag Depth** — how far the gain falls at a fully white picture, from **0 to 0.5**: the gain is
1 − depth × the average. Default **0.3 on the slider, a depth of 0.15**: an all-white picture comes
out at 85%. At 0 the supply is perfect and the other two controls do nothing.

**Sag Attack** — how quickly the supply follows a picture that gets **brighter**, as a time
constant from **5 ms to 2 s** on a logarithmic slider; the middle is 0.1 s. Default **0.35, about
41 ms**: a cut to white dims within a couple of frames.

**Sag Recovery** — how quickly it comes back when the picture gets **darker**, on the same range.
Default **0.6, about 0.18 s**. Make it long for a picture that stays dimmed after every bright
moment and creeps back.

The supply moves once per field, so it runs at 50 or 59.94 steps a second whatever your frame
rate. It follows the input, not what the amplifier puts out.

---

## The Stage group

An optional valve stage after the amplifier. It is a look: its curve is the textbook shape of a
triode, not a measured valve.

**Triode On** — off by default. Bias and Drive do nothing while it is off.

**Bias** — the valve's operating point, from **−0.6 (toward cutoff) to +0.6 (toward grid
current)**; the middle, the default, is 0. The dark side of the signal runs into **cutoff**, a
hard knee: below it the valve conducts nothing and the shadows flatten to a single level. The
bright side runs into **grid current**, a soft knee: the highlights round off gradually. Bias
moves the operating point toward one knee or the other.

**Drive** — how hard the signal is pushed into the valve, from **0.5 to 4**, logarithmic; **1 at a
third of the way, the default**. The stage is normalised so that mid-grey stays mid-grey and small
changes around it keep their size, so Drive decides how far the picture reaches into the two
knees, not how bright it is. At the default the curve is gentle. Toward 4 the shadows flatten
hard onto a floor that sits well above black, and the highlights and saturated colours compress.

**Triode On lifts black; it does not crush it.** The floor is where the stage's output lands when
the valve is cut off and passes nothing, and it comes from that normalisation: mid-grey is pinned
at mid-grey with the curve's slope there made 1, so zero current comes out at mid-grey less the
current at the operating point divided by Drive times that slope — not at black. How far above
black that is depends on Drive and Bias (by arithmetic from the stage's law, not measured):

| Drive, Bias | a black input comes out at | the cutoff floor |
| --- | --- | --- |
| 1, 0 (the defaults) | about 7% grey | below black, never reached |
| 2, 0 | about 17% grey | about 17% grey: black is at cutoff |
| 4, 0 | about 33% grey | about 33% grey |
| 4, −0.6 (toward cutoff) | about 43% grey | about 43% grey |
| 4, +0.6 (toward grid current) | about 9% grey | about 9% grey |

So even at the defaults, turning the triode on raises black slightly; at high Drive, or with Bias
toward cutoff, the whole bottom of the picture sits on a flat grey. Bias toward grid current
brings the floor back down, and at low Drive with Bias toward grid current it goes below black
again, where the output's clip to black does the crushing instead.

---

## The Output group

**Mix** — the amplified picture against the untouched clip, 0 to 1; **1 by default**. Zero is the
clip as it arrived. The capacitor keeps running underneath whatever Mix says. The output always
carries the clip's own alpha.

**Show Blanking** — off by default. Instead of your picture, draws **the whole field** as the
capacitor saw it, stretched to fill the output: each line from left to right starting at sync,
then the back porch where the clamp closes, the picture, and the front porch; every line of the
field from top to bottom, with the blank lines and the final half line below the picture. Sync is
drawn at its tip, in black, so it can be seen.

**It is not a faithful picture of the signal.** It is scaled and lifted so that what happens below
black can be seen: the view shows a quarter grey plus three quarters of the signal (after the
supply's gain), so blanking level is drawn at a quarter grey and full white still reaches white,
with the range between squeezed into the top three quarters. With
Per Channel off — the default — it is also **luma only**: the whole raster, picture included, is
drawn in monochrome, because with Per Channel off the amplifier has only one channel. With Per
Channel on it is drawn in colour, one channel per capacitor.

It is a way to see the circuit working — the porch pulling back toward the reference, the charge
sagging across the blanking — and it is a look in its own right. It is a diagnostic, though, and
behaves like one: the Stage group and Mix do not apply to it, and its output is opaque.

![Show Blanking: the whole field as a raster, sync at the left, the back porch, the picture, the front porch, and the blank lines at the bottom](raster.png)

*The same frame as the hero with Show Blanking on: sync at the left edge, the porch where the
clamp closes, the active line, the front porch, and the vertical blanking below, with the half
line along the bottom, stopping halfway. Rendered by the offline harness.*

---

## How it works

Once a frame:

1. **Sum.** The GPU reads your picture along the field's picture lines — 288 or 240, each taking
   the row of your picture under its centre — and for every run of 16 samples works out how much
   those samples would add to the capacitor's charge. Your picture's width is the sample rate
   across the line. These sums are read back to the CPU.
2. **Walk.** The CPU runs the field in time order, in double precision: sync, back porch (where
   the clamp pulls at the Clamp Health rate), the picture 16 samples at a time, front porch, then
   the blank lines and the half line. Every step is the capacitor's exact response to what it was
   given, not an approximation of it.
3. **Schedule.** From the host's clock it works out which field is under way. Every field that
   began since the last frame is run on this picture; the last one is the one shown. The supply
   steps once per field.
4. **Fill.** The GPU works out the capacitor's charge at every sample of the field on show, from
   the charge at the start of each run of 16.
5. **Display.** Each pixel of your picture, minus the charge on its line at its position, plus
   the Clamp Reference, times the supply's gain; then the triode if it is on, then clipped to
   black and white, then Mix. Or, with Show Blanking, the whole field instead.

The only thing carried from one frame to the next is the capacitor's charge in each channel and
the supply's state — a handful of numbers on the CPU. Nothing on the GPU outlives a frame.

---

## Performance

Measured by the offline harness on an M4 Max at the defaults, best of three runs of 60 frames
after a warm-up, on a GPU shared with other work (single runs moved by up to 2×):

| | ms/frame | % of a 60 fps frame | Per Channel + Triode |
| --- | --- | --- | --- |
| 1280×720 | 0.36 | 2.2% | 0.43 |
| 1920×1080 | 0.41 | 2.4% | 0.41 |
| 3840×2160 | 0.82 | 4.9% | 0.77 |

**It is cheap, and the settings barely change it.** Most of the cost is the one read back from the
GPU every frame, which makes the CPU wait for the sums; that is by design, because the walk needs
them. The capacitor runs on 288 or 240 lines whatever the height of the picture. GPU memory, by
arithmetic rather than measurement, is about 10 MB at 1080p and 20 MB at 4K on 625/50. Nothing was
timed inside Resolume, and nothing was timed on Windows.

---

## If it looks wrong

**Nothing seems to happen, or it is just a bit darker.** A still picture settles and holds. Give
it footage that changes, cut between bright and dark, and turn Clamp Health down to 0. Check Mix.

**Black is grey, even with the clamp healthy.** That is the default Clamp Reference of +0.25. Set
it to 0.25 on the slider for black at black.

**The shadows crush after anything bright.** That is the effect: the bright picture pushed black
down. Raise Clamp Health, raise Clamp Reference, or shorten Coupling so the capacitor forgets
sooner.

**The whole picture dims on bright scenes and comes back slowly.** Sag Depth, and a long Sag
Recovery.

**Black has a colour cast.** Per Channel is on.

**Bias and Drive do nothing.** Triode On is off.

**Black is lifted to grey as soon as Triode On is on.** That is the stage's floor: it is
normalised about mid-grey, so a signal at black comes out above black — slightly at the defaults,
a third grey at Drive 4. See the Stage group.

**Black is a flat grey and the shadows have no detail.** Triode On with Drive high, or Bias toward
cutoff: the dark side is at cutoff, on the floor. Lower Drive, or move Bias toward grid current.

**Show Blanking is black and white, washed out, and Mix does nothing to it.** All by design. With
Per Channel off the view is luma only; turn Per Channel on to see it in colour. It is always lifted
by a quarter so blanking and anything below black show, so its levels are not your picture's.
The Stage group and Mix do not apply to it. See Show Blanking.

**Fine horizontal steps at a very short Coupling.** The capacitor runs on 288 lines (240 on
525/59.94), so on a 1080-row picture each of its lines covers three or four rows. At a coupling
near a line, where every line differs, that can show.

**The picture jumped when I scrubbed or looped.** It should not: a jump in the host's clock steps
the raster on by one sixtieth of a second. If the clock stops, the raster stops, and the picture
holds its level.

**SW Clamp is not in the effects browser.** Check the folder under Installing, and that Resolume
was restarted.

**The effect does nothing at all.** A shader that will not compile looks exactly like that, and
the real message is in the log:

```
macOS    ~/Library/Logs/clamp/clamp.YYYY-MM-DD.log
Windows  %LOCALAPPDATA%\clamp\logs\clamp.YYYY-MM-DD.log
```

It records the build that was loaded, the GL vendor, renderer and version at load, which shader
failed if one did, a buffer that could not be allocated, and at frame 60 the host's clock and the
unit the plugin decided it is in.

---

## Known limits

- **Never loaded into Resolume on macOS**, and nothing has driven the controls in a host. How the
  thirteen controls read in the inspector, and what Resolume's clock does to the raster over a
  long session, is untested.
- **Never seen on footage.** Every picture so far is a synthetic card.
- **The triode stage and Show Blanking are looks**, checked only for being alive, not measured.
- **The fields are not interlaced.** Each field covers the whole picture on 288 or 240 lines,
  top to bottom, with the real field period. It only matters at a coupling of about a line.
- **Sync sits at blanking level** in the circuit, not at its tip, so it does not pull the average
  down the little a real sync pulse would. It is drawn at its tip only in Show Blanking.
- **The capacitor is driven by 288 or 240 rows of your picture**, not all of them. Your picture's
  detail is kept; the charge is at the field's line count.
- **Below the field rate, the fields no frame shows run on the picture of the frame after
  them**, up to a frame early.
- **The supply follows the input's average**, not what the amplifier draws.
- **Checked at up to 1920×1080**, and only timed at 4K.
- **Only ever run on an Apple M4 Max**, although the macOS build contains an Intel slice. On
  Windows, see the note at the top of this guide.
- **No presets, no audio input**, no OpenFX version and no browser demo.

---

## About

The last group, **About**, carries the plugin's name, version, licence and maker, and buttons
that open this user guide ([stoatworks-labs.com/software/clamp/guide/](https://stoatworks-labs.com/software/clamp/guide/)),
the project page, the source on GitHub and the support page in your browser.

## Reporting something

[github.com/stoatworks-labs/clamp/issues](https://github.com/stoatworks-labs/clamp/issues).
A screenshot, the Coupling, Clamp Health, Clamp Reference and Standard settings, and the
composition's resolution and frame rate are usually enough. If the effect did nothing, attach the
log.
