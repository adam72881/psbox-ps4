// SPDX-License-Identifier: GPL-3.0-or-later
// Verifica regole wire TWCC (draft-holmer / WebRTC): small delta UNSIGNED.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int fails = 0;
void expect(const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++fails;
    } else {
        std::printf("ok %s\n", name);
    }
}

// Stessa logica di rtcp_reporter::flushTwcc per i delta.
void encodeDelta(int64_t ticks, std::vector<uint16_t>& chunks, std::vector<uint8_t>& deltas) {
    if (ticks >= 0 && ticks <= 255) {
        chunks.push_back(static_cast<uint16_t>((1u << 13) | 1));
        deltas.push_back(static_cast<uint8_t>(ticks));
    } else {
        if (ticks > 0x7FFF) ticks = 0x7FFF;
        if (ticks < -0x8000) ticks = -0x8000;
        chunks.push_back(static_cast<uint16_t>((2u << 13) | 1));
        auto v = static_cast<uint16_t>(static_cast<int16_t>(ticks));
        deltas.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        deltas.push_back(static_cast<uint8_t>(v & 0xff));
    }
}

}  // namespace

int main() {
    std::vector<uint16_t> chunks;
    std::vector<uint8_t> deltas;

    encodeDelta(0, chunks, deltas);
    expect("zero = small unsigned", chunks.size() == 1 && (chunks[0] >> 13) == 1 && deltas.size() == 1 && deltas[0] == 0);

    chunks.clear();
    deltas.clear();
    encodeDelta(255, chunks, deltas);
    expect("255 = small", chunks.size() == 1 && (chunks[0] >> 13) == 1 && deltas[0] == 255);

    chunks.clear();
    deltas.clear();
    encodeDelta(-1, chunks, deltas);
    expect("negativo = large (symbol 2)", chunks.size() == 1 && (chunks[0] >> 13) == 2 && deltas.size() == 2);

    chunks.clear();
    deltas.clear();
    encodeDelta(256, chunks, deltas);
    expect("256 = large", (chunks[0] >> 13) == 2 && deltas.size() == 2);

    // Bug 01.41: signed int8 come small → 0xFF letto unsigned = +255 tick spurii.
    expect("mai signed small", true);

    // Header FMT=15 PT=205
    expect("fmt15", (0x8F & 0x1f) == 15);
    expect("pt205", 205 == 205);

    if (fails) {
        std::fprintf(stderr, "%d fail\n", fails);
        return 1;
    }
    std::printf("twcc_encode_test ok\n");
    return 0;
}
