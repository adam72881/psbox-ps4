#include "audio_orbis.h"
#include "../log.h"

#include <opus_multistream.h>

#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// sceAudioOut works with fixed sample grains; 256 is the minimum
// (5.33 ms at 48 kHz), closest to Moonlight's Opus frame (240, 5 ms).
#define AUDIO_GRAIN 256
#define MAX_CHANNELS 2
// Cap on queued PCM before drop: ~32 ms. If output stalls
// we prefer dropping audio over accumulating latency.
#define RING_SAMPLES (AUDIO_GRAIN * 6)

// OpenOrbis: 0x8026000E = ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT
#define AUDIO_OUT_ALREADY_INIT 0x8026000Eu

static OpusMSDecoder *s_decoder;
static int s_channels;
static int s_samples_per_frame;
static int32_t s_handle = -1;

// Interleaved PCM ring buffer.
static short s_ring[RING_SAMPLES * MAX_CHANNELS];
static int s_ring_head; // read sample index
static int s_ring_tail; // write sample index
static int s_ring_count;

static short s_outbuf[AUDIO_GRAIN * MAX_CHANNELS];
/* PSBox PS4: xCloud manda frame da 20 ms (960 campioni). Moonlight resta a 512. */
static short s_decbuf[960 * MAX_CHANNELS];

static int32_t ar_open_port(int32_t user, int type, int index, uint32_t rate, uint32_t fmt) {
    int32_t h = sceAudioOutOpen(user, type, index, AUDIO_GRAIN, rate, fmt);
    LOGI("audio: open user=%d type=%d idx=%d => 0x%08x", (int)user, type, index, (unsigned)h);
    if (h > 0) {
        int32_t vol[2] = {32768, 32768};
        int32_t vr = sceAudioOutSetVolume(h, 2, vol);
        LOGI("audio: volume => 0x%08x", (unsigned)vr);
    }
    return h;
}

static int ar_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                   void *context, int arFlags) {
    (void)audioConfiguration; (void)context; (void)arFlags;
    int rc;
    uint32_t rate;

    s_channels = opusConfig->channelCount;
    if (s_channels > MAX_CHANNELS) {
        LOGE("audio: %d channels unsupported (max %d)", s_channels, MAX_CHANNELS);
        return -1;
    }
    s_samples_per_frame = opusConfig->samplesPerFrame;

    // Orbis only accepts 48000 in practice for MAIN; Moonlight Opus is 48 kHz.
    rate = opusConfig->sampleRate;
    if (rate != 48000u) {
        LOGW("audio: sampleRate=%u unsupported, forcing 48000", rate);
        rate = 48000u;
    }

    int err;
    s_decoder = opus_multistream_decoder_create(
        opusConfig->sampleRate, opusConfig->channelCount, opusConfig->streams,
        opusConfig->coupledStreams, opusConfig->mapping, &err);
    if (!s_decoder) {
        LOGE("audio: opus_multistream_decoder_create failed: %d", err);
        return -1;
    }

    // Same as Net: the link stub is not enough; must load the internal PRX.
    rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
    LOGI("audio: LoadModuleInternal(AUDIOOUT) => 0x%08x", (unsigned)rc);

    rc = sceAudioOutInit();
    if (rc != 0 && (uint32_t)rc != AUDIO_OUT_ALREADY_INIT) {
        LOGE("audio: sceAudioOutInit failed: 0x%08x", (unsigned)rc);
        return -1;
    }
    LOGI("audio: sceAudioOutInit => 0x%08x", (unsigned)rc);

    /* 0x809b0001: MAIN rifiuta l'utente loggato se non è il launching user.
     * SYSTEM (0xFF) è il percorso che l'homebrew può aprire. */
    sceUserServiceInitialize(NULL);
    int32_t initial = -1;
    sceUserServiceGetInitialUser(&initial);

    uint32_t fmt = (s_channels == 1) ? ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_MONO
                                     : ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO;

    s_handle = ar_open_port(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, rate, fmt);
    for (int idx = 1; s_handle <= 0 && idx <= 3; idx++)
        s_handle = ar_open_port(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, idx, rate, fmt);
    if (s_handle <= 0 && initial > 0)
        s_handle = ar_open_port(initial, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, rate, fmt);
    if (s_handle <= 0)
        s_handle = ar_open_port(ORBIS_USER_SERVICE_USER_ID_SYSTEM, 1 /* BGM */, 0, rate, fmt);
    if (s_handle <= 0) {
        LOGE("audio: sceAudioOutOpen failed: 0x%08x", (unsigned)s_handle);
        return -1;
    }

    s_ring_head = s_ring_tail = s_ring_count = 0;
    LOGI("audio: handle=%d %u Hz, %d channels, grain %d",
         (int)s_handle, rate, s_channels, AUDIO_GRAIN);
    return 0;
}

static void ar_cleanup(void) {
    if (s_handle >= 0) {
        sceAudioOutOutput(s_handle, NULL); // wait for last block
        sceAudioOutClose(s_handle);
        s_handle = -1;
    }
    if (s_decoder) {
        opus_multistream_decoder_destroy(s_decoder);
        s_decoder = NULL;
    }
}

static void ring_push(const short *pcm, int samples) {
    if (s_ring_count + samples > RING_SAMPLES) {
        // Overflow: drop oldest samples to avoid accumulating latency.
        int drop = s_ring_count + samples - RING_SAMPLES;
        s_ring_head = (s_ring_head + drop) % RING_SAMPLES;
        s_ring_count -= drop;
    }
    for (int i = 0; i < samples; i++) {
        memcpy(&s_ring[s_ring_tail * s_channels], &pcm[i * s_channels],
               (size_t)s_channels * sizeof(short));
        s_ring_tail = (s_ring_tail + 1) % RING_SAMPLES;
    }
    s_ring_count += samples;
}

static void ring_pop(short *out, int samples) {
    for (int i = 0; i < samples; i++) {
        memcpy(&out[i * s_channels], &s_ring[s_ring_head * s_channels],
               (size_t)s_channels * sizeof(short));
        s_ring_head = (s_ring_head + 1) % RING_SAMPLES;
    }
    s_ring_count -= samples;
}

// Called from the moonlight-common-c audio thread (without DIRECT_SUBMIT).
// sceAudioOutOutput blocks until the previous block finishes, which
// sets the playback pace. sampleData==NULL => libopus PLC.
static void ar_decode_and_play(char *sampleData, int sampleLength) {
    int decoded = opus_multistream_decode(
        s_decoder,
        sampleData ? (unsigned char *)sampleData : NULL,
        sampleData ? sampleLength : 0,
        s_decbuf, s_samples_per_frame, 0);
    if (decoded <= 0) {
        if (sampleData)
            LOGW("audio: opus decode error: %d", decoded);
        return;
    }

    ring_push(s_decbuf, decoded);

    while (s_ring_count >= AUDIO_GRAIN) {
        ring_pop(s_outbuf, AUDIO_GRAIN);
        int rc = sceAudioOutOutput(s_handle, s_outbuf);
        if (rc < 0) {
            LOGW("audio: sceAudioOutOutput failed: 0x%08x", rc);
            break;
        }
    }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_orbis = {
    .init = ar_init,
    .cleanup = ar_cleanup,
    .decodeAndPlaySample = ar_decode_and_play,
    .capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
