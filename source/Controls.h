#pragma once

#include "Model.h"

/**
	What a host parameter means.

	Every ranged host parameter is 0..1 (SetParamInfo clamps a STANDARD
	default into 0..1 before a range can be attached), and an option
	parameter's range reads back 0..1 whatever its element count -- so an
	option is its element INDEX, rounded and clamped here, never a fraction
	of a range. Every conversion to a physical unit lives here and nowhere
	else; cltest states the same laws from their definitions and --model holds
	the two together.
*/
namespace clampfx::controls
{

/// An option's stored value, as an index into its `count` elements.
int OptionIndex( float value, int count );

/// Coupling: tau = 10^( -4.7 + 5 v ) s, 20 us (a third of a line) to 2 s
/// (a hundred fields), logarithmic because every decade of it is a
/// different look: a line, a field, a second.
double CouplingSeconds( float value );

/// Clamp Health: how many of its own time constants the switch gets in one
/// back porch, n = 20 x 10^( -5 ( 1 - v ) ) -- 20 at the top (a residual of
/// e^-20: perfect), 2e-4 just above the bottom (a residual of 99.98% per
/// line, which still restores the level over about 300 ms) -- and exactly 0
/// at 0: dead. The rate is n over the Standard's back porch, so a setting
/// means the same clamp on either Standard.
double ClampPerPorch( float value );
double ClampRate( float value, const model::Standard& standard );

/// Clamp Reference: the level the switch pulls black to, -0.25..+0.25, zero
/// at the middle.
double ReferenceLevel( float value );

/// Sag Depth: the gain at full APL is 1 - depth, 0..0.5.
double SagDepth( float value );

/// Sag Attack and Sag Recovery: 10^( -2.3 + 2.6 v ) s, 5 ms to 2 s.
double SagSeconds( float value );

/// Bias: the grid's operating point, -0.6 (toward cutoff) to +0.6 (toward
/// grid current), zero at the middle.
double TriodeBias( float value );

/// Drive: 10^( -0.3 + 0.9 v ), 0.5 to 4; 1 at a third.
double TriodeDrive( float value );

} // namespace clampfx::controls
