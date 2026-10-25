// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder H.264 hardware PS4. La sequenza è quella di Moonlight-ps4
// (videodec2_loader.c + decoder_orbis.c): load dei moduli, coda compute
// su più pipe, memorie ONION/GARLIC, AU da 1 MiB a rotazione, più framebuffer.
// Il present resta su display_ps4: renderer_videoout.c aprirebbe un secondo
// VideoOut e il percorso YCbCr dipende da un kpayload solo per il firmware 9.00.
#include "media/hw_decoder.h"
#include "media/fb_own.h"
#include "util/log.h"

extern "C" {
#include "orbis/videodec2.h"
#include "video/nv12_blit.h"
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

namespace xc::media {

namespace {
constexpr int kProtRw = 0x33;
constexpr size_t kAlign = 0x4000;
// Profondità 2, senza Flush a ogni frame. Sulla 01.22 depth 1 più Flush
// stava sui 10–12 ms; depth 2 senza Flush stava sui 2.4 ms. Moonlight-ps4
// (videodec2.h) nota che depth 1 serializza sulla coda compute.
constexpr int kDepth = 2;
constexpr int kFbCount = kDepth + 2;

struct Block {
    void* ptr = nullptr;
    off_t off = 0;
    size_t size = 0;
};

const char* hex(int rc) {
    static char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", static_cast<unsigned>(rc));
    return buf;
}

Block allocMem(size_t need, int type, const char* what) {
    Block b;
    if (need == 0) return b;
    need = (need + kAlign - 1) & ~(kAlign - 1);
    int rc = sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), need, kAlign, type, &b.off);
    XC_LOGI("hwdec: AllocateDirectMemory %s %zu tipo %d => %s", what, need, type, hex(rc));
    if (rc < 0) return {};
    rc = sceKernelMapDirectMemory(&b.ptr, need, kProtRw, 0, b.off, kAlign);
    XC_LOGI("hwdec: MapDirectMemory %s => %s va=%p", what, hex(rc), b.ptr);
    if (rc < 0 || !b.ptr) {
        sceKernelReleaseDirectMemory(b.off, need);
        return {};
    }
    b.size = need;
    std::memset(b.ptr, 0, need);
    return b;
}

void freeMem(Block& b) {
    if (b.size) sceKernelReleaseDirectMemory(b.off, b.size);
    b = {};
}

struct State {
    videodec2_api_t api{};
    OrbisVideodec2Decoder dec = nullptr;
    bool loaded = false;
    Block cpu, gpu, cpuGpu;
    Block fb;
    void* fbCpu = nullptr;
    bool fbAlias = false;
    size_t fbStride = 0;
    int fbN = 1;
    int fbI = 0;
    FbOwn fbOwn{};
    Block au[kFbCount];
    int auN = 1;
    int auI = 0;
    std::vector<uint8_t> shadow;
    int decodeLogs = 0;
    int maxW = 0;
    int maxH = 0;
};

}  // namespace

struct HwHold {
    State* st = nullptr;
    std::string error;
};

HwDecoder::~HwDecoder() {
    auto* hold = static_cast<HwHold*>(platform_);
    if (!hold) return;
    State* st = hold->st;
    if (st) {
        if (st->dec && st->api.DeleteDecoder) {
            int rc = st->api.DeleteDecoder(st->dec);
            XC_LOGI("hwdec: DeleteDecoder => %s", hex(rc));
        }
        for (int i = 0; i < kFbCount; ++i) freeMem(st->au[i]);
        freeMem(st->fb);
        freeMem(st->cpuGpu);
        freeMem(st->gpu);
        freeMem(st->cpu);
        if (st->loaded) videodec2_unload(&st->api);
        nv12_blit_shutdown();
        delete st;
    }
    delete hold;
    platform_ = nullptr;
    decoder_ = nullptr;
}

bool HwDecoder::init(int maxWidth, int maxHeight) {
    lastError_.clear();
    auto setErr = [&](const std::string& why) {
        lastError_ = why;
        XC_LOGE("hwdec: %s", why.c_str());
        return false;
    };

    if (decoder_) return true;
    auto* hold = new HwHold();
    platform_ = hold;
    auto* st = new State();
    hold->st = st;

    int w = maxWidth > 0 ? maxWidth : 1920;
    int h = maxHeight > 0 ? maxHeight : 1080;
    st->maxW = w;
    st->maxH = h;
    XC_LOGI("hwdec: start %dx%d", w, h);

    int sm = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VDECCORE);
    XC_LOGI("hwdec: VDECCORE %s", hex(sm));
    int sm2 = sceSysmoduleLoadModule(ORBIS_SYSMODULE_VIDEODEC2);
    XC_LOGI("hwdec: VIDEODEC2 %s", hex(sm2));

    if (videodec2_load(&st->api) != 0)
        return setErr("videodec2_load fallito (vedi i LoadStartModule sopra)");
    st->loaded = true;
    XC_LOGI("hwdec: simboli %p", st->api.queue);

    int rc = videodec2_ensure_compute_queue(&st->api);
    XC_LOGI("hwdec: coda %s %p", hex(rc), st->api.queue);
    if (rc < 0 || !st->api.queue)
        return setErr(std::string("AllocateComputeQueue fallito ") + hex(rc));

    OrbisVideodec2DecoderConfigInfo dc;
    videodec2_fill_decoder_config_ex(&dc, st->api.queue, w, h, kDepth, ORBIS_VIDEODEC2_THREAD_PRIO_DEFAULT);
    XC_LOGI("hwdec: cfg %u/%u %dx%d dpb %d dep %u prio %d mem %d",
            dc.profile, dc.maxLevel, dc.maxFrameWidth, dc.maxFrameHeight, dc.maxDpbFrameCount,
            dc.decodePipelineDepth, dc.cpuThreadPriority, dc.checkMemoryType ? 1 : 0);

    OrbisVideodec2DecoderMemoryInfo dm;
    std::memset(&dm, 0, sizeof dm);
    dm.thisSize = sizeof dm;
    rc = st->api.QueryDecoderMemoryInfo(&dc, &dm);
    XC_LOGI("hwdec: mem %s cpu %llu gpu %llu both %llu fb %llu al %u",
            hex(rc), static_cast<unsigned long long>(dm.cpuMemorySize),
            static_cast<unsigned long long>(dm.gpuMemorySize),
            static_cast<unsigned long long>(dm.cpuGpuMemorySize),
            static_cast<unsigned long long>(dm.maxFrameBufferSize), dm.frameBufferAlignment);
    if (rc < 0) return setErr(std::string("QueryDecoderMemoryInfo ") + hex(rc));

    if (dm.cpuMemorySize) {
        st->cpu = allocMem(static_cast<size_t>(dm.cpuMemorySize), ORBIS_KERNEL_WB_ONION, "cpu");
        dm.cpuMemory = st->cpu.ptr;
        if (!dm.cpuMemory) return setErr("memoria CPU (ONION) non allocata");
    }
    if (dm.gpuMemorySize) {
        st->gpu = allocMem(static_cast<size_t>(dm.gpuMemorySize), ORBIS_KERNEL_WC_GARLIC, "gpu");
        dm.gpuMemory = st->gpu.ptr;
        if (!dm.gpuMemory) return setErr("memoria GPU (GARLIC) non allocata");
    }
    if (dm.cpuGpuMemorySize) {
        st->cpuGpu = allocMem(static_cast<size_t>(dm.cpuGpuMemorySize), ORBIS_KERNEL_WB_ONION, "cpuGpu");
        dm.cpuGpuMemory = st->cpuGpu.ptr;
        if (!dm.cpuGpuMemory) return setErr("memoria cpuGpu (ONION) non allocata");
    }

    if (dm.maxFrameBufferSize) {
        size_t align = dm.frameBufferAlignment ? dm.frameBufferAlignment : 0x100u;
        if (align < kAlign) align = kAlign;
        size_t one = (static_cast<size_t>(dm.maxFrameBufferSize) + align - 1) & ~(align - 1);
        size_t total = one * static_cast<size_t>(kFbCount);
        rc = sceKernelAllocateDirectMemory(0, static_cast<off_t>(sceKernelGetDirectMemorySize()), total, align,
                                            ORBIS_KERNEL_WC_GARLIC, &st->fb.off);
        XC_LOGI("hwdec: framebuffer phys %zu x%d => %s", one, kFbCount, hex(rc));
        if (rc < 0) return setErr(std::string("framebuffer AllocateDirectMemory ") + hex(rc));
        st->fb.size = total;
        st->fbStride = one;
        st->fbN = kFbCount;
        fbOwnInit(&st->fbOwn, st->fbN);

        struct Try {
            int type;
            const char* name;
            size_t al;
        } tries[] = {
            {ORBIS_KERNEL_WB_ONION, "ONION", align},
            {ORBIS_KERNEL_WB_ONION, "ONION/16k", kAlign},
            {ORBIS_KERNEL_WB_GARLIC, "WB_GARLIC", align},
            {ORBIS_KERNEL_WB_GARLIC, "WB_GARLIC/16k", kAlign},
        };
        for (const Try& t : tries) {
            void* va = nullptr;
            int m2 = sceKernelMapDirectMemory2(&va, total, t.type, kProtRw, 0, st->fb.off, t.al);
            XC_LOGI("hwdec: framebuffer Map2 %s => %s va=%p", t.name, hex(m2), va);
            if (m2 == 0 && va) {
                st->fb.ptr = va;
                st->fbCpu = va;
                st->fbAlias = true;
                break;
            }
        }
        if (!st->fb.ptr) {
            rc = sceKernelMapDirectMemory(&st->fb.ptr, total, kProtRw, 0, st->fb.off, align);
            XC_LOGI("hwdec: framebuffer Map WC_GARLIC => %s va=%p", hex(rc), st->fb.ptr);
            if (rc < 0 || !st->fb.ptr) return setErr(std::string("framebuffer MapDirectMemory ") + hex(rc));
        }
        nv12_blit_init();
        nv12_blit_bind_decoder_fb(st->fbCpu ? st->fbCpu : st->fb.ptr, st->fb.off, st->fb.size,
                                  st->fbAlias ? 0 : 3);
    }

    rc = st->api.CreateDecoder(&dc, &dm, &st->dec);
    XC_LOGI("hwdec: CreateDecoder => %s dec=%p", hex(rc), st->dec);
    if (rc < 0 || !st->dec) return setErr(std::string("CreateDecoder ") + hex(rc));

    st->auN = st->fbN;
    for (int i = 0; i < st->auN; ++i) {
        st->au[i] = allocMem(1024 * 1024, ORBIS_KERNEL_WB_ONION, "AU");
        if (!st->au[i].ptr) return setErr("buffer AU (ONION, 1 MiB) non allocato");
    }

    decoder_ = st->dec;
    XC_LOGI("hwdec: pronto %dx%d dep %d fb %d %s", dc.maxFrameWidth, dc.maxFrameHeight,
            kDepth, st->fbN, nv12_blit_mode_name());
    return true;
}

bool HwDecoder::reset() {
    lastNeededReset_ = false;
    lastOutputBusy_ = false;
    lastErrorFrame_ = false;
    auto* hold = static_cast<HwHold*>(platform_);
    State* st = hold ? hold->st : nullptr;
    if (!st || !st->dec || !st->api.Reset) return false;
    fbOwnInit(&st->fbOwn, st->fbN);
    int rc = st->api.Reset(st->dec);
    XC_LOGI("hwdec: Reset => %s", hex(rc));
    return rc >= 0;
}

bool HwDecoder::decode(const uint8_t* data, size_t len, uint64_t pts, Picture& out) {
    out = {};
    lastFailed_ = false;
    lastErrorFrame_ = false;
    lastOutputBusy_ = false;
    lastNeededReset_ = false;
    auto* hold = static_cast<HwHold*>(platform_);
    State* st = hold ? hold->st : nullptr;
    if (!st || !st->dec || !data || !len) return false;

    st->auI = (st->auI + 1) % st->auN;
    Block& au = st->au[st->auI];
    if (len > au.size) {
        XC_LOGE("hwdec: AU %zu più grande del buffer %zu", len, au.size);
        if (st->decodeLogs++ < 4) lastError_ = "access unit più grande del buffer Videodec2";
        return false;
    }
    std::memcpy(au.ptr, data, len);

    OrbisVideodec2InputData in;
    std::memset(&in, 0, sizeof in);
    in.thisSize = sizeof in;
    in.auData = au.ptr;
    in.auSize = len;
    in.ptsData = pts;
    in.dtsData = pts;

    int slot = fbOffer(&st->fbOwn);
    if (slot < 0) {
        lastFailed_ = true;
        lastNeededReset_ = true;
        lastOutputBusy_ = true;
        lastError_ = "framebuffer trattenuto";
        XC_LOGE("hwdec: nessun framebuffer libero (trattenuti %d)", fbHeldCount(&st->fbOwn));
        return false;
    }
    OrbisVideodec2FrameBuffer fb;
    std::memset(&fb, 0, sizeof fb);
    fb.thisSize = sizeof fb;
    fb.frameBuffer = static_cast<uint8_t*>(st->fb.ptr) + static_cast<size_t>(slot) * st->fbStride;
    fb.frameBufferSize = st->fbStride;
    fb.isAccepted = false;

    OrbisVideodec2OutputInfo info;
    std::memset(&info, 0, sizeof info);
    info.thisSize = sizeof info;

    // NAL type dal primo start code o dal primo byte dell'AU.
    uint8_t nalType = 0;
    if (len >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
        nalType = data[4] & 0x1F;
    else if (len >= 1)
        nalType = data[0] & 0x1F;

    // Se Decode si pianta (moduli 0x80020002), l'ultima riga è enter e il
    // watchdog vede il video idle. Niente lock condivisi col thread rete.
    static int onceDec = 0;
    if (onceDec == 0) {
        onceDec = 1;
        XC_LOGI("hwdec: Decode enter AU %zu nal %u", len, static_cast<unsigned>(nalType));
    }
    int rc = st->api.Decode(st->dec, &in, &fb, &info);
    // 0x811D0300 = NEW_SEQUENCE (SPS/PPS nuovi o cambio risoluzione): senza
    // Reset ogni Decode successiva fallisce, anche gli IDR. 0x811D0304 =
    // FATAL_STREAM. Si Reset e si ritenta la stessa AU (contiene la sequenza).
    constexpr unsigned kNewSequence = 0x811d0300u;
    constexpr unsigned kFatalStream = 0x811d0304u;
    if (rc < 0) {
        auto u = static_cast<unsigned>(rc);
        if ((u == kNewSequence || u == kFatalStream) && st->api.Reset) {
            lastNeededReset_ = true;
            int rr = st->api.Reset(st->dec);
            XC_LOGW("hwdec: Decode %s → Reset %s, ritento AU %zu nal %u", hex(rc), hex(rr), len,
                    static_cast<unsigned>(nalType));
            if (rr >= 0) {
                fbOwnInit(&st->fbOwn, st->fbN);
                slot = fbOffer(&st->fbOwn);
                std::memset(&info, 0, sizeof info);
                info.thisSize = sizeof info;
                std::memset(&fb, 0, sizeof fb);
                fb.thisSize = sizeof fb;
                if (slot >= 0) {
                    fb.frameBuffer = static_cast<uint8_t*>(st->fb.ptr) + static_cast<size_t>(slot) * st->fbStride;
                    fb.frameBufferSize = st->fbStride;
                    fb.isAccepted = false;
                    rc = st->api.Decode(st->dec, &in, &fb, &info);
                }
            }
        }
    }
    if (onceDec == 1) {
        onceDec = 2;
        XC_LOGI("hwdec: Decode leave rc %s valid=%d", hex(rc), info.isValid ? 1 : 0);
    }
    if (slot >= 0) fbOnAccept(&st->fbOwn, slot, fb.isAccepted ? 1 : 0);
    if (rc < 0) {
        if (!fb.isAccepted && slot >= 0) fbRelease(&st->fbOwn, slot);
        lastFailed_ = true;
        auto u = static_cast<unsigned>(rc);
        if (u == kNewSequence || u == kFatalStream) lastNeededReset_ = true;
        if (st->decodeLogs++ < 12)
            XC_LOGE("hwdec: Decode rc %s AU %zu nal %u buf %dx%d", hex(rc), len,
                    static_cast<unsigned>(nalType), st->maxW, st->maxH);
        char why[96];
        std::snprintf(why, sizeof why, "Decode %s nal %u AU %zu", hex(rc), static_cast<unsigned>(nalType), len);
        lastError_ = why;
        return false;
    }
    if (!info.isValid || info.isErrorFrame || !info.frameWidth || !info.frameHeight || !info.frameBuffer) {
        if (!fb.isAccepted && slot >= 0) fbRelease(&st->fbOwn, slot);
        lastFailed_ = info.isErrorFrame != 0;
        lastErrorFrame_ = info.isErrorFrame != 0;
        if (st->decodeLogs++ < 12)
            XC_LOGI("hwdec: Decode ok senza pic valid=%d err=%d %ux%u nal %u AU %zu", info.isValid ? 1 : 0,
                    info.isErrorFrame ? 1 : 0, info.frameWidth, info.frameHeight, static_cast<unsigned>(nalType),
                    len);
        if (info.isErrorFrame) {
            char why[96];
            std::snprintf(why, sizeof why, "errorFrame nal %u %ux%u AU %zu", static_cast<unsigned>(nalType),
                          info.frameWidth, info.frameHeight, len);
            lastError_ = why;
        }
        return false;
    }
    int outSlot = fbIndexOf(st->fb.ptr, st->fbStride, info.frameBuffer, st->fbN);
    if (outSlot < 0) outSlot = slot;

    uint32_t pitchBytes = info.framePitchInBytes ? info.framePitchInBytes : (info.framePitch ? info.framePitch : info.frameWidth);
    size_t bytes = static_cast<size_t>(pitchBytes) * info.frameHeight * 3 / 2;
    if (bytes > st->fbStride) bytes = st->fbStride;
    const uint8_t* src = static_cast<const uint8_t*>(info.frameBuffer);
    if (st->fbAlias && st->fbCpu && st->fb.ptr) {
        auto off = src - static_cast<const uint8_t*>(st->fb.ptr);
        if (off >= 0 && static_cast<size_t>(off) < st->fb.size) src = static_cast<const uint8_t*>(st->fbCpu) + off;
    }
    // L'alias ONION è già leggibile: il loop video lo copia sullo scanout
    // prima della Decode successiva (tre framebuffer, questo non viene
    // riusato subito). Senza alias la memoria è WC_GARLIC e una lettura
    // sparsa è lentissima: una sola copia in RAM cacheabile, come il bounce
    // di Moonlight-ps4.
    // Copia sempre: il framebuffer si rilascia prima della Decode successiva.
    st->shadow.resize(bytes);
    std::memcpy(st->shadow.data(), src, bytes);
    const uint8_t* picture = st->shadow.data();
    fbRelease(&st->fbOwn, outSlot);
    if (st->decodeLogs == 0) {
        XC_LOGI("hwdec: prima immagine %ux%u pitch %u fmt 0x%x", info.frameWidth, info.frameHeight, pitchBytes,
                info.frameFormat);
        st->decodeLogs = 1;
    }
    out.data = picture;
    out.size = bytes;
    out.width = info.frameWidth;
    out.height = info.frameHeight;
    out.pitch = info.framePitch ? info.framePitch : info.frameWidth;
    out.pitchBytes = pitchBytes;
    out.format = info.frameFormat;
    out.pts = pts;
    out.error = false;
    return true;
}

}  // namespace xc::media
