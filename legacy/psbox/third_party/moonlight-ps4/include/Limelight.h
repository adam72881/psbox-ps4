// Tipi minimi di Limelight.h (moonlight-common-c, GPL-3.0) usati da audio_orbis.c.
// Non è il header completo: serve solo a compilare il renderer audio così com'è.
#pragma once

#define CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION 0x20

typedef struct _OPUS_MULTISTREAM_CONFIGURATION {
    int sampleRate;
    int channelCount;
    int streams;
    int coupledStreams;
    int samplesPerFrame;
    unsigned char mapping[6];
} OPUS_MULTISTREAM_CONFIGURATION, *POPUS_MULTISTREAM_CONFIGURATION;

typedef struct _AUDIO_RENDERER_CALLBACKS {
    int (*init)(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags);
    void (*cleanup)(void);
    void (*decodeAndPlaySample)(char* sampleData, int sampleLength);
    int capabilities;
} AUDIO_RENDERER_CALLBACKS;
