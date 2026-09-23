#include "Clamp.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace clampfx;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Clamp >,// Create method
	"CL01",                // Plugin unique ID of maximum length 4.
	"SW Clamp",            // Plugin name
	2,                     // API major version number
	1,                     // API minor version number
	0,                     // Plugin major version number
	1,                     // Plugin minor version number
	FF_EFFECT,             // Plugin type
	"An AC-coupled video amplifier whose DC restoration has failed.\n\nA capacitor does not pass DC, and the picture's black level is DC: a clamp used to put it back every line, in the back porch. With the clamp weak or dead, the coupling is a high-pass filter run through the whole raster in time order, blanking included. Black wanders with picture content, a bright moment darkens the next one, lines and fields tilt. The supply sags with average picture level, and a triode stage clips one side hard and the other soft.\n\nStart with Clamp Health low and something that cuts between bright and dark.",// Plugin description
	"Clamp FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kStandardNames[] = { "625/50 (PAL)", "525/59.94 (NTSC)" };

} // namespace

//---------------------------------------------------------------------------
Clamp::Clamp()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The raster runs in real time. It has to be the host's time, so an export
	//wanders the same as the preview.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults: a coupling of about a third of a field and a clamp that has
	// all but given up, so black wanders with the picture and every field
	// tilts, out of the box; a little supply sag on top; no triode.
	//
	// Filled BEFORE any declaration: SetParamInfof reads its default out of
	// GetFloatParameter (compander's trap).
	//---------------------------------------------------------------------
	params[ PT_COUPLING ]      = 0.5f;//6.3 ms
	params[ PT_STANDARD ]      = static_cast< float >( model::kPAL );
	params[ PT_PER_CHANNEL ]   = 0.0f;
	params[ PT_HEALTH ]        = 0.2f;//2e-3 of a time constant per porch
	params[ PT_REFERENCE ]     = 0.5f;//0
	params[ PT_SAG_DEPTH ]     = 0.3f;
	params[ PT_SAG_ATTACK ]    = 0.35f;//41 ms
	params[ PT_SAG_RECOVERY ]  = 0.6f; //0.18 s
	params[ PT_TRIODE ]        = 0.0f;
	params[ PT_BIAS ]          = 0.5f;
	params[ PT_DRIVE ]         = 1.0f / 3.0f;//1
	params[ PT_MIX ]           = 1.0f;
	params[ PT_SHOW_BLANKING ] = 0.0f;

	SetParamInfof( PT_COUPLING, "Coupling", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_STANDARD, "Standard", model::kStandardCount, params[ PT_STANDARD ] );
	for( int i = 0; i < model::kStandardCount; ++i )
		SetParamElementInfo( PT_STANDARD, static_cast< unsigned int >( i ), kStandardNames[ i ], static_cast< float >( i ) );
	SetParamInfo( PT_PER_CHANNEL, "Per Channel", FF_TYPE_BOOLEAN, false );

	SetParamInfof( PT_HEALTH, "Clamp Health", FF_TYPE_STANDARD );
	SetParamInfof( PT_REFERENCE, "Clamp Reference", FF_TYPE_STANDARD );

	SetParamInfof( PT_SAG_DEPTH, "Sag Depth", FF_TYPE_STANDARD );
	SetParamInfof( PT_SAG_ATTACK, "Sag Attack", FF_TYPE_STANDARD );
	SetParamInfof( PT_SAG_RECOVERY, "Sag Recovery", FF_TYPE_STANDARD );

	SetParamInfo( PT_TRIODE, "Triode On", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_BIAS, "Bias", FF_TYPE_STANDARD );
	SetParamInfof( PT_DRIVE, "Drive", FF_TYPE_STANDARD );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );
	SetParamInfo( PT_SHOW_BLANKING, "Show Blanking", FF_TYPE_BOOLEAN, false );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_COUPLING; i <= PT_PER_CHANNEL; ++i )
		SetParamGroup( i, "Coupling" );
	for( FFUInt32 i = PT_HEALTH; i <= PT_REFERENCE; ++i )
		SetParamGroup( i, "Clamp" );
	for( FFUInt32 i = PT_SAG_DEPTH; i <= PT_SAG_RECOVERY; ++i )
		SetParamGroup( i, "Supply" );
	for( FFUInt32 i = PT_TRIODE; i <= PT_DRIVE; ++i )
		SetParamGroup( i, "Stage" );
	for( FFUInt32 i = PT_MIX; i <= PT_SHOW_BLANKING; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Declared inline: SetParamInfo is protected on
	// CFFGLPlugin and nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Clamp effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Clamp::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &sumsShader, shaders::kSums, "sums" },
		{ &fillShader, shaders::kFill, "fill" },
		{ &displayShader, shaders::kDisplay, "display" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertex, stage.fragment ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Clamp: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &statesTexture );
	glGenTextures( 1, &edgesTexture );
	statesSize[ 0 ] = statesSize[ 1 ] = 0;
	edgesSize[ 0 ] = edgesSize[ 1 ] = 0;

	primed        = false;
	standardIndex = -1;
	shownField    = -1;
	clock.Reset();

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool Clamp::ensureBuffers( int width, int lines )
{
	const int blocks = ( width + model::kBlock - 1 ) / model::kBlock;
	return sums.Ensure( blocks, lines, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	       && fill.Ensure( width, lines, GL_RGBA32F, PassBuffer::Sampling::Nearest );
}

void Clamp::uploadTexture( GLuint texture, int width, int height, const std::vector< float >& data )
{
	int* size = texture == statesTexture ? statesSize : edgesSize;
	glBindTexture( GL_TEXTURE_2D, texture );
	if( size[ 0 ] != width || size[ 1 ] != height )
	{
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, data.data() );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		size[ 0 ] = width;
		size[ 1 ] = height;
	}
	else
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, data.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

void Clamp::setTable( GLuint program, const char* name, const double* values )
{
	//FFGLShader::Set has no array overload. The program is bound by the
	//caller's ScopedShaderBinding.
	float table[ model::kBlock ];
	for( int i = 0; i < model::kBlock; ++i )
		table[ i ] = static_cast< float >( values[ i ] );
	glUniform1fv( glGetUniformLocation( program, name ), model::kBlock, table );
}

//---------------------------------------------------------------------------
FFResult Clamp::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	clock.SetFloatForTest( ( perturb & model::kPerturbClockFloat ) != 0 );
	clock.Update( hostTime );
	const double now = clock.Now();
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale="
		            + std::to_string( clock.ClockScale() ) + " seconds=" + std::to_string( now ) );

	const int width  = static_cast< int >( picture.Width );
	const int height = static_cast< int >( picture.Height );

	//---------------------------------------------------------------------
	// The settings, in physical units.
	//---------------------------------------------------------------------
	const int stdIndex              = controls::OptionIndex( params[ PT_STANDARD ], model::kStandardCount );
	const model::Standard& standard = model::StandardOf( stdIndex );
	model::Settings settings;
	settings.tau       = controls::CouplingSeconds( params[ PT_COUPLING ] );
	settings.clampRate = controls::ClampRate( params[ PT_HEALTH ], standard );
	settings.reference = controls::ReferenceLevel( params[ PT_REFERENCE ] );
	settings.perturb   = perturb;
	model::Sag sag;
	sag.depth    = controls::SagDepth( params[ PT_SAG_DEPTH ] );
	sag.attack   = controls::SagSeconds( params[ PT_SAG_ATTACK ] );
	sag.recovery = controls::SagSeconds( params[ PT_SAG_RECOVERY ] );
	const bool perChannel = params[ PT_PER_CHANNEL ] >= 0.5f;
	const bool triodeOn   = params[ PT_TRIODE ] >= 0.5f;
	const bool showRaster = params[ PT_SHOW_BLANKING ] >= 0.5f;
	const model::Triode triode = model::MakeTriode( controls::TriodeDrive( params[ PT_DRIVE ] ),
	                                                controls::TriodeBias( params[ PT_BIAS ] ) );

	//---------------------------------------------------------------------
	// A change of Standard re-bases the field count: field 0 of the new
	// raster starts now. The capacitor is the same capacitor, so its charge
	// carries.
	//---------------------------------------------------------------------
	if( stdIndex != standardIndex )
	{
		standardIndex = stdIndex;
		fieldOrigin   = now;
		nextField     = 0;
		shownField    = -1;
	}

	//A resize changes the sample rate and the rows each line reads, not the
	//capacitor: nothing to do, because nothing on the GPU holds state.
	if( primed && ( width != lastWidth || height != lastHeight ) && ( perturb & model::kPerturbResizeReprime ) )
		primed = false;
	lastWidth  = width;
	lastHeight = height;

	//---------------------------------------------------------------------
	// Buffers. Every allocation happens here, before anything binds a
	// texture: FFGLFBO::Initialise sizes its colour texture under a scoped
	// binding, and every ffglex Scoped* binding CLEARS to 0 on exit.
	//---------------------------------------------------------------------
	const int lines  = standard.activeLines;
	const int blocks = ( width + model::kBlock - 1 ) / model::kBlock;
	if( !ensureBuffers( width, lines ) )
	{
		diag::error( "could not allocate the buffers at " + std::to_string( width ) + " wide" );
		return FF_FAIL;
	}
	const model::Timeline timeline = model::MakeTimeline( standard, width, settings );

	//---------------------------------------------------------------------
	// 1. The block sums, read back: the one thing the walk needs from the
	//    picture.
	//---------------------------------------------------------------------
	readback.assign( static_cast< size_t >( blocks ) * lines * 4, 0.0f );
	{
		ScopedFBOBinding fbo( sums.GetGLID(), ScopedFBOBinding::RB_REVERT );
		sums.ResizeViewPort();
		ScopedShaderBinding shader( sumsShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );
		sumsShader.Set( "InputTexture", 0 );
		sumsShader.Set( "InHeight", height );
		sumsShader.Set( "Width", width );
		sumsShader.Set( "Lines", lines );
		setTable( sumsShader.GetGLID(), "WeightP", timeline.weightP );
		quad.Draw();

		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, blocks, lines, GL_RGBA, GL_FLOAT, readback.data() );
	}
	blockSums.assign( readback.begin(), readback.end() );

	//---------------------------------------------------------------------
	// 2. The walk: this picture's field map, in double.
	//---------------------------------------------------------------------
	model::WalkField( timeline, blockSums.data(), walk );

	const double fieldSeconds = standard.Field();
	const int64_t field = model::FieldAt( now - fieldOrigin, standard );

	if( !primed )
	{
		//The first frame starts at this picture's periodic steady state, so
		//a clip does not open with a settle that belongs to nothing.
		for( int c = 0; c < 3; ++c )
			nextState[ c ] = primeOnStart ? model::SteadyState( walk, c ) : 0.0;
		nextSupply = primeOnStart ? walk.apl : 0.0;
		nextField  = field;
		primed     = true;
	}
	if( perturb & model::kPerturbResetEachFrame )
		for( double& s : nextState )
			s = 0.0;

	//---------------------------------------------------------------------
	// 3. The timeline. Every field that started since the last host frame is
	//    run, with this picture: the ones the host will never see through
	//    the field map in closed form, the last one sample by sample for the
	//    display. A host frame with no new field shows the same field again,
	//    re-scanned from its own start with this picture.
	//---------------------------------------------------------------------
	if( field >= nextField )
	{
		const int64_t unseen = field - nextField;
		if( unseen > 100000 )
		{
			for( int c = 0; c < 3; ++c )
				nextState[ c ] = model::SteadyState( walk, c );
			nextSupply = walk.apl;
		}
		else
			for( int64_t k = 0; k < unseen; ++k )
			{
				for( int c = 0; c < 3; ++c )
					nextState[ c ] = model::Advance( walk, c, nextState[ c ] );
				nextSupply = model::SagStep( nextSupply, walk.apl, fieldSeconds, sag, perturb );
			}

		shownField = field;
		for( int c = 0; c < 3; ++c )
		{
			shownState[ c ] = nextState[ c ];
			nextState[ c ]  = model::Advance( walk, c, shownState[ c ] );
		}
		shownSupply = nextSupply;
		nextSupply  = model::SagStep( shownSupply, walk.apl, fieldSeconds, sag, perturb );
		nextField   = field + 1;
	}

	//---------------------------------------------------------------------
	// 4. The field on show: the state at every block start, and at every
	//    blanking edge for Show Blanking.
	//---------------------------------------------------------------------
	states.assign( static_cast< size_t >( blocks ) * lines * 4, 0.0f );
	for( size_t i = 0; i < walk.blockA.size(); ++i )
		for( int c = 0; c < 3; ++c )
			states[ i * 4 + c ] = static_cast< float >( walk.blockA[ i ] * shownState[ c ] + walk.blockB[ i * 3 + c ] );
	edges.assign( static_cast< size_t >( timeline.rows ) * 4 * 4, 0.0f );
	for( size_t i = 0; i < walk.edgeA.size(); ++i )
		for( int c = 0; c < 3; ++c )
			edges[ i * 4 + c ] = static_cast< float >( walk.edgeA[ i ] * shownState[ c ] + walk.edgeB[ i * 3 + c ] );
	uploadTexture( statesTexture, blocks, lines, states );
	uploadTexture( edgesTexture, 4, timeline.rows, edges );

	//---------------------------------------------------------------------
	// 5. Every sample's state.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( fill.GetGLID(), ScopedFBOBinding::RB_REVERT );
		fill.ResizeViewPort();
		ScopedShaderBinding shader( fillShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding input( picture.Handle );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding blockStates( statesTexture );
		fillShader.Set( "InputTexture", 0 );
		fillShader.Set( "States", 1 );
		fillShader.Set( "InHeight", height );
		fillShader.Set( "Lines", lines );
		setTable( fillShader.GetGLID(), "PowA", timeline.powA );
		setTable( fillShader.GetGLID(), "InputW", timeline.inputW );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 6. Display.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( displayShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding input( picture.Handle );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding fillTexture( fill.TextureID() );
		ScopedSamplerActivation s2( 2 );
		Scoped2DTextureBinding edgeTexture( edgesTexture );

		displayShader.Set( "InputTexture", 0 );
		displayShader.Set( "Fill", 1 );
		displayShader.Set( "Edges", 2 );
		displayShader.Set( "InHeight", height );
		displayShader.Set( "Width", width );
		displayShader.Set( "Lines", lines );
		displayShader.Set( "VpX", hostViewport[ 0 ] );
		displayShader.Set( "VpY", hostViewport[ 1 ] );
		displayShader.Set( "VpW", hostViewport[ 2 ] );
		displayShader.Set( "VpH", hostViewport[ 3 ] );
		displayShader.Set( "PerChannel", perChannel ? 1 : 0 );
		displayShader.Set( "Gain", static_cast< float >( model::SagGain( shownSupply, sag ) ) );
		displayShader.Set( "TriodeOn", triodeOn ? 1 : 0 );
		displayShader.Set( "Drive", static_cast< float >( triode.drive ) );
		displayShader.Set( "Bias", static_cast< float >( triode.bias ) );
		displayShader.Set( "PlateAtBias", static_cast< float >( triode.plateAtBias ) );
		displayShader.Set( "SlopeAtBias", static_cast< float >( triode.slopeAtBias ) );
		displayShader.Set( "MixAmount", params[ PT_MIX ] );
		displayShader.Set( "ShowBlanking", showRaster ? 1 : 0 );
		displayShader.Set( "Rows", timeline.rows );
		displayShader.Set( "LineT", static_cast< float >( standard.line ) );
		displayShader.Set( "SyncT", static_cast< float >( standard.sync ) );
		displayShader.Set( "PorchT", static_cast< float >( standard.backPorch ) );
		displayShader.Set( "ActiveT", static_cast< float >( standard.Active() ) );
		displayShader.Set( "SampleT", static_cast< float >( timeline.sample ) );
		displayShader.Set( "Tau", static_cast< float >( settings.tau ) );
		displayShader.Set( "PorchK", static_cast< float >( 1.0 / settings.tau + settings.clampRate ) );
		displayShader.Set( "PorchTarget", static_cast< float >( timeline.porchTarget ) );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Clamp::DeInitGL()
{
	sumsShader.FreeGLResources();
	fillShader.FreeGLResources();
	displayShader.FreeGLResources();
	quad.Release();

	sums.Destroy();
	fill.Destroy();
	if( statesTexture != 0 )
		glDeleteTextures( 1, &statesTexture );
	if( edgesTexture != 0 )
		glDeleteTextures( 1, &edgesTexture );
	statesTexture = edgesTexture = 0;
	statesSize[ 0 ] = statesSize[ 1 ] = 0;
	edgesSize[ 0 ] = edgesSize[ 1 ] = 0;

	primed        = false;
	standardIndex = -1;
	shownField    = -1;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Clamp::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Clamp::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Clamp::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Clamp::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Clamp::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}
