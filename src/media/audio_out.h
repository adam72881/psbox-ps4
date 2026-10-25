// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
// Audio playback: 48 kHz interleaved float stereo pushed from any thread,
// played by a dedicated thread in 256-frame grains (silence on underrun).
#pragma once

#include <cstddef>
#include <cstdint>

namespace xc::media {

enum class AudioRoute {
    Main = 0,  // TV / HDMI / Default output
    Pad = 1    // DualSense Controller Speaker / Headset
};

bool audioStart();
void audioStop();
// Chiusura di sistema: chiude la porta senza join (evita hang → CE-34878-0).
void audioAbortForExit();
// Menu PS: chiude la porta senza restare nel kill. Al ritorno si riapre se era aperta.
void audioSuspendForHome();
void audioResumeFromHome();
void audioSetRoute(AudioRoute route);
AudioRoute audioRoute();
// The game's volume: 1 as it comes, up to 2 (louder, clipped at full scale).
void audioSetVolume(float gain);
// Past ~60 ms queued (a burst from the network) cuts back to ~40 ms, the
// oldest samples first, to keep the sound close to the picture.
void audioPush(const float* interleaved, size_t frames);
uint64_t audioUnderruns();
uint64_t audioTrimmedFrames();

}  // namespace xc::media
