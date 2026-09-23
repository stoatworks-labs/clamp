#include "Model.h"

#include <algorithm>
#include <cmath>

namespace clampfx::model
{
namespace
{
/**
	The two systems, from the documents that define them.

	625/50 (PAL B/G/I): ITU-R BT.470-6 Table 1 and BT.1700 -- line 64 us, line
	blanking 12.05 us made of a 1.65 us front porch, a 4.7 us sync and a 5.7 us
	back porch (breezeway and burst), so 51.95 us active; 625 lines, 576 active,
	two fields of 312.5 lines at 50 fields a second.

	525/59.94 (NTSC-M): SMPTE 170M -- line 1001 / 15 750 000 s (63.5556 us),
	line blanking 10.9 us made of a 1.5 us front porch, a 4.7 us sync and a
	4.7 us back porch, so 52.6556 us active; 525 lines, 480 active in the
	digital raster (170M counts 485 including half lines), two fields of 262.5
	lines at 60000 / 1001 fields a second.

	312.5 x 64 us = 1/50 s and 262.5 x 1001 / 15 750 000 s = 1001 / 60000 s,
	both exactly: a field is fieldLines line periods, and cltest --model holds
	it to 1e-15.
*/
const Standard kStandards[ kStandardCount ] = {
	{ "625/50 (PAL)", 64.0e-6, 1.65e-6, 4.7e-6, 5.7e-6, 288, 312.5, 50, 1 },
	{ "525/59.94 (NTSC)", 1001.0 / 15750000.0, 1.5e-6, 4.7e-6, 4.7e-6, 240, 262.5, 60000, 1001 },
};

double decay( double seconds, double tau )
{
	return std::exp( -seconds / tau );
}
} // namespace

int Standard::BlankLines() const
{
	return static_cast< int >( std::floor( fieldLines - activeLines ) );
}

double Standard::Tail() const
{
	return ( fieldLines - activeLines - BlankLines() ) * line;
}

const Standard& StandardOf( int index )
{
	return kStandards[ std::clamp( index, 0, kStandardCount - 1 ) ];
}

int Timeline::BlockCount( int b ) const
{
	return b < blocks - 1 ? kBlock : width - ( blocks - 1 ) * kBlock;
}

Timeline MakeTimeline( const Standard& standard, int width, const Settings& settings )
{
	Timeline t;
	const int perturb = settings.perturb;
	const double tau  = settings.tau;

	t.width       = std::max( 1, width );
	t.blocks      = ( t.width + kBlock - 1 ) / kBlock;
	t.activeLines = standard.activeLines;
	t.blankLines  = standard.BlankLines();
	t.rows        = t.activeLines + t.blankLines + 1;

	//Blanking. The negative control drops all of it: the timeline becomes
	//the active samples back to back, as if a line were only its picture.
	const bool blank    = !( perturb & kPerturbNoBlanking );
	const double sync   = blank ? standard.sync : 0.0;
	const double porch  = blank ? standard.backPorch : 0.0;
	const double front  = blank ? standard.frontPorch : 0.0;
	const double active = standard.Active();
	const double tail   = blank ? standard.Tail() : 0.0;

	t.sample    = ( ( perturb & kPerturbLineAsActive ) ? standard.line : active ) / t.width;
	t.a         = std::exp( -t.sample / tau );
	t.oneMinusA = -std::expm1( -t.sample / tau );

	const int lastCount = t.BlockCount( t.blocks - 1 );
	const int shortBy   = ( perturb & kPerturbBlockDecay ) ? 1 : 0;
	t.blockDecay        = std::pow( t.a, kBlock - shortBy );
	t.lastDecay         = std::pow( t.a, lastCount - shortBy );

	t.decaySync        = decay( sync, tau );
	t.decayFrontPorch  = decay( front, tau );
	t.decayActiveBlank = blank ? decay( active, tau ) : 1.0;
	t.decayTail        = decay( tail, tau );

	//The porch: x = 0, the switch closed. du/dt = -u ( 1 / tau + 1 / tau_c ):
	//u relaxes toward blanking at the rate k, which puts y at r.
	const double rate   = ( perturb & kPerturbNoClamp ) ? 0.0 : settings.clampRate;
	const double closed = ( perturb & kPerturbHalfPorch ) ? porch * 0.5 : porch;
	const double k      = 1.0 / tau + rate;
	t.porchAlpha        = std::exp( -k * closed );
	t.decayRestOfPorch  = decay( porch - closed, tau );

	for( int j = 0; j < kBlock; ++j )
	{
		t.weightP[ j ] = std::pow( t.a, kBlock - 1 - j );
		t.powA[ j ]    = std::pow( t.a, j );
		t.inputW[ j ]  = t.oneMinusA * std::pow( t.a, j );
	}
	return t;
}

void WalkField( const Timeline& t, const double* sums, FieldWalk& walk )
{
	const int blocks = t.blocks;
	walk.blockA.assign( static_cast< size_t >( t.activeLines ) * blocks, 0.0 );
	walk.blockB.assign( static_cast< size_t >( t.activeLines ) * blocks * 3, 0.0 );
	walk.edgeA.assign( static_cast< size_t >( t.rows ) * 4, 0.0 );
	walk.edgeB.assign( static_cast< size_t >( t.rows ) * 4 * 3, 0.0 );

	//The walk from u = 0: A is the product of every decay so far, B what the
	//picture has put in. Any start state u0 then gives A u0 + B.
	double A          = 1.0;
	double B[ 3 ]     = { 0.0, 0.0, 0.0 };
	double lumaTotal  = 0.0;

	auto step = [ & ]( double d, double e0, double e1, double e2 ) {
		A *= d;
		B[ 0 ] = d * B[ 0 ] + e0;
		B[ 1 ] = d * B[ 1 ] + e1;
		B[ 2 ] = d * B[ 2 ] + e2;
	};
	auto edge = [ & ]( int row, int k ) {
		const size_t i       = static_cast< size_t >( row ) * 4 + k;
		walk.edgeA[ i ]      = A;
		walk.edgeB[ i * 3 ]     = B[ 0 ];
		walk.edgeB[ i * 3 + 1 ] = B[ 1 ];
		walk.edgeB[ i * 3 + 2 ] = B[ 2 ];
	};
	auto porch = [ & ]() {
		step( t.porchAlpha, 0.0, 0.0, 0.0 );
		if( t.decayRestOfPorch != 1.0 )
			step( t.decayRestOfPorch, 0.0, 0.0, 0.0 );
	};

	for( int row = 0; row < t.rows; ++row )
	{
		const bool activeRow = row < t.activeLines;
		const bool tailRow   = row == t.rows - 1;
		if( tailRow )
		{
			//The half line: blanking, no sync worth keying a clamp on.
			for( int k = 0; k < 4; ++k )
				edge( row, k );
			step( t.decayTail, 0.0, 0.0, 0.0 );
			break;
		}

		edge( row, 0 );
		step( t.decaySync, 0.0, 0.0, 0.0 );
		edge( row, 1 );
		porch();
		edge( row, 2 );
		if( activeRow )
		{
			for( int b = 0; b < blocks; ++b )
			{
				const size_t i                 = static_cast< size_t >( row ) * blocks + b;
				walk.blockA[ i ]               = A;
				walk.blockB[ i * 3 ]           = B[ 0 ];
				walk.blockB[ i * 3 + 1 ]       = B[ 1 ];
				walk.blockB[ i * 3 + 2 ]       = B[ 2 ];
				const double* p                = sums + i * 4;
				const double d                 = b == blocks - 1 ? t.lastDecay : t.blockDecay;
				step( d, t.oneMinusA * p[ 0 ], t.oneMinusA * p[ 1 ], t.oneMinusA * p[ 2 ] );
				lumaTotal += p[ 3 ];
			}
		}
		else
			step( t.decayActiveBlank, 0.0, 0.0, 0.0 );
		edge( row, 3 );
		step( t.decayFrontPorch, 0.0, 0.0, 0.0 );
	}

	walk.A      = A;
	walk.B[ 0 ] = B[ 0 ];
	walk.B[ 1 ] = B[ 1 ];
	walk.B[ 2 ] = B[ 2 ];
	walk.apl    = lumaTotal / ( static_cast< double >( t.width ) * t.activeLines );
}

double SagStep( double e, double apl, double fieldSeconds, const Sag& sag, int perturb )
{
	const bool rising = apl > e;
	const bool swap   = ( perturb & kPerturbSagSwap ) != 0;
	const double tau  = rising != swap ? sag.attack : sag.recovery;
	return apl + ( e - apl ) * std::exp( -fieldSeconds / tau );
}

double TriodePlate( double u )
{
	if( u <= -1.0 )
		return 0.0;
	//Grid current: past zero the grid draws current and the source sags, so
	//the grid voltage the valve sees is compressed. The soft knee.
	const double g = u > 0.0 ? u / ( 1.0 + 0.5 * u ) : u;
	return std::pow( 1.0 + g, 1.5 );
}

Triode MakeTriode( double drive, double bias )
{
	Triode t;
	t.drive       = drive;
	t.bias        = bias;
	t.plateAtBias = TriodePlate( bias );
	if( bias > 0.0 )
	{
		const double g     = bias / ( 1.0 + 0.5 * bias );
		const double dg    = 1.0 / ( ( 1.0 + 0.5 * bias ) * ( 1.0 + 0.5 * bias ) );
		t.slopeAtBias      = 1.5 * std::sqrt( 1.0 + g ) * dg;
	}
	else
		t.slopeAtBias = 1.5 * std::sqrt( std::max( 0.0, 1.0 + bias ) );
	return t;
}

double TriodeTransfer( double v, const Triode& t )
{
	const double u = t.drive * ( v - 0.5 ) + t.bias;
	return 0.5 + ( TriodePlate( u ) - t.plateAtBias ) / ( t.drive * t.slopeAtBias );
}

int64_t FieldAt( double seconds, const Standard& standard )
{
	return static_cast< int64_t >( std::floor( seconds / standard.Field() + kFieldSlack ) );
}

int64_t FloorDiv( int64_t a, int64_t b )
{
	const int64_t q = a / b;
	return ( a % b != 0 && a < 0 ) ? q - 1 : q;
}

} // namespace clampfx::model
