#include "PsyX_SPUSpatial.h"
#include "PsyX_SPUCore.h"
#include "../PsyX_main.h"

#include <SDL.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <math.h>
#include <string.h>
#include <vector>

namespace
{
using PsyX::SPUCore;

/* One render block per queued buffer. 512 frames is 11.6ms at 44100, so the
 * four-deep queue below buffers about 46ms: enough to ride out a scheduling
 * hiccup without adding latency anyone notices on a menu blip. */
const int kBlockFrames = 512;
const int kQueueDepth  = 4;
const int kRate        = 44100;

/* Dry buses across the FRONT arc only. The PSX image is stereo, so there is no
 * such thing as a voice the game meant to place behind you, and spreading the
 * dry signal into the rears would invent information the mix never had. Five
 * directions is enough that a hard-panned voice lands convincingly. */
const float kDryAzimuthsDeg[] = { -90.0f, -45.0f, 0.0f, 45.0f, 90.0f };
const int   kDryBuses = (int)(sizeof(kDryAzimuthsDeg) / sizeof(kDryAzimuthsDeg[0]));

/* The reverb return goes BEHIND the listener. It is the one genuinely ambient
 * part of the mix, and it is what makes a surround layout worth having here. */
const float kWetAzimuthsDeg[] = { -135.0f, 135.0f };
const int   kWetBuses = 2;

/* CD/XA (music and voice tracks) is an ordinary stereo bed at the usual angles. */
const float kCdAzimuthsDeg[] = { -30.0f, 30.0f };
const int   kCdBuses = 2;

const int kTotalBuses = kDryBuses + kWetBuses + kCdBuses;

struct Bus
{
    ALuint source;
    ALuint buffers[kQueueDepth];
    std::vector<int16_t> scratch;
};

ALCdevice*  g_dev = NULL;
ALCcontext* g_ctx = NULL;
ALCcontext* g_prevCtx = NULL;
Bus         g_bus[kTotalBuses];
SPUCore*    g_core = NULL;
SDL_mutex*  g_coreMutex = NULL;
SDL_Thread* g_thread = NULL;
volatile int g_running = 0;
int          g_active = 0;

void (*g_xaPump)(void* user, int frames) = NULL;
void*  g_xaPumpUser = NULL;

std::vector<int16_t> g_voiceBuf[PsyX::kNumVoices];
std::vector<int16_t> g_wetBuf;
std::vector<int16_t> g_cdBuf;
std::vector<float>   g_busAccum[kTotalBuses];

void PlaceSource(ALuint src, float azimuthDeg)
{
    const float az = azimuthDeg * 3.14159265358979f / 180.0f;
    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(src, AL_POSITION, sinf(az), 0.0f, -cosf(az));
    alSourcef(src, AL_ROLLOFF_FACTOR, 0.0f); /* placement only, no distance law */
    alSourcef(src, AL_GAIN, 1.0f);
}

/* PSX voice volumes -> where that voice sits across the front arc.
 *
 * The hardware pans by giving a voice independent left and right volumes, so
 * the balance between them IS the azimuth and their magnitude is the level.
 * Everything stays within +-90 degrees because that is the whole of a stereo
 * image: a hard-left voice reaches the left bus and no further. */
void VoiceAzimuthGain(int32_t volL, int32_t volR, float* azDeg, float* gain)
{
    const float l = fabsf((float)volL);
    const float r = fabsf((float)volR);
    const float sum = l + r;

    if (sum <= 0.0f)
    {
        *azDeg = 0.0f;
        *gain  = 0.0f;
        return;
    }

    *azDeg = ((r - l) / sum) * 90.0f;
    *gain  = (l > r ? l : r) / 32767.0f;
    if (*gain > 1.0f)
        *gain = 1.0f;
}

/* Spread one voice across the two buses either side of it, so a voice sweeping
 * across the stage crossfades instead of stepping between speakers. */
void AccumulateVoice(const int16_t* mono, int frames, float azDeg, float gain)
{
    if (gain <= 0.0f)
        return;

    int lo = 0;
    while (lo < kDryBuses - 2 && kDryAzimuthsDeg[lo + 1] < azDeg)
        ++lo;
    const int hi = lo + 1;

    const float span = kDryAzimuthsDeg[hi] - kDryAzimuthsDeg[lo];
    float t = span > 0.0f ? (azDeg - kDryAzimuthsDeg[lo]) / span : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    /* Constant-power crossfade: a linear one dips about 3dB as a voice passes
     * between two buses, which is audible on a slow pan. */
    const float gLo = cosf(t * 1.57079632679f) * gain;
    const float gHi = sinf(t * 1.57079632679f) * gain;

    float* dstLo = g_busAccum[lo].data();
    float* dstHi = g_busAccum[hi].data();
    for (int i = 0; i < frames; ++i)
    {
        const float s = (float)mono[i];
        dstLo[i] += s * gLo;
        dstHi[i] += s * gHi;
    }
}

void AccumulateStereoBed(const int16_t* interleaved, int frames, int busL, int busR)
{
    float* dl = g_busAccum[busL].data();
    float* dr = g_busAccum[busR].data();
    for (int i = 0; i < frames; ++i)
    {
        dl[i] += (float)interleaved[i * 2 + 0];
        dr[i] += (float)interleaved[i * 2 + 1];
    }
}

void RenderBlock(void)
{
    SPUCore::SplitOutput split;
    memset(&split, 0, sizeof(split));

    for (int v = 0; v < PsyX::kNumVoices; ++v)
    {
        g_voiceBuf[v].assign(kBlockFrames, 0);
        split.voiceMono[v] = g_voiceBuf[v].data();
    }
    g_wetBuf.assign((size_t)kBlockFrames * 2, 0);
    g_cdBuf.assign((size_t)kBlockFrames * 2, 0);
    split.wetLR = g_wetBuf.data();
    split.cdLR  = g_cdBuf.data();

    if (g_xaPump)
        g_xaPump(g_xaPumpUser, kBlockFrames);

    SDL_LockMutex(g_coreMutex);
    /* No stereo destination: that downmix is exactly what this replaces. */
    g_core->RenderFrames(NULL, kBlockFrames, &split);
    SDL_UnlockMutex(g_coreMutex);

    for (int b = 0; b < kTotalBuses; ++b)
        memset(g_busAccum[b].data(), 0, sizeof(float) * (size_t)kBlockFrames);

    for (int v = 0; v < PsyX::kNumVoices; ++v)
    {
        float az, gain;
        VoiceAzimuthGain(split.panL[v], split.panR[v], &az, &gain);
        AccumulateVoice(g_voiceBuf[v].data(), kBlockFrames, az, gain);
    }

    AccumulateStereoBed(g_wetBuf.data(), kBlockFrames, kDryBuses, kDryBuses + 1);
    AccumulateStereoBed(g_cdBuf.data(), kBlockFrames, kDryBuses + kWetBuses,
                        kDryBuses + kWetBuses + 1);

    /* Master volume last, matching where the hardware applies it. */
    const float mvRaw = (float)(split.masterL > split.masterR ? split.masterL : split.masterR)
                      / 32767.0f;
    const float master = mvRaw <= 0.0f ? 0.0f : (mvRaw > 1.0f ? 1.0f : mvRaw);

    for (int b = 0; b < kTotalBuses; ++b)
    {
        const float* srcBuf = g_busAccum[b].data();
        int16_t* dst = g_bus[b].scratch.data();
        for (int i = 0; i < kBlockFrames; ++i)
        {
            float s = srcBuf[i] * master;
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            dst[i] = (int16_t)s;
        }
    }
}

int SDLCALL PumpThread(void*)
{
    while (g_running)
    {
        int queuedMin = kQueueDepth;

        for (int b = 0; b < kTotalBuses; ++b)
        {
            ALint processed = 0, queued = 0;
            alGetSourcei(g_bus[b].source, AL_BUFFERS_PROCESSED, &processed);
            while (processed-- > 0)
            {
                ALuint done = 0;
                alSourceUnqueueBuffers(g_bus[b].source, 1, &done);
            }
            alGetSourcei(g_bus[b].source, AL_BUFFERS_QUEUED, &queued);
            if (queued < queuedMin)
                queuedMin = queued;
        }

        if (queuedMin >= kQueueDepth)
        {
            SDL_Delay(2);
            continue;
        }

        RenderBlock();

        /* Every bus is queued the same number of frames on the same pass, so
         * they consume at the same rate and stay locked together for the whole
         * session -- which is the reason for buses instead of 24 sources. */
        for (int b = 0; b < kTotalBuses; ++b)
        {
            ALint queued = 0;
            alGetSourcei(g_bus[b].source, AL_BUFFERS_QUEUED, &queued);
            if (queued >= kQueueDepth)
                continue;

            ALuint buf = g_bus[b].buffers[queued % kQueueDepth];
            alBufferData(buf, AL_FORMAT_MONO16, g_bus[b].scratch.data(),
                         (ALsizei)(kBlockFrames * sizeof(int16_t)), kRate);
            alSourceQueueBuffers(g_bus[b].source, 1, &buf);

            ALint state = 0;
            alGetSourcei(g_bus[b].source, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING)
                alSourcePlay(g_bus[b].source);
        }
    }
    return 0;
}
} // namespace

bool PsyX_SPUSpatial_Start(PsyX::SPUCore* core, SDL_mutex* coreMutex, int speakerMode)
{
    if (g_active || !core)
        return false;

    g_dev = alcOpenDevice(NULL);
    if (!g_dev)
    {
        eprintwarn("[SPATIAL] no OpenAL device; staying on the stereo sink\n");
        return false;
    }

    ALCint attrs[3];
    attrs[0] = ALC_FREQUENCY;
    attrs[1] = kRate;
    attrs[2] = 0;

    g_prevCtx = alcGetCurrentContext();
    g_ctx = alcCreateContext(g_dev, attrs);
    if (!g_ctx || !alcMakeContextCurrent(g_ctx))
    {
        if (g_ctx) alcDestroyContext(g_ctx);
        alcCloseDevice(g_dev);
        g_dev = NULL; g_ctx = NULL;
        eprintwarn("[SPATIAL] could not create an OpenAL context\n");
        return false;
    }

    (void)speakerMode; /* the layout comes from the device configuration */

    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListenerf(AL_GAIN, 1.0f);
    {
        const ALfloat orient[6] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
        alListenerfv(AL_ORIENTATION, orient);
    }

    for (int b = 0; b < kTotalBuses; ++b)
    {
        alGenSources(1, &g_bus[b].source);
        alGenBuffers(kQueueDepth, g_bus[b].buffers);
        g_bus[b].scratch.assign(kBlockFrames, 0);
        g_busAccum[b].assign(kBlockFrames, 0.0f);

        float az;
        if (b < kDryBuses)
            az = kDryAzimuthsDeg[b];
        else if (b < kDryBuses + kWetBuses)
            az = kWetAzimuthsDeg[b - kDryBuses];
        else
            az = kCdAzimuthsDeg[b - kDryBuses - kWetBuses];
        PlaceSource(g_bus[b].source, az);
    }

    g_core = core;
    g_coreMutex = coreMutex;
    g_running = 1;
    g_thread = SDL_CreateThread(PumpThread, "PsyX SPU spatial", NULL);
    if (!g_thread)
    {
        g_running = 0;
        PsyX_SPUSpatial_Stop();
        return false;
    }

    g_active = 1;
    eprintf("[SPATIAL] software SPU through OpenAL: %d dry buses (front arc), %d reverb (rear), %d CD\n",
            kDryBuses, kWetBuses, kCdBuses);
    return true;
}

void PsyX_SPUSpatial_Stop(void)
{
    if (g_running)
    {
        g_running = 0;
        if (g_thread)
        {
            SDL_WaitThread(g_thread, NULL);
            g_thread = NULL;
        }
    }

    for (int b = 0; b < kTotalBuses; ++b)
    {
        if (g_bus[b].source)
        {
            alSourceStop(g_bus[b].source);
            alSourcei(g_bus[b].source, AL_BUFFER, 0);
            alDeleteSources(1, &g_bus[b].source);
            g_bus[b].source = 0;
            alDeleteBuffers(kQueueDepth, g_bus[b].buffers);
            memset(g_bus[b].buffers, 0, sizeof(g_bus[b].buffers));
        }
    }

    if (g_ctx)
    {
        alcMakeContextCurrent(g_prevCtx);
        alcDestroyContext(g_ctx);
        g_ctx = NULL;
    }
    if (g_dev)
    {
        alcCloseDevice(g_dev);
        g_dev = NULL;
    }
    g_core = NULL;
    g_coreMutex = NULL;
    g_active = 0;
}

int PsyX_SPUSpatial_Active(void)
{
    return g_active;
}

void PsyX_SPUSpatial_SetXaPump(void (*pump)(void* user, int frames), void* user)
{
    g_xaPump = pump;
    g_xaPumpUser = user;
}
