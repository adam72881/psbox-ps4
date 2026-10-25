// SPDX-License-Identifier: GPL-3.0-or-later
// Segnaposto del decoder software (FFmpeg) sul target PS4.
// decoder.cpp e audio_encoder.cpp restano nel sorgente, ma non sono nel link
// finché non c'è una build FFmpeg per OpenOrbis. Lo split degli access unit è
// la stessa logica di decoder.cpp (serve ad autoplay e al client).
#include "media/audio_encoder.h"
#include "media/decoder.h"
#include "util/log.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace xc::media {

std::vector<std::pair<size_t, size_t>> splitAccessUnits(const uint8_t* data, size_t size) {
    std::vector<std::pair<size_t, size_t>> aus;
    size_t start = SIZE_MAX;
    int prevType = -1;
    for (size_t i = 0; i + 4 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            int type = data[i + 3] & 0x1f;
            size_t sc = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
            bool boundary = type == 9 || (type == 7 && prevType != 9) ||
                            ((type == 1 || type == 5) && prevType != 7 && prevType != 8 && prevType != 9 &&
                             prevType != 6 && (data[i + 4] & 0x80));
            if (boundary) {
                if (start != SIZE_MAX) aus.emplace_back(start, sc - start);
                start = sc;
            }
            prevType = type;
            i += 3;
        }
    }
    if (start != SIZE_MAX) aus.emplace_back(start, size - start);
    return aus;
}

void disableSimd() { XC_LOGW("FFmpeg non è nel binario PS4: SIMD software non disponibile"); }

VideoDecoder::~VideoDecoder() = default;
bool VideoDecoder::init(int) {
    XC_LOGW("decoder H.264 software assente (manca FFmpeg PS4)");
    return false;
}
bool VideoDecoder::decode(const uint8_t*, size_t, Picture&, uint32_t) { return false; }

AudioDecoder::~AudioDecoder() = default;
bool AudioDecoder::init() {
    XC_LOGW("decoder Opus software assente (manca FFmpeg PS4)");
    return false;
}
bool AudioDecoder::decode(const uint8_t*, size_t, std::vector<float>&) { return false; }

AudioEncoder::AudioEncoder() = default;
AudioEncoder::~AudioEncoder() = default;
bool AudioEncoder::init(int) {
    XC_LOGW("encoder Opus assente (microfono non attivo su PS4)");
    return false;
}
bool AudioEncoder::encode(const int16_t*, size_t, std::vector<uint8_t>&) { return false; }

}  // namespace xc::media
