#pragma once

#include <cstdint>
#include <vector>

/**
	The amplifier as arithmetic. No GL, no FFGL: the plugin and the harness's
	offline checks both link this and nothing else of the model.

	**One RC and the real timing.** The coupling capacitor sits between the
	input x and the next stage's grid, which returns to the bias (0, blanking
	level) through R; the clamp switch, when it conducts, joins the grid to the
	Clamp Reference r through its own resistance. With s the voltage across the
	capacitor and y = x - s what the next stage sees,

	    ds/dt = ( x - s ) / tau  +  g(t) ( x - s - r ) / tau_c

	with tau = RC (Coupling), tau_c = R_s C (Clamp Health) and g(t) = 1 in each
	back porch and 0 everywhere else. Everything else is timing.

	**The timeline.** The host frame is the active area of a real raster. A
	field of the chosen Standard is, from its first active line:

	    per active line   sync | back porch (clamp) | active: W samples | front porch
	    per blank line    sync | back porch (clamp) | blanking          | front porch
	    then              the half line, blanking, no clamp

	with `activeLines` active lines, `floor( fieldLines - activeLines )` blank
	lines and the remaining half line, so a field lasts exactly fieldLines
	line periods = 1 / field rate. Every interval outside the active samples is
	at blanking level (x = 0), sync included, as the spec says.

	**Exact discretisation.** x is constant over a sample (a zero-order hold
	of the host's pixel), so across one sample of length D

	    s[n+1] = a s[n] + ( 1 - a ) x[n],   a = exp( -D / tau )

	is the ODE's exact solution, not an approximation of it; the same is true
	of every blanking interval (x = 0) and every porch (x = 0, clamp closed).
	The output at a sample is y[n] = x[n] - s[n]: the state at the sample's
	start.

	**Why it is linear, and what that buys.** Every step above is affine in s.
	So a whole field is s_end = A s_start + B, where A depends only on the
	timing and B only on the picture; a stretch of fields with one picture is
	a closed form; and the periodic steady state is B / ( 1 - A ). The GPU
	sums each block of kBlock samples into P = sum a^(m-1-j) x[j] (one number
	per block), the CPU walks the blocks and the blanking in double, and the
	GPU fills in each sample inside a block from the state at the block's
	start. See AGENTS.md.
*/
namespace clampfx::model
{

/// Negative-control hooks: a bitmask the shipped plugin always carries at 0.
enum Perturb : int
{
	kPerturbNone          = 0,
	kPerturbNoBlanking    = 1 << 0,///< blanking dropped from the timeline (the spec's droop negative)
	kPerturbResetEachFrame = 1 << 1,///< the state reset every host frame (the spec's continuity negative)
	kPerturbLineAsActive  = 1 << 2,///< the sample period taken from the whole line, not the active part
	kPerturbHalfPorch     = 1 << 3,///< the clamp closed for half the back porch
	kPerturbBlockDecay    = 1 << 4,///< the block-to-block decay one sample short
	kPerturbSagSwap       = 1 << 5,///< the supply's attack and recovery swapped
	kPerturbNoClamp       = 1 << 6,///< the clamp never closes
	kPerturbResizeReprime = 1 << 7,///< a resize re-primes the state
	kPerturbClockFloat    = 1 << 8,///< the clock kept in float
};

/// A television system's line and field timing, in seconds.
struct Standard
{
	const char* name;
	double line;      ///< line period
	double frontPorch;
	double sync;
	double backPorch; ///< where the clamp closes
	int activeLines;  ///< active lines in one field
	double fieldLines;///< line periods in one field, the half line included
	int64_t rateNum;  ///< fields per second, as num / den
	int64_t rateDen;

	/// The active line: what the blanking leaves.
	double Active() const
	{
		return line - frontPorch - sync - backPorch;
	}
	/// 1 / field rate, which is also fieldLines line periods.
	double Field() const
	{
		return static_cast< double >( rateDen ) / static_cast< double >( rateNum );
	}
	/// Whole blank lines after the active ones.
	int BlankLines() const;
	/// What is left of the field after them: the half line.
	double Tail() const;
};

enum StandardIndex
{
	kPAL  = 0,
	kNTSC = 1,
	kStandardCount
};

const Standard& StandardOf( int index );

/// Samples per block: the GPU's partial sums and the within-block fill.
constexpr int kBlock = 16;

/// Rec. 601 luma, the weights PAL and NTSC both use.
constexpr double kLumaR = 0.299;
constexpr double kLumaG = 0.587;
constexpr double kLumaB = 0.114;

/// The physical settings of the coupling and the clamp.
struct Settings
{
	double tau       = 0.01;///< Coupling, seconds
	double clampRate = 0.0; ///< 1 / tau_c, per second; 0 is a dead clamp
	double reference = 0.0; ///< Clamp Reference, in signal units
	int perturb      = 0;
};

/**
	Every coefficient one field's walk needs, for a Standard, a width and a
	Settings. Built in double, once per change.
*/
struct Timeline
{
	int width       = 0;
	int blocks      = 0;///< ceil( width / kBlock )
	int activeLines = 0;
	int blankLines  = 0;
	int rows        = 0;///< activeLines + blankLines + 1 (the half line)

	double sample     = 0.0;///< D, seconds per sample
	double a          = 1.0;///< exp( -D / tau )
	double oneMinusA  = 0.0;///< 1 - a, from expm1, not by subtraction
	double blockDecay = 1.0;///< a^kBlock (a whole block)
	double lastDecay  = 1.0;///< a^( samples in the last block )

	double decaySync        = 1.0;
	double decayFrontPorch  = 1.0;
	double decayActiveBlank = 1.0;///< an active interval at blanking level
	double decayRestOfPorch = 1.0;///< only under kPerturbHalfPorch
	double decayTail        = 1.0;
	double porchAlpha       = 1.0;///< s -> target + ( s - target ) alpha over the porch
	double porchTarget      = 0.0;///< the porch's equilibrium state, s* = -r ( 1 / tau_c ) / k

	/// Per-sample tables in double; the plugin hands them to the GPU as floats.
	double weightP[ kBlock ];///< a^( kBlock - 1 - j ): P's weight of sample j of a full block
	double powA[ kBlock ];   ///< a^j
	double inputW[ kBlock ]; ///< ( 1 - a ) a^i

	/// Samples in block b.
	int BlockCount( int b ) const;
};

Timeline MakeTimeline( const Standard& standard, int width, const Settings& settings );

/**
	One field's walk, from s = 0, recorded so that any start state can be
	applied afterwards: every recorded state is ( A_k s_start + B_k ), A_k a
	scalar (the same for every channel) and B_k one per channel.

	`sums` is the GPU's block sums, [ ( line * blocks + b ) * 4 + c ]: c = 0..2
	the weighted sums P of R, G and B, c = 3 the plain sum of luma.
*/
struct FieldWalk
{
	double A = 1.0;///< the whole field
	double B[ 3 ] = { 0, 0, 0 };
	double apl = 0.0;///< mean luma over the active samples

	std::vector< double > blockA;///< [ line * blocks + b ]: the state at the block's first sample
	std::vector< double > blockB;///< [ ( line * blocks + b ) * 3 + c ]
	/// Per row of the field (active lines, blank lines, the half line), the
	/// state at four instants: sync start, back porch start, active start,
	/// front porch start. [ row * 4 + k ] and [ ( row * 4 + k ) * 3 + c ].
	std::vector< double > edgeA;
	std::vector< double > edgeB;
};

void WalkField( const Timeline& t, const double* sums, FieldWalk& walk );

/// The state after the field, from `start`.
inline double Advance( const FieldWalk& w, int channel, double start )
{
	return w.A * start + w.B[ channel ];
}

/// The periodic steady state of one picture: the fixed point of the field map.
inline double SteadyState( const FieldWalk& w, int channel )
{
	return w.B[ channel ] / ( 1.0 - w.A );
}

/// The supply. The filtered APL e moves toward each field's APL with the
/// attack time constant when rising and the recovery one when falling.
struct Sag
{
	double depth    = 0.0;///< gain = 1 - depth e
	double attack   = 0.05;
	double recovery = 0.5;
};

/// e after one field of `apl`.
double SagStep( double e, double apl, double fieldSeconds, const Sag& sag, int perturb );

inline double SagGain( double e, const Sag& sag )
{
	return 1.0 - sag.depth * e;
}

/**
	The triode stage's constants, for the shader. The grid sees
	u = drive ( v - 1/2 ) + bias; the plate current follows the 3/2 law above
	cutoff at u = -1 and nothing below it (a hard knee), and on the positive
	side grid current loads the source, which is modelled as the grid voltage
	itself compressing, u / ( 1 + u / 2 ) (a soft knee). The output is
	normalised to unit small-signal gain at the operating point, so Drive
	changes how far the swing reaches into the knees and not the level.
*/
struct Triode
{
	double drive = 1.0;
	double bias  = 0.0;
	double plateAtBias = 1.0;///< P( bias )
	double slopeAtBias = 1.5;///< P'( bias )
};

Triode MakeTriode( double drive, double bias );
double TriodePlate( double u );
double TriodeTransfer( double v, const Triode& t );

/**
	The field under way `seconds` after field 0 began: floor( seconds / T ).
	A host frame within kFieldSlack of a field's start counts as at it,
	because a host frame that lands on a field start in exact arithmetic can
	land a rounding short of it in double -- and Resolume's clock, at ~1e9 ms
	after six days, resolves only ~1.2e-10 s, which is 6e-9 of a PAL field.
	1e-6 of a field (20 ns) is far above that and far below anything
	visible.
*/
constexpr double kFieldSlack = 1e-6;
int64_t FieldAt( double seconds, const Standard& standard );

/// floor( a / b ) for b > 0, correct for negative a.
int64_t FloorDiv( int64_t a, int64_t b );

} // namespace clampfx::model
