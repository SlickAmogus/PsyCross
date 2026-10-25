#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define PSYX_STRICMP _stricmp
#else
#include <strings.h>
#define PSYX_STRICMP strcasecmp
#endif

#if defined(__APPLE__)
#include <dlfcn.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#endif

/* The EGL bootstrap below is built wherever ANGLE is a shipped option: Windows
 * (D3D11 / Vulkan / WARP / SwiftShader) and Apple platforms (Metal). */
#if defined(_WIN32) || defined(__APPLE__)
#define PSYX_ANGLE_BOOTSTRAP 1
#endif

#include "PsyX/PsyX_backend.h"
#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_globals.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int			g_cfg_renderBackend = PSYX_BACKEND_GL;
int			g_grActiveBackend   = PSYX_BACKEND_GL;
int			g_grIsGLES          = 0;
PsyXGlCaps	g_grCaps            = { 1, 1, 1, 1, 1, 1, 1, 1 };

/* EGL, declared locally rather than vendoring Khronos headers: this needs nine
 * entry points and a page of constants, all of which are ABI-frozen. Values are
 * taken from ANGLE's own eglext_angle.h. */
typedef void*		EGLDisplay;
typedef void*		EGLSurface;
typedef void*		EGLContext;
typedef void*		EGLConfig;
typedef void*		EGLNativeWindowType;
typedef int			EGLint;
typedef unsigned	EGLBoolean;
typedef unsigned	EGLenum;

#define PSYX_EGL_NONE					0x3038
#define PSYX_EGL_FALSE					0
#define PSYX_EGL_ALPHA_SIZE				0x3021
#define PSYX_EGL_BLUE_SIZE				0x3022
#define PSYX_EGL_GREEN_SIZE				0x3023
#define PSYX_EGL_RED_SIZE				0x3024
#define PSYX_EGL_DEPTH_SIZE				0x3025
#define PSYX_EGL_STENCIL_SIZE			0x3026
#define PSYX_EGL_SAMPLES				0x3031
#define PSYX_EGL_SAMPLE_BUFFERS			0x3032
#define PSYX_EGL_SURFACE_TYPE			0x3033
#define PSYX_EGL_WINDOW_BIT				0x0004
#define PSYX_EGL_RENDERABLE_TYPE		0x3040
#define PSYX_EGL_OPENGL_ES3_BIT			0x00000040
#define PSYX_EGL_CONTEXT_CLIENT_VERSION	0x3098

#define PSYX_EGL_PLATFORM_ANGLE_ANGLE						0x3202
#define PSYX_EGL_PLATFORM_ANGLE_TYPE_ANGLE					0x3203
#define PSYX_EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE			0x3208
#define PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE			0x3209
#define PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE	0x320A
#define PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE	0x320B
#define PSYX_EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE			0x3450
#define PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE 0x3487
#define PSYX_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE			0x3489

typedef struct
{
	int			backend;
	const char* name;
	int			anglePlatformType;	/* EGL_PLATFORM_ANGLE_TYPE_*, 0 = not translated */
	int			angleDeviceType;	/* EGL_PLATFORM_ANGLE_DEVICE_TYPE_*, 0 = default */
	const char* description;
} BackendInfo;

/* ANGLE dropped its D3D9 backend, so "d3d9" is only kept as an alias onto
 * D3D11 in the name parser. WARP and SwiftShader are both CPU rasterizers:
 * WARP is Microsoft's, ships with Windows, and is the better bet on a machine
 * whose GPU driver is the problem; SwiftShader is ANGLE's own and needs no D3D
 * at all. */
static const BackendInfo s_backends[] =
{
	{ PSYX_BACKEND_AUTO,     "auto",     0, 0, "Automatic"                     },
	{ PSYX_BACKEND_GL,       "gl",       0, 0, "OpenGL (native)"               },
	{ PSYX_BACKEND_GLES,     "gles",     0, 0, "OpenGL ES 3.0"                 },
	{ PSYX_BACKEND_D3D11,    "d3d11",    PSYX_EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
	                                        PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
	                                                                            "Direct3D 11 (via ANGLE)"       },
	{ PSYX_BACKEND_VULKAN,   "vulkan",   PSYX_EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
	                                        PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
	                                                                            "Vulkan (via ANGLE)"            },
	{ PSYX_BACKEND_WARP,     "warp",     PSYX_EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
	                                        PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE,
	                                                                            "Direct3D 11 WARP (CPU)"        },
	{ PSYX_BACKEND_SOFTWARE, "software", PSYX_EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
	                                        PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE,
	                                                                            "Software raster (SwiftShader)" },
	/* Apple's OpenGL and OpenGL ES are both deprecated; this runs the same ES 3.0
	 * path against ANGLE's Metal backend. On Windows ANGLE has no Metal device, so
	 * selecting it there fails in eglGetPlatformDisplayEXT or eglInitialize and
	 * falls back to GL like any other unavailable device. */
	{ PSYX_BACKEND_METAL,    "metal",    PSYX_EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
	                                        PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
	                                                                            "Metal (via ANGLE)"             },
};

static const BackendInfo* Backend_Find(int backend)
{
	for (int i = 0; i < (int)(sizeof(s_backends) / sizeof(s_backends[0])); i++)
	{
		if (s_backends[i].backend == backend)
			return &s_backends[i];
	}
	return &s_backends[1]; /* gl */
}

int PsyX_Backend_FromName(const char* name)
{
	if (name == NULL || name[0] == 0)
		return PSYX_BACKEND_GL;

	for (int i = 0; i < (int)(sizeof(s_backends) / sizeof(s_backends[0])); i++)
	{
		if (PSYX_STRICMP(name, s_backends[i].name) == 0)
			return s_backends[i].backend;
	}

	/* Aliases people will actually type. d3d12 and d3d9 both land on d3d11:
	 * ANGLE has no D3D12 backend and has removed its D3D9 one, and silently
	 * giving them the working DXGI-hookable path is better than dropping them
	 * to GL without a word. */
#ifdef _WIN32
	if (PSYX_STRICMP(name, "dx11") == 0 || PSYX_STRICMP(name, "directx11") == 0) return PSYX_BACKEND_D3D11;
	if (PSYX_STRICMP(name, "dx12") == 0 || PSYX_STRICMP(name, "d3d12") == 0 || PSYX_STRICMP(name, "directx12") == 0) return PSYX_BACKEND_D3D11;
	if (PSYX_STRICMP(name, "dx9") == 0 || PSYX_STRICMP(name, "d3d9") == 0) return PSYX_BACKEND_D3D11;
	if (PSYX_STRICMP(name, "vk") == 0)   return PSYX_BACKEND_VULKAN;
	if (PSYX_STRICMP(name, "opengl") == 0) return PSYX_BACKEND_GL;
	if (PSYX_STRICMP(name, "swiftshader") == 0 || PSYX_STRICMP(name, "cpu") == 0) return PSYX_BACKEND_SOFTWARE;
#endif
	if (PSYX_STRICMP(name, "mtl") == 0) return PSYX_BACKEND_METAL;

	return PSYX_BACKEND_GL;
}

const char* PsyX_Backend_GetName(int backend)
{
	return Backend_Find(backend)->name;
}

const char* PsyX_Backend_GetDescription(int backend)
{
	return Backend_Find(backend)->description;
}

int PsyX_Backend_IsTranslated(int backend)
{
	return Backend_Find(backend)->anglePlatformType != 0;
}

/* ---------------------------------------------------------------------------
 * EGL bootstrap (translated backends only)
 * ------------------------------------------------------------------------ */

#if defined(PSYX_ANGLE_BOOTSTRAP)

/* One spelling for the two dynamic loaders. ANGLE is a pair of modules on every
 * platform: libEGL(.dll/.dylib) holds EGL, libGLESv2 holds GL. */
#ifdef _WIN32
typedef HMODULE PsyXLib;
static PsyXLib	Lib_Open(const char* path)             { return LoadLibraryA(path); }
static void*	Lib_Sym(PsyXLib lib, const char* name) { return (void*)GetProcAddress(lib, name); }
static void		Lib_Close(PsyXLib lib)                 { FreeLibrary(lib); }
#else
typedef void*	PsyXLib;
static PsyXLib	Lib_Open(const char* path)             { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void*	Lib_Sym(PsyXLib lib, const char* name) { return dlsym(lib, name); }
static void		Lib_Close(PsyXLib lib)                 { dlclose(lib); }
#endif

#if defined(__APPLE__)
/* Where the ANGLE dylibs live, relative to SDL_GetBasePath: the executable's
 * directory for a plain build (pc_port/angle/ is copied there), and in an app
 * bundle Contents/Resources, whose sibling Contents/Frameworks is where the
 * packager puts them next to SDL and OpenAL. libEGL dlopens libGLESv2 from its
 * own directory, so both must sit in the same place. */
static const char* Angle_Dir(void)
{
	static char s_dir[1024];
	static int  s_probed = 0;

	if (s_probed)
		return s_dir[0] ? s_dir : NULL;
	s_probed = 1;

	char* base = SDL_GetBasePath();	/* "<...>/build/" or "<App>.app/Contents/Resources/" */
	if (!base)
		return NULL;

	const char* rel[2] = { "", "../Frameworks/" };
	for (int i = 0; i < 2; i++)
	{
		char egl[1024], gles[1024];
		snprintf(egl,  sizeof(egl),  "%s%slibEGL.dylib",    base, rel[i]);
		snprintf(gles, sizeof(gles), "%s%slibGLESv2.dylib", base, rel[i]);
		if (access(egl, R_OK) == 0 && access(gles, R_OK) == 0)
		{
			snprintf(s_dir, sizeof(s_dir), "%s%s", base, rel[i]);
			break;
		}
	}
	SDL_free(base);
	return s_dir[0] ? s_dir : NULL;
}
#endif

typedef EGLDisplay	(*PFN_eglGetPlatformDisplayEXT)(EGLenum, void*, const EGLint*);
typedef EGLBoolean	(*PFN_eglInitialize)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean	(*PFN_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLSurface	(*PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*);
typedef EGLContext	(*PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLBoolean	(*PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean	(*PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef EGLBoolean	(*PFN_eglSwapInterval)(EGLDisplay, EGLint);
typedef EGLBoolean	(*PFN_eglTerminate)(EGLDisplay);
typedef EGLBoolean	(*PFN_eglDestroySurface)(EGLDisplay, EGLSurface);
typedef EGLBoolean	(*PFN_eglDestroyContext)(EGLDisplay, EGLContext);
typedef void*		(*PFN_eglGetProcAddress)(const char*);
typedef EGLint		(*PFN_eglGetError)(void);

static struct
{
	PsyXLib		egl;
	PsyXLib		glesv2;
#if defined(__APPLE__)
	void*		glesv2Base;	/* image base of libGLESv2, see PsyX_Angle_GetProcAddress */
#endif
	EGLDisplay	display;
	EGLSurface	surface;
	EGLContext	context;
	int			active;

	PFN_eglGetPlatformDisplayEXT	GetPlatformDisplayEXT;
	PFN_eglInitialize				Initialize;
	PFN_eglChooseConfig				ChooseConfig;
	PFN_eglCreateWindowSurface		CreateWindowSurface;
	PFN_eglCreateContext			CreateContext;
	PFN_eglMakeCurrent				MakeCurrent;
	PFN_eglSwapBuffers				SwapBuffers;
	PFN_eglSwapInterval				SwapInterval;
	PFN_eglTerminate				Terminate;
	PFN_eglDestroySurface			DestroySurface;
	PFN_eglDestroyContext			DestroyContext;
	PFN_eglGetProcAddress			GetProcAddress_;
	PFN_eglGetError					GetError;
} s_egl;

int PsyX_Angle_Active(void)
{
	return s_egl.active;
}

static int Angle_LoadEntryPoints(void)
{
	if (s_egl.egl)
		return 1;

#ifdef _WIN32
	const char* eglPath  = "libEGL.dll";
	const char* glesPath = "libGLESv2.dll";
#else
	char eglPath[1024], glesPath[1024];
	const char* dir = Angle_Dir();
	if (!dir)
	{
		PsyX_Log_Warning("[Psy-X] ANGLE: libEGL.dylib + libGLESv2.dylib not found next to the game or in Contents/Frameworks\n");
		return 0;
	}
	snprintf(eglPath,  sizeof(eglPath),  "%slibEGL.dylib",    dir);
	snprintf(glesPath, sizeof(glesPath), "%slibGLESv2.dylib", dir);
#endif

	s_egl.egl = Lib_Open(eglPath);
	if (!s_egl.egl)
	{
		PsyX_Log_Warning("[Psy-X] ANGLE: %s could not be loaded\n", eglPath);
		return 0;
	}

	/* Held open so the GL entry points stay resolvable for the process
	 * lifetime; ANGLE splits EGL and GL across the two modules. */
	s_egl.glesv2 = Lib_Open(glesPath);
#if defined(__APPLE__)
	s_egl.glesv2Base = NULL;
	if (s_egl.glesv2)
	{
		Dl_info info;
		void* probe = Lib_Sym(s_egl.glesv2, "glGetString");
		if (probe && dladdr(probe, &info))
			s_egl.glesv2Base = info.dli_fbase;
	}
#endif

	#define EGL_BIND(field, name) \
		s_egl.field = (PFN_##name)Lib_Sym(s_egl.egl, #name); \
		if (!s_egl.field) { Lib_Close(s_egl.egl); s_egl.egl = NULL; return 0; }

	EGL_BIND(GetPlatformDisplayEXT, eglGetPlatformDisplayEXT)
	EGL_BIND(Initialize,            eglInitialize)
	EGL_BIND(ChooseConfig,          eglChooseConfig)
	EGL_BIND(CreateWindowSurface,   eglCreateWindowSurface)
	EGL_BIND(CreateContext,         eglCreateContext)
	EGL_BIND(MakeCurrent,           eglMakeCurrent)
	EGL_BIND(SwapBuffers,           eglSwapBuffers)
	EGL_BIND(SwapInterval,          eglSwapInterval)
	EGL_BIND(Terminate,             eglTerminate)
	EGL_BIND(DestroySurface,        eglDestroySurface)
	EGL_BIND(DestroyContext,        eglDestroyContext)
	EGL_BIND(GetProcAddress_,       eglGetProcAddress)
	EGL_BIND(GetError,              eglGetError)
	#undef EGL_BIND

	return 1;
}

int PsyX_Angle_Create(void* nativeWindow, int backend, int msaaSamples)
{
	const BackendInfo* info = Backend_Find(backend);

	if (info->anglePlatformType == 0 || nativeWindow == NULL)
		return 0;

	if (!Angle_LoadEntryPoints())
		return 0;

	/* Steam installs implicit Vulkan layers (its overlay and the Fossilize
	 * shader cache) that the loader puts into every Vulkan instance of a game
	 * Steam launched, ANGLE's included. A Steam-launched run lost its ANGLE
	 * Vulkan device in its first frames while the same build ran clean outside
	 * Steam, so they are switched off through the opt-outs the layers' own
	 * manifests declare. Steam's overlay is then absent on this backend only.
	 * A value the user set themselves is left alone. */
	if (info->anglePlatformType == PSYX_EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE &&
	    (getenv("SteamAppId") || getenv("SteamGameId") || getenv("STEAM_COMPAT_APP_ID")))
	{
		static const char* const kSteamLayerOptOuts[] =
		{
			"DISABLE_VK_LAYER_VALVE_steam_overlay_1",
			"DISABLE_VK_LAYER_VALVE_steam_fossilize_1",
		};
		for (int i = 0; i < 2; i++)
		{
			if (GetEnvironmentVariableA(kSteamLayerOptOuts[i], NULL, 0) == 0)
				SetEnvironmentVariableA(kSteamLayerOptOuts[i], "1");
		}
		PsyX_Log_Warning("[Psy-X] ANGLE: launched by Steam, Steam's Vulkan overlay and shader-cache layers disabled for '%s'\n",
			info->name);
	}

	/* This — not an environment variable — is what actually selects D3D11 vs
	 * Vulkan vs a CPU rasterizer. */
	const EGLint displayAttribs[] =
	{
		PSYX_EGL_PLATFORM_ANGLE_TYPE_ANGLE,        (EGLint)info->anglePlatformType,
		PSYX_EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, (EGLint)info->angleDeviceType,
		PSYX_EGL_NONE
	};

	/* The native display handle. Windows ANGLE accepts -1 here (its legacy
	 * EGL_SOFTWARE_DISPLAY_ANGLE value) and has always been called that way; on
	 * Apple platforms anything but EGL_DEFAULT_DISPLAY (0) is rejected. */
#ifdef _WIN32
	void* nativeDisplay = (void*)(-1);
#else
	void* nativeDisplay = NULL;
#endif
	s_egl.display = s_egl.GetPlatformDisplayEXT(PSYX_EGL_PLATFORM_ANGLE_ANGLE,
		nativeDisplay, displayAttribs);

	if (s_egl.display == NULL)
	{
		PsyX_Log_Warning("[Psy-X] ANGLE: eglGetPlatformDisplayEXT failed for '%s' (egl error 0x%X)\n",
			info->name, s_egl.GetError());
		return 0;
	}

	EGLint major = 0, minor = 0;
	if (!s_egl.Initialize(s_egl.display, &major, &minor))
	{
		PsyX_Log_Warning("[Psy-X] ANGLE: eglInitialize failed for '%s' (egl error 0x%X) - that device is not available on this machine\n",
			info->name, s_egl.GetError());
		s_egl.display = NULL;
		return 0;
	}

	/* Depth 24 / stencil 8 to match what the desktop path asks SDL for; the
	 * renderer uses both. MSAA is requested only when configured, because a
	 * multisample config can fail outright on a CPU rasterizer. */
	EGLint configAttribs[24];
	int n = 0;
	configAttribs[n++] = PSYX_EGL_SURFACE_TYPE;     configAttribs[n++] = PSYX_EGL_WINDOW_BIT;
	configAttribs[n++] = PSYX_EGL_RENDERABLE_TYPE;  configAttribs[n++] = PSYX_EGL_OPENGL_ES3_BIT;
	configAttribs[n++] = PSYX_EGL_RED_SIZE;         configAttribs[n++] = 8;
	configAttribs[n++] = PSYX_EGL_GREEN_SIZE;       configAttribs[n++] = 8;
	configAttribs[n++] = PSYX_EGL_BLUE_SIZE;        configAttribs[n++] = 8;
	configAttribs[n++] = PSYX_EGL_ALPHA_SIZE;       configAttribs[n++] = 8;
	configAttribs[n++] = PSYX_EGL_DEPTH_SIZE;       configAttribs[n++] = 24;
	configAttribs[n++] = PSYX_EGL_STENCIL_SIZE;     configAttribs[n++] = 8;
	if (msaaSamples > 0)
	{
		configAttribs[n++] = PSYX_EGL_SAMPLE_BUFFERS; configAttribs[n++] = 1;
		configAttribs[n++] = PSYX_EGL_SAMPLES;        configAttribs[n++] = msaaSamples;
	}
	configAttribs[n++] = PSYX_EGL_NONE;

	EGLConfig config = NULL;
	EGLint numConfigs = 0;
	if (!s_egl.ChooseConfig(s_egl.display, configAttribs, &config, 1, &numConfigs) || numConfigs < 1)
	{
		/* Retry without MSAA before giving up — same policy as the SDL path. */
		if (msaaSamples > 0)
		{
			n -= 5;
			configAttribs[n++] = PSYX_EGL_NONE;
			if (!s_egl.ChooseConfig(s_egl.display, configAttribs, &config, 1, &numConfigs) || numConfigs < 1)
			{
				PsyX_Angle_Destroy();
				return 0;
			}
		}
		else
		{
			PsyX_Log_Warning("[Psy-X] ANGLE: no ES3 window config for '%s' (egl error 0x%X)\n", info->name, s_egl.GetError());
			PsyX_Angle_Destroy();
			return 0;
		}
	}

	s_egl.surface = s_egl.CreateWindowSurface(s_egl.display, config, (EGLNativeWindowType)nativeWindow, NULL);
	if (s_egl.surface == NULL)
	{
		PsyX_Log_Warning("[Psy-X] ANGLE: eglCreateWindowSurface failed for '%s' (egl error 0x%X)\n", info->name, s_egl.GetError());
		PsyX_Angle_Destroy();
		return 0;
	}

	const EGLint contextAttribs[] = { PSYX_EGL_CONTEXT_CLIENT_VERSION, 3, PSYX_EGL_NONE };
	s_egl.context = s_egl.CreateContext(s_egl.display, config, NULL, contextAttribs);
	if (s_egl.context == NULL)
	{
		PsyX_Log_Warning("[Psy-X] ANGLE: eglCreateContext failed for '%s' (egl error 0x%X)\n", info->name, s_egl.GetError());
		PsyX_Angle_Destroy();
		return 0;
	}

	if (!s_egl.MakeCurrent(s_egl.display, s_egl.surface, s_egl.surface, s_egl.context))
	{
		PsyX_Log_Warning("[Psy-X] ANGLE: eglMakeCurrent failed for '%s' (egl error 0x%X)\n", info->name, s_egl.GetError());
		PsyX_Angle_Destroy();
		return 0;
	}

	s_egl.active = 1;
	return 1;
}

void PsyX_Angle_Destroy(void)
{
	if (!s_egl.egl)
		return;

	if (s_egl.display)
	{
		s_egl.MakeCurrent(s_egl.display, NULL, NULL, NULL);
		if (s_egl.context) s_egl.DestroyContext(s_egl.display, s_egl.context);
		if (s_egl.surface) s_egl.DestroySurface(s_egl.display, s_egl.surface);
		s_egl.Terminate(s_egl.display);
	}

	s_egl.display = NULL;
	s_egl.surface = NULL;
	s_egl.context = NULL;
	s_egl.active  = 0;
}

#define PSYX_EGL_CONTEXT_LOST	0x300E

void PsyX_Angle_Swap(void)
{
	if (s_egl.active && !s_egl.SwapBuffers(s_egl.display, s_egl.surface) &&
	    s_egl.GetError() == PSYX_EGL_CONTEXT_LOST)
		PsyX_Backend_OnContextLost("eglSwapBuffers");
}

void PsyX_Angle_SetSwapInterval(int interval)
{
	if (s_egl.active)
		s_egl.SwapInterval(s_egl.display, (EGLint)interval);
}

void* PsyX_Angle_GetProcAddress(const char* name)
{
	if (!s_egl.active)
		return NULL;

	/* eglGetProcAddress is the documented route, but ANGLE (like most ES
	 * implementations) returns NULL from it for core entry points and expects
	 * those to be linked from libGLESv2 directly. glad needs both kinds, so
	 * fall back to the module export table. */
	void* p = s_egl.GetProcAddress_(name);
	if (!p && s_egl.glesv2)
	{
		p = Lib_Sym(s_egl.glesv2, name);
#if defined(__APPLE__)
		/* dlsym searches the module's dependencies too, and ANGLE's libGLESv2
		 * reaches Apple's OpenGL.framework through Cocoa. Without this check
		 * every desktop-only name (glPolygonMode, glGetTexImage, glMapBuffer,
		 * glDrawBuffer, glClearDepth) resolves to Apple's GL, which has no
		 * context here and crashes on first use; g_grCaps must see them as
		 * absent, as on Windows. Only a symbol from libGLESv2's own image counts. */
		if (p)
		{
			Dl_info info;
			if (!s_egl.glesv2Base || !dladdr(p, &info) || info.dli_fbase != s_egl.glesv2Base)
				p = NULL;
		}
#endif
	}

	return p;
}

#else /* !PSYX_ANGLE_BOOTSTRAP */

int   PsyX_Angle_Active(void) { return 0; }
int   PsyX_Angle_Create(void* nativeWindow, int backend, int msaaSamples) { (void)nativeWindow; (void)backend; (void)msaaSamples; return 0; }
void  PsyX_Angle_Destroy(void) {}
void  PsyX_Angle_Swap(void) {}
void  PsyX_Angle_SetSwapInterval(int interval) { (void)interval; }
void* PsyX_Angle_GetProcAddress(const char* name) { (void)name; return NULL; }

#endif

const char* PsyX_Shader_Preamble(int fragmentStage, int dialect)
{
	/* ES fragment shaders have no default float precision — omitting the
	 * qualifier is a compile error, not a warning — so it is declared here
	 * rather than left to each shader body. highp matches what the desktop
	 * profile gives these shaders implicitly. */
	static const char* const kEs3Vert   = "#version 300 es\nprecision highp float;\n";
	static const char* const kEs3Frag   = "#version 300 es\nprecision highp float;\n";
	static const char* const kEs100Vert = "#version 100\nprecision highp float;\n";
	static const char* const kEs100Frag = "#version 100\nprecision highp float;\n";
	static const char* const kGl140     = "#version 140\n";
	/* Legacy desktop bodies deliberately get NO #version: they rely on the
	 * GLSL 1.10 default, and stamping a version on them would only start
	 * enforcing rules they were never written against. */
	static const char* const kGlLegacy  = "";
#ifdef __APPLE__
	/* ...except on macOS, which only hands out a CORE profile: there an
	 * unversioned body is GLSL 1.10, which the core context rejects outright
	 * ("#version required and missing"), so the quick options overlay and the
	 * other legacy-dialect panels never drew. Compile them as 1.50 and map the
	 * legacy keywords onto their core equivalents. */
	static const char* const kGlLegacyCoreVert =
		"#version 150\n"
		"#define attribute in\n"
		"#define varying out\n"
		"#define texture2D texture\n";
	static const char* const kGlLegacyCoreFrag =
		"#version 150\n"
		"#define varying in\n"
		"#define texture2D texture\n"
		"out vec4 psyx_FragColor;\n"
		"#define gl_FragColor psyx_FragColor\n";
#endif

	if (g_grIsGLES)
	{
		if (dialect == PSYX_GLSL_LEGACY)
			return fragmentStage ? kEs100Frag : kEs100Vert;
		return fragmentStage ? kEs3Frag : kEs3Vert;
	}

#ifdef __APPLE__
	if (dialect == PSYX_GLSL_LEGACY)
		return fragmentStage ? kGlLegacyCoreFrag : kGlLegacyCoreVert;
#endif
	return (dialect == PSYX_GLSL_LEGACY) ? kGlLegacy : kGl140;
}

#ifdef _WIN32
/* ANGLE ships as a pair of DLLs. SDL loads them by bare name, which resolves
 * against the exe directory first, so that is where we look. Probing with
 * GetFileAttributes rather than LoadLibrary keeps them out of the process until
 * SDL genuinely wants them — loading libGLESv2 into a process that then creates
 * a desktop GL context has been known to confuse overlay injectors. */
static int Angle_ProbeDir(const char* dir)
{
	char path[MAX_PATH];
	static const char* kDlls[2] = { "libEGL.dll", "libGLESv2.dll" };

	for (int i = 0; i < 2; i++)
	{
		int n = _snprintf(path, sizeof(path), "%s%s%s", dir ? dir : "", (dir && dir[0]) ? "\\" : "", kDlls[i]);
		if (n < 0 || n >= (int)sizeof(path))
			return 0;

		DWORD attr = GetFileAttributesA(path);
		if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
			return 0;
	}
	return 1;
}
#endif

int PsyX_Backend_AngleAvailable(void)
{
#ifdef _WIN32
	static int s_cached = -1;
	if (s_cached >= 0)
		return s_cached;

	char exePath[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, exePath, sizeof(exePath));
	if (len > 0 && len < sizeof(exePath))
	{
		char* slash = strrchr(exePath, '\\');
		if (slash)
		{
			*slash = 0;
			if (Angle_ProbeDir(exePath))
			{
				s_cached = 1;
				return 1;
			}
		}
	}

	s_cached = Angle_ProbeDir("");
	return s_cached;
#elif defined(__APPLE__)
	return Angle_Dir() != NULL;
#else
	/* Linux: ANGLE is not shipped with the game there — the native GL driver is
	 * the sane path, and Vulkan users are served by Zink/mesa. */
	return 0;
#endif
}

void PsyX_Exit();

static int s_relaunchOnGl = 0;

void PsyX_Backend_OnContextLost(const char* where)
{
	static int s_handled = 0;

	/* Native GL has nothing to fall back to, and a relaunch onto the same
	 * backend would just lose its device again. */
	if (s_handled || !PsyX_Backend_IsTranslated(g_grActiveBackend))
		return;
	s_handled = 1;

	PsyX_Log_Warning("[Psy-X] The '%s' renderer lost its graphics device (%s); restarting the game on OpenGL (native). "
		"Set the renderer to OpenGL in the launcher to skip this.\n",
		PsyX_Backend_GetName(g_grActiveBackend), where);
#ifdef _WIN32
	s_relaunchOnGl = 1;
#endif
	PsyX_Exit();
}

void PsyX_Backend_RelaunchAtExit(void)
{
#ifdef _WIN32
	if (!s_relaunchOnGl)
		return;

	static wchar_t cmd[32768];
	const wchar_t* self = GetCommandLineW();
	if (wcslen(self) + 16 >= sizeof(cmd) / sizeof(cmd[0]))
		return;
	wcscpy(cmd, self);
	wcscat(cmd, L" -renderer gl");

	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
		return;

	DWORD code = 0;
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	ExitProcess(code);
#endif
}
