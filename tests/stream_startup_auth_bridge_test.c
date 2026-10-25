/* SPDX-License-Identifier: GPL-3.0-only */
/* Execute the real device_auth API with its existing no-network host fixture. */
#define X4_AUTH_STARTUP_TEST 1
#define main existing_recent_fixture_main
#include "auth_recent_lifecycle_test.c"
#undef main
#include "../src/streaming/stream_startup.h"
#include "../src/streaming/keyframe_limiter.h"
#include <errno.h>

static void receive_fixture(void *user,int kind,const uint8_t *data,size_t bytes)
{(void)user;(void)kind;(void)data;(void)bytes;}

int main(void)
{
    X4Auth *a=x4_auth_create();CHECK(a);authorize_host(a);
    CHECK(!x4_auth_start(a,X4_AUTH_XBOX_CATALOG));wait_finished(a);
    X4StreamStartup first={0},replacement={0};int receiver=1;
    X4KeyframeLimiter limiter,replacement_limiter;
    x4_keyframe_limiter_init(&limiter);x4_keyframe_limiter_init(&replacement_limiter);
    unsigned before=fixture_join_calls;
    CHECK(!x4_auth_set_media_callback(a,receive_fixture,&receiver,NULL));
    CHECK(fixture_join_calls==before+1&&!a->running);
    CHECK(!x4_auth_set_stream_startup(a,&first));CHECK(a->stream_startup==&first);
    X4StreamRequestConfig config=x4_stream_request_config(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709);
    CHECK(x4_auth_start_session_request(a,0,&config)==X4_AUTH_E_ARGUMENT);
    CHECK(!a->xwork&&!x4_auth_busy(a)); /* Missing limiter fails before HTTP/worker. */
    CHECK(!x4_auth_set_keyframe_limiter(a,&limiter));CHECK(a->keyframe_limiter==&limiter);
    fixture_expected_limiter=&limiter;
    fixture_expected_startup=&first;fixture_expected_callback=receive_fixture;fixture_expected_user=&receiver;
    atomic_store(&fixture_block_session,1);atomic_store(&fixture_session_entered,0);
    CHECK(!x4_auth_start_session_request(a,0,&config));
    for(unsigned i=0;i<2000&&!atomic_load(&fixture_session_entered);++i)sceKernelUsleep(1000);
    CHECK(atomic_load(&fixture_session_entered));CHECK(x4_auth_busy(a));
    /* The worker observes the bound startup before entering the session. */
    CHECK(a->stream_startup==&first&&a->media_user==&receiver);
    before=fixture_join_calls;
    CHECK(x4_auth_set_stream_startup(a,&replacement)==X4_AUTH_E_BUSY);
    CHECK(x4_auth_set_keyframe_limiter(a,&replacement_limiter)==X4_AUTH_E_BUSY);
    CHECK(a->keyframe_limiter==&limiter);
    CHECK(x4_auth_set_media_callback(a,NULL,NULL,NULL)==X4_AUTH_E_BUSY);
    CHECK(a->stream_startup==&first&&a->media_callback==receive_fixture&&a->media_user==&receiver);
    CHECK(fixture_join_calls==before);
    atomic_store(&fixture_block_session,0);wait_finished(a);
    CHECK(a->running); /* finished publication does not substitute for join. */
    fixture_join_error=EIO;
    CHECK(x4_auth_set_media_callback(a,NULL,NULL,NULL)==-EIO);
    CHECK(a->running&&a->stream_startup==&first&&a->media_user==&receiver);
    CHECK(x4_auth_set_stream_startup(a,&replacement)==-EIO);
    CHECK(x4_auth_set_keyframe_limiter(a,&replacement_limiter)==-EIO);
    CHECK(a->keyframe_limiter==&limiter);
    CHECK(a->stream_startup==&first);
    fixture_join_error=0;
    CHECK(!x4_auth_set_media_callback(a,NULL,NULL,NULL));
    CHECK(!a->running&&!a->stream_startup&&!a->media_callback&&!a->media_user&&!a->media_trace&&!a->grace_inbox);
    CHECK(!a->keyframe_limiter);
    CHECK(!x4_auth_set_stream_startup(a,&replacement));CHECK(a->stream_startup==&replacement);
    CHECK(!x4_auth_set_media_callback(a,NULL,NULL,NULL));CHECK(!a->stream_startup);
    CHECK(!x4_auth_set_keyframe_limiter(a,&replacement_limiter));
    CHECK(!x4_auth_set_media_callback(a,NULL,NULL,NULL)&&!a->keyframe_limiter);
    CHECK(!x4_auth_close(a));
    printf("startup auth bridge PASS: %u checks; bound-before-session, busy refusal, joined clear and failed-join retention; no network\n",atomic_load(&checks));
    return 0;
}
