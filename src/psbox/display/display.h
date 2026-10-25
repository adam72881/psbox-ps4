// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

namespace xc::display {

constexpr unsigned kWidth = 1920;
constexpr unsigned kHeight = 1080;

// The GPU (display/gpu.h: FSR upscaling to the 4K display) when `preferGpu`
// and it comes up, else the CPU path below.
bool init(bool preferGpu = true);
void shutdown();
// Chiusura dell'app: chiude VideoOut senza join sui thread di blit.
void releaseForExit();
// Menu PS (bg=1): niente SubmitFlip. Chiude la porta solo dal thread main,
// così «Chiudi applicazione» non resta su un flip. Al ritorno si riapre.
void holdFlips(bool on);
void suspendForHome();
void resumeFromHome();
// Dopo il rest mode: chiude e riapre VideoOut sugli stessi buffer, senza
// un'altra allocazione di memoria diretta.
void resumeAfterRest();
// Held by whoever draws a frame (UI thread or video thread).
// timed_mutex: il main non deve mai bloccarsi in attesa del video
// (ICE/STUN e heartbeat restano vivi anche se un flip è lento).
std::timed_mutex& frameMutex();
// False se il flip non è entrato in coda (menu, coda piena, porta chiusa).
// La porta resta aperta: il chiamante non manda l'ack di un frame non visto.
bool present();
// Vero se lo stream ha preso la porta VideoOut (YCbCr o BGRA): la UI
// non deve riaprire RGBA sopra.
bool streamOwnsDisplay();

// Copies a linear 1920x1080 RGBA (0xAABBGGRR) image to the back buffer
// (multi-threaded). Call present() afterwards.
void drawRgba(const uint32_t* pixels);

// Converts a BT.709 limited-range YUV 4:2:0 picture to the back buffer,
// scaled to the full screen (multi-threaded). Call present() afterwards.
// With two flips still queued it waits for a vblank, or, without `wait`,
// returns false and draws nothing.
bool drawYuv420(const uint8_t* y, const uint8_t* u, const uint8_t* v, int strideY, int strideU, int strideV,
                int width, int height, bool wait);

// NV12 sullo scanout YCbCr420 (Moonlight-ps4), oppure BGRA 1:1 se il
// firmware non accetta quel formato. `wait` falso: se il buffer è occupato
// non si disegna.
bool drawNv12(const uint8_t* nv12, int pitch, int width, int height, bool wait);
// Vero se lo scanout è NV12 (copia, senza conversione). Il fallback BGRA no.
bool fastScanout();

// A picture laid over every video frame drawYuv420() draws from now on (the
// in-game menu, the statistics): `w` x `h` RGBA pixels at (x, y), blended at
// `opacity`. Null `pixels` removes it. Any thread.
void setOverlay(const uint32_t* pixels, int x, int y, int w, int h, uint8_t opacity);

// Sharpening of the video (drawYuv420), 0 = off .. 256 = full CAS.
void setSharpness(int amount);
// Smoothing of compression blocks in flat areas (GPU only): 0 off, 1 low, 2 high.
void setDeband(int level);
// GPU only: 0 FSR 1, 1 Anime4K (gpu::setUpscaler).
void setUpscaler(int mode);
// Anime4K Restore before the upscale (GPU only).
void setRestore(bool on);

// GPU only: the id of the last present(), and the last frame known to be on
// the screen (its id, and platform::nowUs() then). False on the CPU path.
uint64_t lastPresentId();
bool lastShown(uint64_t& id, uint64_t& atUs);

// The back buffer last drawn, untiled, as 8-bit RGB rows (diagnostics:
// what the TV shows), and its size. False without a display.
bool readBackRgb(std::vector<uint8_t>& rgb, int& width, int& height);

} // namespace xc::display

#ifdef __cplusplus
extern "C" {
#endif
/* XCloud4 host: point drawRgba at the current VideoOut RGBA back-buffer. */
void psbox_display_set_target(uint32_t* pixels);
#ifdef __cplusplus
}
#endif
