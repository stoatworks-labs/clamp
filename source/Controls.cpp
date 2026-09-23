#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace clampfx::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}
} // namespace

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double CouplingSeconds( float value )
{
	return std::pow( 10.0, -4.7 + 5.0 * unit( value ) );
}

double ClampPerPorch( float value )
{
	const double v = unit( value );
	return v <= 0.0 ? 0.0 : 20.0 * std::pow( 10.0, -5.0 * ( 1.0 - v ) );
}

double ClampRate( float value, const model::Standard& standard )
{
	return ClampPerPorch( value ) / standard.backPorch;
}

double ReferenceLevel( float value )
{
	return unit( value ) - 0.25;
}

double SagDepth( float value )
{
	return 0.5 * unit( value );
}

double SagSeconds( float value )
{
	return std::pow( 10.0, -2.3 + 2.6 * unit( value ) );
}

double TriodeBias( float value )
{
	return ( unit( value ) - 0.5 ) * 1.2;
}

double TriodeDrive( float value )
{
	return std::pow( 10.0, -0.3 + 0.9 * unit( value ) );
}

} // namespace clampfx::controls
