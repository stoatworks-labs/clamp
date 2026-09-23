#pragma once

/**
	The passes. Every read is `texelFetch` at integer coordinates computed in
	integers, and every weight is computed on the CPU in double
	(`Model.h`) and handed over as a uniform, so nothing here depends on a
	texture unit's filtering precision, on where a rasteriser's interpolated
	uv lands, or on a driver's `exp`. The GPU multiplies and adds.

	Rows in every buffer this plugin owns are LINES of a field, top first:
	texel row l is active line l. Only the passes that read the host's picture
	(and the display, which writes it) know that GL's row 0 is the bottom.

	  sums     host picture -> per block of kBlock samples of each line: the
	                           weighted sum P = sum a^(m-1-j) x[j] of R, G
	                           and B, and the plain sum of luma (blocks x A)
	  fill     host picture -> the capacitor's state at every sample, from
	           + block states  the state at its block's first sample, which
	                           the CPU walked in double (W x A)
	  display  host picture -> x - s, the supply's gain, the triode, the mix;
	           + fill          or the whole raster with its blanking
	           + edge states
*/
namespace clampfx::shaders
{

extern const char* const kVertex;
extern const char* const kSums;
extern const char* const kFill;
extern const char* const kDisplay;

} // namespace clampfx::shaders
