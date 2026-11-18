#ifndef PSYX_AUDIO_SINK_H
#define PSYX_AUDIO_SINK_H

#include "PsyX/PsyX_audio.h"

#include <memory>
#include <vector>

namespace psyx::audio
{

struct AudioSharedState;

struct SinkStartParams
{
	PsyXAudioConfig config;
	PsyXAudioRenderCallback renderCallback;
	PsyXAudioRenderCallbackFloat64 renderCallbackFloat64;
	void* renderUser;
	uint32_t sourceRate;
	PsyXAudioDither dither;
	AudioSharedState* shared;
};

class AudioSink
{
public:
	virtual ~AudioSink() = default;
	virtual PsyXAudioResult start(const SinkStartParams& params) = 0;
	virtual void stop() = 0;
};

std::unique_ptr<AudioSink> createSdlSink();
PsyXAudioResult enumerateSdlDevices(std::vector<PsyXAudioDeviceInfo>& devices);

/* An app container has no IMMDeviceEnumerator; SDL's own WASAPI path activates
 * the device the way a packaged app is allowed to. */
#if defined(_WIN32) && !defined(PSYX_UWP)
#define PSYX_HAVE_WASAPI 1
#endif

#ifdef PSYX_HAVE_WASAPI
std::unique_ptr<AudioSink> createWasapiSink();
PsyXAudioResult enumerateWasapiDevices(std::vector<PsyXAudioDeviceInfo>& devices);
#endif

} // namespace psyx::audio

#endif
