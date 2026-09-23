/**
	cltest -- render Clamp offline, and read the amplifier back out of it.

	How far black falls after a burst of white, how much a line tilts, what
	the clamp leaves behind after a porch: each has one right answer, a
	closed form in the time constants and the raster's real timing. Every
	check here drives the REAL plugin class through a headless GL context and
	measures the answer out of the picture it made:

		cltest --out /tmp/frame.png     a picture, on the moving test card
		cltest --list                   every parameter, its kind and default
		cltest --droop                  after a burst of white lines (or a white
		                                field), the black that follows falls back
		                                on exp( -t / tau ) with the blanking
		                                counted: tau of a line, a field, a second
		cltest --tilt                   a flat field with a dead clamp: each line
		                                tilts by 1 - exp( -T_active / tau )
		cltest --apl                    at steady state, black shifts in
		                                proportion to APL, by the closed form
		cltest --clamp                  the residual after each porch is the
		                                switch's exp( -T_porch / tau_c ); the
		                                level it settles at is the reference's
		cltest --reference              every pixel against a serial run of the
		                                same timeline in double
		cltest --continuity             host frames at six rates give the
		                                trajectory of one continuous run
		cltest --sag                    the supply's gain follows its attack and
		                                recovery time constants
		cltest --identity               a perfect clamp, no sag, no triode: the
		                                input, within a derived bound
		cltest --resize                 a resize mid-run carries the state
		cltest --negative               every check above can FAIL
		cltest --offline                the checks that need no GL
		cltest --bench                  the render cost
		cltest --dump-shaders DIR       the exact GLSL the plugin compiles
		cltest --pipe                   raw frames in, raw frames out

	The standards' timing, the control laws and the serial reference are
	stated HERE, from the documents and the definitions, and never read out
	of Model.h or Controls.h: a constant typed wrong there has to show up as a
	failed check, not as an agreement. The one number read from the plugin is
	kBlock, the length of its float accumulations, which the error bound
	needs. AGENTS.md has one line per check on where each tolerance comes
	from.
*/

#include "Clamp.h"
#include "Clock.h"
#include "Controls.h"
#include "Model.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model = clampfx::model;

int g_checks   = 0;
int g_failures = 0;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The two systems, stated from the documents: ITU-R BT.470-6 / BT.1700 for
// 625/50, SMPTE 170M for 525/59.94. Typed here, not read from Model.h.
//---------------------------------------------------------------------------
struct Stated
{
	const char* name;
	double line;
	double frontPorch;
	double sync;
	double backPorch;
	int activeLines;  //per field
	double fieldLines;//line periods per field
	int64_t rateNum;  //fields per second
	int64_t rateDen;
	int option;       //the Standard parameter's element

	double active() const
	{
		return line - ( frontPorch + sync + backPorch );
	}
	double field() const
	{
		return static_cast< double >( rateDen ) / static_cast< double >( rateNum );
	}
	int blankLines() const
	{
		return static_cast< int >( std::floor( fieldLines - activeLines ) );
	}
	double tail() const
	{
		return ( fieldLines - activeLines - blankLines() ) * line;
	}
};

const Stated kPal  = { "PAL", 64.0e-6, 1.65e-6, 4.7e-6, 5.7e-6, 288, 312.5, 50, 1, 0 };
const Stated kNtsc = { "NTSC", 1001.0 / 15750000.0, 1.5e-6, 4.7e-6, 4.7e-6, 240, 262.5, 60000, 1001, 1 };
const Stated* const kBoth[] = { &kPal, &kNtsc };

//---------------------------------------------------------------------------
// The control laws, stated from their definitions (Controls.h's comments,
// which are the spec of each control), and their inverses for choosing a
// slider. A check converts the FLOAT it hands the plugin, so the stated
// value is exactly what the plugin was asked for.
//---------------------------------------------------------------------------
/// A host slider is 0..1; the plugin clamps what it is handed, and so does
/// every stated law.
double unit( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double statedTau( float v )
{
	return std::pow( 10.0, -4.7 + 5.0 * unit( v ) );
}
float sliderForTau( double tau )
{
	return static_cast< float >( ( std::log10( tau ) + 4.7 ) / 5.0 );
}
double statedPerPorch( float v )
{
	return unit( v ) <= 0.0 ? 0.0 : 20.0 * std::pow( 10.0, -5.0 * ( 1.0 - unit( v ) ) );
}
float sliderForPerPorch( double n )
{
	return static_cast< float >( 1.0 + std::log10( n / 20.0 ) / 5.0 );
}
double statedReference( float v )
{
	return unit( v ) - 0.25;
}
float sliderForReference( double r )
{
	return static_cast< float >( r + 0.25 );
}
double statedDepth( float v )
{
	return 0.5 * unit( v );
}
double statedSagSeconds( float v )
{
	return std::pow( 10.0, -2.3 + 2.6 * unit( v ) );
}
float sliderForSagSeconds( double t )
{
	return static_cast< float >( ( std::log10( t ) + 2.3 ) / 2.6 );
}

/// Every control a check can move, as the sliders the plugin sees.
struct Knobs
{
	float coupling   = 0.5f;
	int standard     = 0;
	bool perChannel  = false;
	float health     = 0.0f;
	float reference  = 0.25f;//a reference of 0: black at black
	float sagDepth   = 0.0f;
	float sagAttack  = 0.35f;
	float sagRecover = 0.6f;
	bool triode      = false;
	float bias       = 0.5f;
	float drive      = 1.0f / 3.0f;
	float mix        = 1.0f;
	bool showRaster  = false;
};

//---------------------------------------------------------------------------
// The bound on any output value against the serial double reference, in
// float ULPs at 1 (u = 2^-24), from where the plugin rounds. X is the
// largest input (1); U the largest state the plugin holds (1: it holds
// u = s + r, a low-pass of x toward blanking, so inside [ 0, 1 ]); R the
// largest reference (0.75). m is the plugin's block.
//
//   sums     each block's P = sum a^(m-1-j) x[j] in float: <= 1.01 (m+2) u
//            sum a^(m-1-j) |x| (m products, m-1 additions, m rounded
//            weights); times ( 1 - a ) it is <= 1.01 (m+2) u (1 - a^m) X.
//            Carried into later states through every later decay, those
//            errors telescope: sum_k ( 1 - a^m ) a^(m k) = 1. So the whole
//            history, however long, contributes at most 1.01 (m+2) u X.
//   upload   the block's start state, double to float: u U.
//   fill     a^j u0 (the weight's rounding, the product's, the final add's)
//            plus the in-block sum, which is at most 1.01 (m+2) u X: 4 u U
//            + 1.01 (m+2) u X.
//   display  luma of the state (three products, two adds: 5 u U), x - u
//            (u (X + U)), + r (u (X + U + R)), the gain (its rounding and the
//            product's: 2 u (X + U + R)).
//   sag      the APL comes from float sums of luma (a dot and sixteen adds
//            per block, 1.01 (m+3) u), into the gain times depth, times
//            |x - u + r| <= X + U + R.
//
// None of these assumes an order of summation: the gamma_n bound holds for
// any order, so a compiler that reassociates or contracts to fma cannot
// break it.
//---------------------------------------------------------------------------
constexpr double kU = 1.0 / 16777216.0;

double outputBound( double depth )
{
	const double m = model::kBlock, X = 1.0, U = 1.0, R = 0.75;
	const double sums    = 1.01 * ( m + 2 ) * X;
	const double upload  = U;
	const double fill    = 4.0 * U + 1.01 * ( m + 2 ) * X;
	const double display = 5.0 * U + ( X + U ) + ( X + U + R ) + 2.0 * ( X + U + R );
	const double sag     = depth * 1.01 * ( m + 3 ) * ( X + U + R );
	return kU * ( sums + upload + fill + display + sag );
}

//---------------------------------------------------------------------------
// The raster's geometry, stated. Active line l (top first) reads the host row
// under its centre; output row r shows the line whose span covers the row's
// centre.
//---------------------------------------------------------------------------
int rowOfLine( int l, int H, int A )
{
	return static_cast< int >( ( static_cast< int64_t >( 2 * l + 1 ) * H ) / ( 2 * A ) );
}
int lineOfRow( int r, int H, int A )
{
	return static_cast< int >( ( static_cast< int64_t >( 2 * r + 1 ) * A ) / ( 2 * H ) );
}

/// The field on show at host frame k: floor( t_k / T_field ), exactly.
int64_t fieldOfFrame( int64_t k, int64_t fpsNum, int64_t fpsDen, const Stated& st )
{
	const __int128 num = static_cast< __int128 >( k ) * fpsDen * st.rateNum;
	const __int128 den = static_cast< __int128 >( fpsNum ) * st.rateDen;
	return static_cast< int64_t >( num / den );
}

//---------------------------------------------------------------------------
// The serial reference: the ODE's exact solution sample by sample, through
// every blanking interval, in double, in time order. Nothing parallel,
// nothing blocked; a different arrangement of the step (x + ( s - x ) e
// rather than a s + ( 1 - a ) x) on purpose.
//---------------------------------------------------------------------------
struct RefSettings
{
	const Stated* st = &kPal;
	double tau       = 0.01;
	double rate      = 0.0;//1 / tau_c
	double ref       = 0.0;
	bool perChannel  = false;
	double depth     = 0.0;
	double attack    = 0.05;
	double recovery  = 0.5;
};

RefSettings statedFrom( const Knobs& k )
{
	RefSettings s;
	s.st         = k.standard == 0 ? &kPal : &kNtsc;
	s.tau        = statedTau( k.coupling );
	s.rate       = statedPerPorch( k.health ) / s.st->backPorch;
	s.ref        = statedReference( k.reference );
	s.perChannel = k.perChannel;
	s.depth      = statedDepth( k.sagDepth );
	s.attack     = statedSagSeconds( k.sagAttack );
	s.recovery   = statedSagSeconds( k.sagRecover );
	return s;
}

struct Reference
{
	RefSettings cfg;
	int W = 0, H = 0;

	double s[ 3 ]  = { 0, 0, 0 };
	double e       = 0.0;
	bool primed    = false;
	int64_t next   = 0;
	int64_t shown  = -1;
	double shownS[ 3 ] = { 0, 0, 0 };
	double shownE      = 0.0;
	double gain        = 1.0;
	std::vector< double > states;//[ ( l * 3 + c ) * W + n ]: the shown field's state at each sample's start

	int A() const
	{
		return cfg.st->activeLines;
	}

	double apl( const std::vector< float >& pic ) const
	{
		double total = 0.0;
		for( int l = 0; l < A(); ++l )
		{
			const float* row = pic.data() + static_cast< size_t >( rowOfLine( l, H, A() ) ) * W * 4;
			for( int n = 0; n < W; ++n )
				total += 0.299 * row[ n * 4 ] + 0.587 * row[ n * 4 + 1 ] + 0.114 * row[ n * 4 + 2 ];
		}
		return total / ( static_cast< double >( W ) * A() );
	}

	double sagStep( double from, double level ) const
	{
		const double tau = level > from ? cfg.attack : cfg.recovery;
		return level + ( from - level ) * std::exp( -cfg.st->field() / tau );
	}

	/// One field, sample by sample, from st: ds/dt = ( x - r - s ) ( 1 / tau
	/// + g / tau_c ), the state stated as the capacitor's own voltage with the
	/// reference inside the dynamics -- not the plugin's u = s + r.
	void walk( const std::vector< float >& pic, double st[ 3 ], std::vector< double >* record ) const
	{
		const Stated& S     = *cfg.st;
		const double tau    = cfg.tau;
		const double r      = cfg.ref;
		const double step   = std::exp( -( S.active() / W ) / tau );
		const double dSync  = std::exp( -S.sync / tau );
		const double dFront = std::exp( -S.frontPorch / tau );
		const double dBlank = std::exp( -S.active() / tau );
		const double dTail  = std::exp( -S.tail() / tau );
		const double alpha  = std::exp( -( 1.0 / tau + cfg.rate ) * S.backPorch );
		//Blanking is x = 0, so there s heads for -r.
		auto blank = [ r ]( double& s, double d ) { s = -r + ( s + r ) * d; };
		if( record )
			record->assign( static_cast< size_t >( A() ) * 3 * W, 0.0 );

		for( int l = 0; l < A(); ++l )
		{
			const float* row = pic.data() + static_cast< size_t >( rowOfLine( l, H, A() ) ) * W * 4;
			for( int c = 0; c < 3; ++c )
			{
				blank( st[ c ], dSync );
				blank( st[ c ], alpha );
				double v = st[ c ];
				double* out = record ? record->data() + ( static_cast< size_t >( l ) * 3 + c ) * W : nullptr;
				for( int n = 0; n < W; ++n )
				{
					if( out )
						out[ n ] = v;
					const double target = row[ n * 4 + c ] - r;
					v                   = target + ( v - target ) * step;
				}
				st[ c ] = v;
				blank( st[ c ], dFront );
			}
		}
		for( int b = 0; b < S.blankLines(); ++b )
			for( int c = 0; c < 3; ++c )
			{
				blank( st[ c ], dSync );
				blank( st[ c ], alpha );
				blank( st[ c ], dBlank * dFront );
			}
		for( int c = 0; c < 3; ++c )
			blank( st[ c ], dTail );
	}

	/// The periodic steady state of one picture, from linearity: the field
	/// map is s -> A s + B, so B is a walk from 0 and A + B one from 1.
	void prime( const std::vector< float >& pic )
	{
		double zero[ 3 ] = { 0, 0, 0 }, one[ 3 ] = { 1, 1, 1 };
		walk( pic, zero, nullptr );
		walk( pic, one, nullptr );
		for( int c = 0; c < 3; ++c )
			s[ c ] = zero[ c ] / ( 1.0 - ( one[ c ] - zero[ c ] ) );
		e = apl( pic );
	}

	/// Host frame showing field n, with this picture: the rule the spec gives
	/// ("advance by real elapsed time") made concrete -- every field that
	/// started since the last frame runs on this frame's picture, the last
	/// one is shown, and a frame with no new field shows the same field
	/// again, re-scanned from its own start.
	void frame( int64_t n, const std::vector< float >& pic, bool primeFirst = true )
	{
		if( !primed )
		{
			if( primeFirst )
				prime( pic );
			else
			{
				s[ 0 ] = s[ 1 ] = s[ 2 ] = 0.0;
				e = 0.0;
			}
			next   = n;
			primed = true;
		}
		const double level = apl( pic );
		if( n >= next )
		{
			for( int64_t k = next; k < n; ++k )
			{
				walk( pic, s, nullptr );
				e = sagStep( e, level );
			}
			shown = n;
			for( int c = 0; c < 3; ++c )
				shownS[ c ] = s[ c ];
			shownE = e;
			walk( pic, s, &states );
			e    = sagStep( shownE, level );
			next = n + 1;
		}
		else
		{
			double t[ 3 ] = { shownS[ 0 ], shownS[ 1 ], shownS[ 2 ] };
			walk( pic, t, &states );
		}
		gain = 1.0 - cfg.depth * shownE;
	}

	double stateAt( int l, int n, int c ) const
	{
		const double* base = states.data() + static_cast< size_t >( l ) * 3 * W;
		if( cfg.perChannel )
			return base[ static_cast< size_t >( c ) * W + n ];
		return 0.299 * base[ n ] + 0.587 * base[ W + n ] + 0.114 * base[ 2 * W + n ];
	}

	/// The output at host pixel (r, c), channel ch, triode off, mix 1.
	double expected( const std::vector< float >& pic, int r, int col, int ch ) const
	{
		const int l    = lineOfRow( r, H, A() );
		const double x = pic[ ( static_cast< size_t >( r ) * W + col ) * 4 + ch ];
		const double y = gain * ( x - stateAt( l, col, ch ) );
		return std::clamp( y, 0.0, 1.0 );
	}
};

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Clamp::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Clamp& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Clamp::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Clamp& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Clamp& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Clamp& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

void apply( Clamp& p, const Knobs& k )
{
	set( p, "Coupling", k.coupling );
	set( p, "Standard", static_cast< float >( k.standard ) );
	set( p, "Per Channel", k.perChannel ? 1.0f : 0.0f );
	set( p, "Clamp Health", k.health );
	set( p, "Clamp Reference", k.reference );
	set( p, "Sag Depth", k.sagDepth );
	set( p, "Sag Attack", k.sagAttack );
	set( p, "Sag Recovery", k.sagRecover );
	set( p, "Triode On", k.triode ? 1.0f : 0.0f );
	set( p, "Bias", k.bias );
	set( p, "Drive", k.drive );
	set( p, "Mix", k.mix );
	set( p, "Show Blanking", k.showRaster ? 1.0f : 0.0f );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
// The clock is a RATIONAL rate, so that frame k lands on k den / num seconds
// and a field's start can be a host frame exactly.
//---------------------------------------------------------------------------
struct Session
{
	Clamp plugin;
	int width      = 0;
	int height     = 0;
	int64_t fpsNum = 60;
	int64_t fpsDen = 1;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip changes size: the SAME instance handed
	/// a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	void rate( int64_t num, int64_t den )
	{
		fpsNum = num;
		fpsDen = den;
	}

	double timeOf( int64_t frame ) const
	{
		return static_cast< double >( frame ) * static_cast< double >( fpsDen ) / static_cast< double >( fpsNum );
	}

	bool renderAt( int64_t frame )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( timeOf( frame ) );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %lld\n", static_cast< long long >( frame ) );
		return ok;
	}

	bool render( int64_t frame, const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	bool render( int64_t frame, const std::vector< float >& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// A check's session: the knobs applied, the perturbation set, the host at
/// the field rate unless told otherwise.
void prepare( Session& s, const Knobs& k, int perturb )
{
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	const Stated& st = k.standard == 0 ? kPal : kNtsc;
	s.rate( st.rateNum, st.rateDen );
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( !quiet )
	{
		va_list args;
		va_start( args, format );
		std::vprintf( format, args );
		va_end( args );
		std::printf( "  %s\n", verdict( ok ) );
	}
	return ok ? 0 : 1;
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
std::vector< float > flat( int W, int H, double level )
{
	std::vector< float > p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ] = 1.0f;
	}
	return p;
}

void paintRow( std::vector< float >& p, int W, int row, double level )
{
	for( int x = 0; x < W; ++x )
	{
		float* px = p.data() + ( static_cast< size_t >( row ) * W + x ) * 4;
		px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( level );
	}
}

/// PCG output mix: exact in 32 bits, the same on every machine.
uint32_t hashInt( uint32_t v )
{
	uint32_t state = v * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

/// Random colour in [0, 1], on a 1/1024 grid so the float is exact.
std::vector< float > noise( int W, int H, uint32_t seed, double scale = 1.0, double offset = 0.0 )
{
	std::vector< float > p( static_cast< size_t >( W ) * H * 4 );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			for( int c = 0; c < 3; ++c )
			{
				const uint32_t h = hashInt( seed * 0x9E3779B9u ^ hashInt( static_cast< uint32_t >( ( y * W + x ) * 3 + c ) ) );
				px[ c ]          = static_cast< float >( offset + scale * ( h % 1024u ) / 1023.0 );
			}
			px[ 3 ] = 1.0f;
		}
	return p;
}

float at( const std::vector< float >& img, int W, int r, int c, int ch = 0 )
{
	return img[ ( static_cast< size_t >( r ) * W + c ) * 4 + ch ];
}

/// A measured value that sits on the output's floor or ceiling was clipped,
/// and says nothing about the signal: a check that reads one is invalid.
bool clipped( double v )
{
	return v <= 1e-5 || v >= 1.0 - 1e-5;
}

/// Render `frames` frames of a picture sequence and keep the chosen ones.
template< typename Picture >
bool run( Session& s, int frames, Picture&& picture, std::vector< std::vector< float > >& out )
{
	out.clear();
	for( int k = 0; k < frames; ++k )
	{
		if( !s.render( k, picture( k ) ) )
			return false;
		out.push_back( s.readBackFloat() );
	}
	return true;
}

//---------------------------------------------------------------------------
// --droop
//
// With a dead clamp, what a burst puts into the capacitor after it has
// passed decays on exp( -t / tau ) and nothing else, t being REAL time with
// every blanking interval counted. Two runs, one with the burst and one
// without, cancel everything but the burst's own contribution: dy is that,
// read out of the picture. Three tau: a line (the burst of white lines leaves
// a streak down the next few), a field (it shades the rest of the field), a
// second (a white field darkens the fields after it).
//---------------------------------------------------------------------------
int runDroop( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	const double eps  = outputBound( 0.0 );
	const double epsD = 2.0 * eps;//a difference of two renders

	for( const Stated* sp : kBoth )
	{
		const Stated& st = *sp;
		const int A      = st.activeLines;
		const char* const cases[] = { "a line", "a field", "a second" };
		const double taus[]       = { st.line, st.field(), 1.0 };
		for( int which = 0; which < 3; ++which )
		{
			Knobs k;
			k.standard = st.option;
			k.coupling = sliderForTau( taus[ which ] );
			k.health   = 0.0f;
			const double tau = statedTau( k.coupling );

			//Measured at each line's first sample, on a bright stripe down the
			//left edge over a darker ground: y there sits well above the floor
			//whatever the capacitor holds, so a perturbed model fails on the
			//exponential and not on a clipped reading (a flat picture with the
			//blanking dropped settles at y = 0 exactly, which is what the first
			//version of this check caught its negative control on).
			const double ground = 0.3, stripe = 0.9, burst = 0.9;
			const int col       = 0;
			const int stripeW   = std::max( 2, W / 32 );
			auto plain          = [ & ]( int ) {
				std::vector< float > p = flat( W, H, ground );
				for( int y = 0; y < H; ++y )
					for( int x = 0; x < stripeW; ++x )
					{
						float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
						px[ 0 ] = px[ 1 ] = px[ 2 ] = static_cast< float >( stripe );
					}
				return p;
			};
			int usable = 0, points = 0;
			double worst = 0.0, fitTau = 0.0;
			bool clip = false;

			if( which < 2 )
			{
				const int first = 8, count = which == 0 ? 12 : 40;
				auto withBurst  = [ & ]( int f ) {
					std::vector< float > p = plain( f );
					for( int l = first; l < first + count; ++l )
						paintRow( p, W, rowOfLine( l, H, A ), burst );
					return p;
				};
				auto without = plain;

				Session a, b;
				prepare( a, k, perturb );
				prepare( b, k, perturb );
				std::vector< std::vector< float > > outA, outB;
				if( !a.begin( W, H ) || !b.begin( W, H ) || !run( a, 4, withBurst, outA ) || !run( b, 4, without, outB ) )
					return report( false, quiet, "droop: could not render" );
				a.end();
				b.end();
				const std::vector< float > pic = withBurst( 0 );

				int anchorLine = -1;
				double anchor  = 0.0;
				int lastLine   = -1;
				double last    = 0.0;
				for( int r = 0; r < H; ++r )
				{
					const int l = lineOfRow( r, H, A );
					if( l < first + count || at( pic, W, r, W - 1 ) != static_cast< float >( ground ) )
						continue;
					const double ya = at( outA[ 3 ], W, r, col ), yb = at( outB[ 3 ], W, r, col );
					clip            = clip || clipped( ya ) || clipped( yb );
					const double dy = ya - yb;
					if( anchorLine < 0 )
					{
						if( std::fabs( dy ) < 100.0 * epsD )
							break;
						anchorLine = l;
						anchor     = dy;
						continue;
					}
					const double decay = std::exp( -( l - anchorLine ) * st.line / tau );
					const double pred  = anchor * decay;
					const double tol   = epsD * ( 1.0 + decay );
					worst              = std::max( worst, std::fabs( dy - pred ) / tol );
					++points;
					if( std::fabs( pred ) > 10.0 * tol )
					{
						++usable;
						lastLine = l;
						last     = dy;
					}
				}
				if( usable > 0 )
					fitTau = ( lastLine - anchorLine ) * st.line / std::log( anchor / last );
			}
			else
			{
				//A white field at frame 3, the base on every other; the host at
				//the field rate, so frame k is field k and consecutive
				//measurements are exactly one field period apart.
				const int frames = 40, burstFrame = 3;
				auto withBurst   = [ & ]( int f ) { return f == burstFrame ? flat( W, H, burst ) : plain( f ); };
				auto without     = plain;
				Session a, b;
				prepare( a, k, perturb );
				prepare( b, k, perturb );
				std::vector< std::vector< float > > outA, outB;
				if( !a.begin( W, H ) || !b.begin( W, H ) || !run( a, frames, withBurst, outA ) || !run( b, frames, without, outB ) )
					return report( false, quiet, "droop: could not render" );
				a.end();
				b.end();

				const int r = H / 2;
				double anchor = 0.0, last = 0.0;
				int lastFrame = -1;
				for( int f = burstFrame + 1; f < frames; ++f )
				{
					const double ya = at( outA[ f ], W, r, col ), yb = at( outB[ f ], W, r, col );
					clip            = clip || clipped( ya ) || clipped( yb );
					const double dy = ya - yb;
					if( f == burstFrame + 1 )
					{
						anchor = dy;
						continue;
					}
					const double decay = std::exp( -( f - burstFrame - 1 ) * st.field() / tau );
					const double pred  = anchor * decay;
					const double tol   = epsD * ( 1.0 + decay );
					worst              = std::max( worst, std::fabs( dy - pred ) / tol );
					++points;
					if( std::fabs( pred ) > 10.0 * tol )
					{
						++usable;
						lastFrame = f;
						last      = dy;
					}
				}
				if( usable > 0 )
					fitTau = ( lastFrame - burstFrame - 1 ) * st.field() / std::log( anchor / last );
			}

			const bool ok = !clip && usable >= 3 && worst <= 1.0;
			failures += report( ok, quiet,
			                    "droop %-4s tau = %-8s (%.4g s): %d points on exp( -t / tau ), %d well clear of the bound; worst %.3f of "
			                    "its tolerance; fitted tau %.6g s%s",
			                    st.name, cases[ which ], tau, points, usable, worst, fitTau, clip ? " -- CLIPPED" : "" );
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --tilt
//
// A flat field, a dead clamp: across each active line x is constant, so
// y[n] = ( x - s0 ) a^n and the line's last sample over its first is
// a^( W - 1 ) exactly. The tilt across the active width is therefore
// 1 - ( y[W-1] / y[0] )^( W / ( W - 1 ) ), which the spec states as
// 1 - exp( -T_active / tau ). Every row, three tau, both standards.
//---------------------------------------------------------------------------
int runTilt( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures     = 0;
	const double eps = outputBound( 0.0 );
	for( const Stated* sp : kBoth )
	{
		const Stated& st = *sp;
		const char* const cases[] = { "a line", "a field", "a second" };
		const double taus[]       = { st.line, st.field(), 1.0 };
		for( int which = 0; which < 3; ++which )
		{
			Knobs k;
			k.standard = st.option;
			k.coupling = sliderForTau( taus[ which ] );
			const double tau    = statedTau( k.coupling );
			const double stated = 1.0 - std::exp( -st.active() / tau );

			Session s;
			prepare( s, k, perturb );
			std::vector< std::vector< float > > out;
			if( !s.begin( W, H ) || !run( s, 3, [ & ]( int ) { return flat( W, H, 0.8 ); }, out ) )
				return report( false, quiet, "tilt: could not render" );
			s.end();

			double worst = 0.0, meanTilt = 0.0, widest = 0.0;
			bool clip = false;
			const double power = static_cast< double >( W ) / ( W - 1 );
			for( int r = 0; r < H; ++r )
			{
				const double y0 = at( out[ 2 ], W, r, 0 ), y1 = at( out[ 2 ], W, r, W - 1 );
				clip            = clip || clipped( y0 ) || clipped( y1 );
				const double tilt = 1.0 - std::pow( y1 / y0, power );
				//The interval the measurement can be anywhere in, from the bound
				//on each of the two values.
				const double lo = 1.0 - std::pow( ( y1 + eps ) / ( y0 - eps ), power );
				const double hi = 1.0 - std::pow( ( y1 - eps ) / ( y0 + eps ), power );
				const double tol = std::max( hi - tilt, tilt - lo );
				widest           = std::max( widest, tol );
				worst            = std::max( worst, std::fabs( tilt - stated ) / tol );
				meanTilt += tilt / H;
			}
			failures += report( !clip && worst <= 1.0, quiet,
			                    "tilt  %-4s tau = %-8s: %d rows tilt %.6g (stated %.6g); worst %.3f of its tolerance (up to %.2g)%s",
			                    st.name, cases[ which ], H, meanTilt, stated, worst, widest, clip ? " -- CLIPPED" : "" );
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --apl
//
// At the periodic steady state of a dead clamp, s(t) = sum over every past
// interval of x of ( e^( -(t - end) / tau ) - e^( -(t - start) / tau ) ) x,
// the convolution of x with the RC's impulse response, summed over all the
// fields before in closed form. A picture of a fixed patch (x = 0.9, the
// right eighth) and a background at L makes s at the patch linear in L with
// slope kappa = that sum over the background's intervals alone. So the
// patch -- and black, and every level: the stage is linear -- shifts by
// -kappa L. Measured on the patch because black itself is driven below the
// output's floor.
//
// Also: the steady state the plugin primes to on its first frame is the one
// it reaches by running from a discharged capacitor.
//---------------------------------------------------------------------------
int runApl( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures     = 0;
	const double eps = outputBound( 0.0 );
	for( const Stated* sp : kBoth )
	{
		const Stated& st = *sp;
		const int A      = st.activeLines;
		Knobs k;
		k.standard       = st.option;
		k.coupling       = sliderForTau( 1.0 );
		const double tau = statedTau( k.coupling );

		const int P      = W / 8;
		const int col    = W - P;
		const int r      = H / 2;
		const int lm     = lineOfRow( r, H, A );
		const double D   = st.active() / W;
		auto picture     = [ & ]( double L ) {
			std::vector< float > p = flat( W, H, L );
			for( int y = 0; y < H; ++y )
				for( int x = col; x < W; ++x )
				{
					float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
					px[ 0 ] = px[ 1 ] = px[ 2 ] = 0.9f;
				}
			return p;
		};

		//The closed form. Line l's background runs from its active start for
		//( W - P ) samples; the measurement is at the patch's first sample on
		//line lm of this field; every earlier field is the same picture, one
		//field period further back, summed as a geometric series.
		const double tm = lm * st.line + st.sync + st.backPorch + ( W - P ) * D;
		auto response   = [ & ]( double back ) {
			double sum = 0.0;
			for( int l = 0; l < A; ++l )
			{
				const double start = l * st.line + st.sync + st.backPorch;
				const double end   = start + ( W - P ) * D;
				if( end > tm + back + 1e-12 )
					continue;
				sum += std::exp( -( tm + back - end ) / tau ) - std::exp( -( tm + back - start ) / tau );
			}
			return sum;
		};
		const double kappa = response( 0.0 ) + response( st.field() ) / ( 1.0 - std::exp( -st.field() / tau ) );

		const double levels[] = { 0.0, 0.1, 0.2, 0.3, 0.4 };
		double y0 = 0.0, worst = 0.0;
		bool clip = false;
		for( double L : levels )
		{
			Session s;
			prepare( s, k, perturb );
			std::vector< std::vector< float > > out;
			if( !s.begin( W, H ) || !run( s, 3, [ & ]( int ) { return picture( L ); }, out ) )
				return report( false, quiet, "apl: could not render" );
			s.end();
			const double y = at( out[ 2 ], W, r, col );
			clip           = clip || clipped( y );
			if( L == 0.0 )
			{
				y0 = y;
				continue;
			}
			worst = std::max( worst, std::fabs( ( y - y0 ) + kappa * L ) / ( 2.0 * eps ) );
		}
		const double perApl = kappa / ( 1.0 - static_cast< double >( P ) / W );
		const double f      = A * st.active() / st.field();
		failures += report( !clip && worst <= 1.0, quiet,
		                    "apl   %-4s tau = 1 s: black shifts %.6f per unit of background (closed form), %.4f per unit of APL "
		                    "(the active fraction of the field is %.4f); worst %.3f of its tolerance%s",
		                    st.name, kappa, perApl, f, worst, clip ? " -- CLIPPED" : "" );

		//Primed against settled: tau of a field, 60 fields from nothing.
		Knobs q         = k;
		q.coupling      = sliderForTau( st.field() );
		const double tq = statedTau( q.coupling );
		Session primed, settled;
		prepare( primed, q, perturb );
		prepare( settled, q, perturb );
		settled.plugin.SetPrimeForTest( false );
		std::vector< std::vector< float > > outP, outS;
		if( !primed.begin( W, H ) || !settled.begin( W, H ) || !run( primed, 3, [ & ]( int ) { return picture( 0.3 ); }, outP )
		    || !run( settled, 60, [ & ]( int ) { return picture( 0.3 ); }, outS ) )
			return report( false, quiet, "apl: could not render" );
		primed.end();
		settled.end();
		const double tol  = 2.0 * eps + 1.25 * std::exp( -59.0 * st.field() / tq );
		double diff = 0.0;
		for( int y = 0; y < H; y += std::max( 1, H / 16 ) )
			for( int x = 0; x < W; x += std::max( 1, W / 16 ) )
				diff = std::max( diff, static_cast< double >( std::fabs( at( outP[ 2 ], W, y, x ) - at( outS[ 59 ], W, y, x ) ) ) );
		failures += report( diff <= tol, quiet, "apl   %-4s the primed steady state is the settled one: after 59 fields from a discharged capacitor, %.3g apart (bound %.3g)",
		                    st.name, diff, tol );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --clamp
//
// A uniform picture whose level steps at field boundaries (up at field 3,
// down at 4, up at 5, down at 6). Every line of a field carries the same
// picture, so from one line's active start to the next the state moves by
// an affine map u -> A_line u + const, with
//
//   A_line = exp( -( T_line - T_porch ) / tau ) exp( -( 1/tau + 1/tau_c ) T_porch )
//          = exp( -T_line / tau ) exp( -T_porch / tau_c )
//
// -- the second factor being the switch's closed form over the porch: the
// residual it leaves. After a step the line starts converge on the new
// fixed point geometrically at that rate. With the fixed point unknown,
// three lines d apart give it anyway: ( y[l+2d] - y[l+d] ) / ( y[l+d] - y[l] )
// = A_line^d. The coupling is short (1 ms) so that a line moves the state
// enough to see; T_line / tau is exact and known, so what is left is the
// switch. Three healths, from 2% of a time constant per porch to two.
//
// At the strongest the map has converged by the bottom of the field, and
// where it settles -- y = g - u* + r, black itself at r - u*, with u* the
// line map's fixed point in closed form -- is checked too: that is the
// Clamp Reference doing its job.
//---------------------------------------------------------------------------
int runClampCheck( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures     = 0;
	const double eps = outputBound( 0.0 );
	struct Case
	{
		double perPorch, low, high, ref;//the levels and reference keep every reading off the floor and ceiling
	};
	const Case cases[] = { { 0.02, 0.4, 0.6, 0.3 }, { 0.2, 0.2, 0.8, 0.1 }, { 2.0, 0.1, 0.9, 0.05 } };
	for( const Stated* sp : kBoth )
	{
		const Stated& st = *sp;
		const int A      = st.activeLines;
		for( const Case& cs : cases )
		{
			Knobs k;
			k.standard       = st.option;
			k.coupling       = sliderForTau( 1e-3 );
			k.health         = sliderForPerPorch( cs.perPorch );
			k.reference      = sliderForReference( cs.ref );
			const double tau = statedTau( k.coupling );
			const double n   = statedPerPorch( k.health );
			const double r   = statedReference( k.reference );
			auto level       = [ & ]( int f ) { return f < 3 ? cs.low : ( f % 2 ? cs.high : cs.low ); };

			Session s;
			prepare( s, k, perturb );
			std::vector< std::vector< float > > out;
			const int frames = 7;
			if( !s.begin( W, H ) || !run( s, frames, [ & ]( int f ) { return flat( W, H, level( f ) ); }, out ) )
				return report( false, quiet, "clamp: could not render" );
			s.end();

			const double perLine = st.line / tau + n;
			int triples = 0;
			double worst = 0.0, fitted = 0.0;
			bool clip = false;
			std::map< int, double > y;//the first active sample of every displayed line
			for( int f = 3; f < frames; ++f )
			{
				y.clear();
				for( int row = 0; row < H; ++row )
				{
					const double v = at( out[ f ], W, row, 0 );
					clip           = clip || clipped( v );
					y[ lineOfRow( row, H, A ) ] = v;
				}
				for( const auto& first : y )
				{
					const int p = first.first;
					for( int d = 1; d <= 8; ++d )
					{
						if( !y.count( p + d ) || !y.count( p + 2 * d ) )
							continue;
						const double d1 = y[ p + d ] - first.second;
						const double d2 = y[ p + 2 * d ] - y[ p + d ];
						if( std::fabs( d1 ) < 100.0 * eps )
							break;
						const double ratio  = d2 / d1;
						const double stated = std::exp( -d * perLine );
						const double tol    = ( 2.0 * eps + std::fabs( ratio ) * 2.0 * eps ) / ( std::fabs( d1 ) - 2.0 * eps );
						worst               = std::max( worst, std::fabs( ratio - stated ) / tol );
						if( triples == 0 )
							fitted = -std::log( ratio ) / d - st.line / tau;
						++triples;
						break;
					}
				}
			}
			failures += report( !clip && triples >= 3 && worst <= 1.0, quiet,
			                    "clamp %-4s %.2g of tau_c per porch: %d line triples step by exp( -T_line/tau - T_porch/tau_c ); "
			                    "residual per porch %.6f (stated %.6f); worst %.3f of its tolerance%s",
			                    st.name, n, triples, std::exp( -fitted ), std::exp( -n ), worst, clip ? " -- CLIPPED" : "" );

			if( cs.perPorch >= 2.0 )
			{
				//Where it settles, in closed form: from one active start to the
				//next, u goes to g + ( u - g ) e_a over the active line, decays
				//through the front porch and sync, and through the porch with
				//the switch closed. Its fixed point is u*.
				const double g      = level( frames - 1 );
				const double alpha  = std::exp( -( 1.0 / tau + n / st.backPorch ) * st.backPorch );
				const double ea     = std::exp( -st.active() / tau );
				const double dfs    = std::exp( -( st.frontPorch + st.sync ) / tau );
				const double uStar  = alpha * dfs * g * ( 1.0 - ea ) / ( 1.0 - alpha * dfs * ea );
				const double yInf   = g - uStar + r;
				const int lastLine  = y.rbegin()->first;
				const double left   = std::exp( -lastLine * perLine ) * 1.0;
				const double tol    = eps + left;
				const double miss   = std::fabs( y.rbegin()->second - yInf );
				failures += report( miss <= tol, quiet,
				                    "clamp %-4s black settles at %.7f (the reference %.2f, less %.2g the coupling lets through), "
				                    "stated %.7f: off by %.2g, bound %.2g",
				                    st.name, y.rbegin()->second - g, r, uStar, yInf - g, miss, tol );
			}
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --reference
//
// Every output pixel of every frame against the serial double run, with the
// picture changing every frame, over four settings that between them take in
// both standards, luma and per channel, a dead clamp, a weak one and a
// strong one with a reference off zero, the supply sagging, and host rates
// below the field rate (fields the host never sees) and above it (fields
// shown twice).
//---------------------------------------------------------------------------
struct Setting
{
	const char* name;
	Knobs knobs;
	int64_t fpsNum, fpsDen;
};

std::vector< Setting > referenceSettings()
{
	std::vector< Setting > list;
	{
		Setting s{ "PAL luma, tau a line, dead clamp, 60 fps", {}, 60, 1 };
		s.knobs.coupling = sliderForTau( 64e-6 );
		list.push_back( s );
	}
	{
		Setting s{ "PAL per channel, tau a field, 0.2/porch to +0.2, 50 fps", {}, 50, 1 };
		s.knobs.perChannel = true;
		s.knobs.coupling   = sliderForTau( 0.02 );
		s.knobs.health     = sliderForPerPorch( 0.2 );
		s.knobs.reference  = sliderForReference( 0.2 );
		list.push_back( s );
	}
	{
		Setting s{ "NTSC per channel, tau 1 s, weak clamp to -0.1, sag, 50 fps", {}, 50, 1 };
		s.knobs.standard   = 1;
		s.knobs.perChannel = true;
		s.knobs.coupling   = sliderForTau( 1.0 );
		s.knobs.health     = sliderForPerPorch( 0.002 );
		s.knobs.reference  = sliderForReference( -0.1 );
		s.knobs.sagDepth   = 0.6f;
		s.knobs.sagAttack  = sliderForSagSeconds( 0.02 );
		s.knobs.sagRecover = sliderForSagSeconds( 0.3 );
		list.push_back( s );
	}
	{
		Setting s{ "NTSC luma, tau 5 ms, 2/porch to +0.1, full sag, 144 fps", {}, 144, 1 };
		//6 ms: the shortest the slider reaches is 10^-2.3 = 5.01 ms. Asking
		//for 5 ms once found the stated law missing the plugin's clamp.
		s.knobs.standard   = 1;
		s.knobs.coupling   = sliderForTau( 0.005 );
		s.knobs.health     = sliderForPerPorch( 2.0 );
		s.knobs.reference  = sliderForReference( 0.1 );
		s.knobs.sagDepth   = 1.0f;
		s.knobs.sagAttack  = sliderForSagSeconds( 0.006 );
		s.knobs.sagRecover = sliderForSagSeconds( 0.1 );
		list.push_back( s );
	}
	return list;
}

int runReference( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( const Setting& setting : referenceSettings() )
	{
		const RefSettings cfg = statedFrom( setting.knobs );
		const Stated& st      = *cfg.st;
		const double eps      = outputBound( cfg.depth );

		Session s;
		prepare( s, setting.knobs, perturb );
		s.rate( setting.fpsNum, setting.fpsDen );
		Reference ref;
		ref.cfg = cfg;
		ref.W   = W;
		ref.H   = H;
		if( !s.begin( W, H ) )
			return report( false, quiet, "reference: could not render" );

		double worst = 0.0;
		size_t compared = 0;
		bool fieldsAgree = true;
		for( int f = 0; f < 10; ++f )
		{
			//Bright and dark frames in turn, so the supply has something to do.
			const std::vector< float > pic = noise( W, H, static_cast< uint32_t >( 17 + f ), ( f / 3 ) % 2 ? 0.4 : 1.0, ( f / 3 ) % 2 ? 0.6 : 0.0 );
			if( !s.render( f, pic ) )
				return report( false, quiet, "reference: could not render" );
			const int64_t n = fieldOfFrame( f, setting.fpsNum, setting.fpsDen, st );
			ref.frame( n, pic );
			fieldsAgree = fieldsAgree && s.plugin.ShownFieldForTest() == n;
			fieldsAgree = fieldsAgree && std::fabs( s.plugin.ShownSupplyForTest() - ref.shownE ) <= 1.01 * ( model::kBlock + 3 ) * kU;
			const std::vector< float > out = s.readBackFloat();
			for( int r = 0; r < H; ++r )
				for( int c = 0; c < W; ++c )
					for( int ch = 0; ch < 3; ++ch )
					{
						const double d = std::fabs( at( out, W, r, c, ch ) - ref.expected( pic, r, c, ch ) );
						worst          = std::max( worst, d );
						++compared;
					}
		}
		s.end();
		failures += report( fieldsAgree && worst <= eps, quiet,
		                    "reference %-58s %zu values, worst %.3g = %.1f u (bound %.1f u)%s", setting.name, compared, worst,
		                    worst / kU, eps / kU, fieldsAgree ? "" : " -- the plugin showed a different field or supply" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --continuity
//
// The same material, cut into host frames at six rates -- the field rate,
// faster, much faster, a rate sharing nothing with it, and two slower ones
// that leave fields unseen -- gives the trajectory of one continuous serial
// run. The material is a function of the field on show, so every rate sees
// the same pictures on the same fields. A dead clamp and tau of a field, so
// the state carries a lot from one frame to the next.
//---------------------------------------------------------------------------
int runContinuity( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures     = 0;
	const double eps = outputBound( 0.0 );
	struct Rate
	{
		int64_t num, den;
		const char* name;
	};
	const Rate palRates[]  = { { 50, 1, "50" }, { 60, 1, "60" }, { 713, 10, "71.3" }, { 144, 1, "144" }, { 30000, 1001, "29.97" }, { 24000, 1001, "23.976" } };
	const Rate ntscRates[] = { { 60000, 1001, "59.94" }, { 60, 1, "60" }, { 50, 1, "50" }, { 24000, 1001, "23.976" } };

	for( const Stated* sp : kBoth )
	{
		const Stated& st = *sp;
		const int A      = st.activeLines;
		Knobs k;
		k.standard = st.option;
		k.coupling = sliderForTau( st.field() );
		const RefSettings cfg = statedFrom( k );

		auto material = [ & ]( int64_t n ) {
			//Levels close enough that black pushed down by the bright stretch
			//stays above the output's floor: the low level must exceed the
			//high one times the field's active fraction (0.75).
			std::vector< float > p = flat( W, H, ( n / 7 ) % 2 ? 0.85 : 0.7 );
			if( n % 5 == 0 )
				for( int l = A / 3; l < A / 2; ++l )
					paintRow( p, W, rowOfLine( l, H, A ), 0.95 );
			return p;
		};
		const int rows[] = { H / 5, H / 2, ( 4 * H ) / 5 };
		const int cols[] = { 0, W / 2, W - 1 };

		const Rate* rates = st.option == 0 ? palRates : ntscRates;
		const int count   = st.option == 0 ? 6 : 4;
		for( int i = 0; i < count; ++i )
		{
			const Rate& rate = rates[ i ];
			Session s;
			prepare( s, k, perturb );
			s.rate( rate.num, rate.den );
			Reference ref;
			ref.cfg = cfg;
			ref.W   = W;
			ref.H   = H;
			if( !s.begin( W, H ) )
				return report( false, quiet, "continuity: could not render" );

			const double seconds = 1.2;
			const int frames     = static_cast< int >( seconds * rate.num / rate.den );
			double worst = 0.0;
			bool clip = false, fieldsAgree = true;
			std::set< int64_t > fields;
			for( int f = 0; f < frames; ++f )
			{
				const int64_t n                = fieldOfFrame( f, rate.num, rate.den, st );
				const std::vector< float > pic = material( n );
				if( !s.render( f, pic ) )
					return report( false, quiet, "continuity: could not render" );
				ref.frame( n, pic );
				fieldsAgree = fieldsAgree && s.plugin.ShownFieldForTest() == n;
				fields.insert( n );
				const std::vector< float > out = s.readBackFloat();
				for( int r : rows )
					for( int c : cols )
					{
						const double v = at( out, W, r, c );
						clip           = clip || clipped( v );
						worst          = std::max( worst, std::fabs( v - ref.expected( pic, r, c, 0 ) ) );
					}
			}
			s.end();
			failures += report( !clip && fieldsAgree && worst <= eps, quiet,
			                    "continuity %-4s host at %-6s fps: %d frames showing %zu of %lld fields on the continuous run's "
			                    "trajectory, worst %.3g (bound %.3g)%s",
			                    st.name, rate.name, frames, fields.size(), static_cast< long long >( *fields.rbegin() + 1 ), worst, eps,
			                    clip ? " -- CLIPPED" : ( fieldsAgree ? "" : " -- a different field shown" ) );
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --sag
//
// A perfect clamp and a long coupling, so the signal is the picture; a fixed
// patch and a background that steps from dark to bright at field 10 and back
// at field 40. The gain on the patch, out( depth ) / out( 0 ), is
// 1 - depth e, and e -- the supply's memory of APL -- follows
//
//   e = APL_new + ( e_step - APL_new ) exp( -( n - n_step ) T_field / tau )
//
// with tau the attack while it rises and the recovery while it falls. The
// field on show carries the supply as it stood at the field's start.
//---------------------------------------------------------------------------
int runSag( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( const Stated* sp : kBoth )
	{
		const Stated& st = *sp;
		Knobs k;
		k.standard   = st.option;
		k.coupling   = 1.0f;
		k.health     = 1.0f;
		k.sagDepth   = 1.0f;
		k.sagAttack  = sliderForSagSeconds( 0.04 );
		k.sagRecover = sliderForSagSeconds( 0.2 );
		const double depth    = statedDepth( k.sagDepth );
		const double attack   = statedSagSeconds( k.sagAttack );
		const double recovery = statedSagSeconds( k.sagRecover );
		const double eps      = outputBound( depth );
		const double eps0     = outputBound( 0.0 );

		const int P        = W / 8;
		const int up = 10, down = 40, frames = 90;
		auto bg            = [ & ]( int f ) { return f >= up && f < down ? 0.9 : 0.1; };
		auto picture       = [ & ]( int f ) {
			std::vector< float > p = flat( W, H, bg( f ) );
			for( int y = 0; y < H; ++y )
				for( int x = 0; x < P; ++x )
				{
					float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
					px[ 0 ] = px[ 1 ] = px[ 2 ] = 0.5f;
				}
			return p;
		};
		auto apl = [ & ]( int f ) { return ( P * 0.5 + ( W - P ) * bg( f ) ) / W; };

		Session sagged, flatSupply;
		prepare( sagged, k, perturb );
		Knobs none = k;
		none.sagDepth = 0.0f;
		prepare( flatSupply, none, perturb );
		std::vector< std::vector< float > > outS, outN;
		if( !sagged.begin( W, H ) || !flatSupply.begin( W, H ) || !run( sagged, frames, picture, outS ) || !run( flatSupply, frames, picture, outN ) )
			return report( false, quiet, "sag: could not render" );
		sagged.end();
		flatSupply.end();

		const int r = H / 2, c = P / 2;
		double worst = 0.0, eStep = apl( 0 );
		double fitA = 0.0, fitR = 0.0;
		for( int f = 0; f < frames; ++f )
		{
			double e;
			if( f <= up )
				e = apl( 0 );
			else if( f <= down )
				e = apl( up ) + ( apl( 0 ) - apl( up ) ) * std::exp( -( f - up ) * st.field() / attack );
			else
				e = apl( down ) + ( eStep - apl( down ) ) * std::exp( -( f - down ) * st.field() / recovery );
			if( f == down )
				eStep = e;

			const double yS = at( outS[ f ], W, r, c ), yN = at( outN[ f ], W, r, c );
			const double gain   = yS / yN;
			const double stated = 1.0 - depth * e;
			//Each value within its bound; the APL's own float error in the
			//gain is in eps (the sag term).
			const double tol = ( eps + gain * eps0 ) / ( yN - eps0 );
			worst            = std::max( worst, std::fabs( gain - stated ) / tol );

			if( f == up + 3 )
				fitA = 3.0 * st.field() / std::log( ( apl( up ) - apl( 0 ) ) / ( apl( up ) - ( 1.0 - gain ) / depth ) );
			if( f == down + 5 )
				fitR = 5.0 * st.field() / std::log( ( eStep - apl( 0 ) ) / ( ( 1.0 - gain ) / depth - apl( 0 ) ) );
		}
		failures += report( worst <= 1.0, quiet,
		                    "sag   %-4s depth %.2f: %d fields on the attack (%.3g s) and recovery (%.3g s) curves; fitted %.4g s and %.4g s; "
		                    "worst %.3f of its tolerance",
		                    st.name, depth, frames, attack, recovery, fitA, fitR, worst );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --identity
//
// A perfect clamp (e^-20 left per porch), no sag, no triode, the reference
// at zero: what comes out is the input, within
//
//   ( 1 - exp( -T_active / tau ) )   the line's own drift toward the bias
//                                    after the clamp has let go of it
//   + 1.25 e^( -20 - T_porch / tau ) what the porch leaves of the worst state
//   + the float bound
//
// Random colour, both standards, luma and per channel.
//---------------------------------------------------------------------------
int runIdentity( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( const Stated* sp : kBoth )
		for( bool perChannel : { false, true } )
		{
			const Stated& st = *sp;
			Knobs k;
			k.standard       = st.option;
			k.perChannel     = perChannel;
			k.coupling       = 1.0f;
			k.health         = 1.0f;
			const double tau = statedTau( k.coupling );
			const double n   = statedPerPorch( k.health );
			const double bound = ( 1.0 - std::exp( -st.active() / tau ) ) + 1.25 * std::exp( -n - st.backPorch / tau ) + outputBound( 0.0 );

			Session s;
			prepare( s, k, perturb );
			s.rate( 60, 1 );
			if( !s.begin( W, H ) )
				return report( false, quiet, "identity: could not render" );
			double worst = 0.0;
			for( int f = 0; f < 3; ++f )
			{
				const std::vector< float > pic = noise( W, H, static_cast< uint32_t >( 101 + f ) );
				if( !s.render( f, pic ) )
					return report( false, quiet, "identity: could not render" );
				const std::vector< float > out = s.readBackFloat();
				for( size_t i = 0; i < out.size(); ++i )
					worst = std::max( worst, static_cast< double >( std::fabs( out[ i ] - pic[ i ] ) ) );
			}
			s.end();
			failures += report( worst <= bound, quiet, "identity %-4s %-11s the input to within %.3g (bound %.3g, of which the line's drift %.3g)",
			                    st.name, perChannel ? "per channel" : "luma", worst, bound, 1.0 - std::exp( -st.active() / tau ) );
		}
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//
// The host's picture doubles in size mid-run. The capacitor does not know:
// with a picture that is uniform along every line, the state at each line's
// active start does not depend on the width at all (x is constant over the
// active interval however it is sampled). So the black level at the start of
// a line follows the serial run straight through the resize.
//---------------------------------------------------------------------------
int runResize( int W, int H, int perturb = 0, bool quiet = false )
{
	int failures     = 0;
	const double eps = outputBound( 0.0 );
	for( const Stated* sp : kBoth )
	{
		const Stated& st = *sp;
		const int A      = st.activeLines;
		Knobs k;
		k.standard = st.option;
		k.coupling = sliderForTau( 0.1 );
		const RefSettings cfg = statedFrom( k );

		auto level = [ & ]( int f ) { return ( f / 6 ) % 2 ? 0.85 : 0.7; };
		Session s;
		prepare( s, k, perturb );
		Reference ref;
		ref.cfg = cfg;
		ref.W   = W;
		ref.H   = H;
		if( !s.begin( W, H ) )
			return report( false, quiet, "resize: could not render" );

		double worst = 0.0;
		int after = 0;
		bool clip = false;
		for( int f = 0; f < 40; ++f )
		{
			if( f == 20 )
				s.resize( 2 * W, 2 * H );
			const int w = s.width, h = s.height;
			if( !s.render( f, flat( w, h, level( f ) ) ) )
				return report( false, quiet, "resize: could not render" );
			ref.frame( f, flat( W, H, level( f ) ) );
			const std::vector< float > out = s.readBackFloat();
			for( int r : { h / 4, h / 2, ( 3 * h ) / 4 } )
			{
				const int l          = lineOfRow( r, h, A );
				const double v       = at( out, w, r, 0 );
				const double stated  = level( f ) - ref.stateAt( l, 0, 0 );
				clip                 = clip || clipped( v );
				worst                = std::max( worst, std::fabs( v - stated ) );
			}
			if( f >= 20 )
				++after;
		}
		s.end();
		failures += report( !clip && worst <= eps, quiet,
		                    "resize %-4s %dx%d to %dx%d at field 20: %d fields after it on the serial run, worst %.3g (bound %.3g)%s", st.name, W,
		                    H, 2 * W, 2 * H, after, worst, eps, clip ? " -- CLIPPED" : "" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --model (no GL)
//
// The plugin's arithmetic against the statements above: the two standards'
// timing, every control law, and the plugin's blocked walk -- fed block sums
// the harness computes in double from its own statement of P -- against the
// serial per-sample walk at every block start, plus the field map and its
// fixed point, and the supply's step.
//---------------------------------------------------------------------------
int runModel( int perturb = 0, bool quiet = false )
{
	int failures = 0;

	//Timing.
	for( const Stated* sp : kBoth )
	{
		const model::Standard& m = model::StandardOf( sp->option );
		const bool same = m.line == sp->line && m.frontPorch == sp->frontPorch && m.sync == sp->sync && m.backPorch == sp->backPorch
		                  && m.activeLines == sp->activeLines && m.fieldLines == sp->fieldLines && m.rateNum == sp->rateNum
		                  && m.rateDen == sp->rateDen && m.BlankLines() == sp->blankLines();
		const double fieldErr = std::fabs( sp->fieldLines * sp->line - sp->field() ) / sp->field();
		const double activeErr = std::fabs( m.Active() - sp->active() );
		failures += report( same && fieldErr < 1e-15 && activeErr < 1e-18, quiet,
		                    "model %-4s timing as stated: line %.4f us, active %.4f us, %d + %d lines + %.1f, a field is %.1f lines = %.6f ms "
		                    "(%.1e off)",
		                    sp->name, m.line * 1e6, m.Active() * 1e6, m.activeLines, m.BlankLines(), m.Tail() / m.line, m.fieldLines,
		                    m.Field() * 1e3, fieldErr );
	}

	//Control laws.
	{
		double worst = 0.0;
		for( int i = 0; i <= 20; ++i )
		{
			const float v = static_cast< float >( i ) / 20.0f;
			auto rel      = []( double a, double b ) { return b == 0.0 ? std::fabs( a ) : std::fabs( a - b ) / std::fabs( b ); };
			worst = std::max( { worst, rel( clampfx::controls::CouplingSeconds( v ), statedTau( v ) ),
			                    rel( clampfx::controls::ClampPerPorch( v ), statedPerPorch( v ) ),
			                    rel( clampfx::controls::ClampRate( v, model::StandardOf( 0 ) ), statedPerPorch( v ) / kPal.backPorch ),
			                    rel( clampfx::controls::ReferenceLevel( v ), statedReference( v ) ),
			                    rel( clampfx::controls::SagDepth( v ), statedDepth( v ) ),
			                    rel( clampfx::controls::SagSeconds( v ), statedSagSeconds( v ) ) } );
		}
		failures += report( worst < 1e-12, quiet, "model controls: Coupling, Clamp Health, Clamp Reference, Sag Depth and the sag times as stated at 21 "
		                                           "points, worst %.2g relative", worst );
	}

	//The walk.
	struct Case
	{
		const Stated* st;
		int W;
		double tau, perPorch, ref;
	};
	const Case cases[] = {
		{ &kPal, 320, 64e-6, 0.0, 0.0 },      { &kPal, 333, 0.02, 0.2, 0.2 },   { &kPal, 1280, 1.0, 2.0, -0.25 },
		{ &kNtsc, 320, 0.005, 0.002, 0.7 }, { &kNtsc, 1920, 64e-6, 20.0, 0.0 },
	};
	double worstState = 0.0, worstMap = 0.0, worstFixed = 0.0;
	for( const Case& cs : cases )
	{
		const Stated& st = *cs.st;
		const int W = cs.W, H = st.activeLines, A = st.activeLines;
		RefSettings cfg;
		cfg.st   = &st;
		cfg.tau  = cs.tau;
		cfg.rate = cs.perPorch / st.backPorch;
		cfg.ref  = cs.ref;
		Reference ref;
		ref.cfg = cfg;
		ref.W   = W;
		ref.H   = H;
		const std::vector< float > pic = noise( W, H, static_cast< uint32_t >( W + A ) );

		//P, stated: a block of m samples moves s to a^m s + ( 1 - a ) P,
		//P = sum_j a^( m - 1 - j ) x[ j ] -- which is what the recurrence
		//unrolled over the block says.
		const double a  = std::exp( -( st.active() / W ) / cs.tau );
		const int m     = model::kBlock;
		const int blocks = ( W + m - 1 ) / m;
		std::vector< double > sums( static_cast< size_t >( A ) * blocks * 4, 0.0 );
		for( int l = 0; l < A; ++l )
			for( int b = 0; b < blocks; ++b )
			{
				const int count = std::min( m, W - b * m );
				for( int j = 0; j < count; ++j )
				{
					const float* px = pic.data() + ( static_cast< size_t >( rowOfLine( l, H, A ) ) * W + b * m + j ) * 4;
					for( int c = 0; c < 3; ++c )
						sums[ ( static_cast< size_t >( l ) * blocks + b ) * 4 + c ] += std::pow( a, count - 1 - j ) * px[ c ];
					sums[ ( static_cast< size_t >( l ) * blocks + b ) * 4 + 3 ] += 0.299 * px[ 0 ] + 0.587 * px[ 1 ] + 0.114 * px[ 2 ];
				}
			}

		model::Settings settings;
		settings.tau       = cs.tau;
		settings.clampRate = cfg.rate;
		settings.perturb   = perturb;
		const model::Timeline t = model::MakeTimeline( model::StandardOf( st.option ), W, settings );
		model::FieldWalk walk;
		model::WalkField( t, sums.data(), walk );

		const double start[ 3 ] = { 0.3, -0.2, 0.9 };
		double st3[ 3 ]         = { start[ 0 ], start[ 1 ], start[ 2 ] };
		std::vector< double > serial;
		ref.walk( pic, st3, &serial );
		for( int l = 0; l < A; ++l )
			for( int b = 0; b < blocks; ++b )
				for( int c = 0; c < 3; ++c )
				{
					//The plugin holds u = s + r.
					const size_t i      = static_cast< size_t >( l ) * blocks + b;
					const double plugin = walk.blockA[ i ] * ( start[ c ] + cs.ref ) + walk.blockB[ i * 3 + c ] - cs.ref;
					worstState          = std::max( worstState, std::fabs( plugin - serial[ ( static_cast< size_t >( l ) * 3 + c ) * W + b * m ] ) );
				}
		for( int c = 0; c < 3; ++c )
			worstMap = std::max( worstMap, std::fabs( model::Advance( walk, c, start[ c ] + cs.ref ) - cs.ref - st3[ c ] ) );

		Reference primed = ref;
		primed.prime( pic );
		for( int c = 0; c < 3; ++c )
			worstFixed = std::max( worstFixed, std::fabs( model::SteadyState( walk, c ) - cs.ref - primed.s[ c ] ) );
		worstFixed = std::max( worstFixed, std::fabs( walk.apl - ref.apl( pic ) ) );
	}
	failures += report( worstState < 1e-12, quiet, "model walk: the blocked walk equals the serial one at every block start of five fields "
	                                                "(two standards, 320 to 1920 wide, tau 64 us to 1 s, dead to perfect), worst %.2g",
	                    worstState );
	failures += report( worstMap < 1e-12, quiet, "model walk: the field map A s + B equals a serial field, worst %.2g", worstMap );
	failures += report( worstFixed < 1e-12, quiet, "model walk: the steady state and the APL equal the serial run's, worst %.2g", worstFixed );

	//The supply.
	{
		model::Sag sag;
		sag.depth    = 0.5;
		sag.attack   = 0.04;
		sag.recovery = 0.3;
		double e = 0.1, worst = 0.0;
		const double T = kPal.field();
		for( int n = 0; n < 60; ++n )
		{
			const double level  = n < 20 ? 0.8 : 0.2;
			const double tau    = level > e ? sag.attack : sag.recovery;
			const double stated = level + ( e - level ) * std::exp( -T / tau );
			const double plugin = model::SagStep( e, level, T, sag, perturb );
			worst               = std::max( worst, std::fabs( plugin - stated ) );
			e                   = stated;
		}
		failures += report( worst < 1e-15, quiet, "model sag: the supply's step attacks rising and recovers falling, worst %.2g", worst );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --clock (no GL): a six-day millisecond host clock decides the same fields
// as a fresh seconds one, and both the fields exact rational arithmetic
// says -- including every host frame that lands exactly on a field start
// (every sixth at 60 fps against PAL).
//---------------------------------------------------------------------------
int runClock( int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( const Stated* sp : kBoth )
	{
		clampfx::Clock seconds, millis;
		seconds.SetScaleForTest( 1.0 );
		millis.SetScaleForTest( 0.001 );
		millis.SetFloatForTest( ( perturb & model::kPerturbClockFloat ) != 0 );
		const double origin = 499000000.0 + 6.0 * 86400.0 * 1000.0;//ms: Resolume's measured clock, plus six days
		int differ = 0;
		for( int64_t k = 0; k < 3000; ++k )
		{
			seconds.Update( static_cast< double >( k ) / 60.0 );
			millis.Update( origin + static_cast< double >( k ) * 1000.0 / 60.0 );
			const model::Standard& m = model::StandardOf( sp->option );
			const int64_t a          = model::FieldAt( seconds.Now(), m );
			const int64_t b          = model::FieldAt( millis.Now(), m );
			if( a != b || a != fieldOfFrame( k, 60, 1, *sp ) )
				++differ;
		}
		failures += report( differ == 0, quiet, "clock %-4s a six-day millisecond clock, a fresh seconds clock and exact arithmetic: %d of 3000 frames disagree on the field",
		                    sp->name, differ );
	}
	return failures;
}

int runNames( bool quiet = false )
{
	Clamp plugin;
	std::set< std::string > seen;
	int bad = 0;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name.size() > 16 || !seen.insert( p.name ).second )
			++bad;

	//The name the host reads: plugMain's info block, 16 bytes, not
	//null-terminated.
	const FFMixed info            = plugMain( FF_GET_INFO, FFMixed{ 0 }, 0 );
	const PluginInfoStruct* block = static_cast< const PluginInfoStruct* >( info.PointerValue );
	std::string name, id;
	if( block )
	{
		name.assign( block->PluginName, strnlen( block->PluginName, 16 ) );
		id.assign( block->PluginUniqueID, 4 );
	}
	return report( bad == 0 && name == "SW Clamp" && id == "CL01" && block->PluginType == FF_EFFECT, quiet,
	               "names: %zu parameters, unique and within 16 characters; the host reads '%s' / %s / %s", seen.size(), name.c_str(),
	               id.c_str(), block && block->PluginType == FF_EFFECT ? "effect" : "not an effect" );
}

//---------------------------------------------------------------------------
// --negative: every check above can fail.
//---------------------------------------------------------------------------
struct NegativeControl
{
	const char* what;
	int failuresSeen;
};

int summariseNegatives( const std::vector< NegativeControl >& controls )
{
	int failures = 0;
	for( const NegativeControl& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		++g_checks;
		std::printf( "negative %-60s %s  %s\n", c.what, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
		{
			++failures;
			++g_failures;
		}
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

/// Run a check against a perturbation quietly, and report whether it failed
/// -- without counting its failures as the run's.
template< typename F >
int caught( F&& check )
{
	const int checks = g_checks, failures = g_failures;
	const int seen   = check();
	g_checks         = checks;
	g_failures       = failures;
	return seen;
}

int runNegativeOffline()
{
	return summariseNegatives( {
		{ "model: blanking dropped from the timeline", caught( [] { return runModel( model::kPerturbNoBlanking, true ); } ) },
		{ "model: the sample period from the whole line", caught( [] { return runModel( model::kPerturbLineAsActive, true ); } ) },
		{ "model: the clamp closed for half the porch", caught( [] { return runModel( model::kPerturbHalfPorch, true ); } ) },
		{ "model: the block decay one sample short", caught( [] { return runModel( model::kPerturbBlockDecay, true ); } ) },
		{ "model: attack and recovery swapped", caught( [] { return runModel( model::kPerturbSagSwap, true ); } ) },
		{ "model: the clamp never closes", caught( [] { return runModel( model::kPerturbNoClamp, true ); } ) },
		{ "clock: kept in float", caught( [] { return runClock( model::kPerturbClockFloat, true ); } ) },
	} );
}

int runNegative( int W, int H )
{
	return summariseNegatives( {
		{ "droop: blanking dropped from the timeline (the spec's)", caught( [ & ] { return runDroop( W, H, model::kPerturbNoBlanking, true ); } ) },
		{ "tilt: the sample period from the whole line", caught( [ & ] { return runTilt( W, H, model::kPerturbLineAsActive, true ); } ) },
		{ "apl: blanking dropped from the timeline", caught( [ & ] { return runApl( W, H, model::kPerturbNoBlanking, true ); } ) },
		{ "clamp: the switch closed for half the porch", caught( [ & ] { return runClampCheck( W, H, model::kPerturbHalfPorch, true ); } ) },
		{ "reference: the block decay one sample short", caught( [ & ] { return runReference( W, H, model::kPerturbBlockDecay, true ); } ) },
		{ "continuity: the state reset every host frame (the spec's)", caught( [ & ] { return runContinuity( W, H, model::kPerturbResetEachFrame, true ); } ) },
		{ "sag: attack and recovery swapped", caught( [ & ] { return runSag( W, H, model::kPerturbSagSwap, true ); } ) },
		{ "identity: the clamp never closes", caught( [ & ] { return runIdentity( W, H, model::kPerturbNoClamp, true ); } ) },
		{ "resize: a resize re-primes the state", caught( [ & ] { return runResize( W, H, model::kPerturbResizeReprime, true ); } ) },
	} );
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep, the bench and a default --pipe. It
// has to change its average level -- a coupling does nothing to a picture
// that never changes but shift it -- and carry colour, for Per Channel.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t    = static_cast< double >( frame ) / 60.0;
	const bool flash  = std::fmod( t, 0.4 ) < 0.2;
	const double barX = std::fmod( 40.0 + 240.0 * t, static_cast< double >( width ) );
	const unsigned char bars[ 7 ][ 3 ] = { { 191, 191, 191 }, { 191, 191, 0 }, { 0, 191, 191 }, { 0, 191, 0 },
		                                   { 191, 0, 191 },   { 191, 0, 0 },   { 0, 0, 191 } };
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 16, g = 16, b = 16;
			if( fy < 0.3 )
			{
				const unsigned char* c = bars[ std::min( 6, x * 7 / width ) ];
				r = c[ 0 ];
				g = c[ 1 ];
				b = c[ 2 ];
			}
			else if( fy < 0.45 )
				r = g = b = 255.0 * fx;//a ramp
			else if( fx > 0.3 && fx < 0.7 && fy > 0.5 && fy < 0.85 )
				r = g = b = flash ? 235.0 : 16.0;//the block that cuts
			else if( fy > 0.9 )
			{
				r = 200;
				g = 60;
				b = 30;
			}
			if( std::fabs( x + 0.5 - barX ) < std::max( 2.0, width / 120.0 ) )
				r = g = b = 250.0;
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ] = static_cast< unsigned char >( r );
			px[ 1 ] = static_cast< unsigned char >( g );
			px[ 2 ] = static_cast< unsigned char >( b );
			px[ 3 ] = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		applySetting( session.plugin, setting, error );
	}
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is built once per frame of a short loop and uploaded ahead, so
	//the upload is not in the figure; the plugin still sees a moving picture.
	std::vector< std::vector< unsigned char > > loop;
	for( int i = 0; i < 4; ++i )
		loop.push_back( buildCard( width, height, i * 7 ) );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, loop[ static_cast< size_t >( frame ) % loop.size() ] );
	glFinish();

	//Best of three: the GPU is shared with other builds on this machine.
	double best = 1e9;
	int64_t frame = warmup;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int i = 0; i < frames; ++i, ++frame )
			session.renderAt( frame );
		glFinish();
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best                 = std::min( best, seconds * 1000.0 / frames );
	}
	session.end();
	return best;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720  ", 1280, 720 }, { "1920x1080 ", 1920, 1080 }, { "3840x2160 ", 3840, 2160 } };
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   %% of a 60fps frame   Per Channel + Triode\n" );
	std::vector< std::string > heavy = settings;
	heavy.push_back( "Per Channel=1" );
	heavy.push_back( "Triode On=1" );
	for( const Size& size : sizes )
	{
		const double ms = benchAt( settings, size.width, size.height, frames );
		const double hv = benchAt( heavy, size.width, size.height, frames );
		std::printf( "%s    %7.3f        %5.1f%%             %7.3f\n", size.name, ms, ms / 16.667 * 100.0, hv );
	}
	std::printf( "\nEach frame: one pass of block sums read back to the CPU (a stall, by\n"
	             "design), the walk in double, one fill pass at the field's lines, the display.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace sh = clampfx::shaders;
	const std::pair< const char*, const char* > files[] = {
		{ "vertex.vert", sh::kVertex }, { "sums.frag", sh::kSums }, { "fill.frag", sh::kFill }, { "display.frag", sh::kDisplay },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"cltest -- render and measure the Clamp AC-coupled amplifier\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/clamp.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --droop             after a burst, black falls back on exp( -t / tau ), blanking counted\n"
		"  --tilt              a flat field with a dead clamp tilts 1 - exp( -T_active / tau ) per line\n"
		"  --apl               at steady state black shifts in proportion to APL, by the closed form\n"
		"  --clamp             the residual after each porch is exp( -T_porch / tau_c ); black settles where stated\n"
		"  --reference         every pixel against a serial double run of the same timeline\n"
		"  --continuity        six host rates give the continuous run's trajectory\n"
		"  --sag               the supply's gain follows its attack and recovery\n"
		"  --identity          a perfect clamp, no sag, no triode returns the input within a derived bound\n"
		"  --resize            a resize mid-run carries the state\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --model             the plugin's timing, control laws, walk and supply against the statements\n"
		"  --clock             a six-day millisecond clock decides the same fields as a fresh one\n"
		"  --names             nothing the host will silently truncate; the host reads SW Clamp / CL01\n"
		"  --offline           all three, and their negative controls; says loudly what it skipped. For CI.\n"
		"  --allow-no-gl       with the rendering checks: SKIP loudly, not FAIL, when no GL 4.1 context exists\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/clamp.png";
	std::string scriptPath;
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--droop", "--tilt", "--apl", "--clamp", "--reference",
		                                       "--continuity", "--sag", "--identity", "--resize", "--negative" };
	const std::set< std::string > offline  = { "--model", "--clock", "--names", "--negative-offline" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );//run the checks verbosely against a perturbed model (Model.h)
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( argument == "--offline" )
			for( const char* m : { "--model", "--clock", "--names", "--negative-offline" } )
				checks.push_back( m );
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Clamp plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		//The checks with no GL first; a context only if a rendering one asks.
		bool needGL     = false;
		bool offlineRan = false;
		for( const std::string& check : checks )
		{
			if( check == "--model" )
				runModel( perturb );
			else if( check == "--clock" )
				runClock( perturb );
			else if( check == "--names" )
				runNames();
			else if( check == "--negative-offline" )
			{
				runNegativeOffline();
				offlineRan = true;
			}
			else
			{
				needGL = true;
				continue;
			}
			std::printf( "\n" );
		}
		if( offlineRan )
			std::printf( "   OFFLINE: --droop, --tilt, --apl, --clamp, --reference, --continuity, --sag,\n"
			             "   --identity, --resize and their negative controls were NOT run. Nothing here\n"
			             "   drew a pixel through a GL driver; the shaders were not exercised, only (in CI)\n"
			             "   compiled by glslc.\n\n" );

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--droop" )
						runDroop( width, height, perturb );
					else if( check == "--tilt" )
						runTilt( width, height, perturb );
					else if( check == "--apl" )
						runApl( width, height, perturb );
					else if( check == "--clamp" )
						runClampCheck( width, height, perturb );
					else if( check == "--reference" )
						runReference( width, height, perturb );
					else if( check == "--continuity" )
						runContinuity( width, height, perturb );
					else if( check == "--sag" )
						runSag( width, height, perturb );
					else if( check == "--identity" )
						runIdentity( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 40 ? 60 : frames ) );

	Session session;
	session.floatOutput = false;
	//--fps as a rational close enough for a clock: 59.94 becomes 60000/1001.
	session.fpsNum = static_cast< int64_t >( std::llround( fps * 1001.0 ) );
	session.fpsDen = 1001;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so. rebate's
		//--pipe does not do this and dies of the signal.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			//Frame n is clocked at n / fps, never the wall clock: a stall
			//upstream must not show up in the reel as the raster speeding up.
			const bool rendered = index != failRender && session.render( index, frame );
			if( !rendered )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( frame, buildCard( width, height, frame ) ) )
			return finish( 1 );

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
