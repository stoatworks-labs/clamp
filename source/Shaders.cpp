#include "Shaders.h"

namespace clampfx::shaders
{

const char* const kVertex = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// sums: one texel per block of kBlock samples of one active line.
//
// Active line l (top first) is host row floor( ( 2l + 1 ) H / ( 2 A ) ): the
// row under the line's centre. Integer arithmetic, so the row it picks is the
// same on every rasteriser. Sample j of a line is host column j: the host's
// width is the sample rate.
//
// A block of m samples moves the capacitor's state from s to
// a^m s + ( 1 - a ) P, P = sum_j a^( m - 1 - j ) x[ j ]; this pass is P, the
// only thing the CPU needs from the picture to walk the field. The last block
// of a line may be short: its weights are the table's last m entries, which
// are a^( m - 1 - j ) exactly. Alpha carries the plain sum of luma, for the
// supply's APL.
//---------------------------------------------------------------------------
const char* const kSums = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// fill: the capacitor's state at the start of every sample.
//
// The CPU has walked the field in double and knows the state s0 at the first
// sample of every block. Inside the block, j samples in,
//
//   s[ j ] = a^j s0 + sum_{i < j} ( 1 - a ) a^( j - 1 - i ) x[ i ]
//
// which is the recurrence s <- a s + ( 1 - a ) x unrolled, not approximated.
// The sum is accumulated on its own and added last: it is small against s0
// when a is near 1, and folding s0 into the running sum would round it j
// times instead of once.
//---------------------------------------------------------------------------
const char* const kFill = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// display: what the next stage sees, y = x - s, through the supply's gain and
// the triode, onto the host's framebuffer. Output row Y shows active line
// floor( ( 2Y + 1 ) A / ( 2H ) ), the line whose span covers the row's
// centre; the x subtracted is the row's own pixel, so the host's detail
// survives and only the capacitor's state is at the field's line count. It
// is a low-pass, and has no detail to lose.
//
// Show Blanking maps the whole field instead -- every row of it, active,
// blank and the half line, left to right from the leading edge of sync --
// and evaluates the state in the blanking from the CPU's edge states. That
// is the one place a driver's exp is used, and it is a diagnostic: no check
// reads it. Sync is drawn at its tip for legibility; the filter holds it at
// blanking level, as the spec says.
//---------------------------------------------------------------------------
const char* const kDisplay = R"(#version 410 core

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
uniform float PorchTarget;  //the porch's equilibrium state

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
		return 0.25 - 0.75 * Gain * stateOf( e0 * exp( -t / Tau ) );
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
		y = -( PorchTarget + ( e1 - PorchTarget ) * exp( -PorchK * ( t - tPorch ) ) );
	}
	else if( t < tFront )
	{
		if( row < Lines )
		{
			int c  = clamp( int( ( t - tActive ) / SampleT ), 0, Width - 1 );
			int rr = ( ( 2 * row + 1 ) * InHeight ) / ( 2 * Lines );
			y = pictureAt( c, rr ).rgb - texelFetch( Fill, ivec2( c, row ), 0 ).rgb;
		}
		else
		{
			vec3 e2 = texelFetch( Edges, ivec2( 2, row ), 0 ).rgb;
			y = -e2 * exp( -( t - tActive ) / Tau );
		}
	}
	else
	{
		vec3 e3 = texelFetch( Edges, ivec2( 3, row ), 0 ).rgb;
		y = -e3 * exp( -( t - tFront ) / Tau );
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
	vec3 v = Gain * ( x.rgb - s );
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
)";

} // namespace clampfx::shaders
