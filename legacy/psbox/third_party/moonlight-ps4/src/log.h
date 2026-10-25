// Ponte di log per audio_orbis.c (che include "../log.h"). Non è il logger di Moonlight.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void xc_audio_log(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#define LOGI(fmt, ...) xc_audio_log("I", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) xc_audio_log("W", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) xc_audio_log("E", fmt, ##__VA_ARGS__)
