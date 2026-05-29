#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "PsyX/PsyX_public.h"

#include "../platform.h"
#include "../gpu/PsyX_GPU.h"

#include "PsyX/PsyX_render.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/util/timer.h"

#include <assert.h>
#include <string.h>

#ifdef _WIN32

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif //def WIN32

#if defined(RENDERER_OGL)

#define USE_PBO					1
#define USE_OFFSCREEN_BLIT		1
#define USE_FRAMEBUFFER_BLIT	1

#else

// OpenGL ES/Web GL has slowdowns and doesn't allow GL_LUMINANCE_ALPHA format as framebuffer, so it's disabled
#define USE_PBO					(OGLES_VERSION == 3)
#define USE_OFFSCREEN_BLIT		(OGLES_VERSION == 3)
#define USE_FRAMEBUFFER_BLIT	(OGLES_VERSION == 3)

#endif

extern SDL_Window* g_window;


#define MAX_NUM_VERTEX_BUFFERS		(2)
#define PSX_SCREEN_ASPECT	(240.0f / 320.0f)			// PSX screen is mapped always to this aspect

/* PSX NTSC pixel aspect: a PSX 320×240 framebuffer displayed on a real
 * 4:3 NTSC CRT presents pixels that are ~1.094× taller than wide. The
 * 320×240 logical framebuffer therefore visually fills a vertical extent
 * that's 1.094× the "square pixel" assumption. Without compensating for
 * this, characters and assets look ~9% wider on a modern display than
 * the original artists intended (Harry's torso, arms, and legs visibly
 * thicker on PC vs CRT). Applied during Hor+ ortho setup so 1 horizontal
 * world unit on screen ≈ 1/1.094 the size of 1 vertical world unit.
 *
 * Now a runtime-settable global instead of a macro so the host
 * application can override it from config (see pc_port/config.cfg
 * pixel_aspect). The cull-bound sites in the game (vw_calc.c and
 * Gfx_MeshDraw in bodyprog_80055028.c) read this same global so
 * their bounds always track the active ortho. Default 1.09375 (CRT
 * NTSC); 1.0 = square pixels (modern emulator look); 1.143 = 8:7. */
extern "C" {
float g_PsxPixelAspect = 1.09375f;
}
#define PSX_NTSC_PIXEL_ASPECT (g_PsxPixelAspect)

int g_PreviousBlendMode = BM_NONE;
int g_PreviousDepthMode = 0;
int g_PreviousStencilMode = 0;
int g_PreviousScissorState = 0;
int g_PreviousOffscreenState = 0;
RECT16 g_PreviousFramebuffer = { 0,0,0,0 };
RECT16 g_PreviousOffscreen = { 0,0,0,0 };

ShaderID g_PreviousShader = -1;

TextureID g_vramTexturesDouble[2];
TextureID g_vramTexture;
int g_vramTextureIdx = 0;

TextureID g_fbTexture = -1;
TextureID g_offscreenRTTexture = -1;

TextureID g_whiteTexture = -1;
TextureID g_lastBoundTexture = -1;

int g_windowWidth = 0;
int g_windowHeight = 0;

int g_dbg_wireframeMode = 0;
int g_dbg_texturelessMode = 0;

// Set to 1 by game code only during states that render a 3D world (InGame, MapEvent).
// When 0, GR_SetOffscreenState uses 4:3 ortho so 2D UI screens don't show VRAM garbage.
int g_PcHorPlusEnabled = 1;

int g_cfg_pgxpTextureCorrection = 1;
int g_cfg_pgxpZBuffer = 1;

/* PC port (Silent Hill): runtime master gate for PGXP. When 0,
 * MakeVertex* writes 0 into a_zw, the shader takes the `a_zw.y > 100.0`
 * else branch (2D-ortho path). Set from main_pc.c after PcConfig_Load runs
 * (config key: use_pgxp). */
int g_PsxUsePgxp = 0;
int g_cfg_bilinearFiltering = 0;
int g_cfg_affineTextures = 0;
/* When non-zero, the GPU_DITHERING macro applies the 4x4 PSX-style ordered
 * dither to every fragment regardless of the per-primitive `a_texcoord.w`
 * dither flag. Lets the PC port mimic the PSX 5-bit framebuffer noise
 * (which masks texture-page seams and gives the authentic look) on
 * primitives that don't request dither at the prim-tag level. */
int g_cfg_psxDither = 1;
int g_PsxDitherSuppressed = 0;

int vram_need_update = 1;

/* PC port: runtime gate for framebuffer→VRAM feedback. See PsyX_render.h. */
int g_PsxSkipFramebufferStore = 0;
int framebuffer_need_update = 0;

#if defined(__EMSCRIPTEN__) || defined(__RPI__) || defined(__ANDROID__)
#if defined(RENDERER_OGL)
#error It should not be enabled
#endif
#endif



#if USE_OPENGL
typedef struct
{
	GLenum fmt;
	GLuint* pbos;
	uint64_t num_pbos;
	uint64_t dx;
	uint64_t num_downloads;

	int width;
	int height;
	int nbytes; /* number of bytes in the pbo buffer. */
	unsigned char* pixels; /* the downloaded pixels. */
} GrPBO;

int PBO_Init(GrPBO* pbo, GLenum format, int w, int h, int num)
{
	if (pbo->pbos)
	{
		eprinterr("Already initialized. Not necessary to initialize again; or shutdown first.");
		return -1;
	}

	if (0 >= num)
	{
		eprinterr("Invalid number of PBOs: %d", num);
		return -2;
	}

	pbo->fmt = format;
	pbo->width = w;
	pbo->height = h;
	pbo->num_pbos = num;

#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#if USE_PBO
	if (GL_RED == pbo->fmt || GL_GREEN == pbo->fmt || GL_BLUE == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height;
	}
	else if (GL_RGB == pbo->fmt || GL_BGR == pbo->fmt)
	{
		pbo->nbytes = pbo->width * pbo->height * 3;
	}
	else if (GL_RGBA == pbo->fmt || GL_BGRA == pbo->fmt) {
		pbo->nbytes = pbo->width * pbo->height * 4;
	}
	else
	{
		eprinterr("Unhandled pixel format, use GL_R, GL_RG, GL_RGB or GL_RGBA.");
		return -3;
	}

	if (pbo->nbytes == 0)
	{
		eprinterr("Invalid width or height given: %d x %d", pbo->width, pbo->height);
		return -4;
	}

	pbo->pbos = (GLuint*)malloc(sizeof(GLuint) * num);
	pbo->pixels = (u_char*)malloc(pbo->nbytes);

	glGenBuffers(num, pbo->pbos);
	for (int i = 0; i < num; ++i)
	{
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[i]);
		glBufferData(GL_PIXEL_PACK_BUFFER, pbo->nbytes, NULL, GL_STREAM_READ);
	}

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#endif
	return 0;
}

void PBO_Destroy(GrPBO* pbo)
{
#if USE_PBO
	if(pbo->pbos)
	{
		glDeleteBuffers(pbo->num_pbos, pbo->pbos);
	
		free(pbo->pbos);
		pbo->num_pbos = 0;
		pbo->pbos = NULL;
	}

#endif
	if (pbo->pixels)
	{
		free(pbo->pixels);
		pbo->pixels = NULL;
	}

	pbo->num_downloads = 0;
	pbo->dx = 0;
	pbo->fmt = 0;
	pbo->nbytes = 0;
}

void PBO_Download(GrPBO* pbo)
{
	unsigned char* ptr;
	
#if USE_PBO
	if (pbo->num_downloads < pbo->num_pbos)
	{
		/*
		   First we need to make sure all our pbos are bound, so glMap/Unmap will
		   read from the oldest bound buffer first.
		*/
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, pbo->fmt, GL_UNSIGNED_BYTE, 0);   /* When a GL_PIXEL_PACK_BUFFER is bound, the last 0 is used as offset into the buffer to read into. */
#endif
	}
	else
	{
		/* Read from the oldest bound pbo */
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo->pbos[pbo->dx]);

#if defined(RENDERER_OGL)
		ptr = (unsigned char*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
		if (NULL != ptr)
		{
			memcpy(pbo->pixels, ptr, pbo->nbytes);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		else
			eprintwarn("Failed to map the buffer\n");

		/* Trigger the next read. */
		glGetTexImage(GL_TEXTURE_2D, 0, pbo->fmt, GL_UNSIGNED_BYTE, 0);
#else
		glReadPixels(0, 0, pbo->width, pbo->height, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
	}

	++pbo->dx;
	pbo->dx = pbo->dx % pbo->num_pbos;

	pbo->num_downloads++;

	if (pbo->num_downloads == UINT64_MAX)
		pbo->num_downloads = pbo->num_pbos;

	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#else
	// FIXME: THIS is very slow
	// Do not use at all

	// glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); /* just make sure we're not accidentilly using a PBO. */
	// glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pbo->pixels);
#endif
}

GLuint		g_glVertexArray[2];
GLuint		g_glVertexBuffer[2];
int			g_curVertexBuffer = 0;

GLuint		g_glBlitFramebuffer;
GrPBO		g_glFramebufferPBO;

GLuint		g_glVRAMFramebuffer;

GLuint		g_glOffscreenFramebuffer;
GrPBO		g_glOffscreenPBO;

#endif

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
int GR_InitialiseGLContext(char* windowName, int fullscreen)
{
	int windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

#if defined(__ANDROID__)
	windowFlags |= SDL_WINDOW_FULLSCREEN;
#else
	if (fullscreen)
		windowFlags |= SDL_WINDOW_FULLSCREEN;
#endif

	if(g_windowWidth <= 0 || g_windowHeight <= 0)
		windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

	g_window = SDL_CreateWindow(windowName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, g_windowWidth, g_windowHeight, windowFlags);

	if (g_window == NULL)
	{
		eprinterr("Failed to initialise SDL window!\n");
		return 0;
	}
	
#if defined(RENDERER_OGLES)

#if defined(__ANDROID__)
	//Override to full screen.
	SDL_DisplayMode displayMode;
	if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0)
	{
		screenWidth = displayMode.w;
		windowWidth = displayMode.w;
		screenHeight = displayMode.h;
		windowHeight = displayMode.h;
	}
#endif

	//SDL_GL_SetAttribute(SDL_GL_CONTEXT_EGL, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, OGLES_VERSION);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

	if(!SDL_GL_CreateContext(g_window))
	{
		eprinterr("Failed to initialise - OpenGL ES %d.x is not supported.\n", OGLES_VERSION);
		return 0;
	}

#elif defined(RENDERER_OGL)

	int major_version = 3;
	int minor_version = 3;
	int profile = SDL_GL_CONTEXT_PROFILE_CORE;

	// find best OpenGL version
	do
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);

		if (SDL_GL_CreateContext(g_window))
			break;
	
		minor_version--;
		
	} while (minor_version >= 0);

	if (minor_version == -1)
	{
		eprinterr("Failed to initialise - OpenGL 3.x is not supported. Please update video drivers.\n");
		return 0;
	}
#endif

	return 1;
}
#endif

int GR_InitialiseGLExt()
{
#ifdef USE_GLAD
	GLenum err = gladLoadGL();

	if (err == 0)
		return 0;
#endif
	
	const char* rend = (const char*)glGetString(GL_RENDERER);
	const char* vendor = (const char*)glGetString(GL_VENDOR);
	eprintf("*Video adapter: %s by %s\n", rend, vendor);

	const char* versionStr = (const char*)glGetString(GL_VERSION);
	eprintf("*OpenGL version: %s\n", versionStr);

	const char* glslVersionStr = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
	eprintf("*GLSL version: %s\n", glslVersionStr);

	return 1;
}

int GR_InitialiseRender(char* windowName, int width, int height, int fullscreen)
{
	g_windowWidth = width;
	g_windowHeight = height;

	// Due to debugging in fullscreen
	SDL_SetHint(SDL_HINT_ALLOW_TOPMOST, "0");
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#if USE_OPENGL
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	if (!GR_InitialiseGLContext(windowName, fullscreen))
	{
		eprinterr("Failed to Initialise GL Context!\n");
		return 0;
	}
#endif

	if (!GR_InitialiseGLExt())
	{
		eprinterr("Failed to Intialise GL extensions\n");
		return 0;
	}
#endif
	
	return 1;
}

void GR_Shutdown()
{
#if USE_OPENGL
	glDeleteVertexArrays(2, g_glVertexArray);
	glDeleteBuffers(2, g_glVertexBuffer);

	PBO_Destroy(&g_glFramebufferPBO);
	PBO_Destroy(&g_glOffscreenPBO);

	glDeleteFramebuffers(1, &g_glBlitFramebuffer);
	glDeleteFramebuffers(1, &g_glOffscreenFramebuffer);
	glDeleteFramebuffers(1, &g_glVRAMFramebuffer);

	GR_DestroyTexture(g_vramTexturesDouble[0]);
	GR_DestroyTexture(g_vramTexturesDouble[1]);

	GR_DestroyTexture(g_whiteTexture);
	GR_DestroyTexture(g_fbTexture);
	GR_DestroyTexture(g_offscreenRTTexture);
#endif
}

void GR_UpdateSwapIntervalState(int swapInterval)
{
#if defined(RENDERER_OGL)
	SDL_GL_SetSwapInterval(swapInterval);
#endif
}

void GR_BeginScene()
{
	g_lastBoundTexture = 0;

#if USE_OPENGL
#ifdef RENDERER_OGLES
	glClearDepthf(1.0f);
#else
	glClearDepth(1.0f);
#endif
	glClear(GL_DEPTH_BUFFER_BIT);
	glClear(GL_STENCIL_BUFFER_BIT);
#endif

	GR_UpdateVRAM();
	GR_SetViewPort(0, 0, g_windowWidth, g_windowHeight);

	if (g_dbg_wireframeMode)
	{
		GR_SetWireframe(1);

#if USE_OPENGL
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
#endif
	}
}

void GR_EndScene()
{
	framebuffer_need_update = 1;
	
	if (g_dbg_wireframeMode)
		GR_SetWireframe(0);

#if USE_OPENGL
	glBindVertexArray(0);
#endif
}

//----------------------------------------------------------------------------------------

unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];

void GR_ResetDevice()
{
	GR_UpdateSwapIntervalState(0);
}

typedef struct
{
	// shader itself
	ShaderID shader;

#if USE_OPENGL
	GLint projectionLoc;
	GLint projection3DLoc;
	GLint bilinearFilterLoc;
	GLint ditherForceLoc;
	GLint pixelScaleLoc;
	GLint texelSizeLoc;
	GLint fogColorLoc;
	GLint pgxpEnabledLoc;
#endif
} GTEShader;

GTEShader g_gte_shader_4;
GTEShader g_gte_shader_8;
GTEShader g_gte_shader_16;
GTEShader g_gte_shader_32_rgba;

#if USE_OPENGL

GLint u_projectionLoc;
GLint u_projection3DLoc;
GLint u_bilinearFilterLoc;
GLint u_ditherForceLoc;
GLint u_pixelScaleLoc;
GLint u_texelSizeLoc;
GLint u_fogColorLoc;
GLint u_pgxpEnabledLoc;

float g_PsyX_FogColor[3] = { 0.0f, 0.0f, 0.0f };

#define GPU_PACK_RG\
	"		float color_16 = (color_rg.y * 256.0 + color_rg.x) * 255.0;\n"

#define GPU_DISCARD\
	"		if (color_16 == 0.0) { discard; }\n"

#define GPU_DECODE_RG\
	"		fragColor = fract(floor(color_16 / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0);\n"

#define GPU_PACK_RG_FUNC\
	"	const float c_PackRange = 255.001;\n"\
	"	float packRG(vec2 rg) { return (rg.y * 256.0 + rg.x) * c_PackRange;}\n"

#define GPU_DECODE_RG_FUNC\
	" vec4 decodeRG(float rg) {\n"\
	" 	vec4 value = fract(floor(rg / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0);\n"\
	" 	return vec4(value.xyz, rg == 0.0 ? rg : (1.0 - value.w * 16.0));\n"\
	" }\n"
	//"	vec4 decodeRG(float rg) { return fract(floor(rg / vec4(1.0, 32.0, 1024.0, 32768.0)) / 32.0); }\n"

#if defined(RENDERER_OGL) || (OGLES_VERSION == 3)

/* PSX 4x4 ordered dither.
 *
 * Native hardware applies a signed offset before quantizing to the 5-bit
 * framebuffer. We match the matrix the original game's GPU uses (values
 * in [-4..+3]) divided by 255 so it lands in 24-bit color space.
 *
 * `v_texcoord.w` carries the per-primitive dither flag from the prim's
 * tpage. `u_ditherForce` is a global override the PC config exposes —
 * when non-zero, dither is applied to every fragment regardless of the
 * per-prim flag. We combine via max() so a prim that requests dither
 * still gets it even when force is 0, and a prim that didn't request
 * dither gets the global override when force is 1.
 *
 * After adding the dither offset we quantize to 5 bits per channel
 * (PSX framebuffer depth) so the noise translates to actual color
 * banding rather than staying as a smooth gradient — that's what
 * produces the visible "film grain" look. */
#	define GPU_DITHERING\
		"		fragColor *= v_color;\n"\
		"		mat4 dither = mat4(\n"\
		"			-4.0,  +0.0,  -3.0,  +1.0,\n"\
		"			+2.0,  -2.0,  +3.0,  -1.0,\n"\
		"			-3.0,  +1.0,  -4.0,  +0.0,\n"\
		"			+3.0,  -1.0,  +2.0,  -2.0) / 255.0;\n"\
		"		ivec2 dc = ivec2(fract(gl_FragCoord.xy / 4.0) * 4.0);\n"\
		"		float dStrength = max(v_texcoord.w, u_ditherForce) * v_is3d;\n"\
		"		fragColor.xyz += vec3(dither[dc.x][dc.y] * dStrength);\n"\
		"		if (u_ditherForce > 0.5 && v_is3d > 0.5) {\n"\
		"		    fragColor.xyz = floor(fragColor.xyz * 32.0 + 0.5) / 32.0;\n"\
		"		}\n"

/* Same dither as GPU_DITHERING but skips the `fragColor *= v_color`
 * step — used by the main fragment shader where we now multiply by
 * v_color and apply fog BEFORE dithering, so the dither + quantize is
 * the very last operation on the fragment and the fog mix() can't
 * smooth out the quantization steps that produce the visible noise.
 *
 * 8-pixel cell, native PSX dither strength (1:1 with the original
 * matrix). Earlier resolution-scaling and 1.6x intensity made it
 * look chunkier than PSX; this version sticks closer to the
 * authentic noise level. */
#	define GPU_DITHERING_NO_VCOLOR\
		"		mat4 dither = mat4(\n"\
		"			-4.0,  +0.0,  -3.0,  +1.0,\n"\
		"			+2.0,  -2.0,  +3.0,  -1.0,\n"\
		"			-3.0,  +1.0,  -4.0,  +0.0,\n"\
		"			+3.0,  -1.0,  +2.0,  -2.0) / 255.0;\n"\
		"		ivec2 dc = ivec2(fract(gl_FragCoord.xy / 8.0) * 4.0);\n"\
		"		float dStrength = max(v_texcoord.w, u_ditherForce) * v_is3d;\n"\
		"		fragColor.xyz += vec3(dither[dc.x][dc.y] * dStrength);\n"\
		"		if (u_ditherForce > 0.5 && v_is3d > 0.5) {\n"\
		"		    fragColor.xyz = floor(fragColor.xyz * 32.0 + 0.5) / 32.0;\n"\
		"		}\n"

#	define GPU_ARRAY_FUNC\
		"	float _idx2(vec2 array, int idx) { return array[idx]; }"

#else

#	define GPU_DITHERING\
		"		fragColor *= v_color;\n"

/* GLES 2 fallback: no dither (would need ivec2 / mat4 indexing). */
#	define GPU_DITHERING_NO_VCOLOR\
		""

#	define GPU_ARRAY_FUNC\
		"	float _idx2(vec2 array, int idx) { if(idx == 0) return array.x; else return array.y; }"

#endif

#define GPU_SAMPLE_TEXTURE_4BIT_FUNC\
    "   // returns 16 bit colour\n"\
    "   float samplePSX(vec2 tc){\n"\
    "       vec2 uv = (tc * vec2(0.25, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
    "       vec2 comp = VRAM(uv);\n"\
    "       int index = int(fract(tc.x / 4.0 + 0.0001) * 4.0);\n"\
    "       float v = _idx2(comp, index / 2) * (c_PackRange / 16.0);\n"\
    "       float f = floor(v);\n"\
    "       vec2 c = vec2( (v - f) * 16.0, f );\n"\
    "       vec2 clut_pos = v_page_clut.zw;\n"\
    "       clut_pos.x += mix(c[0], c[1], mod(float(index), 2.0)) * c_VRAMTexel.x;\n"\
    "       return packRG(VRAM(clut_pos));\n"\
    "   }\n"

#define GPU_SAMPLE_TEXTURE_8BIT_FUNC\
	"	// returns 16 bit colour\n"\
	"	float samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc * vec2(0.5, 1.0) + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 comp = VRAM(uv);\n"\
	"		vec2 clut_pos = v_page_clut.zw;\n"\
	"		int index = int(mod(tc.x, 2.0));\n"\
	"		clut_pos.x += _idx2(comp, index) * c_PackRange * c_VRAMTexel.x;\n"\
	"		vec2 color_rg = VRAM(clut_pos);\n"\
	"		return packRG(VRAM(clut_pos));\n"\
	"	}\n"

#define GPU_SAMPLE_TEXTURE_16BIT_FUNC\
	"	float samplePSX(vec2 tc){\n"\
	"		vec2 uv = (tc + v_page_clut.xy) * c_VRAMTexel;\n"\
	"		vec2 color_rg = VRAM(uv);\n"\
	"		return packRG(color_rg);\n"\
	"	}\n"


#define GPU_BILINEAR_SAMPLE_FUNC \
	"	float c_textureSize = 1.0;\n"\
	"	float c_onePixel = 1.0;\n"\
	"	vec4 BilinearTextureSample(vec2 P) {\n"\
	"		vec2 frac = fract(P);\n"\
	"		vec2 pixel = floor(P);\n"\
	"		float C11 = samplePSX(pixel);\n"\
	"		float C21 = samplePSX(pixel + vec2(c_onePixel, 0.0));\n"\
	"		float C12 = samplePSX(pixel + vec2(0.0, c_onePixel));\n"\
	"		float C22 = samplePSX(pixel + vec2(c_onePixel, c_onePixel));\n"\
	"		float ax1 = mix(float(C11 > 0.0), float(C21 > 0.0), frac.x);\n"\
	"		float ax2 = mix(float(C12 > 0.0), float(C22 > 0.0), frac.x);\n"\
	"		if(mix(ax1, ax2, frac.y) < 0.5) { discard; }\n"\
	"		vec4 x1 = mix(decodeRG(C11), decodeRG(C21), frac.x);\n"\
	"		vec4 x2 = mix(decodeRG(C12), decodeRG(C22), frac.x);\n"\
	"		return mix(x1, x2, frac.y);\n"\
	"	}\n"

#define GPU_NEAREST_SAMPLE_FUNC \
	"vec4 NearestTextureSample(vec2 P) {\n"\
	"	float color_16 = samplePSX(P);\n"\
	"	if(color_16 == 0.0) {discard;}\n"\
	"	return decodeRG(color_16);\n"\
	"}\n"

#if (VRAM_FORMAT == GL_LUMINANCE_ALPHA)
#define GPU_FETCH_VRAM_FUNC\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).ra; }\n"
#else
#define GPU_FETCH_VRAM_FUNC\
		"	uniform sampler2D s_texture;\n"\
		"	vec2 VRAM(vec2 uv) { return texture2D(s_texture, uv).rg; }\n"
#endif

#define GTE_PERSPECTIVE_CORRECTION \
		"	gl_Position = Projection * vec4(a_position.xy, a_zw.x, 1.0);\n"

#define GTE_VERTEX_SHADER \
	"	attribute vec4 a_position;\n"\
	"	attribute vec4 a_texcoord; // uv, color multiplier, dither\n"\
	"	attribute vec4 a_color;\n"\
	"	attribute vec4 a_extra; // texcoord.xy ofs, unused.xy\n"\
	"	attribute vec4 a_zw;\n"\
	"	uniform mat4 Projection;\n"\
	"	uniform mat4 Projection3D;\n"\
	"	uniform int u_pgxpEnabled;\n"\
	"	const vec2 c_UVFudge = vec2(0.00025, 0.00025);\n"\
	"	void main() {\n"\
	"		v_texcoord = a_texcoord;\n"\
	"		v_texcoord.xy += a_extra.xy * 0.5;\n"\
	"		v_color = a_color;\n"\
	"		v_color.xyz *= a_texcoord.z;\n"\
	"		v_page_clut.x = fract(a_position.z / 16.0) * 1024.0;\n"\
	"		v_page_clut.y = floor(a_position.z / 16.0) * 256.0;\n"\
	"		v_page_clut.z = fract(a_position.w / 64.0);\n"\
	"		v_page_clut.w = floor(a_position.w / 64.0) / 512.0;\n"\
	"		v_page_clut.xy += c_UVFudge;\n"\
	"		v_page_clut.zw += c_UVFudge;\n"\
	GTE_PERSPECTIVE_CORRECTION\
	/* v_is3d gates dither + bilinear so 2D logos/UI render sharp.
	 * The `a_zw.y > 100` test only distinguishes 3D from 2D when the
	 * runtime PGXP master gate is on (then a_zw.y is the screen
	 * height ~240 for 3D content, 0 for 2D). With PGXP off at
	 * runtime, ApplyVertexPGXP zeroes a_zw for everything → without
	 * the u_pgxpEnabled override every prim would read v_is3d=0 and
	 * we'd lose dither / bilinear on real 3D geometry too (visibly
	 * blocky tree leaves, etc.). When pgxp off, fall back to legacy
	 * "always treat as 3D" behavior — matches legacy behavior. */	"		v_is3d = (u_pgxpEnabled > 0) ? ((a_zw.y > 100.0) ? 1.0 : 0.0) : 1.0;\n"\
	"		v_z = (gl_Position.z - 40.0) * 0.005;\n"\
	"		v_fogAmount = clamp(a_extra.z / 127.0, 0.0, 1.0);\n"\
	"	}\n"

#define GPU_FRAGMENT_SAMPLE_SHADER(bit) \
	GPU_PACK_RG_FUNC\
	GPU_DECODE_RG_FUNC\
	GPU_FETCH_VRAM_FUNC\
	"	const vec2 c_VRAMTexel = vec2(1.0 / 1024.0, 1.0 / 512.0);\n"\
	GPU_ARRAY_FUNC\
	GPU_SAMPLE_TEXTURE_## bit ##BIT_FUNC\
	GPU_BILINEAR_SAMPLE_FUNC\
	GPU_NEAREST_SAMPLE_FUNC\
	"	uniform int bilinearFilter;\n"\
	"	uniform float u_ditherForce;\n"\
	"	uniform float u_pixelScale;\n"\
	"	uniform vec3 u_fogColor;\n"\
	"	void main() {\n"\
	"		if(bilinearFilter > 0 && v_is3d > 0.5)\n"\
	"			fragColor = BilinearTextureSample(v_texcoord.xy);\n"\
	"		else\n"\
	"			fragColor = NearestTextureSample(v_texcoord.xy);\n"\
	"		fragColor *= v_color;\n"\
	"		fragColor.rgb = mix(fragColor.rgb, u_fogColor, v_fogAmount);\n"\
	GPU_DITHERING_NO_VCOLOR\
	"	}\n"

const char* gte_shader_4 =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(4)
	"#endif\n";

const char* gte_shader_8 =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(8)
	"#endif\n";

const char* gte_shader_16 =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	GPU_FRAGMENT_SAMPLE_SHADER(16)
	"#endif\n";

const char* gte_shader_32_rgba =
	"AFFINE_VARYING vec4 v_texcoord;\n"
	"varying vec4 v_color;\n"
	"AFFINE_VARYING vec4 v_page_clut;\n"
	"varying float v_z;\n"
	"varying float v_fogAmount;\n"
	"varying float v_is3d;\n"
	"#ifdef VERTEX\n"
	GTE_VERTEX_SHADER
	"#else\n"
	"	uniform sampler2D s_texture;\n"\
	"	uniform int bilinearFilter;\n"\
	"	uniform float u_ditherForce;\n"\
	"	uniform float u_pixelScale;\n"\
	"	uniform vec2 texelSize;\n"\
	"	void main() {\n"\
	"		vec2 tc = v_texcoord.xy * texelSize + texelSize * 0.5;\n"\
	"		fragColor = texture2D(s_texture, tc);\n"\
	GPU_DITHERING\
	"	}\n"
	"#endif\n";

int GR_Shader_CheckShaderStatus(GLuint shader)
{
	char info[1024];
	GLint result;

	glGetShaderiv(shader, GL_COMPILE_STATUS, &result);

	if (result == GL_TRUE)
		return 1;
	
	glGetShaderInfoLog(shader, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

int GR_Shader_CheckProgramStatus(GLuint program)
{
	char info[1024];
	GLint result;

	glGetProgramiv(program, GL_LINK_STATUS, &result);

	if (result == GL_TRUE)
		return 1;

	glGetProgramInfoLog(program, sizeof(info), NULL, info);
	if (info[0] && strlen(info) > 8)
	{
		eprinterr("%s\n", info);
		assert(0);
	}

	return 0;
}

ShaderID GR_Shader_Compile(const char* source)
{
#if defined(ES2_SHADERS)
	const char* GLSL_HEADER_VERT =
		"#version 100\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define VERTEX\n";

	const char* GLSL_HEADER_FRAG =
		"#version 100\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define fragColor gl_FragColor\n";
#elif defined(ES3_SHADERS)
	const char* GLSL_HEADER_VERT =
		"#version 300 es\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	const char* GLSL_HEADER_FRAG =
		"#version 300 es\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";
#else
	const char* GLSL_HEADER_VERT =
		"#version 140\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define VERTEX\n"
		"#define varying   out\n"
		"#define attribute in\n"
		"#define texture2D texture\n";

	const char* GLSL_HEADER_FRAG =
		"#version 140\n"
		"precision lowp  int;\n"
		"precision highp float;\n"
		"#define varying     in\n"
		"#define texture2D   texture\n"
		"out vec4 fragColor;\n";
#endif

	char extra_vs_defines[1024];
	char extra_fs_defines[1024];
	extra_vs_defines[0] = 0;
	extra_fs_defines[0] = 0;

	if (g_cfg_bilinearFiltering)
	{
		strcat(extra_fs_defines, "#define BILINEAR_FILTER\n");
	}

	/* Affine (non-perspective-correct) texture mapping — matches PSX GPU behaviour.
	 * Uses noperspective interpolation qualifier (GLSL 1.30+, desktop only). */
	if (g_cfg_affineTextures)
	{
		strcat(extra_vs_defines, "#define AFFINE_VARYING noperspective varying\n");
		strcat(extra_fs_defines, "#define AFFINE_VARYING noperspective varying\n");
	}
	else
	{
		strcat(extra_vs_defines, "#define AFFINE_VARYING varying\n");
		strcat(extra_fs_defines, "#define AFFINE_VARYING varying\n");
	}

	const char* vs_list[] = { GLSL_HEADER_VERT, extra_vs_defines, source };
	const char* fs_list[] = { GLSL_HEADER_FRAG, extra_fs_defines, source };

	GLuint program = glCreateProgram();

	{
		GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(vertexShader, 3, vs_list, NULL);
		glCompileShader(vertexShader);

		if( GR_Shader_CheckShaderStatus(vertexShader) == 0 )
			eprinterr("Failed to compile Vertex Shader!\n");
	
		glAttachShader(program, vertexShader);
		glDeleteShader(vertexShader);
	}

	{
		GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(fragmentShader, 3, fs_list, NULL);
		glCompileShader(fragmentShader);

		if(GR_Shader_CheckShaderStatus(fragmentShader) == 0)
			eprinterr("Failed to compile Fragment Shader!\n");
	
		glAttachShader(program, fragmentShader);
		glDeleteShader(fragmentShader);
	}

	glBindAttribLocation(program, a_position, "a_position");
	glBindAttribLocation(program, a_zw, "a_zw");
	glBindAttribLocation(program, a_texcoord, "a_texcoord");
	glBindAttribLocation(program, a_color, "a_color");
	glBindAttribLocation(program, a_extra, "a_extra");

	glLinkProgram(program);
	if(GR_Shader_CheckProgramStatus(program) == 0)
		eprinterr("Failed to link Shader!\n");

	GLint sampler = 0;
	glUseProgram(program);
	glUniform1iv(glGetUniformLocation(program, "s_texture"), 1, &sampler);
	glUseProgram(0);

	return program;
}
#else
#error
#endif

//--------------------------------------------------------------------------------------------

void GR_GenerateCommonTextures()
{
	unsigned int pixelData = 0xFFFFFFFF;

#if USE_OPENGL
	glGenTextures(1, &g_whiteTexture);
	{
		glBindTexture(GL_TEXTURE_2D, g_whiteTexture);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixelData);

		glBindTexture(GL_TEXTURE_2D, 0);
	}
#endif
}

TextureID GR_CreateRGBATexture(int width, int height, u_char* data /*= nullptr*/)
{
	TextureID newTexture;
	glGenTextures(1, &newTexture);

	glBindTexture(GL_TEXTURE_2D, newTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, g_cfg_bilinearFiltering ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_cfg_bilinearFiltering ? GL_LINEAR : GL_NEAREST);
	
	// another WebGL stuff. Texture will be black without clamp to edge
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
	glBindTexture(GL_TEXTURE_2D, 0);

	return newTexture;
}

void GR_CompilePSXShader(GTEShader* sh, const char* source)
{
	sh->shader = GR_Shader_Compile(source);

#if USE_OPENGL
	
	sh->bilinearFilterLoc = glGetUniformLocation(sh->shader, "bilinearFilter");
	sh->ditherForceLoc = glGetUniformLocation(sh->shader, "u_ditherForce");
	sh->pixelScaleLoc = glGetUniformLocation(sh->shader, "u_pixelScale");
	sh->projectionLoc = glGetUniformLocation(sh->shader, "Projection");
	sh->texelSizeLoc = glGetUniformLocation(sh->shader, "texelSize");
	sh->fogColorLoc = glGetUniformLocation(sh->shader, "u_fogColor");
	sh->pgxpEnabledLoc = glGetUniformLocation(sh->shader, "u_pgxpEnabled");
#endif
}

void GR_InitialisePSXShaders()
{
	GR_CompilePSXShader(&g_gte_shader_4, gte_shader_4);
	GR_CompilePSXShader(&g_gte_shader_8, gte_shader_8);
	GR_CompilePSXShader(&g_gte_shader_16, gte_shader_16);
	GR_CompilePSXShader(&g_gte_shader_32_rgba, gte_shader_32_rgba);
}

int GR_InitialisePSX()
{
	SDL_memset(vram, 0, VRAM_WIDTH * VRAM_HEIGHT * sizeof(unsigned short));
	GR_GenerateCommonTextures();
	GR_InitialisePSXShaders();

#if USE_OPENGL
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_STENCIL_TEST);
	glBlendColor(0.5f, 0.5f, 0.5f, 0.25f);

	// gen framebuffer
	{
		memset(&g_glFramebufferPBO, 0, sizeof(g_glFramebufferPBO));
		PBO_Init(&g_glFramebufferPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// make a special texture
		// it will be resized later
		glGenTextures(1, &g_fbTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_fbTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glBlitFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen offscreen RT
	{
		memset(&g_glOffscreenPBO, 0, sizeof(g_glOffscreenPBO));
		PBO_Init(&g_glOffscreenPBO, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 2);
		
		// offscreen texture render target
		glGenTextures(1, &g_offscreenRTTexture);
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// default to VRAM size
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VRAM_WIDTH, VRAM_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

			glBindTexture(GL_TEXTURE_2D, 0);
		}

		glGenFramebuffers(1, &g_glOffscreenFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_offscreenRTTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen VRAM textures.
	// double-buffered
	{
		int i;

		glGenTextures(2, g_vramTexturesDouble);

		for(i = 0; i < 2; i++)
		{
			glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

			// set storage size
			glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, NULL);
		}

		g_vramTexture = g_vramTexturesDouble[0];

		glBindTexture(GL_TEXTURE_2D, 0);

		// VRAM framebuffer for offscreen blitting to VRAM
		glGenFramebuffers(1, &g_glVRAMFramebuffer);
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	// gen vertex buffer and index buffer
	{
		int i;

		glGenBuffers(MAX_NUM_VERTEX_BUFFERS, g_glVertexBuffer);
		glGenVertexArrays(MAX_NUM_VERTEX_BUFFERS, g_glVertexArray);

		for (i = 0; i < MAX_NUM_VERTEX_BUFFERS; i++)
		{
			glBindVertexArray(g_glVertexArray[i]);

			glBindBuffer(GL_ARRAY_BUFFER, g_glVertexBuffer[i]);
			glBufferData(GL_ARRAY_BUFFER, sizeof(GrVertex) * MAX_VERTEX_BUFFER_SIZE, NULL, GL_DYNAMIC_DRAW);
		}

		glBindVertexArray(0);
	}
#else
#error
#endif

	GR_ResetDevice();

	return 1;
}

void GR_Ortho2D(float left, float right, float bottom, float top, float znear, float zfar)
{
	float a = 2.0f / (right - left);
	float b = 2.0f / (top - bottom);
	float c = 2.0f / (znear - zfar);

	float x = (left + right) / (left - right);
	float y = (bottom + top) / (bottom - top);

#if USE_OPENGL 
	// -1..1
	float z = (znear + zfar) / (znear - zfar);
#endif

	float ortho[16] = {
		a, 0, 0, 0,
		0, b, 0, 0,
		0, 0, c, 0,
		x, y, z, 1
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, ortho);
#endif
}

void GR_Perspective3D(const float fov, const float width, const float height, const float zNear, const float zFar)
{
	float sinF, cosF;
	sinF = sinf(0.5f * fov);
	cosF = cosf(0.5f * fov);

	float h = cosF / sinF;
	float w = (h * height) / width;

	float persp[16] = {
		w, 0, 0, 0,
		0, h, 0, 0,
		0, 0, (zFar + zNear) / (zFar - zNear), 1,
		0, 0, -(2 * zFar * zNear) / (zFar - zNear), 0
	};

#if USE_OPENGL
	glUniformMatrix4fv(u_projection3DLoc, 1, GL_FALSE, persp);
#endif
}

void GR_SetupClipMode(const RECT16* rect, int enable)
{
	// [A] isinterlaced dirty hack for widescreen
	const bool scissorOn = enable && (activeDispEnv.isinter ||
		(	rect->x - activeDispEnv.disp.x > 0 ||
			rect->y - activeDispEnv.disp.y > 0 ||
			rect->w < activeDispEnv.disp.w - 1 ||
			rect->h < activeDispEnv.disp.h - 1));

	GR_SetScissorState(scissorOn);

	if (!scissorOn)
		return;

	// Runtime gate: same as the ortho selection — non-PGXP path uses
	// emuScreenAspect=1 (no widescreen aspect remap) so the scissor
	// rectangle stays in pixel-space coordinates that match the legacy
	// 2D ortho. With PGXP at compile time but off at runtime, falling
	// through to the PGXP aspect calc would shrink the scissor and
	// cause clipping inconsistent with the prim ortho.
	const float emuScreenAspect = 1.0f;

	const float psxScreenWInv = 1.0f / (float)activeDispEnv.disp.w;
	const float psxScreenHInv = 1.0f / (float)activeDispEnv.disp.h;

	// first map to 0..1
	float clipRectX = (float)(rect->x - activeDispEnv.disp.x) * psxScreenWInv;
	float clipRectY = (float)(rect->y - activeDispEnv.disp.y) * psxScreenHInv;
	float clipRectW = (float)(rect->w) * psxScreenWInv;
	float clipRectH = (float)(rect->h) * psxScreenHInv;

	// then map to screen
	{
		clipRectX -= 0.5f;

		clipRectX *= emuScreenAspect;
		clipRectW *= emuScreenAspect;

		clipRectX += 0.5f;
	}

#if USE_OPENGL
	// adjust scissor rectangle by the backbuffer size (window dimensions)
	const float flipOffset = g_windowHeight - clipRectH * (float)g_windowHeight;
	const float crx = clipRectX * (float)g_windowWidth;
	const float cry = clipRectY * (float)g_windowHeight;
	const float crw = clipRectW * (float)g_windowWidth;
	const float crh = clipRectH * (float)g_windowHeight;

	glScissor(crx, flipOffset - cry, crw, crh);
#endif
}

void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect)
{

	rect->x = activeDispEnv.screen.x;
	rect->y = activeDispEnv.screen.y;
	rect->w = activeDispEnv.disp.w;
	rect->h = activeDispEnv.disp.h;
}

void GR_SetShader(const ShaderID shader)
{
	if (g_PreviousShader != shader)
	{
#if USE_OPENGL
		glUseProgram(shader);
#else
#error
#endif

		g_PreviousShader = shader;
	}
}


void GR_SetTexture(TextureID texture, TexFormat texFormat)
{
	switch (texFormat)
	{
	case TF_4_BIT:
		GR_SetShader(g_gte_shader_4.shader);
		u_bilinearFilterLoc = g_gte_shader_4.bilinearFilterLoc;
		u_ditherForceLoc = g_gte_shader_4.ditherForceLoc;
		u_pixelScaleLoc = g_gte_shader_4.pixelScaleLoc;
		u_projectionLoc = g_gte_shader_4.projectionLoc;
		u_projection3DLoc = g_gte_shader_4.projection3DLoc;
		u_texelSizeLoc = -1;
		u_fogColorLoc = g_gte_shader_4.fogColorLoc;
		u_pgxpEnabledLoc = g_gte_shader_4.pgxpEnabledLoc;
		break;
	case TF_8_BIT:
		GR_SetShader(g_gte_shader_8.shader);
		u_bilinearFilterLoc = g_gte_shader_8.bilinearFilterLoc;
		u_ditherForceLoc = g_gte_shader_8.ditherForceLoc;
		u_pixelScaleLoc = g_gte_shader_8.pixelScaleLoc;
		u_projectionLoc = g_gte_shader_8.projectionLoc;
		u_projection3DLoc = g_gte_shader_8.projection3DLoc;
		u_texelSizeLoc = -1;
		u_fogColorLoc = g_gte_shader_8.fogColorLoc;
		u_pgxpEnabledLoc = g_gte_shader_8.pgxpEnabledLoc;
		break;
	case TF_16_BIT:
		GR_SetShader(g_gte_shader_16.shader);
		u_bilinearFilterLoc = g_gte_shader_16.bilinearFilterLoc;
		u_ditherForceLoc = g_gte_shader_16.ditherForceLoc;
		u_pixelScaleLoc = g_gte_shader_16.pixelScaleLoc;
		u_projectionLoc = g_gte_shader_16.projectionLoc;
		u_projection3DLoc = g_gte_shader_16.projection3DLoc;
		u_texelSizeLoc = -1;
		u_fogColorLoc = g_gte_shader_16.fogColorLoc;
		u_pgxpEnabledLoc = g_gte_shader_16.pgxpEnabledLoc;
		break;
	case TF_32_BIT_RGBA:
		GR_SetShader(g_gte_shader_32_rgba.shader);
		u_bilinearFilterLoc = -1;
		u_ditherForceLoc = -1;
		u_pixelScaleLoc = -1;
		u_projectionLoc = g_gte_shader_32_rgba.projectionLoc;
		u_projection3DLoc = g_gte_shader_32_rgba.projection3DLoc;
		u_texelSizeLoc = g_gte_shader_32_rgba.texelSizeLoc;
		u_fogColorLoc = g_gte_shader_32_rgba.fogColorLoc;
		u_pgxpEnabledLoc = g_gte_shader_32_rgba.pgxpEnabledLoc;
		break;
	}

	/* Push u_pgxpEnabled every shader bind so vertex shader's v_is3d
	 * fallback ((u_pgxpEnabled > 0) ? a_zw.y test : 1.0) correctly
	 * drops the 3D-only gate when PGXP is off at runtime. Without
	 * this fallback, every prim got a_zw.y=0 → v_is3d=0 → no dither
	 * and forced-nearest sampling on actual 3D geometry (visible as
	 * blocky tree leaves). */
	if (u_pgxpEnabledLoc != -1)
		glUniform1i(u_pgxpEnabledLoc, g_PsxUsePgxp);

	if (u_fogColorLoc != -1)
		glUniform3fv(u_fogColorLoc, 1, g_PsyX_FogColor);

	/* Push the dither-force uniform every shader bind. Cheap (single
	 * float upload) and ensures runtime config changes (if we add a
	 * hotkey toggle later) take effect on the next primitive.
	 * g_PsxDitherSuppressed lets the game disable dither per-frame on
	 * 2D-only states (menus, logos, inventory) without changing the
	 * config flag. */
	if (u_ditherForceLoc != -1)
		glUniform1f(u_ditherForceLoc,
		            (g_cfg_psxDither && !g_PsxDitherSuppressed) ? 1.0f : 0.0f);

	/* Pixel scale = window width / PSX native (320). Scales the dither
	 * cell so each PSX-pixel-equivalent on screen gets its own matrix
	 * lookup, keeping the pattern visually proportional to PSX
	 * regardless of resolution. */
	if (u_pixelScaleLoc != -1) {
		float pixelScale = (g_windowWidth > 0)
			? ((float)g_windowWidth / 320.0f)
			: 1.0f;
		glUniform1f(u_pixelScaleLoc, pixelScale);
	}

	if (g_dbg_texturelessMode) {
		texture = g_whiteTexture;
	}

	if (g_lastBoundTexture == texture) {
		return;
	}

#if USE_OPENGL
	glBindTexture(GL_TEXTURE_2D, texture);
	if(u_bilinearFilterLoc != -1)
		glUniform1i(u_bilinearFilterLoc, g_cfg_bilinearFiltering && !g_PsxDitherSuppressed);

#endif

	g_lastBoundTexture = texture;
}

void GR_SetOverrideTextureSize(int width, int height)
{
	if(u_texelSizeLoc == -1)
		return;

	// WebGL is fucking around with glUniform2f, so use vector version
	float vec[] = { 1.0f / (float)width, 1.0f / (float)height };
	glUniform2fv(u_texelSizeLoc, 1, vec);
}

void GR_DestroyTexture(TextureID texture)
{
	if (texture == -1)
		return;

#if USE_OPENGL
	glDeleteTextures(1, &texture);
#else
#error
#endif
}

void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	vram_need_update = 1;

	u_short* dst = vram + x + y * VRAM_WIDTH;

	if (x + w > VRAM_WIDTH)
		w = VRAM_WIDTH - x;

	if (y + h > VRAM_HEIGHT)
		h = VRAM_HEIGHT - y;

	// clear VRAM region with given color
	for (int i = 0; i < h; i++)
	{
		u_short* tmp = dst;

		for (int j = 0; j < w; j++)
			*tmp++ = r | (g << 5) | (b << 11);

		dst += VRAM_WIDTH;
	}
}

void GR_Clear(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
	framebuffer_need_update = 1;

#if USE_OPENGL
	glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#endif
}

void GR_SaveVRAM(const char* outputFileName, int x, int y, int width, int height, int bReadFromFrameBuffer)
{
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)

#if USE_OPENGL

#define FLIP_Y (VRAM_HEIGHT - i - 1)

#endif

	FILE* fp = fopen(outputFileName, "wb");
	if (fp == NULL)
		return;

	unsigned char TGAheader[12] = { 0,0,2,0,0,0,0,0,0,0,0,0 };
	unsigned char header[6];
	header[0] = (width % 256);
	header[1] = (width / 256);
	header[2] = (height % 256);
	header[3] = (height / 256);
	header[4] = 16;
	header[5] = 0;

	fwrite(TGAheader, sizeof(unsigned char), 12, fp);
	fwrite(header, sizeof(unsigned char), 6, fp);

	for (int i = 0; i < VRAM_HEIGHT; i++)
	{
		fwrite(vram + VRAM_WIDTH * FLIP_Y, sizeof(short), VRAM_WIDTH, fp);
	}

	fclose(fp);

#undef FLIP_Y
#endif
}

void GR_CopyRGBAFramebufferToVRAM(u_int* src, int x, int y, int w, int h, int update_vram, int flip_y)
{
	assert(x >= 0);
	assert(y >= 0);
	assert(x + w <= VRAM_WIDTH);
	assert(y + h <= VRAM_WIDTH);

	ushort* fb = (ushort*)malloc(w * h * sizeof(ushort));
	uint* data_src = (uint*)src;
	ushort* data_dst = (ushort*)fb;

	for (int i = 0; i < h; i++)
	{
		for (int j = 0; j < w; j++)
		{
			uint c = *data_src++;

			u_char b = ((c >> 3) & 0x1F);
			u_char g = ((c >> 11) & 0x1F);
			u_char r = ((c >> 19) & 0x1F);
			//u_char a = ((c >> 24) & 0x1F);

			int a = r == g == b == 0 ? 0 : 1;

			*data_dst++ = r | (g << 5) | (b << 10) | (a << 15);
		}
	}

	ushort* ptr = (ushort*)vram + VRAM_WIDTH * y + x;

	for (int fy = 0; fy < h; fy++)
	{
		int py = flip_y ? (h - fy - 1) : fy;
		ushort* fb_ptr = fb + (h * py / h) * w;

		for (int fx = 0; fx < w; fx++)
			ptr[fx] = fb_ptr[w * fx / w];

		ptr += VRAM_WIDTH;
	}

	free(fb);

	if (update_vram)
		vram_need_update = 1;
}

void GR_ReadFramebufferDataToVRAM()
{
	int x, y, w, h;
	if (!framebuffer_need_update)
		return;

	framebuffer_need_update = 0;

	/* PC port: skip readback while a paper-map / TIM-protect screen is active.
	 * The readback writes the framebuffer back into vram[] at (disp.x, disp.y)
	 * which can clobber CLUT/texture rows that live inside the display rect
	 * (paper-map CLUT lives at VRAM (224,15) — inside (0,0)-(320,240)). */
	if (g_PsxSkipFramebufferStore)
		return;

	x = g_PreviousFramebuffer.x;
	y = g_PreviousFramebuffer.y;
	w = g_PreviousFramebuffer.w;
	h = g_PreviousFramebuffer.h;

	// now we can read it back to VRAM texture

#if USE_OPENGL && defined(USE_PBO)
	// read the texture
	if(g_glFramebufferPBO.pixels)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		PBO_Download(&g_glFramebufferPBO);
		glBindTexture(GL_TEXTURE_2D, 0);
		GR_CopyRGBAFramebufferToVRAM((u_int*)g_glFramebufferPBO.pixels, x, y, w, h, 0, 0);
	}
#endif
}

void GR_SetScissorState(int enable)
{
	if (g_PreviousScissorState == enable)
		return;

#if USE_OPENGL
	if (g_PreviousScissorState)
		glDisable(GL_SCISSOR_TEST);
	else
		glEnable(GL_SCISSOR_TEST);
#endif
	g_PreviousScissorState = enable;
}

void GR_SetOffscreenState(const RECT16* offscreenRect, int enable)
{
	if (enable)
	{
		// setup render target viewport
		GR_Ortho2D(0, offscreenRect->w, offscreenRect->h, 0, -1.0f, 1.0f);
	}
	else
	{
		// setup default viewport

		{
			// Hor+ widescreen: expand x range so the vertical FOV is preserved and
			// extra horizontal content fills the wider display instead of stretching.
			// Only apply during 3D gameplay states (g_PcHorPlusEnabled=1); 2D UI
			// screens use 4:3 ortho to avoid exposing adjacent VRAM as garbage.
			//
			// PSX NTSC pixel-aspect compensation: multiply the effective scale by
			// PSX_NTSC_PIXEL_ASPECT (~1.094). PSX's 320×240 framebuffer on a 4:3
			// CRT had pixels ~1.094× taller than wide; without this factor every
			// character renders ~9% horizontally fatter than the original (most
			// visible on Harry's torso/arms/legs).
			const float psxW = (float)activeDispEnv.disp.w;  // 320
			const float psxH = (float)activeDispEnv.disp.h;  // 240
			const float psxAspect = psxW / psxH;             // 4/3
			const float winAspect = (g_windowHeight > 0)
				? ((float)g_windowWidth / (float)g_windowHeight)
				: psxAspect;
			const float horScale = winAspect / psxAspect;
			const float effectiveScale = (g_PcHorPlusEnabled && horScale > 1.0f)
				? horScale * PSX_NTSC_PIXEL_ASPECT
				: 1.0f;
			const float margin = (effectiveScale > 1.0f)
				? psxW * (effectiveScale - 1.0f) * 0.5f
				: 0.0f;
			GR_Ortho2D(-margin, psxW + margin, psxH, 0, -1.0f, 1.0f);
		}

	}

	if (g_PreviousOffscreenState == enable)
		return;

	g_PreviousOffscreenState = enable;

#if USE_OPENGL
	if (enable)
	{
		// set storage size first
		if (g_PreviousOffscreen.w != offscreenRect->w &&
			g_PreviousOffscreen.h != offscreenRect->h)
		{
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, offscreenRect->w, offscreenRect->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glBindTexture(GL_TEXTURE_2D, 0);
		}

		g_PreviousOffscreen = *offscreenRect;

		GR_SetViewPort(0, 0, offscreenRect->w, offscreenRect->h);
		glBindFramebuffer(GL_FRAMEBUFFER, g_glOffscreenFramebuffer);

		// clear it out
		glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	else
	{
		GR_SetViewPort(0, 0, g_windowWidth, g_windowHeight);

		/* PC port: skip the offscreen-RT → VRAM writeback when a TIM-protect
		 * screen is active. This branch fires whenever a draw split has
		 * dfe=0 (offscreen render target). Both the GL blit and the CPU
		 * GR_CopyRGBAFramebufferToVRAM below write to VRAM at
		 * (g_PreviousOffscreen.x, g_PreviousOffscreen.y, w, h) — for paper-map
		 * pickup screens that rect overlaps the CLUT at (224,15) and texture
		 * origin (320,16), tiling rendered framebuffer content over the
		 * just-uploaded TIM. The viewport / state-tracking above must still
		 * run so subsequent draws use the correct framebuffer binding;
		 * only the writes are suppressed. */
		if (g_PsxSkipFramebufferStore)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
		else
		{
#if USE_OFFSCREEN_BLIT
		// before drawing set source and target
		{
			glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

			// rebind texture
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

			// setup draw and read framebuffers
			glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glOffscreenFramebuffer);					// source is backbuffer
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

			glBlitFramebuffer(0, 0, g_PreviousOffscreen.w, g_PreviousOffscreen.h,
								g_PreviousOffscreen.x, g_PreviousOffscreen.y + g_PreviousOffscreen.h, g_PreviousOffscreen.x + g_PreviousOffscreen.w, g_PreviousOffscreen.y,
								GL_COLOR_BUFFER_BIT, GL_NEAREST);

			// done, unbind
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		}
#endif

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		// copy rendering results to VRAM texture
		{
			// reat the texture
			glBindTexture(GL_TEXTURE_2D, g_offscreenRTTexture);
			//glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
			PBO_Download(&g_glOffscreenPBO);
			glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);

			// Don't forcely update VRAM
			GR_CopyRGBAFramebufferToVRAM((u_int*)g_glOffscreenPBO.pixels,
				g_PreviousOffscreen.x, g_PreviousOffscreen.y, g_PreviousOffscreen.w, g_PreviousOffscreen.h,
				USE_OFFSCREEN_BLIT == 0, 1);
		}
		}

	}
#endif
}

void GR_StoreFrameBuffer(int x, int y, int w, int h)
{
	/* PC port: skip the entire framebuffer→VRAM blit when a TIM-protect
	 * screen is active. Without this, every PsyX_EndScene blits the rendered
	 * frame onto g_vramTexture at (disp.x, disp.y, disp.w, disp.h), which
	 * destroys CLUTs/textures that the game just LoadImage'd into that
	 * region (paper-map CLUT at (224,15) is the canonical victim). */
	if (g_PsxSkipFramebufferStore)
		return;

#if USE_OPENGL
	// set storage size first
	if (g_PreviousFramebuffer.w != w ||
		g_PreviousFramebuffer.h != h)
	{
		glBindTexture(GL_TEXTURE_2D, g_fbTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	g_PreviousFramebuffer.x = x;
	g_PreviousFramebuffer.y = y;
	g_PreviousFramebuffer.w = w;
	g_PreviousFramebuffer.h = h;

#if USE_FRAMEBUFFER_BLIT
	glBindFramebuffer(GL_FRAMEBUFFER, g_glBlitFramebuffer);

	// before drawing set source and target
	{
		// setup draw and read framebuffers
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);					// source is backbuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glBlitFramebuffer);

		glBlitFramebuffer(0, 0, g_windowWidth, g_windowHeight, x, y + h, x + w, y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

		// Blit framebuffer to VRAM screen area

		// before drawing set source and target
		glBindFramebuffer(GL_FRAMEBUFFER, g_glVRAMFramebuffer);

		// rebind vram texture
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_vramTexture, 0);

		// setup draw and read framebuffers
		glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glBlitFramebuffer);					// source is backbuffer
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_glVRAMFramebuffer);

		glBlitFramebuffer(0, 0, w, h,
			x, y + h, x + w, y,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);

		
		// done, unbind
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	}

	// after drawing
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glFlush();
#endif

	GR_ReadFramebufferDataToVRAM();
#endif
}

void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
	vram_need_update = 1;

	int stride = w;

	if (!src)
	{
		framebuffer_need_update = 1;

		src = vram;
		stride = VRAM_WIDTH;
	}

	src += x + y * stride;

	unsigned short* dst = vram + dst_x + dst_y * VRAM_WIDTH;

	for (int i = 0; i < h; i++) {
		SDL_memcpy(dst, src, w * sizeof(short));
		dst += VRAM_WIDTH;
		src += stride;
	}
}

void GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h)
{
	unsigned short* src = vram + x + VRAM_WIDTH * y;

	for (int i = 0; i < dst_h; i++) {
		SDL_memcpy(dst, src, dst_w * sizeof(short));
		dst += dst_w;
		src += VRAM_WIDTH;
	}
}

void GR_UpdateVRAM()
{
	if (!vram_need_update)
		return;

	vram_need_update = 0;

#if USE_OPENGL
	g_vramTexture = g_vramTexturesDouble[g_vramTextureIdx];
	g_vramTextureIdx++;
	g_vramTextureIdx &= 1;

	glBindTexture(GL_TEXTURE_2D, g_vramTexture);

#if defined(RENDERER_OGL)
	glTexImage2D(GL_TEXTURE_2D, 0, VRAM_INTERNAL_FORMAT, VRAM_WIDTH, VRAM_HEIGHT, 0, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#else
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, VRAM_FORMAT, GL_UNSIGNED_BYTE, vram);
#endif

#endif
}

/* PC port: directly upload a sub-region of vram[] to BOTH double-buffered VRAM
 * textures, bypassing the vram_need_update / texture-swap dance. Used by the
 * paper-map TIM-protect helper to guarantee the paper-map data is present in
 * whichever texture the renderer is about to sample, even if some unfound
 * code path has stamped framebuffer pixels into the other texture.
 *
 * Why this exists: the gating in GR_StoreFrameBuffer / GR_ReadFramebufferDataToVRAM
 * via g_PsxSkipFramebufferStore is necessary but apparently not sufficient —
 * paper-map pickup screens still show tiled gameplay framebuffer content
 * sampled from the (320, 16+) VRAM region even with all known framebuffer→VRAM
 * paths gated. A direct upload is the nuclear-option fallback that defeats
 * any unfound corruption path: whatever wrote framebuffer pixels into the
 * GPU texture after the last GR_UpdateVRAM gets unconditionally overwritten. */
void GR_DirectUploadVRAMRegion(int x, int y, int w, int h)
{
#if USE_OPENGL
	if (x < 0 || y < 0 || w <= 0 || h <= 0)
		return;
	if (x + w > VRAM_WIDTH)  w = VRAM_WIDTH  - x;
	if (y + h > VRAM_HEIGHT) h = VRAM_HEIGHT - y;
	if (w <= 0 || h <= 0)
		return;

	/* Build a contiguous block from the vram[] sub-region (vram is 1024 stride). */
	unsigned short* block = (unsigned short*)malloc((size_t)w * (size_t)h * sizeof(unsigned short));
	if (!block)
		return;

	for (int row = 0; row < h; row++)
	{
		SDL_memcpy(block + (size_t)row * w,
		           vram + (size_t)(y + row) * VRAM_WIDTH + x,
		           (size_t)w * sizeof(unsigned short));
	}

	for (int i = 0; i < 2; i++)
	{
		glBindTexture(GL_TEXTURE_2D, g_vramTexturesDouble[i]);
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, VRAM_FORMAT, GL_UNSIGNED_BYTE, block);
	}
	glBindTexture(GL_TEXTURE_2D, g_lastBoundTexture);

	free(block);
#endif
}

void GR_SwapWindow()
{
#if defined(RENDERER_OGL) || defined(RENDERER_OGLES)
	SDL_GL_SwapWindow(g_window);
#endif

	//glFinish();
}

void GR_EnableDepth(int enable)
{
	if (g_PreviousDepthMode == enable)
		return;

	g_PreviousDepthMode = enable;

#if USE_OPENGL
	if (enable && g_cfg_pgxpZBuffer)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
#endif
}

void GR_SetStencilMode(int drawPrim)
{
	if (g_PreviousStencilMode == drawPrim)
		return;

	g_PreviousStencilMode = drawPrim;

#if USE_OPENGL
	if (drawPrim)
	{
		glStencilFunc(GL_ALWAYS, 1, 0x10);
		glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
	}
	else
	{
		glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
		glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
	}
#endif
}

void GR_SetBlendMode(BlendMode blendMode)
{
	if (g_PreviousBlendMode == blendMode)
		return;

#if USE_OPENGL
	if (blendMode == BM_NONE)
	{
		if (g_PreviousBlendMode != BM_NONE)
		{
			glBlendColor(1.f, 1.f, 1.f, 1.f);
			glDisable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(1);
		return;
	}
	else
	{
		if(g_PreviousBlendMode == BM_NONE)
		{
			glBlendColor(0.25f, 0.25f, 0.25f, 0.5f);
			glEnable(GL_BLEND);
		}

		g_PreviousBlendMode = blendMode;
		GR_EnableDepth(0);
	}

	glBlendEquationSeparate(blendMode == BM_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
	switch (blendMode) {
	case BM_AVERAGE:
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case BM_ADD:
	case BM_SUBTRACT:
		glBlendFunc(GL_ONE, GL_ONE);
		break;
	case BM_ADD_QUATER_SOURCE:
		glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE); 
		break;
	}
#endif

	g_PreviousBlendMode = blendMode;
}

void GR_SetPolygonOffset(float ofs)
{
#if USE_OPENGL
	if (ofs == 0.0f)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
	else
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(0.0f, ofs);
	}
#endif
}

void GR_SetViewPort(int x, int y, int width, int height)
{
#if USE_OPENGL
	glViewport(x, y, width, height);
#endif
}

void GR_SetWireframe(int enable)
{
#if defined(RENDERER_OGL)
	glPolygonMode(GL_FRONT_AND_BACK, enable ? GL_LINE : GL_FILL);
#endif
}

void GR_BindVertexBuffer()
{
#if USE_OPENGL
	glBindVertexArray(g_glVertexArray[g_curVertexBuffer]);

	glEnableVertexAttribArray(a_position);
	glEnableVertexAttribArray(a_texcoord);
	glEnableVertexAttribArray(a_color);
	glEnableVertexAttribArray(a_extra);

	glVertexAttribPointer(a_position, 4, GL_SHORT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->x);
	glVertexAttribPointer(a_zw, 1, GL_FLOAT, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->z);
	glEnableVertexAttribArray(a_zw);
	glVertexAttribPointer(a_texcoord, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->u);
	glVertexAttribPointer(a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GrVertex), &((GrVertex*)NULL)->r);
	glVertexAttribPointer(a_extra, 4, GL_BYTE, GL_FALSE, sizeof(GrVertex), &((GrVertex*)NULL)->tcx);

	g_curVertexBuffer++;
	g_curVertexBuffer &= 1;
#else
#error
#endif
}

void GR_UpdateVertexBuffer(const GrVertex* vertices, int num_vertices)
{
	if (num_vertices >= MAX_VERTEX_BUFFER_SIZE)
	{
		eprinterr("MAX_VERTEX_BUFFER_SIZE reached, expect rendering errors\n");
		num_vertices = MAX_VERTEX_BUFFER_SIZE;
	}

	//assert(num_vertices <= MAX_VERTEX_BUFFER_SIZE);
	GR_BindVertexBuffer();

#if USE_OPENGL
	glBufferSubData(GL_ARRAY_BUFFER, 0, num_vertices * sizeof(GrVertex), vertices);
#else
#error
#endif
}

void GR_DrawTriangles(int start_vertex, int triangles)
{
#if USE_OPENGL
	glDrawArrays(GL_TRIANGLES, start_vertex, triangles * 3);
#else
#error
#endif
}

void GR_PushDebugLabel(const char* label)
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPushDebugGroup)
		return;
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0x8000, strlen(label), label);
#endif
}

void GR_PopDebugLabel()
{
#if USE_OPENGL && !defined(__EMSCRIPTEN__) && defined(GL_DEBUG_SOURCE_APPLICATION)
	if (!glPopDebugGroup)
		return;
	glPopDebugGroup();
#endif
}