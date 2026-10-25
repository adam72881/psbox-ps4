/* SPDX-License-Identifier: GPL-3.0-only */
#include "stream_profile.h"
#include "../ui/lang.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* One atomic word publishes all menu selections together. There is no
 * persistence, polling, lock or process-global mutable session description. */
static atomic_uint settings = X4_REQUEST_720P_HQ;

X4StreamRequestConfig x4_stream_request_config(X4StreamRequestProfile profile,
    X4StreamColorPreference color)
{
    if ((unsigned)profile > X4_REQUEST_PROFILE_MAX) profile = X4_REQUEST_720P60;
    if ((unsigned)color > X4_COLOR_FORCE_601) color = X4_COLOR_AUTO_709;
    bool hd = profile==X4_REQUEST_1080P_G||profile==X4_REQUEST_1080P_H||profile==X4_REQUEST_1080P_HQ||profile==X4_REQUEST_1080P_G8;
    X4StreamRequestConfig c = {.profile=profile, .color_preference=color,
        .width=hd?1920u:1280u, .height=hd?1080u:720u, .fps=60,
        .bitrate_kbps=hd?12000u:10000u,
        .os_name=profile==X4_REQUEST_720P60?"android":"windows",
        .label=profile==X4_REQUEST_720P60?"720p60":profile==X4_REQUEST_1080P_G?"1080p-G":
            profile==X4_REQUEST_1080P_H?"1080p-H":profile==X4_REQUEST_1080P_HQ?"1080p-HQ":
            profile==X4_REQUEST_1080P_G8?"1080p-G8":"720p-HQ",
        .h264_fmtp=hd?
            "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a;max-fs=8704;max-mbps=522240":
            "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e020;max-fs=5120;max-mbps=216000",
        .display_info=profile==X4_REQUEST_1080P_G||profile==X4_REQUEST_1080P_HQ||profile==X4_REQUEST_720P_HQ||profile==X4_REQUEST_1080P_G8,
        .h264_profile=hd?0x42e02au:0x42e020u, .max_fs=hd?8704u:5120u,
        .max_mbps=hd?522240u:216000u};
    return c;
}

X4StreamRequestConfig x4_stream_settings_get(void)
{
#if X4_PUBLIC
    (void)settings;
    X4StreamRequestConfig result=x4_stream_request_config(X4_REQUEST_1080P_G8,X4_COLOR_AUTO_709);
    result.cpu_variant=X4_CPU_0;
    result.decoder_depth=X4_DEPTH_AUTO;
    result.interaction_modes=X4_INTERACTION_VSYNC_OFF;
    return result;
#else
    unsigned word=atomic_load_explicit(&settings,memory_order_acquire);
    X4StreamRequestConfig result=x4_stream_request_config((X4StreamRequestProfile)(word&7u),
        (X4StreamColorPreference)((word>>3)&1u));
    result.cpu_variant=(unsigned char)((word>>4)&3u);
    result.decoder_depth=(unsigned char)((word>>6)&3u);
    result.interaction_modes=(unsigned char)((word>>8)&X4_INTERACTION_MASK);
    return result;
#endif
}
bool x4_stream_settings_set(X4StreamRequestProfile profile, X4StreamColorPreference color)
{
    if ((unsigned)profile>X4_REQUEST_PROFILE_MAX || (unsigned)color>X4_COLOR_FORCE_601) return false;
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire), desired;
    do { desired=(old&0xff0u)|(unsigned)profile|((unsigned)color<<3); }
    while(!atomic_compare_exchange_weak_explicit(&settings,&old,desired,memory_order_release,memory_order_acquire));
    return true;
}
bool x4_stream_settings_set_cpu(X4StreamRequestProfile profile,X4StreamColorPreference color,X4StreamCpuVariant cpu)
{
    if((unsigned)profile>X4_REQUEST_PROFILE_MAX||(unsigned)color>X4_COLOR_FORCE_601||(unsigned)cpu>X4_CPU_3)return false;
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire), desired;
    do { desired=(old&0xfc0u)|(unsigned)profile|((unsigned)color<<3)|((unsigned)cpu<<4); }
    while(!atomic_compare_exchange_weak_explicit(&settings,&old,desired,memory_order_release,memory_order_acquire));
    return true;
}
bool x4_stream_settings_set_all(X4StreamRequestProfile profile,X4StreamColorPreference color,
    X4StreamCpuVariant cpu,X4StreamDecoderDepth depth)
{
    if((unsigned)profile>X4_REQUEST_PROFILE_MAX||(unsigned)color>X4_COLOR_FORCE_601||
        (unsigned)cpu>X4_CPU_3||(unsigned)depth>X4_DEPTH_2)return false;
    unsigned old=atomic_load_explicit(&settings,memory_order_acquire), desired;
    do { desired=(old&0xf00u)|(unsigned)profile|((unsigned)color<<3)|((unsigned)cpu<<4)|
        ((unsigned)depth<<6); }
    while(!atomic_compare_exchange_weak_explicit(&settings,&old,desired,memory_order_release,memory_order_acquire));
    return true;
}
bool x4_stream_settings_set_extended(X4StreamRequestProfile profile,X4StreamColorPreference color,
    X4StreamCpuVariant cpu,X4StreamDecoderDepth depth,unsigned interaction_modes)
{
    if((unsigned)profile>X4_REQUEST_PROFILE_MAX||(unsigned)color>X4_COLOR_FORCE_601||
        (unsigned)cpu>X4_CPU_3||(unsigned)depth>X4_DEPTH_2||interaction_modes>X4_INTERACTION_MASK)return false;
    atomic_store_explicit(&settings,(unsigned)profile|((unsigned)color<<3)|((unsigned)cpu<<4)|
        ((unsigned)depth<<6)|(interaction_modes<<8),memory_order_release);
    return true;
}
X4StreamRequestConfig x4_stream_request_capture(const X4StreamRequestConfig *request)
{
    if(!request)return x4_stream_settings_get();
    X4StreamRequestConfig c=x4_stream_request_config(request->profile,request->color_preference);
    c.cpu_variant=request->cpu_variant<=X4_CPU_3?request->cpu_variant:X4_CPU_0;
    c.decoder_depth=request->decoder_depth<=X4_DEPTH_2?request->decoder_depth:X4_DEPTH_AUTO;
    c.interaction_modes=(unsigned char)(request->interaction_modes&X4_INTERACTION_MASK);
    return c;
}
unsigned x4_stream_request_resolve_depth(const X4StreamRequestConfig *request,unsigned width,unsigned height)
{
    X4StreamRequestConfig c=x4_stream_request_capture(request);
    if(c.decoder_depth!=X4_DEPTH_AUTO)return c.decoder_depth;
    if(!width||!height){width=c.width;height=c.height;}
    return width>1280u||height>720u?2u:1u;
}

static X4StreamRequestConfig normalized(const X4StreamRequestConfig *c)
{
    return x4_stream_request_capture(c);
}
const char *x4_stream_request_resolution_alias(const X4StreamRequestConfig *request)
{
    X4StreamRequestConfig c=normalized(request);
    return c.profile==X4_REQUEST_720P60?NULL:c.profile==X4_REQUEST_720P_HQ?"720HQ":
        c.profile==X4_REQUEST_1080P_HQ?"1080HQ":"1080";
}
static bool fits(int n, char *out, size_t capacity)
{
    if (n>0 && (size_t)n<capacity) return true;
    if (out && capacity) out[0]=0;
    return false;
}
bool x4_stream_request_device_info(const X4StreamRequestConfig *request, char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    X4StreamRequestConfig c=normalized(request);
    if (c.profile==X4_REQUEST_720P60)
        return fits(snprintf(out,capacity,"%s",X4_STREAM_NATIVE_720_DEVICE_INFO),out,capacity);
    if (c.profile==X4_REQUEST_1080P_H)
        return fits(snprintf(out,capacity,
            "{\"appInfo\":{\"env\":{\"clientAppId\":\"XCloud4\",\"clientAppType\":\"native\","
            "\"clientAppVersion\":\"0.7.33\",\"httpEnvironment\":\"prod\"}},\"dev\":{\"hw\":{\"make\":\"Sony\","
            "\"model\":\"PS4\"},\"os\":{\"name\":\"windows\",\"platform\":\"console\"}}}"),out,capacity);
    /* Public wire structure: Better xCloud (MIT), pinned at
     * f8397043f6d2148d2345d508902a38c69cf1ee20, network.ts:316-337.
     * clientAppId is window.location.host there; this experiment uses
     * www.xbox.com. G/HQ and 720p-HQ keep Windows/4096x2160. No TS
     * interception implementation is copied. See upstream license evidence. */
    unsigned display_width=4096u, display_height=2160u;
    return fits(snprintf(out,capacity,
        "{\"appInfo\":{\"env\":{\"clientAppId\":\"www.xbox.com\",\"clientAppType\":\"browser\","
        "\"clientAppVersion\":\"26.1.97\",\"clientSdkVersion\":\"10.3.7\","
        "\"httpEnvironment\":\"prod\",\"sdkInstallId\":\"\"}},\"dev\":{\"os\":{\"name\":\"%s\","
        "\"ver\":\"22631.2715\",\"platform\":\"desktop\"},\"hw\":{\"make\":\"Microsoft\","
        "\"model\":\"unknown\",\"sdktype\":\"web\"},\"browser\":{\"browserName\":\"chrome\","
        "\"browserVersion\":\"140.0.3485.54\"},\"displayInfo\":{\"dimensions\":{\"widthInPixels\":%u,"
        "\"heightInPixels\":%u},\"pixelDensity\":{\"dpiX\":1,\"dpiY\":1}}}}",
        c.os_name,display_width,display_height),out,capacity);
}
bool x4_stream_request_play_body(const X4StreamRequestConfig *request, const char *id,
    char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    out[0]=0;
    size_t length=0;
    if (!id) return false;
    for (;length<=128 && id[length];++length) {
        unsigned char v=(unsigned char)id[length];
        if (!((v>='A'&&v<='Z')||(v>='a'&&v<='z')||(v>='0'&&v<='9')||v=='-'||v=='_'||v=='.')) return false;
    }
    if (!length || length>128) return false;
    X4StreamRequestConfig c=normalized(request);
    return fits(snprintf(out,capacity,
        "{\"clientSessionId\":\"\",\"titleId\":\"%s\",\"systemUpdateGroup\":\"\",\"settings\":{"
        "\"nanoVersion\":\"V3;WebrtcTransport.dll\",\"enableOptionalDataCollection\":false,"
        "\"enableTextToSpeech\":false,\"highContrast\":0,\"locale\":\"%s\",\"useIceConnection\":false,"
        "\"timezoneOffsetMinutes\":-360,\"sdkType\":\"web\",\"osName\":\"%s\"},"
        "\"serverId\":\"\",\"fallbackRegionNames\":[]}",id,x4_lang_locale(),c.os_name),out,capacity);
}
bool x4_stream_request_capabilities(const X4StreamRequestConfig *request, char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    X4StreamRequestConfig c=normalized(request);
    if (c.profile!=X4_REQUEST_720P60)
        return fits(snprintf(out,capacity,"{}"),out,capacity);
    return fits(snprintf(out,capacity,
        "{\"supportsCustomResolution\":true,\"supportsHevc\":false,\"supportsHdr\":false,\"supportsFps\":%u,"
        "\"maxWidth\":%u,\"maxHeight\":%u,\"maxBitrateKbps\":%u,\"video\":{\"width\":%u,\"height\":%u,"
        "\"maxWidth\":%u,\"maxHeight\":%u,\"maxBitrateKbps\":%u}}",
        c.fps,c.width,c.height,c.bitrate_kbps,c.width,c.height,c.width,c.height,c.bitrate_kbps),out,capacity);
}
bool x4_stream_request_dimensions(const X4StreamRequestConfig *request, char *out, size_t capacity)
{
    if (!out || !capacity) return false;
    X4StreamRequestConfig c=normalized(request);
    return fits(snprintf(out,capacity,
        "{\"horizontal\":%u,\"vertical\":%u,\"preferredWidth\":%u,\"preferredHeight\":%u,"
        "\"safeAreaLeft\":0,\"safeAreaTop\":0,\"safeAreaRight\":%u,\"safeAreaBottom\":%u,"
        "\"supportsCustomResolution\":true}",c.width,c.height,c.width,c.height,c.width,c.height),out,capacity);
}
