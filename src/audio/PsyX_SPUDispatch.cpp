#include "PsyX_SPUAL_ext.h"
#include "PsyX/PsyX_audio.h"

extern "C"
{

#define DECLARE_BACKEND(prefix) \
int prefix##_InitSound(); \
void prefix##_ShutdownSound(); \
int prefix##_Alloc(int); \
int prefix##_InitAlloc(int, char*); \
void prefix##_Free(u_int); \
u_int prefix##_Write(u_char*, u_int); \
u_int prefix##_Read(u_char*, u_int); \
u_int prefix##_SetTransferStartAddr(u_int); \
void prefix##_GetVoiceVolume(int, short*, short*); \
void prefix##_GetVoicePitch(int, u_short*); \
void prefix##_SetVoiceAttr(SpuVoiceAttr*); \
void prefix##_GetVoiceAttr(SpuVoiceAttr*); \
void prefix##_SetKey(int, u_int); \
void prefix##_Update(); \
void prefix##_SetAdsrEnabled(int); \
int prefix##_GetAdsrEnabled(); \
void prefix##_SetOutputMode(int); \
int prefix##_ApplyOutputMode(int); \
int prefix##_GetOutputMode(); \
int prefix##_GetSurroundActive(); \
void prefix##_SetNextKeyOnAzimuth(int); \
void prefix##_ClearNextKeyOnAzimuth(); \
void prefix##_SetVoiceAzimuth(int, int); \
int prefix##_GetKeyStatus(u_int); \
void prefix##_GetAllKeysStatus(char*); \
int prefix##_SetMute(int); \
int prefix##_SetReverb(int); \
int prefix##_GetReverbState(); \
u_int prefix##_SetReverbVoice(int, u_int); \
u_int prefix##_GetReverbVoice(); \
void prefix##_SetReverbDepthMasked(int, int, short, short); \
int prefix##_SetReverbMode(int); \
void prefix##_SetReverbDepthScale(float); \
float prefix##_GetReverbDepthScale()

/* PSYX_NO_OPENAL drops PsyX_SPULegacy.cpp (and the PsyX_SPUAL.cpp it includes)
 * from the build on platforms with no OpenAL — Android, where SDL2's own
 * AAudio/OpenSL sink is what PsyX_SPUSoftware writes to. Declaring the legacy
 * entry points here would leave every one of them undefined at link. */
#if !defined(PSYX_NO_OPENAL)
DECLARE_BACKEND(PsyX_SPULegacy);
#endif
DECLARE_BACKEND(PsyX_SPUSoftware);

int PsyX_SPUSoftware_AllocAt(u_int, int);
int PsyX_SPUSoftware_SetTransferMode(int);
void PsyX_SPUSoftware_SetCommonAttr(SpuCommonAttr*);
void PsyX_SPUSoftware_GetCommonAttr(SpuCommonAttr*);
void PsyX_SPUSoftware_GetReverbModeParam(SpuReverbAttr*);
int PsyX_SPUSoftware_ClearReverbWorkArea();
void PsyX_SPUSoftware_ConfigureOutput(int, int, int, int);
int PsyX_SPUSoftware_ConfigureRenderer(int, int, int, int);
void PsyX_SPUSoftware_ConfigureSpatial(int, int);
int PsyX_SPUSoftware_PushXaFrames(const short*, u_int, int, int);
void PsyX_SPUSoftware_ResetXa();
void PsyX_SPUSoftware_FinishXa();
int PsyX_SPUSoftware_IsXaDrained();
void PsyX_SPUSoftware_SetXaMasterGain(double);
void PsyX_SPUSoftware_SetXaPaused(int);
u_int PsyX_SPUSoftware_GetQueuedXaFrames();

#undef DECLARE_BACKEND
}

#if defined(PSYX_NO_OPENAL)
#include "PsyX/PsyX_public.h" /* PsyX_SfxOverrideFn */
/* Normally defined by PsyX_SPUAL.cpp, which PSYX_NO_OPENAL leaves out of the
 * build. The game assigns it unconditionally (pc_sfx_override.c), so it has to
 * exist here. NOTE: only the legacy AL mixer ever consumed this hook -- the
 * software SPU does not check it -- so SFX overrides are inert on
 * OpenAL-less platforms until PsyX_SPUSoftware learns to honour it. */
extern "C" PsyX_SfxOverrideFn g_PsyX_SfxOverride = NULL;
#endif

namespace
{
int g_renderer = 0;
bool g_initialized = false;

bool UseSoftware()
{
#if defined(PSYX_NO_OPENAL)
	return true;
#else
	return g_renderer != 0;
#endif
}
}

extern "C"
{

#if defined(PSYX_NO_OPENAL)
#define DISPATCH_VOID(name, args) \
	do { PsyX_SPUSoftware_##name args; } while (0)
#define DISPATCH_RETURN(name, args) \
	return PsyX_SPUSoftware_##name args
#else
#define DISPATCH_VOID(name, args) \
	do { if (UseSoftware()) PsyX_SPUSoftware_##name args; else PsyX_SPULegacy_##name args; } while (0)
#define DISPATCH_RETURN(name, args) \
	return UseSoftware() ? PsyX_SPUSoftware_##name args : PsyX_SPULegacy_##name args
#endif

void PsyX_SPUAL_ConfigureOutput(int backend, int mode, int rate, int bitPerfect)
{
	PsyX_SPUSoftware_ConfigureOutput(backend, mode, rate, bitPerfect);
}

/* Spatial output is a software-backend capability: the legacy OpenAL backend
 * already has its own layout handling, so this is simply ignored there. */
void PsyX_SPUAL_ConfigureSpatial(int enable, int speakers)
{
	PsyX_SPUSoftware_ConfigureSpatial(enable, speakers);
}

int PsyX_SPUAL_ConfigureRenderer(
	int renderer, int idealClip, int referenceClip, int referenceDither)
{
	if (g_initialized || renderer < 0 || renderer > 3 ||
		idealClip < 0 || idealClip > 2 ||
		referenceClip < 0 || referenceClip > 2 ||
		referenceDither < 0 || referenceDither > 1)
		return 0;

#if defined(PSYX_NO_OPENAL)
	/* renderer 0 selects the legacy AL backend, which this build does not
	 * contain. Leaving it at 0 would skip ConfigureRenderer below and hand
	 * UseSoftware() an unconfigured software renderer, so promote it to the
	 * PSX-accurate one instead of failing the call. */
	if (renderer == 0)
		renderer = 1;
#endif

	if (renderer != 0 &&
		!PsyX_SPUSoftware_ConfigureRenderer(
			renderer - 1, idealClip, referenceClip, referenceDither))
		return 0;

	g_renderer = renderer;
	return 1;
}

int PsyX_SPUAL_InitSound()
{
#if defined(PSYX_NO_OPENAL)
	const int result = PsyX_SPUSoftware_InitSound();
#else
	const int result = UseSoftware()
		? PsyX_SPUSoftware_InitSound()
		: PsyX_SPULegacy_InitSound();
#endif
	g_initialized = result != 0;
	return result;
}

void PsyX_SPUAL_ShutdownSound()
{
#if defined(PSYX_NO_OPENAL)
	PsyX_SPUSoftware_ShutdownSound();
#else
	if (UseSoftware())
		PsyX_SPUSoftware_ShutdownSound();
	else
		PsyX_SPULegacy_ShutdownSound();
#endif
	g_initialized = false;
}

int PsyX_SPUAL_Alloc(int size) { DISPATCH_RETURN(Alloc, (size)); }
int PsyX_SPUAL_AllocAt(u_int addr, int size)
{
	return UseSoftware() ? PsyX_SPUSoftware_AllocAt(addr, size) : 0;
}
int PsyX_SPUAL_InitAlloc(int num, char* top) { DISPATCH_RETURN(InitAlloc, (num, top)); }
void PsyX_SPUAL_Free(u_int addr) { DISPATCH_VOID(Free, (addr)); }
u_int PsyX_SPUAL_Write(u_char* addr, u_int size) { DISPATCH_RETURN(Write, (addr, size)); }
u_int PsyX_SPUAL_Read(u_char* addr, u_int size) { DISPATCH_RETURN(Read, (addr, size)); }
u_int PsyX_SPUAL_SetTransferStartAddr(u_int addr) { DISPATCH_RETURN(SetTransferStartAddr, (addr)); }
int PsyX_SPUAL_SetTransferMode(int mode)
{
	return UseSoftware()
		? PsyX_SPUSoftware_SetTransferMode(mode)
		: (mode == SPU_TRANSFER_BY_IO ? SPU_TRANSFER_BY_IO : SPU_TRANSFER_BY_DMA);
}
void PsyX_SPUAL_GetVoiceVolume(int voice, short* left, short* right) { DISPATCH_VOID(GetVoiceVolume, (voice, left, right)); }
void PsyX_SPUAL_GetVoicePitch(int voice, u_short* pitch) { DISPATCH_VOID(GetVoicePitch, (voice, pitch)); }
void PsyX_SPUAL_SetVoiceAttr(SpuVoiceAttr* attr) { DISPATCH_VOID(SetVoiceAttr, (attr)); }
void PsyX_SPUAL_GetVoiceAttr(SpuVoiceAttr* attr) { DISPATCH_VOID(GetVoiceAttr, (attr)); }
void PsyX_SPUAL_SetCommonAttr(SpuCommonAttr* attr)
{
	if (UseSoftware()) PsyX_SPUSoftware_SetCommonAttr(attr);
}
void PsyX_SPUAL_GetCommonAttr(SpuCommonAttr* attr)
{
	if (UseSoftware()) PsyX_SPUSoftware_GetCommonAttr(attr);
}
void PsyX_SPUAL_SetKey(int onOff, u_int voiceBits) { DISPATCH_VOID(SetKey, (onOff, voiceBits)); }
void PsyX_SPUAL_Update() { DISPATCH_VOID(Update, ()); }
void PsyX_SPUAL_SetAdsrEnabled(int on) { DISPATCH_VOID(SetAdsrEnabled, (on)); }
int PsyX_SPUAL_GetAdsrEnabled() { DISPATCH_RETURN(GetAdsrEnabled, ()); }
void PsyX_SPUAL_SetOutputMode(int mode) { DISPATCH_VOID(SetOutputMode, (mode)); }
int PsyX_SPUAL_ApplyOutputMode(int mode) { DISPATCH_RETURN(ApplyOutputMode, (mode)); }
int PsyX_SPUAL_GetOutputMode() { DISPATCH_RETURN(GetOutputMode, ()); }
int PsyX_SPUAL_GetSurroundActive() { DISPATCH_RETURN(GetSurroundActive, ()); }
void PsyX_SPUAL_SetNextKeyOnAzimuth(int azimuth) { DISPATCH_VOID(SetNextKeyOnAzimuth, (azimuth)); }
void PsyX_SPUAL_ClearNextKeyOnAzimuth() { DISPATCH_VOID(ClearNextKeyOnAzimuth, ()); }
void PsyX_SPUAL_SetVoiceAzimuth(int voice, int azimuth) { DISPATCH_VOID(SetVoiceAzimuth, (voice, azimuth)); }
int PsyX_SPUAL_GetKeyStatus(u_int voiceBit) { DISPATCH_RETURN(GetKeyStatus, (voiceBit)); }
void PsyX_SPUAL_GetAllKeysStatus(char* status) { DISPATCH_VOID(GetAllKeysStatus, (status)); }
int PsyX_SPUAL_SetMute(int onOff) { DISPATCH_RETURN(SetMute, (onOff)); }
int PsyX_SPUAL_SetReverb(int onOff) { DISPATCH_RETURN(SetReverb, (onOff)); }
int PsyX_SPUAL_GetReverbState() { DISPATCH_RETURN(GetReverbState, ()); }
u_int PsyX_SPUAL_SetReverbVoice(int onOff, u_int voiceBits) { DISPATCH_RETURN(SetReverbVoice, (onOff, voiceBits)); }
u_int PsyX_SPUAL_GetReverbVoice() { DISPATCH_RETURN(GetReverbVoice, ()); }
void PsyX_SPUAL_SetReverbDepthMasked(int maskL, int maskR, short depthL, short depthR)
{
	DISPATCH_VOID(SetReverbDepthMasked, (maskL, maskR, depthL, depthR));
}
int PsyX_SPUAL_SetReverbMode(int mode) { DISPATCH_RETURN(SetReverbMode, (mode)); }
void PsyX_SPUAL_GetReverbModeParam(SpuReverbAttr* attr)
{
	if (UseSoftware()) PsyX_SPUSoftware_GetReverbModeParam(attr);
}
void PsyX_SPUAL_SetReverbDepthScale(float scale) { DISPATCH_VOID(SetReverbDepthScale, (scale)); }
float PsyX_SPUAL_GetReverbDepthScale() { DISPATCH_RETURN(GetReverbDepthScale, ()); }
int PsyX_SPUAL_ClearReverbWorkArea()
{
	return UseSoftware() ? PsyX_SPUSoftware_ClearReverbWorkArea() : 0;
}

int PsyX_SPUAL_PushXaFrames(const short* samples, u_int frames, int sourceRate, int channels)
{
	return UseSoftware()
		? PsyX_SPUSoftware_PushXaFrames(samples, frames, sourceRate, channels)
		: 0;
}
void PsyX_SPUAL_ResetXa() { if (UseSoftware()) PsyX_SPUSoftware_ResetXa(); }
void PsyX_SPUAL_FinishXa() { if (UseSoftware()) PsyX_SPUSoftware_FinishXa(); }
int PsyX_SPUAL_IsXaDrained() { return UseSoftware() ? PsyX_SPUSoftware_IsXaDrained() : 1; }
void PsyX_SPUAL_SetXaMasterGain(double gain) { if (UseSoftware()) PsyX_SPUSoftware_SetXaMasterGain(gain); }
void PsyX_SPUAL_SetXaPaused(int paused) { if (UseSoftware()) PsyX_SPUSoftware_SetXaPaused(paused); }
u_int PsyX_SPUAL_GetQueuedXaFrames() { return UseSoftware() ? PsyX_SPUSoftware_GetQueuedXaFrames() : 0; }

int PsyX_AudioPushXaFrames(const int16_t* samples, uint32_t frames,
	uint32_t sourceRate, uint32_t channels)
{
	return PsyX_SPUAL_PushXaFrames(samples, frames, sourceRate, channels);
}
void PsyX_AudioResetXa() { PsyX_SPUAL_ResetXa(); }
void PsyX_AudioFinishXa() { PsyX_SPUAL_FinishXa(); }
int PsyX_AudioIsXaDrained() { return PsyX_SPUAL_IsXaDrained(); }
void PsyX_AudioSetXaMasterGain(double gain) { PsyX_SPUAL_SetXaMasterGain(gain); }
void PsyX_AudioSetXaPaused(int paused) { PsyX_SPUAL_SetXaPaused(paused); }
uint32_t PsyX_AudioGetQueuedXaFrames() { return PsyX_SPUAL_GetQueuedXaFrames(); }

#if defined(PSYX_NO_OPENAL)
void Pc_SpuStopLoopingVoices(void)
{
	/* On PSYX_NO_OPENAL builds (Android), the software SPU is used and loops are handled by the SPU core */
}
#endif

#undef DISPATCH_RETURN
#undef DISPATCH_VOID

}
