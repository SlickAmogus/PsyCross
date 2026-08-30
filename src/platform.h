#ifndef PLATFORM_SETUP_H
#define PLATFORM_SETUP_H

#ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>

//-----------------------------------------------------------------------

#if defined(__APPLE__)
#   include <SDL2/SDL.h>
#elif defined(__EMSCRIPTEN__)
#   include <emscripten/emscripten.h>
#   include <emscripten/html5.h>
#   include <SDL2/SDL.h>
#elif defined(_WINDOWS) || defined(__MINGW32__) || defined(__linux__) || defined(__ANDROID__) || defined(__RPI__)
#   include <SDL.h>
#endif

//-----------------------------------------------------------------------

#ifdef _MSC_VER
#define FUNCNAME __FUNCTION__
#else
#define FUNCNAME __func__
#endif

#include "PsyX/PsyX_globals.h"

/* Android used to get its own logging branch here, but it defined
 * eprintinfof/eprintwarnf — names no call site in this codebase ever used (they
 * all call eprintinfo/eprintwarn), so that branch could never have compiled and
 * was dead upstream leftovers. Every platform now routes through PsyX_Log*,
 * which is what puts diagnostics in g_logStream where the port's triage
 * workflow expects to read them. SDL redirects stdout/stderr into logcat on
 * Android, so `adb logcat` still sees this. */
#define eprintf(fmt, ...)       PsyX_Log("[Psy-X] " fmt, ##__VA_ARGS__)
#define eprintinfo(fmt, ...)    PsyX_Log_Info("[Psy-X] " fmt, ##__VA_ARGS__)
#define eprintwarn(fmt, ...)    PsyX_Log_Warning("[Psy-X] " fmt, ##__VA_ARGS__)
#define eprinterr(fmt, ...)     PsyX_Log_Error("[Psy-X] [%s] - " fmt, FUNCNAME, ##__VA_ARGS__);

//-----------------------------------------------------------------------

#if defined(__EMSCRIPTEN__) || !defined(_DEBUG)
#   define PSYX_UNIMPLEMENTED()
#else
#   define PSYX_UNIMPLEMENTED() eprinterr("Unimplemented!\n");
#endif

#endif