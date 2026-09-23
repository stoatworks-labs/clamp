/**
 * Clamp — browser demo.
 *
 * An AC-coupled video amplifier whose DC restoration has failed. The one idea,
 * from `source/Clamp.h`: a capacitor does not pass DC, and a picture's black
 * level is DC. A clamp used to put it back in every back porch; with the clamp
 * weak or dead, the coupling is a high-pass filter run through the whole raster
 * in time order, blanking included — so black wanders with the picture, a
 * bright moment darkens the next one, and lines and fields tilt.
 *
 * Like galvo, this plugin is **not only a shader**, and the two halves of the
 * page are not equally faithful:
 *
 *   The shaders are the plugin's. `VERTEX`, `SUMS`, `FILL` and `DISPLAY` below
 *   are `kVertex`, `kSums`, `kFill` and `kDisplay` from `source/Shaders.cpp`,
 *   copied across unedited. `demo/tools/check_shaders.py` compares them
 *   character for character and `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT — of `Controls.cpp`, `Model.cpp` (the timeline, the
 *   field walk, the supply and the triode constants), `Clock.cpp`, and the
 *   state carried across host frames in `Clamp::ProcessOpenGL` — function for
 *   function. Nothing checks a port but a reader. `cltest --model`,
 *   `--droop`, `--continuity` and the rest check the C++ originals and have
 *   no idea this page exists.
 *
 * ------------------------------------------------------ the readback
 *
 * The plugin reads the block sums back with `glReadPixels( …, GL_FLOAT )` every
 * frame, walks the field on the CPU in double, and uploads the state at every
 * block start as a float texture. The page does exactly that: one `readPixels`
 * of an RGBA32F target as FLOAT, which WebGL2 allows once
 * EXT_color_buffer_float is present, so there is no RGBA8 compromise here of
 * the kind galvo needed. It stalls the pipeline where the plugin's does. The
 * walk runs in JavaScript numbers, which are IEEE doubles, as the C++ does.
 *
 * ------------------------------------------------------- the clock
 *
 * The raster runs in real time. The page's clock is the kit's `time` — seconds
 * since the page started, paused by Pause and stepped by Step — and it is fed
 * to the ported `Clock` with the unit DECLARED as seconds, the way `cltest`
 * declares its own. The unit vote the plugin runs against Resolume's
 * millisecond clock therefore never runs here. Restart sends the clock
 * backwards, which the port treats as the plugin treats a scrub: one nominal
 * frame on, and the capacitor keeps its charge.
 *
 * ------------------------------------------------------- what is missing
 *
 * **Nothing audio.** Clamp has no audio path, so there is no caveat to make.
 * **The About block is absent**, as on every page in this suite.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const SUMS = `#version 410 core

uniform sampler2D InputTexture;
uniform int InHeight;       //the picture's rows
uniform int Width;          //samples per line: the picture's columns
uniform int Lines;          //active lines per field
uniform float WeightP[ 16 ];//a^( 15 - j ), from the CPU in double

out vec4 fragColor;

const vec3 kLuma = vec3( 0.299, 0.587, 0.114 );

void main()
{
	int b = int( gl_FragCoord.x );
	int l = int( gl_FragCoord.y );

	int r     = ( ( 2 * l + 1 ) * InHeight ) / ( 2 * Lines );
	int row   = InHeight - 1 - r;
	int first = b * 16;
	int count = min( 16, Width - first );
	int skip  = 16 - count;

	vec3 p     = vec3( 0.0 );
	float luma = 0.0;
	for( int j = 0; j < count; ++j )
	{
		vec3 x = texelFetch( InputTexture, ivec2( first + j, row ), 0 ).rgb;
		p     += WeightP[ j + skip ] * x;
		luma  += dot( x, kLuma );
	}
	fragColor = vec4( p, luma );
}
`;

const FILL = `#version 410 core

uniform sampler2D InputTexture;
uniform sampler2D States;   //blocks x Lines: the state at each block's first sample
uniform int InHeight;
uniform int Lines;
uniform float PowA[ 16 ];   //a^j
uniform float InputW[ 16 ]; //( 1 - a ) a^i

out vec4 fragColor;

void main()
{
	int c = int( gl_FragCoord.x );
	int l = int( gl_FragCoord.y );

	int r     = ( ( 2 * l + 1 ) * InHeight ) / ( 2 * Lines );
	int row   = InHeight - 1 - r;
	int b     = c / 16;
	int first = b * 16;
	int j     = c - first;

	vec3 s0  = texelFetch( States, ivec2( b, l ), 0 ).rgb;
	vec3 sum = vec3( 0.0 );
	for( int i = 0; i < j; ++i )
		sum += InputW[ j - 1 - i ] * texelFetch( InputTexture, ivec2( first + i, row ), 0 ).rgb;
	fragColor = vec4( PowA[ j ] * s0 + sum, 0.0 );
}
`;

const DISPLAY = `#version 410 core

uniform sampler2D InputTexture;
uniform sampler2D Fill;     //Width x Lines
uniform sampler2D Edges;    //4 x Rows: the state at sync, porch, active and front porch starts
uniform int InHeight;
uniform int Width;
uniform int Lines;
uniform int VpX;
uniform int VpY;
uniform int VpW;
uniform int VpH;

uniform int PerChannel;
uniform float Gain;         //the supply, for the field on show

uniform int TriodeOn;
uniform float Drive;
uniform float Bias;
uniform float PlateAtBias;
uniform float SlopeAtBias;

uniform float MixAmount;

uniform int ShowBlanking;
uniform int Rows;           //active + blank lines + the half line
uniform float LineT;        //seconds
uniform float SyncT;
uniform float PorchT;
uniform float ActiveT;
uniform float SampleT;
uniform float Tau;
uniform float PorchK;       //the porch's rate, 1 / tau + 1 / tau_c
uniform float Reference;    //the Clamp Reference, added back: y = x - u + r

in vec2 uv;
out vec4 fragColor;

const vec3 kLuma = vec3( 0.299, 0.587, 0.114 );

vec3 stateOf( vec3 s )
{
	return PerChannel == 1 ? s : vec3( dot( s, kLuma ) );
}

float plate( float u )
{
	if( u <= -1.0 )
		return 0.0;
	float g = u > 0.0 ? u / ( 1.0 + 0.5 * u ) : u;
	return pow( 1.0 + g, 1.5 );
}

vec3 triode( vec3 v )
{
	vec3 u = Drive * ( v - 0.5 ) + Bias;
	vec3 p = vec3( plate( u.r ), plate( u.g ), plate( u.b ) );
	return 0.5 + ( p - PlateAtBias ) / ( Drive * SlopeAtBias );
}

vec4 pictureAt( int c, int rr )
{
	return texelFetch( InputTexture, ivec2( c, InHeight - 1 - rr ), 0 );
}

vec3 raster( int X, int Y )
{
	int row  = ( Y * Rows ) / VpH;
	float t  = ( float( X ) + 0.5 ) / float( VpW ) * LineT;
	vec3 e0  = texelFetch( Edges, ivec2( 0, row ), 0 ).rgb;

	if( row == Rows - 1 )
	{
		//The half line: blanking, then nothing.
		if( t >= 0.5 * LineT )
			return vec3( 0.0 );
		return 0.25 + 0.75 * Gain * ( Reference - stateOf( e0 * exp( -t / Tau ) ) );
	}
	if( t < SyncT )
		return vec3( 0.0 );

	float tPorch  = SyncT;
	float tActive = SyncT + PorchT;
	float tFront  = tActive + ActiveT;
	vec3 y;
	if( t < tActive )
	{
		vec3 e1 = texelFetch( Edges, ivec2( 1, row ), 0 ).rgb;
		y = Reference - e1 * exp( -PorchK * ( t - tPorch ) );
	}
	else if( t < tFront )
	{
		if( row < Lines )
		{
			int c  = clamp( int( ( t - tActive ) / SampleT ), 0, Width - 1 );
			int rr = ( ( 2 * row + 1 ) * InHeight ) / ( 2 * Lines );
			y = pictureAt( c, rr ).rgb - texelFetch( Fill, ivec2( c, row ), 0 ).rgb + Reference;
		}
		else
		{
			vec3 e2 = texelFetch( Edges, ivec2( 2, row ), 0 ).rgb;
			y = Reference - e2 * exp( -( t - tActive ) / Tau );
		}
	}
	else
	{
		vec3 e3 = texelFetch( Edges, ivec2( 3, row ), 0 ).rgb;
		y = Reference - e3 * exp( -( t - tFront ) / Tau );
	}
	if( PerChannel == 0 )
		y = vec3( dot( y, kLuma ) );
	return 0.25 + 0.75 * Gain * y;
}

void main()
{
	int X = int( gl_FragCoord.x ) - VpX;
	int Y = VpH - 1 - ( int( gl_FragCoord.y ) - VpY );

	if( ShowBlanking == 1 )
	{
		fragColor = vec4( clamp( raster( X, Y ), 0.0, 1.0 ), 1.0 );
		return;
	}

	int c  = clamp( ( ( 2 * X + 1 ) * Width ) / ( 2 * VpW ), 0, Width - 1 );
	int rr = clamp( ( ( 2 * Y + 1 ) * InHeight ) / ( 2 * VpH ), 0, InHeight - 1 );
	int l  = ( ( 2 * rr + 1 ) * Lines ) / ( 2 * InHeight );

	vec4 x = pictureAt( c, rr );
	vec3 s = stateOf( texelFetch( Fill, ivec2( c, l ), 0 ).rgb );
	vec3 v = Gain * ( x.rgb - s + Reference );
	if( TriodeOn == 1 )
		v = triode( v );
	vec4 amplified = vec4( clamp( v, 0.0, 1.0 ), x.a );

	if( MixAmount >= 1.0 )
	{
		fragColor = amplified;
		return;
	}
	fragColor = mix( x, amplified, MixAmount );
}
`;

//===========================================================================
// Controls.cpp, ported. Every conversion from the host's 0..1 to a physical
// unit lives there and nowhere else, so it lives here and nowhere else too.
//===========================================================================

const unit = (v) => Math.min(1, Math.max(0, v));

const optionIndex = (value, count) => Math.min(count - 1, Math.max(0, Math.round(value)));

/// tau = 10^( -4.7 + 5 v ) s, 20 us to 2 s.
const couplingSeconds = (v) => Math.pow(10, -4.7 + 5 * unit(v));

/// n = 20 x 10^( -5 ( 1 - v ) ) time constants per porch, exactly 0 at 0.
const clampPerPorch = (v) => {
  const u = unit(v);
  return u <= 0 ? 0 : 20 * Math.pow(10, -5 * (1 - u));
};
const clampRate = (v, standard) => clampPerPorch(v) / standard.backPorch;

/// -0.25 .. +0.75.
const referenceLevel = (v) => unit(v) - 0.25;

/// 0 .. 0.5.
const sagDepth = (v) => 0.5 * unit(v);

/// 10^( -2.3 + 2.6 v ) s, 5 ms to 2 s.
const sagSeconds = (v) => Math.pow(10, -2.3 + 2.6 * unit(v));

/// -0.6 .. +0.6, zero at the middle.
const triodeBias = (v) => (unit(v) - 0.5) * 1.2;

/// 10^( -0.3 + 0.9 v ), 0.5 to 4; 1 at a third.
const triodeDrive = (v) => Math.pow(10, -0.3 + 0.9 * unit(v));

//===========================================================================
// Model.cpp, ported. Everything is in u = s + r, the state with the Clamp
// Reference taken out; the display adds r back.
//===========================================================================

/// ITU-R BT.470-6 / BT.1700 and SMPTE 170M, as Model.cpp tabulates them.
const STANDARDS = [
  { name: '625/50 (PAL)', line: 64.0e-6, frontPorch: 1.65e-6, sync: 4.7e-6, backPorch: 5.7e-6, activeLines: 288, fieldLines: 312.5, rateNum: 50, rateDen: 1 },
  { name: '525/59.94 (NTSC)', line: 1001.0 / 15750000.0, frontPorch: 1.5e-6, sync: 4.7e-6, backPorch: 4.7e-6, activeLines: 240, fieldLines: 262.5, rateNum: 60000, rateDen: 1001 },
];
const K_STANDARD_COUNT = 2;

const active = (s) => s.line - s.frontPorch - s.sync - s.backPorch;
const fieldSeconds = (s) => s.rateDen / s.rateNum;
const blankLines = (s) => Math.floor(s.fieldLines - s.activeLines);
const tail = (s) => (s.fieldLines - s.activeLines - blankLines(s)) * s.line;
const standardOf = (index) => STANDARDS[Math.min(K_STANDARD_COUNT - 1, Math.max(0, index))];

/// Samples per block: the GPU's partial sums and the within-block fill.
const K_BLOCK = 16;

const decay = (seconds, tau) => Math.exp(-seconds / tau);

function makeTimeline(standard, width, settings) {
  const tau = settings.tau;
  const t = {};

  t.width = Math.max(1, width);
  t.blocks = Math.floor((t.width + K_BLOCK - 1) / K_BLOCK);
  t.activeLines = standard.activeLines;
  t.blankLines = blankLines(standard);
  t.rows = t.activeLines + t.blankLines + 1;

  // The negative-control perturbations are harness-only and always 0 in the
  // plugin, so they are not carried here.
  const sync = standard.sync;
  const porch = standard.backPorch;
  const front = standard.frontPorch;
  const act = active(standard);
  const tl = tail(standard);

  t.sample = act / t.width;
  t.a = Math.exp(-t.sample / tau);
  t.oneMinusA = -Math.expm1(-t.sample / tau);

  // Timeline::BlockCount( blocks - 1 ): the last block may be short.
  const lastCount = t.width - (t.blocks - 1) * K_BLOCK;
  t.blockDecay = Math.pow(t.a, K_BLOCK);
  t.lastDecay = Math.pow(t.a, lastCount);

  t.decaySync = decay(sync, tau);
  t.decayFrontPorch = decay(front, tau);
  t.decayActiveBlank = decay(act, tau);
  t.decayTail = decay(tl, tau);

  // The porch: x = 0, the switch closed. u relaxes toward blanking at the rate
  // k, which puts y at r.
  const k = 1.0 / tau + settings.clampRate;
  t.porchAlpha = Math.exp(-k * porch);
  t.decayRestOfPorch = decay(porch - porch, tau);

  t.weightP = new Float64Array(K_BLOCK);
  t.powA = new Float64Array(K_BLOCK);
  t.inputW = new Float64Array(K_BLOCK);
  for (let j = 0; j < K_BLOCK; j += 1) {
    t.weightP[j] = Math.pow(t.a, K_BLOCK - 1 - j);
    t.powA[j] = Math.pow(t.a, j);
    t.inputW[j] = t.oneMinusA * Math.pow(t.a, j);
  }
  return t;
}

/// One field's walk from u = 0, recorded so any start state can be applied
/// afterwards: every recorded state is A_k s_start + B_k.
function walkField(t, sums, walk) {
  const blocks = t.blocks;
  const nBlocks = t.activeLines * blocks;
  if (!walk.blockA || walk.blockA.length !== nBlocks) {
    walk.blockA = new Float64Array(nBlocks);
    walk.blockB = new Float64Array(nBlocks * 3);
  }
  if (!walk.edgeA || walk.edgeA.length !== t.rows * 4) {
    walk.edgeA = new Float64Array(t.rows * 4);
    walk.edgeB = new Float64Array(t.rows * 4 * 3);
  }

  let A = 1.0;
  let B0 = 0.0;
  let B1 = 0.0;
  let B2 = 0.0;
  let lumaTotal = 0.0;

  const step = (d, e0, e1, e2) => {
    A *= d;
    B0 = d * B0 + e0;
    B1 = d * B1 + e1;
    B2 = d * B2 + e2;
  };
  const edge = (row, k) => {
    const i = row * 4 + k;
    walk.edgeA[i] = A;
    walk.edgeB[i * 3] = B0;
    walk.edgeB[i * 3 + 1] = B1;
    walk.edgeB[i * 3 + 2] = B2;
  };
  const porch = () => {
    step(t.porchAlpha, 0, 0, 0);
    if (t.decayRestOfPorch !== 1.0) step(t.decayRestOfPorch, 0, 0, 0);
  };

  for (let row = 0; row < t.rows; row += 1) {
    const activeRow = row < t.activeLines;
    const tailRow = row === t.rows - 1;
    if (tailRow) {
      // The half line: blanking, no sync worth keying a clamp on.
      for (let k = 0; k < 4; k += 1) edge(row, k);
      step(t.decayTail, 0, 0, 0);
      break;
    }

    edge(row, 0);
    step(t.decaySync, 0, 0, 0);
    edge(row, 1);
    porch();
    edge(row, 2);
    if (activeRow) {
      for (let b = 0; b < blocks; b += 1) {
        const i = row * blocks + b;
        walk.blockA[i] = A;
        walk.blockB[i * 3] = B0;
        walk.blockB[i * 3 + 1] = B1;
        walk.blockB[i * 3 + 2] = B2;
        const p = i * 4;
        const d = b === blocks - 1 ? t.lastDecay : t.blockDecay;
        step(d, t.oneMinusA * sums[p], t.oneMinusA * sums[p + 1], t.oneMinusA * sums[p + 2]);
        lumaTotal += sums[p + 3];
      }
    } else {
      step(t.decayActiveBlank, 0, 0, 0);
    }
    edge(row, 3);
    step(t.decayFrontPorch, 0, 0, 0);
  }

  walk.A = A;
  walk.B = [B0, B1, B2];
  walk.apl = lumaTotal / (t.width * t.activeLines);
}

const advance = (w, channel, start) => w.A * start + w.B[channel];
const steadyState = (w, channel) => w.B[channel] / (1.0 - w.A);

function sagStep(e, apl, seconds, sag) {
  const rising = apl > e;
  const tau = rising ? sag.attack : sag.recovery;
  return apl + (e - apl) * Math.exp(-seconds / tau);
}

const sagGain = (e, sag) => 1.0 - sag.depth * e;

function triodePlate(u) {
  if (u <= -1.0) return 0.0;
  const g = u > 0.0 ? u / (1.0 + 0.5 * u) : u;
  return Math.pow(1.0 + g, 1.5);
}

function makeTriode(drive, bias) {
  const t = { drive, bias, plateAtBias: triodePlate(bias), slopeAtBias: 1.5 };
  if (bias > 0.0) {
    const g = bias / (1.0 + 0.5 * bias);
    const dg = 1.0 / ((1.0 + 0.5 * bias) * (1.0 + 0.5 * bias));
    t.slopeAtBias = 1.5 * Math.sqrt(1.0 + g) * dg;
  } else {
    t.slopeAtBias = 1.5 * Math.sqrt(Math.max(0.0, 1.0 + bias));
  }
  return t;
}

/// A host frame within 1e-6 of a field's start counts as at it.
const K_FIELD_SLACK = 1e-6;
const fieldAt = (seconds, standard) => Math.floor(seconds / fieldSeconds(standard) + K_FIELD_SLACK);

//===========================================================================
// Clock.cpp, ported, with the unit declared as seconds (SetScaleForTest(1),
// as cltest does). A delta is believed unless it is backwards or longer than
// half a second, in which case the clock steps on by one nominal frame.
//===========================================================================

class Clock {
  constructor() {
    this.reset();
  }

  reset() {
    this.started = false;
    this.lastScaled = -1;
    this.anchor = 0;
    this.offset = 0;
    this.now = 0;
    this.jumped = false;
  }

  update(scaled) {
    this.jumped = false;
    if (!this.started) {
      this.started = true;
      this.anchor = scaled;
      this.offset = 0;
      this.now = 0;
      this.lastScaled = scaled;
      return;
    }
    const delta = scaled - this.lastScaled;
    if (delta < 0 || delta > Clock.kMaxFrameSeconds) {
      this.jumped = true;
      this.offset = this.now + Clock.kNominalFrameSeconds;
      this.anchor = scaled;
      this.now = this.offset;
    } else {
      this.now = this.offset + (scaled - this.anchor);
    }
    this.lastScaled = scaled;
  }
}
Clock.kMaxFrameSeconds = 0.5;
Clock.kNominalFrameSeconds = 1.0 / 60.0;

//===========================================================================
// The renderer: Clamp::ProcessOpenGL, in its order.
//
//   1. sums      blocks x lines, RGBA32F, read back with readPixels as FLOAT
//   2. walk      the field map, in double, on the CPU
//   3. timeline  every field since the last host frame, the carried state
//   4. states    the state at every block start and blanking edge, uploaded
//   5. fill      width x lines, RGBA32F: the state at every sample
//   6. display   onto the canvas
//===========================================================================

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = { field: -1, standard: '', apl: 0, gain: 1, black: 0, walkMillis: 0 };

function createFloatTexture(gl) {
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return { texture, width: 0, height: 0 };
}

function uploadTexture(gl, target, width, height, data) {
  gl.bindTexture(gl.TEXTURE_2D, target.texture);
  if (target.width !== width || target.height !== height) {
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, width, height, 0, gl.RGBA, gl.FLOAT, data);
    target.width = width;
    target.height = height;
  } else {
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.FLOAT, data);
  }
  gl.bindTexture(gl.TEXTURE_2D, null);
}

const toFloat32 = (values) => Float32Array.from(values);

function createRenderer(gl, quad) {
  const sumsShader = new Program(gl, VERTEX, SUMS, 'sums');
  const fillShader = new Program(gl, VERTEX, FILL, 'fill');
  const displayShader = new Program(gl, VERTEX, DISPLAY, 'display');

  const sums = new PassBuffer(gl, { filter: 'nearest' });
  const fill = new PassBuffer(gl, { filter: 'nearest' });
  const statesTexture = createFloatTexture(gl);
  const edgesTexture = createFloatTexture(gl);

  let readback = new Float32Array(0);
  let states = new Float32Array(0);
  let edges = new Float32Array(0);
  const walk = {};
  const clock = new Clock();

  // The amplifier's state, carried across host frames. All CPU, all double.
  let primed = false;
  let standardIndex = -1;
  let fieldOrigin = 0;
  let nextField = 0;
  const nextState = [0, 0, 0];
  let nextSupply = 0;
  let shownField = -1;
  const shownState = [0, 0, 0];
  let shownSupply = 0;

  return {
    render({ input, params, width: vpW, height: vpH, time }) {
      clock.update(time);
      const now = clock.now;
      const p = (id) => params.get(id);

      const picture = input;
      const width = picture.width;
      const height = picture.height;

      //------------------------------------------------------------------
      // The settings, in physical units.
      //------------------------------------------------------------------
      const stdIndex = optionIndex(p('standard'), K_STANDARD_COUNT);
      const standard = standardOf(stdIndex);
      const settings = {
        tau: couplingSeconds(p('coupling')),
        clampRate: clampRate(p('health'), standard),
      };
      const reference = referenceLevel(p('reference'));
      const sag = {
        depth: sagDepth(p('sagDepth')),
        attack: sagSeconds(p('sagAttack')),
        recovery: sagSeconds(p('sagRecovery')),
      };
      const perChannel = p('perChannel') >= 0.5;
      const triodeOn = p('triode') >= 0.5;
      const showRaster = p('showBlanking') >= 0.5;
      const triode = makeTriode(triodeDrive(p('drive')), triodeBias(p('bias')));

      // A change of Standard re-bases the field count. The capacitor is the
      // same capacitor, so its charge carries.
      if (stdIndex !== standardIndex) {
        standardIndex = stdIndex;
        fieldOrigin = now;
        nextField = 0;
        shownField = -1;
      }

      //------------------------------------------------------------------
      // Buffers.
      //------------------------------------------------------------------
      const lines = standard.activeLines;
      const blocks = Math.floor((width + K_BLOCK - 1) / K_BLOCK);
      sums.ensure(blocks, lines, gl.RGBA32F);
      fill.ensure(width, lines, gl.RGBA32F);
      const timeline = makeTimeline(standard, width, settings);

      //------------------------------------------------------------------
      // 1. The block sums, read back.
      //------------------------------------------------------------------
      const started = performance.now();
      sums.bind();
      gl.disable(gl.BLEND);
      sumsShader.use();
      bindTexture(gl, 0, picture.texture);
      sumsShader.setSampler('InputTexture', 0);
      sumsShader.setInt('InHeight', height);
      sumsShader.setInt('Width', width);
      sumsShader.setInt('Lines', lines);
      sumsShader.setArray('WeightP', toFloat32(timeline.weightP));
      quad.draw();

      if (readback.length !== blocks * lines * 4) readback = new Float32Array(blocks * lines * 4);
      gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
      gl.readPixels(0, 0, blocks, lines, gl.RGBA, gl.FLOAT, readback);

      //------------------------------------------------------------------
      // 2. The walk: this picture's field map, in double.
      //------------------------------------------------------------------
      walkField(timeline, readback, walk);

      const fieldT = fieldSeconds(standard);
      const field = fieldAt(now - fieldOrigin, standard);

      if (!primed) {
        // The first frame starts at this picture's periodic steady state, so
        // a clip does not open with a settle that belongs to nothing.
        for (let c = 0; c < 3; c += 1) nextState[c] = steadyState(walk, c);
        nextSupply = walk.apl;
        nextField = field;
        primed = true;
      }

      //------------------------------------------------------------------
      // 3. The timeline. Every field that started since the last host frame
      //    is run, with this picture. A host frame with no new field shows
      //    the same field again, re-scanned with this picture.
      //------------------------------------------------------------------
      if (field >= nextField) {
        const unseen = field - nextField;
        if (unseen > 100000) {
          for (let c = 0; c < 3; c += 1) nextState[c] = steadyState(walk, c);
          nextSupply = walk.apl;
        } else {
          for (let k = 0; k < unseen; k += 1) {
            for (let c = 0; c < 3; c += 1) nextState[c] = advance(walk, c, nextState[c]);
            nextSupply = sagStep(nextSupply, walk.apl, fieldT, sag);
          }
        }

        shownField = field;
        for (let c = 0; c < 3; c += 1) {
          shownState[c] = nextState[c];
          nextState[c] = advance(walk, c, shownState[c]);
        }
        shownSupply = nextSupply;
        nextSupply = sagStep(shownSupply, walk.apl, fieldT, sag);
        nextField = field + 1;
      }

      //------------------------------------------------------------------
      // 4. The field on show: the state at every block start, and at every
      //    blanking edge for Show Blanking.
      //------------------------------------------------------------------
      if (states.length !== blocks * lines * 4) states = new Float32Array(blocks * lines * 4);
      states.fill(0);
      for (let i = 0; i < walk.blockA.length; i += 1) {
        for (let c = 0; c < 3; c += 1) states[i * 4 + c] = walk.blockA[i] * shownState[c] + walk.blockB[i * 3 + c];
      }
      if (edges.length !== timeline.rows * 4 * 4) edges = new Float32Array(timeline.rows * 4 * 4);
      edges.fill(0);
      for (let i = 0; i < walk.edgeA.length; i += 1) {
        for (let c = 0; c < 3; c += 1) edges[i * 4 + c] = walk.edgeA[i] * shownState[c] + walk.edgeB[i * 3 + c];
      }
      uploadTexture(gl, statesTexture, blocks, lines, states);
      uploadTexture(gl, edgesTexture, 4, timeline.rows, edges);

      telemetry.walkMillis = performance.now() - started;
      telemetry.field = shownField;
      telemetry.standard = standard.name;
      telemetry.apl = walk.apl;
      telemetry.gain = sagGain(shownSupply, sag);
      // What black (x = 0) comes out as at the first active sample of the
      // middle line, before the gain: y = 0 - u + r, in luma.
      const middle = (Math.floor(lines / 2) * blocks) * 4;
      telemetry.black = reference - (0.299 * states[middle] + 0.587 * states[middle + 1] + 0.114 * states[middle + 2]);

      //------------------------------------------------------------------
      // 5. Every sample's state.
      //------------------------------------------------------------------
      fill.bind();
      fillShader.use();
      bindTexture(gl, 0, picture.texture);
      bindTexture(gl, 1, statesTexture.texture);
      fillShader.setSampler('InputTexture', 0);
      fillShader.setSampler('States', 1);
      fillShader.setInt('InHeight', height);
      fillShader.setInt('Lines', lines);
      fillShader.setArray('PowA', toFloat32(timeline.powA));
      fillShader.setArray('InputW', toFloat32(timeline.inputW));
      quad.draw();

      //------------------------------------------------------------------
      // 6. Display, onto the canvas. The host's viewport is the whole canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);

      displayShader.use();
      bindTexture(gl, 0, picture.texture);
      bindTexture(gl, 1, fill.texture);
      bindTexture(gl, 2, edgesTexture.texture);
      displayShader.setSampler('InputTexture', 0);
      displayShader.setSampler('Fill', 1);
      displayShader.setSampler('Edges', 2);
      displayShader.setInt('InHeight', height);
      displayShader.setInt('Width', width);
      displayShader.setInt('Lines', lines);
      displayShader.setInt('VpX', 0);
      displayShader.setInt('VpY', 0);
      displayShader.setInt('VpW', vpW);
      displayShader.setInt('VpH', vpH);
      displayShader.setInt('PerChannel', perChannel ? 1 : 0);
      displayShader.set('Gain', sagGain(shownSupply, sag));
      displayShader.setInt('TriodeOn', triodeOn ? 1 : 0);
      displayShader.set('Drive', triode.drive);
      displayShader.set('Bias', triode.bias);
      displayShader.set('PlateAtBias', triode.plateAtBias);
      displayShader.set('SlopeAtBias', triode.slopeAtBias);
      displayShader.set('MixAmount', p('mix'));
      displayShader.setInt('ShowBlanking', showRaster ? 1 : 0);
      displayShader.setInt('Rows', timeline.rows);
      displayShader.set('LineT', standard.line);
      displayShader.set('SyncT', standard.sync);
      displayShader.set('PorchT', standard.backPorch);
      displayShader.set('ActiveT', active(standard));
      displayShader.set('SampleT', timeline.sample);
      displayShader.set('Tau', settings.tau);
      displayShader.set('PorchK', 1.0 / settings.tau + settings.clampRate);
      displayShader.set('Reference', reference);
      quad.draw();

      // Textures bound to units 1 and 2 are left alone by the kit's source
      // pass, which only uses unit 0; unbind anyway so nothing reads a
      // framebuffer's own texture next frame.
      bindTexture(gl, 1, null);
      bindTexture(gl, 2, null);
      gl.activeTexture(gl.TEXTURE0);
    },
  };
}

//===========================================================================
// The controls, read out of Clamp::Clamp(). Same names, same groups, same
// order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const ms = (seconds) => (seconds < 1 ? `${(seconds * 1000).toPrecision(3)} ms` : `${seconds.toPrecision(3)} s`);
const timeConstant = (seconds) => (seconds < 1e-3 ? `${(seconds * 1e6).toPrecision(3)} µs` : ms(seconds));

const demo = mountDemo({
  name: 'Clamp',
  pluginId: 'CL01',
  tagline:
    'An AC-coupled video amplifier whose DC restoration has failed. A capacitor does not pass DC, and the picture’s black level is DC: a clamp used to put it back in every back porch. With the clamp weak or dead, the coupling is a high-pass filter run through the whole raster in time order, blanking included — black wanders with picture content, a bright moment darkens the next one, lines and fields tilt. The shaders here are the plugin’s own; the field walk that carries the capacitor’s charge from field to field is a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/clamp',

  // The stock sentence says "same maths", which is only half true here: the
  // shaders are the plugin's, the field walk between them is a port.
  blurb:
    'It is Clamp’s own GLSL, ported from the repository to WebGL2, with the CPU walk that carries the capacitor’s charge from field to field ported to JavaScript by hand — nothing checks that port but a reader. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  // Every buffer in the chain is RGBA32F, and the block sums are read back as
  // floats: the CPU walk is in double and a byte readback would put 8-bit
  // steps into the capacitor.
  needFloat: true,

  params: [
    std('coupling', 'Coupling', 0.5, 'Coupling', {
      display: (v) => `τ ${timeConstant(couplingSeconds(v))}`,
      hint: 'The coupling time constant, RC. Logarithmic, 20 µs (a third of a line) to 2 s (a hundred fields), because every decade of it is a different look: a line, a field, a second.',
    }),
    opt('standard', 'Standard', ['625/50 (PAL)', '525/59.94 (NTSC)'], 0, 'Coupling',
      'The raster the picture is run through: its line period, porches, active and blank lines and field rate, from BT.470 and SMPTE 170M. The host frame is the active area of a real raster.'),
    bool('perChannel', 'Per Channel', 0, 'Coupling',
      'Off: the wander is the luma of the state, the same on every channel, as a composite amplifier would show it. On: R, G and B each have their own capacitor, and the wander tints.'),

    std('health', 'Clamp Health', 0.2, 'Clamp', {
      display: (v) => (clampPerPorch(v) === 0 ? 'dead' : `${clampPerPorch(v).toPrecision(3)} τc / porch`),
      hint: 'How many of its own time constants the clamp switch gets in one back porch: 20 at the top (perfect), 2e-4 just above the bottom, and exactly 0 at 0 — a dead clamp. The default has all but given up.',
    }),
    std('reference', 'Clamp Reference', 0.5, 'Clamp', {
      display: (v) => referenceLevel(v).toFixed(2),
      hint: 'Where the clamp puts black back, and where the picture’s average is forced when the clamp does not. -0.25 to +0.75; +0.25 at the default middle, where a failed clamp’s picture sits in view rather than below black.',
    }),

    std('sagDepth', 'Sag Depth', 0.3, 'Supply', {
      display: (v) => `gain ${(1 - sagDepth(v)).toFixed(2)} at full APL`,
      hint: 'The supply sagging with average picture level: the gain at full APL is 1 - depth, 0 to 0.5.',
    }),
    std('sagAttack', 'Sag Attack', 0.35, 'Supply', {
      display: (v) => ms(sagSeconds(v)),
      hint: 'How fast the supply follows a rise in APL. 5 ms to 2 s.',
    }),
    std('sagRecovery', 'Sag Recovery', 0.6, 'Supply', {
      display: (v) => ms(sagSeconds(v)),
      hint: 'How fast the supply recovers when APL falls. 5 ms to 2 s.',
    }),

    bool('triode', 'Triode On', 0, 'Stage',
      'A triode stage after the coupling: a 3/2-law plate with a hard knee at cutoff and a soft one where the grid draws current, normalised to unit small-signal gain.'),
    std('bias', 'Bias', 0.5, 'Stage', {
      display: (v) => triodeBias(v).toFixed(2),
      hint: 'The grid’s operating point, -0.6 (toward cutoff) to +0.6 (toward grid current).',
    }),
    std('drive', 'Drive', 1 / 3, 'Stage', {
      display: (v) => `×${triodeDrive(v).toFixed(2)}`,
      hint: 'How far the swing reaches into the knees, 0.5 to 4; 1 at a third. It changes the curvature, not the level.',
    }),

    std('mix', 'Mix', 1.0, 'Output'),
    bool('showBlanking', 'Show Blanking', 0, 'Output',
      'Maps the whole field instead of the picture: every row of it, active, blank and the half line, left to right from the leading edge of sync, with the capacitor’s state in the blanking drawn from the CPU walk. Sync is drawn at its tip for legibility. The view is lifted to a quarter grey so the level below black is visible.'),
  ],

  // Clips that cut and move between bright and dark, which is what makes the
  // black level wander. The synthetic scene moves across its whole range.
  sources: ['scene', 'spot', 'bars', 'ramp', 'grid', 'detail'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the sliders.
  presets: {
    'Healthy clamp': { health: 1.0, sagDepth: 0 },
    'Dead clamp, long coupling': { health: 0, coupling: 0.8 },
    'Line tilt (short coupling)': { coupling: 0.15, health: 0.3 },
    'Per-channel wander': { perChannel: 1, health: 0.05, coupling: 0.6 },
    'Supply sag': { sagDepth: 1, sagAttack: 0.2, sagRecovery: 0.8, health: 1.0 },
    'Overdriven triode': { triode: 1, drive: 0.9, bias: 0.35 },
    'The whole raster': { showBlanking: 1, coupling: 0.35 },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Clamp walks every field of the raster on the CPU in double — the timeline, the field map, the carried capacitor state, the supply — in Model.cpp, Controls.cpp and Clamp::ProcessOpenGL. That walk is ported here function for function, because without it the page would have no state to fill. Nothing checks a port but a reader; the repository’s cltest checks the C++ and has never heard of this page.',
    'The GPU half is not a port. The sums, fill and display passes are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the four shaders drifts.',
    'The block sums are read back from the GPU with readPixels as floats every frame, exactly as the plugin’s glReadPixels does, and stall the pipeline where it does. Every intermediate is RGBA32F, as in the plugin.',
    'The raster runs on the page’s clock with its unit declared as seconds, as the repository’s harness declares it. The plugin votes on Resolume’s clock unit over its first frames; that vote never runs here. Restart is treated as the plugin treats a scrub: the clock steps on one frame and the capacitor keeps its charge.',
    'There is no audio caveat on this page: Clamp has no audio path.',
    'The plugin’s numerical proof — the field map against the ODE’s exact solution, the droop, the continuity across host frames, the triode’s knees — is an offline harness in the repository. Nothing on this page measures anything; the line under the canvas reports what the ported walk computed.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas. It reports the ported walk's own numbers: which
// field of the raster is on show, the picture's APL, the supply's gain and
// where black currently sits. A wandering black level is the whole effect, and
// without a number it is easy to read as the clip changing. Skipped in embed
// mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      const { field, standard, apl, gain, black, walkMillis } = telemetry;
      if (field < 0) return;
      line.textContent =
        `${standard}, field ${field.toLocaleString('en-GB')} on show. `
        + `APL ${apl.toFixed(3)}, supply gain ${gain.toFixed(3)}, `
        + `black on the middle line at ${black >= 0 ? '+' : ''}${black.toFixed(3)}. `
        + `${walkMillis.toFixed(1)} ms for the readback and the walk.`;
    }, 250);
  }
}
