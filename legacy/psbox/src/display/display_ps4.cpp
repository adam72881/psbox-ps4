// SPDX-License-Identifier: GPL-3.0-or-later
// Uscita video PS4. La UI registra RGBA sulla prima porta: Unregister non
// libera lo slot (Moonlight-ps4, renderer_videoout.c: 0x80290010
// SLOT_OCCUPIED). Lo stream chiude, riapre e registra YCbCr su una porta
// nuova, con buffer WC_GARLIC allineati a 2 MiB. Se il firmware rifiuta,
// stessa cosa con BGRA 1280x720 e una conversione SSE2 (stessi coefficienti
// di Moonlight-ps4, nv12_to_bgra). Lo stub OpenOrbis di sceGnmFlushGarlic è
// un jmp infinito: il simbolo si prende dallo SPRX, come AddBufferYccPrivilege.
#include "display/display.h"
#include "display/flip_sched.h"
#include "display/gpu.h"
#include "platform/platform.h"
#include "util/log.h"

#include <algorithm>
#include <atomic>

namespace xc::app {
extern std::atomic<bool> g_appStopping;
extern std::atomic<bool> g_psMenu;
}
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <emmintrin.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

namespace xc::display {

std::atomic<bool> g_holdFlips{false};
bool g_homeSuspended = false;

namespace {
// 4 buffer come AJ-GFN: con 2, pending+shown lasciava un solo slot libero e
// si riscriveva il buffer ancora in flip (tear / present fallito / late).
constexpr int kBuffers = 4;
constexpr size_t kAlign = 0x200000;
constexpr int kYccBuffers = 4;
constexpr uint32_t kYccFormat = 0x08322200u;       // YCbCr420 BT.709, Moonlight
constexpr uint32_t kBgraFormat = 0x80000000u;      // B8G8R8A8, Moonlight
constexpr unsigned kViewW = 1280;
constexpr unsigned kViewH = 720;

enum class Scan { Ui, Ycc, Bgra };

int g_video = -1;
void* g_base = nullptr;
off_t g_phys = 0;
size_t g_bytes = 0;
uint8_t* g_fb[kBuffers] = {};
int g_index = 0;
int g_bufCount = kBuffers;
uint64_t g_presentId = 0;
int64_t g_shownArg = -1;
// Indice del buffer passato a SubmitFlip (frame compositato sullo scanout).
int g_lastFlipped = -1;
Scan g_scan = Scan::Ui;
bool g_bound = false;
// Su 11.52 RegisterBuffers YCbCr420 torna 0x80290003 (niente kpayload).
// Si va subito a BGRA720: una sola reopen della porta, niente doppio open.
bool g_yccFailed = true;
int g_flipMode = 1;  // VSYNC; si prova anche HSYNC/0 al primo present
bool g_flipProbed = false;
// Ultimi indici dati a SubmitFlip, il più recente in [0]. La coda ne tiene 2:
// currentBuffer == -1 non basta a sapere quali sono ancora in volo.
int g_submitHist[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
int g_submitCount = 0;

void noteSubmit(int idx) {
    for (int i = 7; i > 0; --i) g_submitHist[i] = g_submitHist[i - 1];
    g_submitHist[0] = idx;
    if (g_submitCount < 8) ++g_submitCount;
}

void clearSubmitHist() {
    g_submitCount = 0;
    for (int i = 0; i < 8; ++i) g_submitHist[i] = -1;
}

struct Dmem {
    void* base = nullptr;
    off_t phys = 0;
    size_t bytes = 0;
    size_t stride = 0;
};
Dmem g_yccMem;
Dmem g_bgraMem;
uint8_t* g_yccFb[kYccBuffers] = {};
uint8_t* g_bgraFb[kBuffers] = {};

struct Overlay {
    std::vector<uint32_t> px;
    int x = 0, y = 0, w = 0, h = 0;
    uint8_t opacity = 0;
    bool on = false;
};
Overlay g_overlay;
int g_sharpness = 0;

using FlushFn = void (*)();
FlushFn g_flush = nullptr;
bool g_flushTried = false;

struct BlitWork {
    const uint8_t* y = nullptr;
    const uint8_t* uv = nullptr;
    uint8_t* dst = nullptr;
    const int* xLut = nullptr;
    int pitchY = 0, pitchUv = 0, dstPitch = 0;
    int srcW = 0, srcH = 0, dstW = 0, dstH = 0;
    int row0 = 0, row1 = 0;
    bool sse = false;
};
std::mutex g_blitMu;
std::condition_variable g_blitCv;
BlitWork g_blitJob;
int g_blitGen = 0;
int g_blitSeen = 0;
bool g_blitStop = false;
bool g_blitReady = false;
std::thread g_blitThread;
std::vector<int> g_xLut;
int g_lutSrcW = -1;

bool ensureBlitThread();
void resolveFlush();

size_t frameBytes() { return static_cast<size_t>(kWidth) * kHeight * 4u; }

void blendOverlay(uint32_t* dst) {
    if (!g_overlay.on || g_overlay.px.empty() || g_overlay.opacity == 0) return;
    const int a = g_overlay.opacity;
    for (int y = 0; y < g_overlay.h; ++y) {
        int dy = g_overlay.y + y;
        if (dy < 0 || dy >= static_cast<int>(kHeight)) continue;
        for (int x = 0; x < g_overlay.w; ++x) {
            int dx = g_overlay.x + x;
            if (dx < 0 || dx >= static_cast<int>(kWidth)) continue;
            uint32_t s = g_overlay.px[static_cast<size_t>(y) * g_overlay.w + x];
            uint32_t sa = ((s >> 24) & 0xff) * a / 255;
            if (!sa) continue;
            uint32_t* d = dst + static_cast<size_t>(dy) * kWidth + dx;
            uint32_t b = *d;
            auto ch = [&](int shift) {
                int sv = (s >> shift) & 0xff;
                int dv = (b >> shift) & 0xff;
                return (sv * sa + dv * (255 - sa)) / 255;
            };
            *d = (0xffu << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        }
    }
}

bool bufferFree(int index) {
    OrbisVideoOutFlipStatus st{};
    if (sceVideoOutGetFlipStatus(g_video, &st) != 0) return true;
    g_shownArg = st.flipArg;
    // Libero se non è quello a schermo, oppure non c'è flip in coda.
    return st.currentBuffer != index || st.numFlipPending == 0;
}

// Sceglie un indice diverso da currentBuffer (e dal precedente submit se
// pending). Aggiorna g_index. False se nessun buffer è libero.
bool pickFree(bool block) {
    if (g_video < 0 || g_bufCount <= 0) return false;
    int spins = block ? 8 : 1;
    for (int spin = 0; spin < spins; ++spin) {
        OrbisVideoOutFlipStatus st{};
        if (sceVideoOutGetFlipStatus(g_video, &st) != 0) return true;
        g_shownArg = st.flipArg;
        int idx = flipPick(g_bufCount, g_index, st.currentBuffer, st.numFlipPending, g_submitHist, g_submitCount);
        if (idx >= 0) {
            g_index = idx;
            return true;
        }
        if (!block) return false;
        sceKernelUsleep(2000);
    }
    return false;
}

bool waitFree(int index, bool block) {
    // Legacy: un indice fisso. Preferire pickFree() sul path stream.
    int spins = block ? 8 : 1;
    for (int i = 0; i < spins; ++i) {
        if (bufferFree(index)) return true;
        if (!block) return false;
        sceKernelUsleep(2000);
    }
    return false;
}

void drainFlips() {
    if (g_video < 0) return;
    // Fino ~80 ms: liberare prima di Unregister/Close evita free sotto flip
    // (AJ-GFN: WaitForFlipsSettled prima di reinit).
    for (int i = 0; i < 40; ++i) {
        OrbisVideoOutFlipStatus st{};
        if (sceVideoOutGetFlipStatus(g_video, &st) != 0) return;
        if (st.numFlipPending <= 0) return;
        sceKernelUsleep(2000);
    }
}

bool reopenVideo() {
    drainFlips();
    if (g_video >= 0) {
        OrbisVideoOutFlipStatus st{};
        sceVideoOutGetFlipStatus(g_video, &st);
        XC_LOGI("video: chiudo la porta, flip in coda %d", st.numFlipPending);
        sceVideoOutUnregisterBuffers(g_video, 0);
        sceVideoOutClose(g_video);
        g_video = -1;
    }
    g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (g_video < 0) {
        XC_LOGE("video: sceVideoOutOpen 0x%08x", static_cast<unsigned>(g_video));
        return false;
    }
    sceVideoOutSetFlipRate(g_video, ORBIS_VIDEO_OUT_FLIP_60HZ);
    g_bound = false;
    clearSubmitHist();
    XC_LOGI("video: porta nuova handle %d", g_video);
    return true;
}

bool allocDmem(Dmem& d, size_t payload, int n, const char* tag) {
    if (d.base) return true;
    size_t stride = (payload + kAlign - 1) & ~(kAlign - 1);
    size_t bytes = stride * static_cast<size_t>(n);
    int rc = sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), bytes, kAlign,
                                           ORBIS_KERNEL_WC_GARLIC, &d.phys);
    XC_LOGI("video: %s memoria rc 0x%08x phys 0x%llx size %zu align %zu stride %zu n %d type %d", tag,
            static_cast<unsigned>(rc), static_cast<unsigned long long>(d.phys), bytes, kAlign, stride, n,
            ORBIS_KERNEL_WC_GARLIC);
    if (rc < 0) return false;
    rc = sceKernelMapDirectMemory(&d.base, bytes, 0x33, 0, d.phys, kAlign);
    XC_LOGI("video: %s map rc 0x%08x va %p", tag, static_cast<unsigned>(rc), d.base);
    if (rc < 0 || !d.base) return false;
    std::memset(d.base, 0, bytes);
    d.bytes = bytes;
    d.stride = stride;
    return true;
}

int registerScan(const char* tag, uint32_t format, uint32_t tile, uint32_t w, uint32_t h, uint32_t pitch,
                 void* const* ptrs, int n, const Dmem& d) {
    if (g_video < 0) {
        XC_LOGW("video: register %s salto, porta chiusa", tag);
        return -1;
    }
    OrbisVideoOutBufferAttribute attr{};
    sceVideoOutSetBufferAttribute(&attr, format, tile, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, w, h, pitch);
    XC_LOGI("video: %s format 0x%08x tile %u %ux%u pitch %u n %d size %zu align %zu", tag, format, tile, w, h, pitch, n,
            d.bytes, kAlign);
    for (int i = 0; i < n; ++i) {
        unsigned long long phys = static_cast<unsigned long long>(d.phys + static_cast<off_t>(i) * static_cast<off_t>(d.stride));
        XC_LOGI("video: %s fb[%d] va %p phys 0x%llx", tag, i, ptrs[i], phys);
    }
    int rc = sceVideoOutRegisterBuffers(g_video, 0, ptrs, n, &attr);
    if (rc == 0) XC_LOGI("video: register %s ok", tag);
    else XC_LOGE("video: register %s 0x%08x", tag, static_cast<unsigned>(rc));
    return rc;
}

// Lo stub OpenOrbis di AddBufferYccPrivilege è un jmp infinito.
bool grantYcc() {
    using YccFn = int32_t (*)(int32_t);
    const char* paths[] = {"/system/common/lib/libSceVideoOut.sprx", "libSceVideoOut.sprx"};
    int mod = -1;
    for (const char* path : paths) {
        int32_t rc = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
        XC_LOGI("video: LoadStartModule(%s) 0x%08x", path, static_cast<unsigned>(rc));
        if (rc > 0) {
            mod = rc;
            break;
        }
    }
    if (mod < 0) return false;
    void* sym = nullptr;
    if (sceKernelDlsym(mod, "sceVideoOutAddBufferYccPrivilege", &sym) != 0 || !sym) {
        XC_LOGW("video: AddBufferYccPrivilege assente");
        return false;
    }
    int ycc = reinterpret_cast<YccFn>(sym)(g_video);
    XC_LOGI("video: AddBufferYccPrivilege 0x%08x", static_cast<unsigned>(ycc));
    void* sys = nullptr;
    if (sceKernelDlsym(mod, "sceVideoOutSysUpdatePrivilege", &sys) == 0 && sys)
        XC_LOGI("video: SysUpdatePrivilege 0x%08x", static_cast<unsigned>(reinterpret_cast<YccFn>(sys)(g_video)));
    return ycc == 0;
}

bool switchToUi() {
    if (g_bound && g_scan == Scan::Ui && g_video >= 0 && g_fb[0]) return true;
    // Dopo releaseForExit/shutdown non si riapre: evita register a caso in chiusura.
    if (g_video < 0 && !g_fb[0]) return false;
    if (!g_fb[0] || !reopenVideo()) return false;
    Dmem view;
    view.base = g_base;
    view.phys = g_phys;
    view.bytes = g_bytes;
    view.stride = frameBytes();
    int rc = registerScan("RGBA", ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, kWidth,
                          kHeight, kWidth, reinterpret_cast<void* const*>(g_fb), kBuffers, view);
    if (rc < 0) return false;
    g_scan = Scan::Ui;
    g_bound = true;
    g_bufCount = kBuffers;
    g_index = 0;
    return true;
}

bool switchToYcc() {
    if (g_bound && g_scan == Scan::Ycc && g_video >= 0) return true;
    if (!reopenVideo()) return false;
    grantYcc();
    constexpr uint32_t pitch = kWidth;
    size_t payload = static_cast<size_t>(pitch) * kHeight * 3 / 2;
    if (!allocDmem(g_yccMem, payload, kYccBuffers, "NV12")) return false;
    for (int i = 0; i < kYccBuffers; ++i)
        g_yccFb[i] = static_cast<uint8_t*>(g_yccMem.base) + static_cast<size_t>(i) * g_yccMem.stride;
    size_t uvOff = static_cast<size_t>(pitch) * kHeight;
    XC_LOGI("video: NV12 LINEAR Y +0 CbCr +%zu (subito dopo la riga %u, pitch multiplo di 64)", uvOff, kHeight);
    int rc = registerScan("YCbCr420", kYccFormat, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, kWidth, kHeight, pitch,
                          reinterpret_cast<void* const*>(g_yccFb), kYccBuffers, g_yccMem);
    if (rc < 0) return false;
    g_scan = Scan::Ycc;
    g_bound = true;
    g_bufCount = kYccBuffers;
    g_index = 0;
    return true;
}

bool switchToBgra() {
    if (g_bound && g_scan == Scan::Bgra && g_video >= 0) return true;
    if (!reopenVideo()) return false;
    size_t payload = static_cast<size_t>(kViewW) * kViewH * 4u;
    if (!allocDmem(g_bgraMem, payload, kBuffers, "BGRA720")) return false;
    for (int i = 0; i < kBuffers; ++i)
        g_bgraFb[i] = static_cast<uint8_t*>(g_bgraMem.base) + static_cast<size_t>(i) * g_bgraMem.stride;
    int rc = registerScan("BGRA720", kBgraFormat, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, kViewW, kViewH, kViewW,
                          reinterpret_cast<void* const*>(g_bgraFb), kBuffers, g_bgraMem);
    if (rc < 0) {
        switchToUi();
        return false;
    }
    g_scan = Scan::Bgra;
    g_bound = true;
    g_bufCount = kBuffers;
    g_index = 0;
    g_flipProbed = false;
    // Flush/thread già risolti in init(): niente LoadStartModule sotto
    // frameMutex al primo blit (poteva congelare il processo).
    ensureBlitThread();
    XC_LOGI("video: scanout BGRA 1280x720 (formato unico, niente YCbCr)");
    return true;
}

void blendOverlayNv12(uint8_t* dst, int pitch, int height) {
    if (!g_overlay.on || g_overlay.px.empty() || g_overlay.opacity == 0) return;
    uint8_t* uv = dst + static_cast<size_t>(pitch) * static_cast<size_t>(height);
    const int a = g_overlay.opacity;
    for (int y = 0; y < g_overlay.h; ++y) {
        int dy = g_overlay.y + y;
        if (dy < 0 || dy >= static_cast<int>(kHeight)) continue;
        for (int x = 0; x < g_overlay.w; ++x) {
            int dx = g_overlay.x + x;
            if (dx < 0 || dx >= static_cast<int>(kWidth)) continue;
            uint32_t s = g_overlay.px[static_cast<size_t>(y) * g_overlay.w + x];
            int sa = static_cast<int>(((s >> 24) & 0xff) * a / 255);
            if (sa < 16) continue;
            int r = s & 0xff, g = (s >> 8) & 0xff, b = (s >> 16) & 0xff;
            int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            int uu = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int vv = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            uint8_t* yp = dst + static_cast<size_t>(dy) * pitch + dx;
            int old = *yp;
            *yp = static_cast<uint8_t>((yy * sa + old * (255 - sa)) / 255);
            if ((dx & 1) == 0 && (dy & 1) == 0) {
                uint8_t* up = uv + static_cast<size_t>(dy / 2) * pitch + dx;
                up[0] = static_cast<uint8_t>(std::clamp(uu, 0, 255));
                up[1] = static_cast<uint8_t>(std::clamp(vv, 0, 255));
            }
        }
    }
}

inline uint8_t sat8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// B, G, R, A. Coefficienti >>7 di Moonlight-ps4 (BT.601, niente wrap su epi16).
inline void bgraPx(uint8_t* dp, int yy, int uu, int vv) {
    dp[0] = sat8(yy + ((227 * uu) >> 7));
    dp[1] = sat8(yy - ((44 * uu + 92 * vv) >> 7));
    dp[2] = sat8(yy + ((179 * vv) >> 7));
    dp[3] = 0xFF;
}

inline void bgraStore8(uint8_t* dp, __m128i y16, __m128i rv, __m128i guv, __m128i bu, __m128i a255) {
    __m128i r = _mm_add_epi16(y16, rv);
    __m128i g = _mm_sub_epi16(y16, guv);
    __m128i b = _mm_add_epi16(y16, bu);
    __m128i br = _mm_packus_epi16(b, r);
    __m128i ga = _mm_packus_epi16(g, a255);
    __m128i bg0 = _mm_unpacklo_epi8(br, ga);
    __m128i ra0 = _mm_unpackhi_epi8(br, ga);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dp), _mm_unpacklo_epi16(bg0, ra0));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dp + 16), _mm_unpackhi_epi16(bg0, ra0));
}

void sseRows(const BlitWork& j) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i m00ff = _mm_set1_epi16(0x00FF);
    const __m128i c128 = _mm_set1_epi16(128);
    const __m128i c179 = _mm_set1_epi16(179);
    const __m128i c44 = _mm_set1_epi16(44);
    const __m128i c92 = _mm_set1_epi16(92);
    const __m128i c227 = _mm_set1_epi16(227);
    const __m128i a255 = _mm_set1_epi16(255);
    int row = j.row0;
    for (; row + 1 < j.row1; row += 2) {
        uint8_t* d0 = j.dst + static_cast<size_t>(row) * j.dstPitch;
        uint8_t* d1 = d0 + j.dstPitch;
        const uint8_t* y0 = j.y + static_cast<size_t>(row) * j.pitchY;
        const uint8_t* y1 = y0 + j.pitchY;
        const uint8_t* uv = j.uv + static_cast<size_t>(row / 2) * j.pitchUv;
        int x = 0;
        for (; x + 15 < j.dstW; x += 16) {
            __m128i uv8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uv + x));
            __m128i u16 = _mm_sub_epi16(_mm_and_si128(uv8, m00ff), c128);
            __m128i v16 = _mm_sub_epi16(_mm_srli_epi16(uv8, 8), c128);
            __m128i rv = _mm_srai_epi16(_mm_mullo_epi16(v16, c179), 7);
            __m128i guv = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(u16, c44), _mm_mullo_epi16(v16, c92)), 7);
            __m128i bu = _mm_srai_epi16(_mm_mullo_epi16(u16, c227), 7);
            __m128i y0v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y0 + x));
            __m128i y1v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y1 + x));
            bgraStore8(d0 + x * 4, _mm_unpacklo_epi8(y0v, zero), _mm_unpacklo_epi16(rv, rv), _mm_unpacklo_epi16(guv, guv),
                       _mm_unpacklo_epi16(bu, bu), a255);
            bgraStore8(d0 + x * 4 + 32, _mm_unpackhi_epi8(y0v, zero), _mm_unpackhi_epi16(rv, rv),
                       _mm_unpackhi_epi16(guv, guv), _mm_unpackhi_epi16(bu, bu), a255);
            bgraStore8(d1 + x * 4, _mm_unpacklo_epi8(y1v, zero), _mm_unpacklo_epi16(rv, rv), _mm_unpacklo_epi16(guv, guv),
                       _mm_unpacklo_epi16(bu, bu), a255);
            bgraStore8(d1 + x * 4 + 32, _mm_unpackhi_epi8(y1v, zero), _mm_unpackhi_epi16(rv, rv),
                       _mm_unpackhi_epi16(guv, guv), _mm_unpackhi_epi16(bu, bu), a255);
        }
        for (; x < j.dstW; ++x) {
            int uu = uv[(x & ~1)] - 128;
            int vv = uv[(x & ~1) + 1] - 128;
            bgraPx(d0 + x * 4, y0[x], uu, vv);
            bgraPx(d1 + x * 4, y1[x], uu, vv);
        }
    }
    for (; row < j.row1; ++row) {
        uint8_t* drow = j.dst + static_cast<size_t>(row) * j.dstPitch;
        const uint8_t* yrow = j.y + static_cast<size_t>(row) * j.pitchY;
        const uint8_t* uvrow = j.uv + static_cast<size_t>(row / 2) * j.pitchUv;
        for (int x = 0; x < j.dstW; ++x) {
            int uvx = x & ~1;
            bgraPx(drow + x * 4, yrow[x], uvrow[uvx] - 128, uvrow[uvx + 1] - 128);
        }
    }
}

void scaleRows(const BlitWork& j) {
    for (int row = j.row0; row < j.row1; ++row) {
        int sy = j.dstH ? row * j.srcH / j.dstH : 0;
        if (sy >= j.srcH) sy = j.srcH - 1;
        if (sy < 0) sy = 0;
        const uint8_t* yrow = j.y + static_cast<size_t>(sy) * j.pitchY;
        const uint8_t* uvrow = j.uv + static_cast<size_t>(sy / 2) * j.pitchUv;
        uint8_t* drow = j.dst + static_cast<size_t>(row) * j.dstPitch;
        for (int x = 0; x < j.dstW; ++x) {
            int sx = j.xLut[x];
            int uvx = sx & ~1;
            bgraPx(drow + x * 4, yrow[sx], uvrow[uvx] - 128, uvrow[uvx + 1] - 128);
        }
    }
}

// 1920×1080 → 1280×720 (esattamente 3:2): nearest-neighbor + SSE, senza
// il loop scalare per-pixel che sulla Jaguar teneva draw a 0 e il bitrate basso.
void scale3to2Rows(const BlitWork& j) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i m00ff = _mm_set1_epi16(0x00FF);
    const __m128i c128 = _mm_set1_epi16(128);
    const __m128i c179 = _mm_set1_epi16(179);
    const __m128i c44 = _mm_set1_epi16(44);
    const __m128i c92 = _mm_set1_epi16(92);
    const __m128i c227 = _mm_set1_epi16(227);
    const __m128i a255 = _mm_set1_epi16(255);
    const int srcW = j.srcW > 0 ? j.srcW : 1920;
    for (int row = j.row0; row < j.row1; ++row) {
        int sy = row + (row >> 1);  // row * 3/2
        if (sy >= j.srcH) sy = j.srcH - 1;
        if (sy < 0) sy = 0;
        uint8_t* drow = j.dst + static_cast<size_t>(row) * j.dstPitch;
        const uint8_t* yrow = j.y + static_cast<size_t>(sy) * j.pitchY;
        const uint8_t* uvrow = j.uv + static_cast<size_t>(sy / 2) * j.pitchUv;
        int x = 0;
        for (; x + 15 < j.dstW; x += 16) {
            alignas(16) uint8_t ys[16];
            // Come NV12 a destinazione: u0 v0 u1 v1 … (stesso layout di sseRows).
            alignas(16) uint8_t uvs[16];
            for (int k = 0; k < 16; ++k) {
                int sx = (x + k) + ((x + k) >> 1);  // * 3/2
                if (sx >= srcW) sx = srcW - 1;
                ys[k] = yrow[sx];
                int uvx = sx & ~1;
                uvs[k] = (k & 1) ? uvrow[uvx + 1] : uvrow[uvx];
            }
            __m128i uv8 = _mm_load_si128(reinterpret_cast<const __m128i*>(uvs));
            __m128i u16 = _mm_sub_epi16(_mm_and_si128(uv8, m00ff), c128);
            __m128i v16 = _mm_sub_epi16(_mm_srli_epi16(uv8, 8), c128);
            __m128i rv = _mm_srai_epi16(_mm_mullo_epi16(v16, c179), 7);
            __m128i guv = _mm_srai_epi16(_mm_add_epi16(_mm_mullo_epi16(u16, c44), _mm_mullo_epi16(v16, c92)), 7);
            __m128i bu = _mm_srai_epi16(_mm_mullo_epi16(u16, c227), 7);
            __m128i yv = _mm_load_si128(reinterpret_cast<const __m128i*>(ys));
            bgraStore8(drow + x * 4, _mm_unpacklo_epi8(yv, zero), _mm_unpacklo_epi16(rv, rv),
                       _mm_unpacklo_epi16(guv, guv), _mm_unpacklo_epi16(bu, bu), a255);
            bgraStore8(drow + x * 4 + 32, _mm_unpackhi_epi8(yv, zero), _mm_unpackhi_epi16(rv, rv),
                       _mm_unpackhi_epi16(guv, guv), _mm_unpackhi_epi16(bu, bu), a255);
        }
        for (; x < j.dstW; ++x) {
            int sx = x + (x >> 1);
            if (sx >= srcW) sx = srcW - 1;
            int uvx = sx & ~1;
            bgraPx(drow + x * 4, yrow[sx], uvrow[uvx] - 128, uvrow[uvx + 1] - 128);
        }
    }
}

void blitRows(const BlitWork& j) {
    if (j.row1 <= j.row0) return;
    if (j.sse) sseRows(j);
    else if (j.srcH == 1080 && j.dstH == 720 && j.dstW == 1280)
        scale3to2Rows(j);
    else
        scaleRows(j);
}

void blitThreadMain() {
    int seen = 0;
    while (true) {
        BlitWork job;
        {
            std::unique_lock<std::mutex> lock(g_blitMu);
            // Timeout: in chiusura o dopo un flip bloccato non si resta
            // appesi per sempre su wait (il main deve restare vivo).
            while (!g_blitStop && g_blitGen == seen) {
                if (g_blitCv.wait_for(lock, std::chrono::milliseconds(200)) == std::cv_status::timeout) {
                    if (xc::app::g_appStopping.load(std::memory_order_relaxed)) return;
                }
            }
            if (g_blitStop && g_blitGen == seen) return;
            seen = g_blitGen;
            job = g_blitJob;
        }
        blitRows(job);
        {
            std::lock_guard<std::mutex> lock(g_blitMu);
            g_blitSeen = seen;
        }
        g_blitCv.notify_one();
    }
}

bool ensureBlitThread() {
    if (g_blitReady) return true;
    try {
        g_blitThread = std::thread(blitThreadMain);
        g_blitReady = true;
        return true;
    } catch (...) {
        XC_LOGW("video: thread blit non creato, conversione single-thread");
        return false;
    }
}

void runSplit(const BlitWork& top, const BlitWork& bot) {
    if (!ensureBlitThread()) {
        blitRows(top);
        blitRows(bot);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_blitMu);
        g_blitJob = top;
        ++g_blitGen;
    }
    g_blitCv.notify_one();
    blitRows(bot);
    std::unique_lock<std::mutex> lock(g_blitMu);
    // 80 ms bastano per mezza 720p. Niente wait infinito: il main e ICE
    // devono continuare anche se l'helper è lento.
    if (!g_blitCv.wait_for(lock, std::chrono::milliseconds(80), [&] { return g_blitSeen == g_blitGen; })) {
        XC_LOGW("video: blit helper lento, attendo ancora 120 ms");
        g_blitCv.wait_for(lock, std::chrono::milliseconds(120), [&] { return g_blitSeen == g_blitGen; });
    }
}

void ensureLut(int srcW) {
    if (g_lutSrcW == srcW && static_cast<int>(g_xLut.size()) == static_cast<int>(kViewW)) return;
    g_xLut.resize(kViewW);
    for (unsigned x = 0; x < kViewW; ++x) {
        int sx = static_cast<int>(x) * srcW / static_cast<int>(kViewW);
        if (sx >= srcW) sx = srcW - 1;
        if (sx < 0) sx = 0;
        g_xLut[x] = sx;
    }
    g_lutSrcW = srcW;
}

void paintBgra(uint8_t* dst, const uint8_t* nv12, int srcPitch, int srcW, int srcH) {
    const uint8_t* y = nv12;
    const uint8_t* uv = nv12 + static_cast<size_t>(srcPitch) * static_cast<size_t>(srcH);
    bool same = srcW == static_cast<int>(kViewW) && srcH == static_cast<int>(kViewH);
    if (!same) ensureLut(srcW);
    auto job = [&](int row0, int row1) {
        BlitWork j;
        j.y = y;
        j.uv = uv;
        j.dst = dst;
        j.xLut = g_xLut.empty() ? nullptr : g_xLut.data();
        j.pitchY = srcPitch;
        j.pitchUv = srcPitch;
        j.dstPitch = static_cast<int>(kViewW) * 4;
        j.srcW = srcW;
        j.srcH = srcH;
        j.dstW = static_cast<int>(kViewW);
        j.dstH = static_cast<int>(kViewH);
        j.row0 = row0;
        j.row1 = row1;
        j.sse = same;
        return j;
    };
    int mid = static_cast<int>(kViewH) / 2;
    runSplit(job(0, mid), job(mid, static_cast<int>(kViewH)));
}

void blendOverlayBgra(uint8_t* dst) {
    if (!g_overlay.on || g_overlay.px.empty() || g_overlay.opacity == 0) return;
    const int a = g_overlay.opacity;
    for (int y = 0; y < g_overlay.h; ++y) {
        int dy = (g_overlay.y + y) * static_cast<int>(kViewH) / static_cast<int>(kHeight);
        if (dy < 0 || dy >= static_cast<int>(kViewH)) continue;
        uint8_t* row = dst + static_cast<size_t>(dy) * kViewW * 4u;
        for (int x = 0; x < g_overlay.w; ++x) {
            int dx = (g_overlay.x + x) * static_cast<int>(kViewW) / static_cast<int>(kWidth);
            if (dx < 0 || dx >= static_cast<int>(kViewW)) continue;
            uint32_t s = g_overlay.px[static_cast<size_t>(y) * g_overlay.w + x];
            int sa = static_cast<int>(((s >> 24) & 0xff) * a / 255);
            if (sa < 16) continue;
            int r = s & 0xff, g = (s >> 8) & 0xff, b = (s >> 16) & 0xff;
            uint8_t* dp = row + dx * 4;
            dp[0] = static_cast<uint8_t>((b * sa + dp[0] * (255 - sa)) / 255);
            dp[1] = static_cast<uint8_t>((g * sa + dp[1] * (255 - sa)) / 255);
            dp[2] = static_cast<uint8_t>((r * sa + dp[2] * (255 - sa)) / 255);
            dp[3] = 0xFF;
        }
    }
}

void resolveFlush() {
    if (g_flushTried) return;
    g_flushTried = true;
    const char* paths[] = {"/system/common/lib/libSceGnmDriver.sprx", "libSceGnmDriver.sprx"};
    for (const char* path : paths) {
        int32_t rc = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr));
        XC_LOGI("video: LoadStartModule(%s) 0x%08x", path, static_cast<unsigned>(rc));
        if (rc > 0) {
            void* sym = nullptr;
            if (sceKernelDlsym(rc, "sceGnmFlushGarlic", &sym) == 0 && sym) g_flush = reinterpret_cast<FlushFn>(sym);
            break;
        }
    }
    XC_LOGI("video: sceGnmFlushGarlic %s", g_flush ? "preso dallo sprx" : "assente, solo sfence");
}

void flushScanout() {
    __asm__ volatile("sfence" ::: "memory");
    // Resolve già fatto a bind: niente LoadStartModule sotto frameMutex.
    if (g_flush) g_flush();
}

bool bindScan(Scan want) {
    if (g_video >= 0) {
        drainFlips();
        sceVideoOutUnregisterBuffers(g_video, 0);
        sceVideoOutClose(g_video);
        g_video = -1;
    }
    g_bound = false;
    g_scan = Scan::Ui;
    if (want == Scan::Ycc) return switchToYcc() || switchToUi();
    if (want == Scan::Bgra) return switchToBgra() || switchToUi();
    return switchToUi();
}
}  // namespace

bool init(bool) {
    g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (g_video < 0) {
        XC_LOGE("sceVideoOutOpen: 0x%08x", static_cast<unsigned>(g_video));
        platform::notify("PSBox: schermo non disponibile");
        return false;
    }
    size_t one = frameBytes();
    g_bytes = (one * kBuffers + kAlign - 1) & ~(kAlign - 1);
    int rc = sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), g_bytes, kAlign,
                                           ORBIS_KERNEL_WC_GARLIC, &g_phys);
    if (rc < 0) {
        XC_LOGE("memoria video: 0x%08x", static_cast<unsigned>(rc));
        return false;
    }
    rc = sceKernelMapDirectMemory(&g_base, g_bytes, 0x33, 0, g_phys, kAlign);
    if (rc < 0 || !g_base) {
        XC_LOGE("map video: 0x%08x", static_cast<unsigned>(rc));
        return false;
    }
    std::memset(g_base, 0, one * kBuffers);
    for (int i = 0; i < kBuffers; ++i) g_fb[i] = static_cast<uint8_t*>(g_base) + i * one;

    OrbisVideoOutBufferAttribute attr{};
    sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
                                  ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, kWidth, kHeight, kWidth);
    rc = sceVideoOutRegisterBuffers(g_video, 0, reinterpret_cast<void* const*>(g_fb), kBuffers, &attr);
    if (rc < 0) {
        XC_LOGE("sceVideoOutRegisterBuffers: 0x%08x", static_cast<unsigned>(rc));
        return false;
    }
    sceVideoOutSetFlipRate(g_video, ORBIS_VIDEO_OUT_FLIP_60HZ);
    g_scan = Scan::Ui;
    g_bound = true;
    resolveFlush();  // LoadStartModule Gnm ora, non al primo frame stream
    XC_LOGI("schermo PS4 1080p lineare, %d buffer", kBuffers);
    return true;
}

void shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_blitMu);
        g_blitStop = true;
    }
    g_blitCv.notify_all();
    if (g_blitThread.joinable()) g_blitThread.join();
    if (g_video >= 0) {
        drainFlips();
        sceVideoOutUnregisterBuffers(g_video, 0);
        sceVideoOutClose(g_video);
        g_video = -1;
    }
}

void releaseForExit() {
    // Niente mutex bloccante, niente sceVideoOutClose: in chiusura di
    // sistema Close resta appeso → CE-34878-0. Si invalida l'handle; i
    // draw vedono g_video < 0. Il kernel ripulisce al _exit.
    g_holdFlips.store(true);
    g_blitStop = true;
    g_blitCv.notify_all();
    g_video = -1;
    g_bound = false;
}

void holdFlips(bool on) { g_holdFlips.store(on); }

void suspendForHome() {
    // Dal thread main, dopo che present() ha visto il flag e non è più in
    // SubmitFlip. Close qui (menu PS, non il kill) libera il bus: il sistema
    // può sospendere e poi chiudere senza marcare l'app danneggiata.
    g_holdFlips.store(true);
    if (g_homeSuspended || xc::app::g_appStopping.load()) return;
    g_homeSuspended = true;
    drainFlips();
    if (g_video >= 0) {
        sceVideoOutUnregisterBuffers(g_video, 0);
        int rc = sceVideoOutClose(g_video);
        XC_LOGI("video: porta chiusa per il menu PS 0x%08x", static_cast<unsigned>(rc));
        g_video = -1;
    }
    g_bound = false;
}

void resumeFromHome() {
    if (!g_homeSuspended && g_video >= 0) {
        g_holdFlips.store(false);
        return;
    }
    g_homeSuspended = false;
    g_holdFlips.store(false);
    Scan keep = (g_scan == Scan::Ycc || g_scan == Scan::Bgra) ? g_scan : Scan::Ui;
    XC_LOGI("video: riapro dopo il menu PS");
    if (!bindScan(keep)) XC_LOGE("video: ripresa dal menu PS fallita");
    else XC_LOGI("video: ripreso dopo il menu PS");
}

void resumeAfterRest() {
    XC_LOGI("exit: riapro il video");
    Scan keep = g_scan;
    if (!bindScan(keep)) XC_LOGE("exit: video non riaperto");
    else XC_LOGI("exit: video riaperto");
}

std::timed_mutex& frameMutex() {
    static std::timed_mutex m;
    return m;
}

bool streamOwnsDisplay() { return g_scan == Scan::Ycc || g_scan == Scan::Bgra; }

bool present() {
    if (g_holdFlips.load(std::memory_order_relaxed) || xc::app::g_psMenu.load(std::memory_order_relaxed)) return false;
    if (g_video < 0 || xc::app::g_appStopping.load(std::memory_order_relaxed)) return false;
    flushScanout();
    if (g_video < 0 || xc::app::g_appStopping.load(std::memory_order_relaxed)) return false;
    ++g_presentId;
    const int flipIdx = g_index;
    auto flip = [&](int mode) {
        return sceVideoOutSubmitFlip(g_video, flipIdx, static_cast<uint32_t>(mode), static_cast<int64_t>(g_presentId));
    };
    int rc = -1;
    if (!g_flipProbed) {
        // Come Moonlight: VSYNC (1), HSYNC/ASAP (2), 0. Il primo che
        // torna 0 resta; niente attesa su equeue/waitEvent.
        static const int kModes[] = {1, 2, 0};
        for (int mode : kModes) {
            rc = flip(mode);
            OrbisVideoOutFlipStatus st{};
            sceVideoOutGetFlipStatus(g_video, &st);
            XC_LOGI("video: flip probe mode=%d => 0x%08x pending=%d cur=%d", mode, static_cast<unsigned>(rc),
                    st.numFlipPending, st.currentBuffer);
            if (rc == 0) {
                g_flipMode = mode;
                g_flipProbed = true;
                break;
            }
        }
        if (!g_flipProbed) {
            XC_LOGE("video: SubmitFlip fallito su tutti i mode, porta tenuta");
            return false;
        }
    } else {
        rc = flip(g_flipMode);
        int alive = 1;
        if (!flipOnSubmit(rc, &alive)) {
            static int logged = 0;
            if (logged < 4) {
                ++logged;
                XC_LOGE("video: SubmitFlip 0x%08x, porta tenuta", static_cast<unsigned>(rc));
            }
            return false;
        }
    }
    noteSubmit(flipIdx);
    g_lastFlipped = flipIdx;
    g_shownArg = static_cast<int64_t>(g_presentId);
    g_index = (g_index + 1) % g_bufCount;
    return true;
}

void drawRgba(const uint32_t* pixels) {
    if (g_holdFlips.load(std::memory_order_relaxed) || xc::app::g_psMenu.load(std::memory_order_relaxed)) return;
    if (!pixels || !g_fb[0] || xc::app::g_appStopping.load(std::memory_order_relaxed)) return;
    // Lo stream ha la porta: non si riapre RGBA (era la doppia open / steal).
    if (streamOwnsDisplay()) return;
    if (g_scan != Scan::Ui && !switchToUi()) return;
    if (!waitFree(g_index, false)) return;  // non bloccare il main sullo spinner
    auto* dst = reinterpret_cast<uint32_t*>(g_fb[g_index]);
    std::memcpy(dst, pixels, frameBytes());
    blendOverlay(dst);
}

bool drawYuv420(const uint8_t*, const uint8_t*, const uint8_t*, int, int, int, int, int, bool) { return false; }

bool drawNv12(const uint8_t* nv12, int pitch, int width, int height, bool wait) {
    if (g_holdFlips.load(std::memory_order_relaxed) || xc::app::g_psMenu.load(std::memory_order_relaxed)) return false;
    if (!nv12 || width < 2 || height < 2 || pitch < width || !g_fb[0]) return false;
    if (xc::app::g_appStopping.load(std::memory_order_relaxed) || g_video < 0) return false;
    if (g_scan != Scan::Ycc && g_scan != Scan::Bgra) {
        // Formato scelto una volta: su 11.52 YCbCr420 → 0x80290003.
        if (!g_yccFailed) {
            if (!switchToYcc()) g_yccFailed = true;
        } else {
            static bool loggedSkip = false;
            if (!loggedSkip) {
                loggedSkip = true;
                XC_LOGI("video: salto YCbCr420 (0x80290003 su questo FW), apro solo BGRA720");
            }
        }
        if (g_scan != Scan::Ycc && !switchToBgra()) return false;
    }
    // Non latchare su un unico g_index occupato: scegli un buffer libero.
    // Se nessuno è libero e !wait, false senza present (niente "generation"
    // marcata: il frame successivo riprova — lezione AJ-GFN sul gate stuck).
    if (!pickFree(wait)) return false;
    if (g_scan == Scan::Ycc) {
        uint8_t* dst = g_yccFb[g_index];
        int rows = height < static_cast<int>(kHeight) ? height : static_cast<int>(kHeight);
        int cols = width < static_cast<int>(kWidth) ? width : static_cast<int>(kWidth);
        const uint8_t* srcUv = nv12 + static_cast<size_t>(pitch) * static_cast<size_t>(height);
        uint8_t* dstUv = dst + static_cast<size_t>(kWidth) * kHeight;
        for (int y = 0; y < rows; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * kWidth, nv12 + static_cast<size_t>(y) * pitch, static_cast<size_t>(cols));
        for (int y = 0; y < rows / 2; ++y)
            std::memcpy(dstUv + static_cast<size_t>(y) * kWidth, srcUv + static_cast<size_t>(y) * pitch, static_cast<size_t>(cols));
        blendOverlayNv12(dst, static_cast<int>(kWidth), static_cast<int>(kHeight));
        return true;
    }
    paintBgra(g_bgraFb[g_index], nv12, pitch, width, height);
    blendOverlayBgra(g_bgraFb[g_index]);
    return true;
}

bool fastScanout() { return g_scan == Scan::Ycc; }

void setOverlay(const uint32_t* pixels, int x, int y, int w, int h, uint8_t opacity) {
    if (!pixels || w <= 0 || h <= 0) {
        g_overlay.on = false;
        g_overlay.px.clear();
        return;
    }
    g_overlay.px.assign(pixels, pixels + static_cast<size_t>(w) * h);
    g_overlay.x = x;
    g_overlay.y = y;
    g_overlay.w = w;
    g_overlay.h = h;
    g_overlay.opacity = opacity;
    g_overlay.on = true;
}

void setSharpness(int amount) { g_sharpness = amount; }
void setDeband(int) {}
void setUpscaler(int) {}
void setRestore(bool) {}

uint64_t lastPresentId() { return g_presentId; }

bool lastShown(uint64_t& id, uint64_t& atUs) {
    if (g_shownArg < 0) return false;
    id = static_cast<uint64_t>(g_shownArg);
    atUs = platform::nowUs();
    return true;
}

bool readBackRgb(std::vector<uint8_t>& rgb, int& width, int& height) {
    // Solo il buffer passato a SubmitFlip (UI/stream + overlay già compositati).
    // Non usare g_index (back buffer parziale) né un layer offscreen.
    if (g_lastFlipped < 0 || g_lastFlipped >= g_bufCount) return false;
    if (g_scan == Scan::Ui) {
        if (!g_fb[0]) return false;
        auto* src = reinterpret_cast<uint32_t*>(g_fb[g_lastFlipped]);
        width = static_cast<int>(kWidth);
        height = static_cast<int>(kHeight);
        rgb.resize(static_cast<size_t>(width) * height * 3);
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
            uint32_t p = src[i];
            rgb[i * 3] = static_cast<uint8_t>(p);
            rgb[i * 3 + 1] = static_cast<uint8_t>(p >> 8);
            rgb[i * 3 + 2] = static_cast<uint8_t>(p >> 16);
        }
        return true;
    }
    if (g_scan == Scan::Bgra && g_bgraFb[g_lastFlipped]) {
        // Scanout 1280×720 BGRA (stesso buffer inviato a VideoOut).
        auto* src = reinterpret_cast<uint32_t*>(g_bgraFb[g_lastFlipped]);
        width = static_cast<int>(kViewW);
        height = static_cast<int>(kViewH);
        rgb.resize(static_cast<size_t>(width) * height * 3);
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
            uint32_t p = src[i];  // BGRA in memoria
            rgb[i * 3] = static_cast<uint8_t>(p >> 16);
            rgb[i * 3 + 1] = static_cast<uint8_t>(p >> 8);
            rgb[i * 3 + 2] = static_cast<uint8_t>(p);
        }
        return true;
    }
    // YCbCr420: niente conversione qui (FirstStream su FW YCC resta skip).
    return false;
}

namespace gpu {
bool init() { return false; }
bool ready() { return false; }
void probe(int) {}
bool drawYuv420(const uint8_t*, const uint8_t*, const uint8_t*, int, int, int, int, int, bool) { return false; }
void drawRgba(const uint32_t*) {}
void present() {}
void setOverlay(const uint32_t*, int, int, int, int, uint8_t) {}
void setSharpness(int) {}
void setDeband(int) {}
void setUpscaler(int) {}
void setRestore(bool) {}
bool setHighRefresh(bool) { return false; }
bool canSwitchRefresh() { return false; }
void setSwapImages(int) {}
void setPresentWait(bool) {}
uint64_t lastPresentId() { return 0; }
bool lastShown(uint64_t&, uint64_t&) { return false; }
bool readBack(std::vector<uint8_t>&, int&, int&) { return false; }
}  // namespace gpu

}  // namespace xc::display
