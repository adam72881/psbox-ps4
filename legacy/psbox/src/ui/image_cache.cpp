// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RafaelNGP
#include "ui/image_cache.h"

#include "net/http.h"
#include "util/log.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 2048
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include "stb_image_resize2.h"

#include <atomic>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <exception>

namespace xc::ui {

bool decodeImage(const std::string& bytes, Image& out) {
    if (bytes.size() < 8 || bytes.size() > (12u << 20)) {
        XC_LOGW("image: scarto %zu byte", bytes.size());
        return false;
    }
    int w = 0, h = 0, n = 0;
    unsigned char* data = nullptr;
    try {
        data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                     static_cast<int>(bytes.size()), &w, &h, &n, 4);
    } catch (const std::exception& e) {
        XC_LOGE("image: decode eccezione %s", e.what());
        return false;
    } catch (...) {
        XC_LOGE("image: decode eccezione");
        return false;
    }
    if (!data) return false;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        XC_LOGW("image: scarto %dx%d", w, h);
        stbi_image_free(data);
        return false;
    }
    try {
        out.w = w;
        out.h = h;
        out.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
        std::memcpy(out.px.data(), data, out.px.size() * 4);
    } catch (const std::exception& e) {
        XC_LOGE("image: memoria %dx%d %s", w, h, e.what());
        stbi_image_free(data);
        return false;
    }
    stbi_image_free(data);
    return true;
}

Image coverResize(const Image& src, int w, int h) {
    Image out;
    if (src.w <= 0 || src.h <= 0 || w <= 0 || h <= 0 || w > 2048 || h > 2048) return out;
    if (src.px.size() < static_cast<size_t>(src.w) * static_cast<size_t>(src.h)) return out;
    try {
    out.w = w;
    out.h = h;
    out.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    // Crop the source to the target aspect ratio, centred.
    double scale = std::max(static_cast<double>(w) / src.w, static_cast<double>(h) / src.h);
    int cw = std::min(src.w, static_cast<int>(w / scale + 0.5));
    int ch = std::min(src.h, static_cast<int>(h / scale + 0.5));
    int cx = (src.w - cw) / 2, cy = (src.h - ch) / 2;
    const auto* start = reinterpret_cast<const unsigned char*>(&src.px[static_cast<size_t>(cy) * src.w + cx]);
        if (cw < 1 || ch < 1) return Image{};
        stbir_resize_uint8_srgb(start, cw, ch, src.w * 4, reinterpret_cast<unsigned char*>(out.px.data()), w, h, w * 4,
                                STBIR_RGBA);
    } catch (const std::exception& e) {
        XC_LOGE("image: resize eccezione %s", e.what());
        return Image{};
    } catch (...) {
        XC_LOGE("image: resize eccezione");
        return Image{};
    }
    return out;
}

namespace {

// store-images.s-microsoft.com resizes on request: fetch close to the size
// we draw instead of the multi-megapixel original.
std::string sizedUrl(std::string url, int w, int h) {
    if (url.rfind("//", 0) == 0) url = "https:" + url;
    if (url.find("store-images.s-microsoft.com") != std::string::npos) {
        url += url.find('?') == std::string::npos ? "?" : "&";
        url += "w=" + std::to_string(w) + "&h=" + std::to_string(h) + "&mode=scale&q=90&format=jpg";
    }
    return url;
}

}  // namespace

ImageCache::ImageCache(std::function<void()> onReady, size_t budgetBytes, std::string diskDir,
                       size_t diskBudgetBytes)
    : onReady_(std::move(onReady)), budget_(budgetBytes), diskDir_(std::move(diskDir)), diskBudget_(diskBudgetBytes) {
    if (!diskDir_.empty()) {
        ::mkdir(diskDir_.c_str(), 0777);  // like the data folder: removable over FTP too
        ::chmod(diskDir_.c_str(), 0777);  // one made 0755 by an older version
        trimDisk();
    }
    threads_.resize(3);
    for (auto& t : threads_) platform::startThread(t, [this] { loop(); }, 8u << 20);
}

void ImageCache::shutdownThreads() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

ImageCache::~ImageCache() { shutdownThreads(); }

std::shared_ptr<const Image> ImageCache::get(const std::string& url, int w, int h) {
    if (url.empty() || w <= 0 || h <= 0) return nullptr;
    std::string key = url + "#" + std::to_string(w) + "x" + std::to_string(h);
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = entries_[key];
    e.lastUse = ++clock_;
    if (!e.image && !e.loading && (!e.failed || (e.retryAt && platform::nowMs() >= e.retryAt))) {
        e.loading = true;
        jobs_.push_front({key, url, w, h});
        cv_.notify_one();
    }
    return e.image;
}

void ImageCache::evict() {
    // Caller holds mutex_.
    while (bytes_ > budget_) {
        auto victim = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.image && (victim == entries_.end() || it->second.lastUse < victim->second.lastUse))
                victim = it;
        if (victim == entries_.end()) break;
        bytes_ -= victim->second.image->bytes();
        entries_.erase(victim);
    }
}

std::string ImageCache::diskPath(const std::string& url) const {
    uint64_t h = 1469598103934665603ull;  // FNV-1a of the sized URL
    for (unsigned char ch : url) h = (h ^ ch) * 1099511628211ull;
    char name[24];
    std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(h));
    return diskDir_ + "/" + name;
}

void ImageCache::trimDisk() {
    struct File {
        std::string path;
        off_t size;
        time_t mtime;
    };
    std::vector<File> files;
    size_t total = 0;
    if (DIR* d = ::opendir(diskDir_.c_str())) {
        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string path = diskDir_ + "/" + e->d_name;
            struct stat st {};
            if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            files.push_back({path, st.st_size, st.st_mtime});
            total += static_cast<size_t>(st.st_size);
        }
        ::closedir(d);
    }
    if (total <= diskBudget_) {
        XC_LOGI("image cache: %zu files, %zu MB on disk", files.size(), total >> 20);
        return;
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.mtime < b.mtime; });
    size_t removed = 0;
    for (const auto& f : files) {
        if (total <= diskBudget_ * 3 / 4) break;  // some headroom
        if (::unlink(f.path.c_str()) == 0) {
            total -= static_cast<size_t>(f.size);
            ++removed;
        }
    }
    XC_LOGI("image cache: trimmed %zu files, %zu MB left", removed, total >> 20);
}

void ImageCache::loop() {
    static std::atomic<int> logged{0};
    XC_LOGI("image: thread stack 8 MiB");
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
        net::Request req;
        req.url = sizedUrl(job.url, job.w, job.h);
        req.headers = {{"Accept", "image/*"}};
        if (logged.fetch_add(1) < 4)
            XC_LOGI("image: lavoro %dx%d %s", job.w, job.h, req.url.c_str());
        Image decoded;
        std::shared_ptr<Image> result;
        std::string cached;
        std::string path = diskDir_.empty() ? std::string() : diskPath(req.url);
        net::Response r;
        if (!path.empty() && platform::readFile(path, cached) && decodeImage(cached, decoded)) {
            r.status = 200;  // from disk
        } else {
            r = net::perform(req);
            if (r.ok() && !path.empty() && decodeImage(r.body, decoded)) {
                static std::atomic<bool> warned{false};  // once: a full disk would say it for every image
                if (!platform::writeFileAtomic(path, r.body) && !warned.exchange(true))
                    XC_LOGW("image cache: could not save to %s", diskDir_.c_str());
            } else if (r.ok()) {
                decodeImage(r.body, decoded);
            }
        }
        if (r.ok() && decoded.w > 0) {
            Image fitted = decoded.w == job.w && decoded.h == job.h ? std::move(decoded)
                                                                    : coverResize(decoded, job.w, job.h);
            if (fitted.w > 0 && fitted.px.size() >= static_cast<size_t>(fitted.w) * static_cast<size_t>(fitted.h))
                result = std::make_shared<Image>(std::move(fitted));
        } else {
            XC_LOGW("image %s: %s", req.url.c_str(), r.status ? ("HTTP " + std::to_string(r.status)).c_str()
                                                              : r.error.c_str());
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Entry& e = entries_[job.key];
            e.loading = false;
            if (result) {
                e.image = result;
                bytes_ += result->bytes();
                evict();
            } else {
                e.failed = true;
                bool passing = r.status == 0 || r.status >= 500;
                e.retryAt = passing && ++e.failures <= 3 ? platform::nowMs() + 30000u * e.failures : 0;
            }
        }
        if (result && onReady_) onReady_();
        } catch (const std::exception& e) {
            XC_LOGE("image: lavoro eccezione %s", e.what());
            std::lock_guard<std::mutex> lock(mutex_);
            Entry& ent = entries_[job.key];
            ent.loading = false;
            ent.failed = true;
        } catch (...) {
            XC_LOGE("image: lavoro eccezione");
            std::lock_guard<std::mutex> lock(mutex_);
            Entry& ent = entries_[job.key];
            ent.loading = false;
            ent.failed = true;
        }
    }
}

}  // namespace xc::ui
