// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "stream/rtcp_reporter.h"

#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace xc::stream {

namespace {

constexpr uint64_t kReportIntervalUs = 500000;  // 500 ms come PS5 REMB
constexpr uint64_t kTwccIntervalUs = 100000;    // 100 ms
constexpr uint64_t kPliQuietUs = 2000000;       // PLI se video fermo ≥2 s
constexpr uint32_t kMinBitrate = 1500000;

uint64_t nowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}

bool readTwccSeq(const uint8_t* d, size_t n, uint16_t& out) {
    if (n < 12 || !(d[0] & 0x10)) return false;
    size_t off = 12 + 4u * (d[0] & 0x0f);
    if (off + 4 > n) return false;
    uint16_t profile = get16(d + off);
    size_t words = get16(d + off + 2);
    size_t end = off + 4 + words * 4;
    if (end > n) return false;
    size_t i = off + 4;
    if (profile == 0xBEDE) {
        while (i < end) {
            if (d[i] == 0) {
                ++i;
                continue;
            }
            uint8_t id = d[i] >> 4;
            uint8_t len = (d[i] & 0x0f) + 1;
            ++i;
            if (i + len > end) break;
            // Solo id 3: quello negoziato nell'offerta. Id 2/4 potevano
            // matchare altre estensioni → seq spurie → "loss" TWCC → sto