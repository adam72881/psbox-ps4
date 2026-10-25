/* SPDX-License-Identifier: GPL-3.0-only */
/* Real RTC dispatcher/startup code, native channel/entropy doubles only. */
#define main existing_rtc_startup_fixture_main
#include "rtc_resolution_startup_test.c"
#undef main

#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
static void wire_case(unsigned profile)
{
    X4Rtc *rtc=fixture(profile);
    unsigned before=sent;
    CHECK(x4_rtc_request_keyframe(rtc)==RTC_ERR_NOT_AVAIL);
    CHECK(!native_keyframes&&sent==before); /* No reservation, no channel I/O. */
    CHECK(x4_rtc_set_keyframe_limiter(rtc,NULL)==RTC_ERR_NOT_AVAIL);
    if(expected_alias(profile)){
        x4_keyframe_limiter_request(&bound_limiter,X4_KEYFRAME_STARTUP);
        CHECK(!native_keyframes); /* A pending intention does not send. */
    }
    rtc->handshake_ack=true;start_channels(rtc);
    CHECK(x4_rtc_keyframe_startup_ready(rtc));before=sent;
    CHECK(x4_rtc_request_keyframe(rtc)==RTC_ERR_NOT_AVAIL&&sent==before&&!native_keyframes);
    failed_fragment="videoKeyframeRequested";failure_return=-81;native_keyframe_result=-87;
    for(uint64_t i=0;i<10;++i){
        x4_keyframe_limiter_observe_gap(&bound_limiter);
        x4_keyframe_limiter_request(&bound_limiter,X4_KEYFRAME_ASSEMBLY|X4_KEYFRAME_PIPELINE);
        CHECK(x4_keyframe_limiter_take(&bound_limiter,1000000+i*1000,NULL));
        CHECK(x4_rtc_request_keyframe(rtc)==-87); /* Preserve native PLI return. */
        CHECK(x4_rtc_request_keyframe(rtc)==RTC_ERR_NOT_AVAIL); /* Cannot reuse grant. */
    }
    X4KeyframeLimiterSnapshot s;x4_keyframe_limiter_snapshot(&bound_limiter,&s);
    CHECK(s.granted==10&&s.transport_decisions==10);
    CHECK(s.control_attempts==10&&s.control_errors==10&&!s.control_local_successes);
    CHECK(s.pli_attempts==10&&s.pli_errors==10&&!s.pli_local_successes);
    CHECK(native_keyframes==10&&sent==before+10);
    for(unsigned i=before;i<sent;++i)
        CHECK(!strcmp(events[i].text,"{\"message\":\"videoKeyframeRequested\",\"ifrRequested\":true}"));
    x4_keyframe_limiter_observe_idr(&bound_limiter);
    x4_keyframe_limiter_request(&bound_limiter,X4_KEYFRAME_AU_QUEUE);
    CHECK(!x4_keyframe_limiter_take(&bound_limiter,5999999,NULL));
    CHECK(x4_rtc_request_keyframe(rtc)==RTC_ERR_NOT_AVAIL&&native_keyframes==10);
    CHECK(x4_keyframe_limiter_take(&bound_limiter,6000000,NULL));
    open_control=false;native_keyframe_result=0;
    CHECK(x4_rtc_request_keyframe(rtc)==0&&native_keyframes==11&&sent==before+10);
    x4_keyframe_limiter_snapshot(&bound_limiter,&s);
    CHECK(s.granted==11&&s.transport_decisions==11&&s.control_attempts==10);
    CHECK(s.pli_attempts==11&&s.pli_local_successes==1&&s.pli_errors==10);
    CHECK(!s.pending_reasons&&rtc->keyframe_grant_consumed==11);
    destroy(rtc);
}

int main(void)
{
    for(unsigned profile=0;profile<X4_REQUEST_PROFILE_COUNT;++profile)wire_case(profile);
    printf("PASS real RTC/limiter wiring: %u checks, five profiles, startup-before-grant, hard cap, failed calls retained, no grant reuse; no network.\n",atomic_load(&checks));
    return 0;
}
#else
int main(void){return existing_rtc_startup_fixture_main();}
#endif
