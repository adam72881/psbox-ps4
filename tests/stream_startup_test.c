/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/streaming/stream_startup.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    X4StreamStartup s;bool request=true,attempted=true;
    x4_stream_startup_init(&s,false);
    x4_stream_startup_note_alias(&s,0,1000000);
    assert(!x4_stream_startup_waiting(&s)&&!x4_stream_startup_alias_sent_us(&s));
    assert(!x4_stream_startup_hold_size(&s,2560,1440));
    assert(!x4_stream_startup_poll(&s,20000000,&request)&&!request);
    x4_stream_startup_init(&s,true);
    assert(x4_stream_startup_waiting(&s)&&!x4_stream_startup_alias_result(&s,&attempted)&&!attempted);
    assert(x4_stream_startup_hold_size(&s,2560,1440));
    assert(!x4_stream_startup_poll(&s,20000000,&request)&&!request&&!s.timed_out);
    x4_stream_startup_note_alias(&s,-3,20000000);
    assert(x4_stream_startup_alias_result(&s,&attempted)==-3&&attempted&&!x4_stream_startup_alias_sent_us(&s));
    assert(!x4_stream_startup_poll(&s,40000000,&request)&&!request);
    x4_stream_startup_note_alias(&s,0,50000000);
    assert(!x4_stream_startup_poll(&s,50000000,&request)&&request);
    assert(!x4_stream_startup_poll(&s,50299999,&request)&&!request);
    assert(!x4_stream_startup_poll(&s,50300000,&request)&&request);
    x4_stream_startup_note_alias(&s,0,51000000); /* A duplicate cannot renew the clock. */
    assert(x4_stream_startup_alias_sent_us(&s)==50000000);
    assert(x4_stream_startup_hold_size(&s,2560,1440)&&s.dropped_aus==2);
    x4_stream_startup_admit(&s,53000000);
    assert(s.admitted&&s.elapsed_available&&s.alias_to_sps_us==3000000&&!x4_stream_startup_waiting(&s));
    assert(!x4_stream_startup_hold_size(&s,2560,1440)); /* Reconfiguration gets no exception. */
    assert(!x4_stream_startup_poll(&s,90000000,&request)&&!request);
    x4_stream_startup_init(&s,true);x4_stream_startup_note_alias(&s,0,1000000);
    assert(!x4_stream_startup_poll(&s,10999999,&request));
    assert(x4_stream_startup_poll(&s,11000000,&request)==X4_STREAM_INITIAL_RESOLUTION_TIMEOUT&&!request);
    assert(s.timed_out&&!x4_stream_startup_waiting(&s));
    x4_stream_startup_admit(&s,11000001);assert(!s.admitted);
    x4_stream_startup_init(&s,true);x4_stream_startup_note_alias(&s,0,1000000);
    x4_stream_startup_cancel(&s);assert(!x4_stream_startup_waiting(&s));
    assert(!x4_stream_startup_poll(&s,20000000,&request)&&!request);
    assert(!x4_stream_startup_hold_size(&s,2560,1440));
    x4_stream_startup_init(&s,true);x4_stream_startup_admit(&s,42);
    assert(s.admitted&&!s.elapsed_available&&!s.alias_to_sps_us); /* No causal timing is invented. */
    puts("PASS per-session initial-resolution clock, 300ms retry, strict 10s expiry, no pre-alias deadline, admission and cancellation");
    return 0;
}
