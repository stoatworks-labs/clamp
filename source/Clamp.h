#pragma once

#include "Clock.h"
#include "Model.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Clamp -- an AC-coupled video amplifier whose DC restoration has failed, as
	an FFGL effect.

	**The one idea.** Video amplifiers were joined by capacitors, and a
	capacitor does not pass DC. The picture's black level is DC; it was put
	back every line by a clamp, a switch that pulled the signal to a reference
	in the back porch. When the clamp is weak or dead, the coupling capacitor
	and the next stage's input resistance are a high-pass filter on the whole
	raster, run in time order through every line and every blanking interval.
	Black wanders with picture content, a bright moment darkens the next one,
	lines and fields tilt, and a healthy clamp takes it all out again except
	the residual its own switch leaves over the porch. The supply sagging
	with APL and an optional triode stage are separate mechanisms on top.

	**Two processors.** The CPU walks the raster's timeline in double: one
	affine step per block of 16 samples and one per blanking interval
	(`Model.h`). The GPU does everything that touches a sample: the block sums
	the walk needs, the fill of every sample from its block's state, and the
	display (`Shaders.h`). The state that carries across host frames is a
	handful of doubles on the CPU -- no buffer outlives a frame, so a resize
	cannot lose it. See AGENTS.md for the traps and what is verified.
*/
class Clamp : public CFFGLPlugin
{
public:
	Clamp();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by cltest; the plugin's own operation never uses
	//--- them, and the perturbation is always 0 outside the harness.

	/// The harness DECLARES its clock unit rather than leaving the voting to
	/// infer one: it renders as fast as the GPU allows.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}

	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}

	/// Off: the first frame starts from a discharged capacitor and a rested
	/// supply rather than the first picture's steady state, so a check can
	/// watch the plugin settle by running.
	void SetPrimeForTest( bool on )
	{
		primeOnStart = on;
	}

	/// The field the last frame displayed, counted from the Standard's origin.
	int64_t ShownFieldForTest() const
	{
		return shownField;
	}

	/// The supply's filtered APL at the start of the field on show.
	double ShownSupplyForTest() const
	{
		return shownSupply;
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Coupling
		PT_COUPLING,
		PT_STANDARD,
		PT_PER_CHANNEL,

		//Clamp
		PT_HEALTH,
		PT_REFERENCE,

		//Supply
		PT_SAG_DEPTH,
		PT_SAG_ATTACK,
		PT_SAG_RECOVERY,

		//Stage
		PT_TRIODE,
		PT_BIAS,
		PT_DRIVE,

		//Output
		PT_MIX,
		PT_SHOW_BLANKING,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	bool ensureBuffers( int width, int lines );
	void uploadTexture( GLuint texture, int width, int height, const std::vector< float >& data );
	void setTable( GLuint program, const char* name, const double* values );

	ffglex::FFGLShader sumsShader;
	ffglex::FFGLShader fillShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	clampfx::PassBuffer sums;///< blocks x lines: P per channel and the luma sum
	clampfx::PassBuffer fill;///< width x lines: the state at every sample
	GLuint statesTexture = 0;///< blocks x lines: the state at each block's first sample
	GLuint edgesTexture  = 0;///< 4 x rows: the state at each blanking edge
	int statesSize[ 2 ]  = { 0, 0 };
	int edgesSize[ 2 ]   = { 0, 0 };

	std::vector< float > readback;
	std::vector< double > blockSums;
	std::vector< float > states;
	std::vector< float > edges;
	clampfx::model::FieldWalk walk;

	//--- The amplifier's state, carried across host frames. All CPU, all
	//--- double. `next` is the field not yet run; `shown` the one on screen.
	bool primed          = false;
	bool primeOnStart    = true;
	int standardIndex    = -1;
	double fieldOrigin   = 0.0;///< clock seconds at which field 0 of this Standard began
	int64_t nextField    = 0;
	double nextState[ 3 ] = { 0, 0, 0 };
	double nextSupply     = 0.0;
	int64_t shownField   = -1;
	double shownState[ 3 ] = { 0, 0, 0 };
	double shownSupply     = 0.0;
	int lastWidth        = 0;
	int lastHeight       = 0;

	clampfx::Clock clock;
	double hostTime = -1.0;
	int clockFrames = 0;

	int perturb = 0;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
