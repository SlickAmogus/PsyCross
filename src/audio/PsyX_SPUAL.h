#ifndef PSYX_SPUAL_H
#define PSYX_SPUAL_H

#include "psx/types.h"
#include "psx/libspu.h"
#include "PsyX/PsyX_config.h"
#include "PsyX/common/pgxp_defs.h"

#if defined(PSYX_SPUAL_LEGACY_BACKEND)
#define PSYX_SPUAL_BACKEND_NAME(name) PsyX_SPULegacy_##name
#elif defined(PSYX_SPUAL_SOFTWARE_BACKEND)
#define PSYX_SPUAL_BACKEND_NAME(name) PsyX_SPUSoftware_##name
#endif

#ifdef PSYX_SPUAL_BACKEND_NAME
#define PsyX_SPUAL_InitSound PSYX_SPUAL_BACKEND_NAME(InitSound)
#define PsyX_SPUAL_ShutdownSound PSYX_SPUAL_BACKEND_NAME(ShutdownSound)
#define PsyX_SPUAL_Alloc PSYX_SPUAL_BACKEND_NAME(Alloc)
#define PsyX_SPUAL_AllocAt PSYX_SPUAL_BACKEND_NAME(AllocAt)
#define PsyX_SPUAL_InitAlloc PSYX_SPUAL_BACKEND_NAME(InitAlloc)
#define PsyX_SPUAL_Free PSYX_SPUAL_BACKEND_NAME(Free)
#define PsyX_SPUAL_Write PSYX_SPUAL_BACKEND_NAME(Write)
#define PsyX_SPUAL_Read PSYX_SPUAL_BACKEND_NAME(Read)
#define PsyX_SPUAL_SetTransferStartAddr PSYX_SPUAL_BACKEND_NAME(SetTransferStartAddr)
#define PsyX_SPUAL_SetTransferMode PSYX_SPUAL_BACKEND_NAME(SetTransferMode)
#define PsyX_SPUAL_GetVoiceVolume PSYX_SPUAL_BACKEND_NAME(GetVoiceVolume)
#define PsyX_SPUAL_GetVoicePitch PSYX_SPUAL_BACKEND_NAME(GetVoicePitch)
#define PsyX_SPUAL_SetVoiceAttr PSYX_SPUAL_BACKEND_NAME(SetVoiceAttr)
#define PsyX_SPUAL_GetVoiceAttr PSYX_SPUAL_BACKEND_NAME(GetVoiceAttr)
#define PsyX_SPUAL_SetCommonAttr PSYX_SPUAL_BACKEND_NAME(SetCommonAttr)
#define PsyX_SPUAL_GetCommonAttr PSYX_SPUAL_BACKEND_NAME(GetCommonAttr)
#define PsyX_SPUAL_SetKey PSYX_SPUAL_BACKEND_NAME(SetKey)
#define PsyX_SPUAL_Update PSYX_SPUAL_BACKEND_NAME(Update)
#define PsyX_SPUAL_SetAdsrEnabled PSYX_SPUAL_BACKEND_NAME(SetAdsrEnabled)
#define PsyX_SPUAL_GetAdsrEnabled PSYX_SPUAL_BACKEND_NAME(GetAdsrEnabled)
#define PsyX_SPUAL_SetOutputMode PSYX_SPUAL_BACKEND_NAME(SetOutputMode)
#define PsyX_SPUAL_ApplyOutputMode PSYX_SPUAL_BACKEND_NAME(ApplyOutputMode)
#define PsyX_SPUAL_GetOutputMode PSYX_SPUAL_BACKEND_NAME(GetOutputMode)
#define PsyX_SPUAL_GetSurroundActive PSYX_SPUAL_BACKEND_NAME(GetSurroundActive)
#define PsyX_SPUAL_SetNextKeyOnAzimuth PSYX_SPUAL_BACKEND_NAME(SetNextKeyOnAzimuth)
#define PsyX_SPUAL_ClearNextKeyOnAzimuth PSYX_SPUAL_BACKEND_NAME(ClearNextKeyOnAzimuth)
#define PsyX_SPUAL_SetVoiceAzimuth PSYX_SPUAL_BACKEND_NAME(SetVoiceAzimuth)
#define PsyX_SPUAL_GetKeyStatus PSYX_SPUAL_BACKEND_NAME(GetKeyStatus)
#define PsyX_SPUAL_GetAllKeysStatus PSYX_SPUAL_BACKEND_NAME(GetAllKeysStatus)
#define PsyX_SPUAL_SetMute PSYX_SPUAL_BACKEND_NAME(SetMute)
#define PsyX_SPUAL_SetReverb PSYX_SPUAL_BACKEND_NAME(SetReverb)
#define PsyX_SPUAL_GetReverbState PSYX_SPUAL_BACKEND_NAME(GetReverbState)
#define PsyX_SPUAL_SetReverbVoice PSYX_SPUAL_BACKEND_NAME(SetReverbVoice)
#define PsyX_SPUAL_GetReverbVoice PSYX_SPUAL_BACKEND_NAME(GetReverbVoice)
#define PsyX_SPUAL_SetReverbDepthMasked PSYX_SPUAL_BACKEND_NAME(SetReverbDepthMasked)
#define PsyX_SPUAL_SetReverbMode PSYX_SPUAL_BACKEND_NAME(SetReverbMode)
#define PsyX_SPUAL_GetReverbModeParam PSYX_SPUAL_BACKEND_NAME(GetReverbModeParam)
#define PsyX_SPUAL_SetReverbDepthScale PSYX_SPUAL_BACKEND_NAME(SetReverbDepthScale)
#define PsyX_SPUAL_GetReverbDepthScale PSYX_SPUAL_BACKEND_NAME(GetReverbDepthScale)
#define PsyX_SPUAL_ClearReverbWorkArea PSYX_SPUAL_BACKEND_NAME(ClearReverbWorkArea)
#define PsyX_SPUAL_ConfigureOutput PSYX_SPUAL_BACKEND_NAME(ConfigureOutput)
#define PsyX_SPUAL_ConfigureRenderer PSYX_SPUAL_BACKEND_NAME(ConfigureRenderer)
#define PsyX_SPUAL_PushXaFrames PSYX_SPUAL_BACKEND_NAME(PushXaFrames)
#define PsyX_SPUAL_ResetXa PSYX_SPUAL_BACKEND_NAME(ResetXa)
#define PsyX_SPUAL_FinishXa PSYX_SPUAL_BACKEND_NAME(FinishXa)
#define PsyX_SPUAL_IsXaDrained PSYX_SPUAL_BACKEND_NAME(IsXaDrained)
#define PsyX_SPUAL_SetXaMasterGain PSYX_SPUAL_BACKEND_NAME(SetXaMasterGain)
#define PsyX_SPUAL_SetXaPaused PSYX_SPUAL_BACKEND_NAME(SetXaPaused)
#define PsyX_SPUAL_GetQueuedXaFrames PSYX_SPUAL_BACKEND_NAME(GetQueuedXaFrames)
#endif

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern int PsyX_SPUAL_InitSound();
extern void PsyX_SPUAL_ShutdownSound();

// Private
extern int PsyX_SPUAL_Alloc(int size);
extern int PsyX_SPUAL_AllocAt(u_int addr, int size);
extern int PsyX_SPUAL_InitAlloc(int num, char* top);
extern void PsyX_SPUAL_Free(u_int addr);
extern u_int PsyX_SPUAL_Write(u_char* addr, u_int size);
extern u_int PsyX_SPUAL_Read(u_char* addr, u_int size);
extern u_int PsyX_SPUAL_SetTransferStartAddr(u_int addr);
extern int PsyX_SPUAL_SetTransferMode(int mode);

extern void PsyX_SPUAL_GetVoiceVolume(int vNum, short* volL, short* volR);
extern void PsyX_SPUAL_GetVoicePitch(int vNum, u_short* pitch);
extern void PsyX_SPUAL_SetVoiceAttr(SpuVoiceAttr* psxAttrib);
extern void PsyX_SPUAL_GetVoiceAttr(SpuVoiceAttr* psxAttrib);
extern void PsyX_SPUAL_SetCommonAttr(SpuCommonAttr* psxAttrib);
extern void PsyX_SPUAL_GetCommonAttr(SpuCommonAttr* psxAttrib);
extern void PsyX_SPUAL_SetKey(int on_off, u_int voice_bit);
extern void PsyX_SPUAL_Update();
extern void PsyX_SPUAL_SetAdsrEnabled(int on);
extern int  PsyX_SPUAL_GetAdsrEnabled(void);

/* Speaker layout (0=auto 1=stereo 2=quad 3=5.1 4=7.1 5=hrtf). Set latches a
 * request for init; Apply renegotiates a live device (alcResetDeviceSOFT).
 * Get returns the ACHIEVED layout, which is what routing must trust. */
extern void PsyX_SPUAL_SetOutputMode(int mode);
extern int  PsyX_SPUAL_ApplyOutputMode(int mode);
extern int  PsyX_SPUAL_GetOutputMode(void);
extern int  PsyX_SPUAL_GetSurroundActive(void);

/* Emitter azimuth side-channel (PSX Q12 angle: 0 = ahead, positive = right).
 * SetNext arms the sound about to key on; SetVoiceAzimuth live-updates an
 * already-identified voice (Sd_SfxAttributesUpdate path). */
extern void PsyX_SPUAL_SetNextKeyOnAzimuth(int azimuthQ12);
extern void PsyX_SPUAL_ClearNextKeyOnAzimuth(void);
extern void PsyX_SPUAL_SetVoiceAzimuth(int voiceIdx, int azimuthQ12);

extern int PsyX_SPUAL_GetKeyStatus(u_int voice_bit);
extern void PsyX_SPUAL_GetAllKeysStatus(char* status);

extern int PsyX_SPUAL_SetMute(int on_off);
extern int PsyX_SPUAL_SetReverb(int on_off);
extern int PsyX_SPUAL_GetReverbState();
extern u_int PsyX_SPUAL_SetReverbVoice(int on_off, u_int voice_bit);
extern u_int PsyX_SPUAL_GetReverbVoice();
extern void PsyX_SPUAL_SetReverbDepthMasked(int maskL, int maskR, short depthL, short depthR);
extern int PsyX_SPUAL_SetReverbMode(int mode);
extern void PsyX_SPUAL_GetReverbModeParam(SpuReverbAttr* attr);
extern void PsyX_SPUAL_SetReverbDepthScale(float scale);
extern float PsyX_SPUAL_GetReverbDepthScale(void);
extern int PsyX_SPUAL_ClearReverbWorkArea(void);

/* Software-SPU output configuration. backend: 0=auto, 1=WASAPI, 2=SDL.
 * mode: 0=auto, 1=shared, 2=exclusive event-driven. */
extern void PsyX_SPUAL_ConfigureOutput(int backend, int mode, int rate, int bitPerfect);
/* renderer: 0=legacy OpenAL (default), 1=exact, 2=ideal, 3=reference. */
extern int PsyX_SPUAL_ConfigureRenderer(int renderer, int idealClip, int referenceClip, int referenceDither);

/* Decoded XA/CD ingress. Input is mono/stereo s16 at 37800 or 18900 Hz.
 * The legacy OpenAL backend does not support this decoded stream path:
 * Push returns 0 and the remaining functions report an empty/drained stream. */
extern int PsyX_SPUAL_PushXaFrames(const short* samples, u_int frames, int sourceRate, int channels);
extern void PsyX_SPUAL_ResetXa(void);
extern void PsyX_SPUAL_FinishXa(void);
extern int PsyX_SPUAL_IsXaDrained(void);
extern void PsyX_SPUAL_SetXaMasterGain(double gain);
extern void PsyX_SPUAL_SetXaPaused(int paused);
extern u_int PsyX_SPUAL_GetQueuedXaFrames(void);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif // PSYX_SPUAL_H