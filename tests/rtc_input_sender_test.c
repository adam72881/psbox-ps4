/* SPDX-License-Identifier: GPL-3.0-only */
/* Real sender and wire encoder, deterministic source/clock/native channel.
 * No packets leave the process; sleeps only advance the simulated clock. */
#define _POSIX_C_SOURCE 200809L
#include "../src/streaming/rtc_transport.c"
#include <assert.h>
#include <limits.h>

enum { INPUT_CHANNEL=12, MAX_SENDS=600 };
typedef struct { uint64_t at; int rc; uint8_t bytes[38]; } Attempt;
static Attempt attempts[MAX_SENDS];
static unsigned attempted, priority_calls, source_polls, buffer_polls;
static uint64_t now_us, until_us, origin_us, blocked_until, send_cost;
static int priority_result, fail_attempt, buffer_result;
static unsigned scenario;
static X4Rtc *running;
static const uint64_t ORIGIN=1000000u;

uint64_t sceKernelGetProcessTime(void){return now_us;}
int sceKernelUsleep(unsigned us)
{
    assert(us>0);now_us+=us;
    if(now_us>=until_us)atomic_store(&running->input_stop,true);
    return 0;
}
OrbisPthread scePthreadSelf(void){return (OrbisPthread)1;}
int32_t scePthreadSetprio(OrbisPthread thread,int priority)
{assert(thread==(OrbisPthread)1 && priority==400);++priority_calls;return priority_result;}
bool rtcIsOpen(int channel){assert(channel==INPUT_CHANNEL);return true;}
int rtcGetBufferedAmount(int channel)
{assert(channel==INPUT_CHANNEL);++buffer_polls;return now_us<blocked_until?buffer_result:0;}
int rtcSendMessage(int channel,const char *data,int size)
{
    assert(channel==INPUT_CHANNEL && size==38 && attempted<MAX_SENDS);
    Attempt *a=&attempts[attempted++];a->at=now_us;
    memcpy(a->bytes,data,38);a->rc=(int)attempted==fail_attempt?-97:0;
    now_us+=send_cost;return a->rc;
}
static bool source(void *user,X4GamepadFrame *out)
{
    assert(user==running);++source_polls;
    *out=(X4GamepadFrame){.connected=true,.sampled_us=origin_us};
    uint64_t elapsed=now_us-origin_us;
    switch(scenario) {
    case 1:if(elapsed>=1001){out->buttons=X4_GAMEPAD_A;out->sampled_us=origin_us+1001;}break;
    case 2:
        if(elapsed>=1001){out->buttons=X4_GAMEPAD_A;out->sampled_us=origin_us+1001;}
        if(elapsed>=3001){out->buttons=X4_GAMEPAD_B;out->sampled_us=origin_us+3001;}break;
    case 3:
        out->buttons=elapsed>=2000?1:0;
        out->left_x=elapsed>=2000?-32767:INT16_MIN;break;
    case 4:out->left_x=(int16_t)(elapsed/2000);out->sampled_us=now_us;break;
    case 5:
        if(elapsed>=5000){out->buttons=X4_GAMEPAD_Y;out->sampled_us=origin_us+5000;}break;
    case 6:
        if(elapsed>=2001){out->buttons=X4_GAMEPAD_X;out->sampled_us=origin_us+2001;}break;
    case 7:if(elapsed>=2000){out->connected=false;out->sampled_us=0;}break;
    case 9:out->sampled_us=now_us+1;break;
    case 10:return false;
    case 11:out->sampled_us=now_us;break;
    case 8:
        out->buttons=(uint16_t)(0xff00u|((elapsed/1000u)&0xfeu));
        out->left_x=(int16_t)(elapsed%65535u-32767);out->left_y=INT16_MIN;
        out->right_x=2;out->right_y=-3;out->left_trigger=123;out->right_trigger=65432;
        out->sampled_us=now_us;break;
    default:break;
    }
    return true;
}
static uint16_t u16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static void fixture(X4Rtc *rtc,bool legacy,unsigned which,uint64_t duration)
{
    memset(rtc,0,sizeof(*rtc));memset(attempts,0,sizeof(attempts));
    now_us=origin_us=ORIGIN;until_us=origin_us+duration;scenario=which;
    attempted=priority_calls=source_polls=buffer_polls=0;
    blocked_until=0;send_cost=0;priority_result=0;fail_attempt=0;buffer_result=38;running=rtc;
    atomic_flag_clear(&rtc->data_gate);atomic_init(&rtc->state,X4_RTC_CONNECTED);
    atomic_init(&rtc->error,0);atomic_init(&rtc->input_stop,false);
    atomic_init(&rtc->input_ready,false);atomic_init(&rtc->input_metadata_sent,true);
    atomic_init(&rtc->startup_complete,true);atomic_init(&rtc->input_packets,0);
    atomic_init(&rtc->input_dropped,0);atomic_init(&rtc->input_error,0);
    rtc->channels[2]=INPUT_CHANNEL;rtc->started_at=origin_us;
    rtc->input_source=source;rtc->input_user=rtc;
    rtc->request_config=x4_stream_request_config(X4_REQUEST_1080P_G,X4_COLOR_AUTO_709);
    rtc->request_config.interaction_modes=legacy?X4_INTERACTION_INPUT_60HZ:0;
    x4_rtc_input_metrics_reset();
}

/* Byte-for-byte scheduling reference copied from this project's 0.7.47.
 * No reference implementation of the new scheduler is substituted. */
static void *input_sender_0747_reference(void *pointer)
{
    X4Rtc *rtc=pointer;
    while(!atomic_load(&rtc->input_stop)) {
        uint64_t until=sceKernelGetProcessTime()+INPUT_INTERVAL_USEC;
        lock(&rtc->data_gate);
        X4GamepadSource source=rtc->input_source;
        void *user=rtc->input_user;
        unlock(&rtc->data_gate);
        bool ready=source && atomic_load(&rtc->input_metadata_sent) &&
            atomic_load(&rtc->startup_complete) && !atomic_load(&rtc->error) &&
            atomic_load(&rtc->state)==X4_RTC_CONNECTED && rtcIsOpen(rtc->channels[2]);
        atomic_store(&rtc->input_ready,ready);
        if(ready) {
            X4GamepadFrame frame={0};
            /* Auth copies its mailbox under its own gate. No RTC gate is
             * held across this call; close joins before freeing either. */
            if(!source(user,&frame) || !frame.connected)frame=(X4GamepadFrame){0};
            if(!atomic_load(&rtc->input_stop)) {
                int buffered=rtcGetBufferedAmount(rtc->channels[2]);
                if(buffered) {
                    atomic_fetch_add(&rtc->input_dropped,1);
                    if(buffered<0)input_error(rtc,buffered);
                } else {
                    uint8_t packet[38];
                    input_gamepad(rtc,&frame,packet);
                    int rc=rtcSendMessage(rtc->channels[2],(const char *)packet,sizeof(packet));
                    if(rc<0) {atomic_fetch_add(&rtc->input_dropped,1);input_error(rtc,rc);}
                    else {atomic_fetch_add(&rtc->input_packets,1);input_error(rtc,0);}
                }
            }
        }
        /* No catch-up bursts: each iteration starts a fresh monotonic
         * deadline; interrupted sleeps cannot exceed the 60 Hz ceiling. */
        while(!atomic_load(&rtc->input_stop)) {
            uint64_t t=sceKernelGetProcessTime();
            if(t>=until)break;
            sceKernelUsleep((unsigned)(until-t));
        }
    }
    atomic_store(&rtc->input_ready,false);
    return NULL;
}

static void changed_and_latest(void)
{
    X4Rtc rtc;fixture(&rtc,false,1,12000);input_sender(&rtc);
    assert(priority_calls==1 && source_polls==6 && attempted==2);
    assert(attempts[0].at==ORIGIN && attempts[1].at==ORIGIN+4000);
    assert(attempts[1].at-(ORIGIN+1001)<=4000);
    assert(u16(attempts[1].bytes+16)==X4_GAMEPAD_A);
    assert(input_metrics.changes==2 && input_metrics.keepalives==0);
    assert(input_metrics.cap_delays==1 && input_metrics.state_age.count==2);
    fixture(&rtc,false,2,12000);input_sender(&rtc);
    assert(attempted==2 && u16(attempts[1].bytes+16)==X4_GAMEPAD_B);
    assert(input_metrics.changes==2); /* The older A change was not queued. */
}
static void mapped_normalization_and_connected(void)
{
    X4Rtc rtc;fixture(&rtc,false,3,12000);input_sender(&rtc);
    assert(attempted==1 && input_metrics.changes==1);
    assert(u16(attempts[0].bytes+18)==(uint16_t)-32767);
    fixture(&rtc,false,7,12000);input_sender(&rtc);
    assert(attempted==2 && attempts[1].at==ORIGIN+4000);
    assert(input_metrics.changes==2 && input_metrics.age_unavailable==1);
}
static void keepalive(void)
{
    X4Rtc rtc;fixture(&rtc,false,0,100000);input_sender(&rtc);
    assert(attempted==6 && input_metrics.changes==1 && input_metrics.keepalives==5);
    for(unsigned i=1;i<attempted;++i) {
        uint64_t gap=attempts[i].at-attempts[i-1].at;
        assert(gap>=16667 && gap<=18667);
    }
    /* Physical samples remain fresh every poll, but the mapped report is
     * unchanged. sampled_us must not create change-triggered sends. */
    fixture(&rtc,false,11,100000);input_sender(&rtc);
    assert(source_polls==50 && attempted==6 && input_metrics.changes==1 && input_metrics.keepalives==5);
    for(unsigned i=1;i<attempted;++i)assert(attempts[i].at-attempts[i-1].at>=16667);
}
static void backlog_retry_and_send_failure(void)
{
    X4Rtc rtc;fixture(&rtc,false,5,16000);blocked_until=ORIGIN+7000;input_sender(&rtc);
    assert(attempted==1 && attempts[0].at==ORIGIN+8000);
    assert(u16(attempts[0].bytes+16)==X4_GAMEPAD_Y);
    assert(input_metrics.busy_deferrals==4 && input_metrics.successful==1 && input_metrics.buffer_query_errors==0);
    fixture(&rtc,false,5,16000);blocked_until=ORIGIN+7000;buffer_result=-77;input_sender(&rtc);
    assert(attempted==1 && attempts[0].at==ORIGIN+8000 && u16(attempts[0].bytes+16)==X4_GAMEPAD_Y);
    assert(input_metrics.busy_deferrals==0 && input_metrics.buffer_query_errors==4 && input_metrics.successful==1);
    assert(atomic_load(&rtc.input_dropped)==4 && atomic_load(&rtc.input_error)==0);
    fixture(&rtc,false,6,16000);fail_attempt=1;input_sender(&rtc);
    assert(attempted==2 && attempts[0].rc==-97 && attempts[1].at==ORIGIN+4000);
    assert(u16(attempts[1].bytes+16)==X4_GAMEPAD_X);
    assert(input_metrics.successful==1 && input_metrics.send_errors==1);
    assert(input_metrics.changes==1 && atomic_load(&rtc.input_error)==0);
}
static void continuous_cap_and_nonfatal_priority(void)
{
    X4Rtc rtc;fixture(&rtc,false,4,1000000);priority_result=-55;send_cost=150;input_sender(&rtc);
    assert(attempted==250 && input_metrics.successful==250 && input_metrics.maximum_per_second==250);
    assert(input_metrics.priority_return==-55 && priority_calls==1 && atomic_load(&rtc.error)==0);
    for(unsigned i=1;i<attempted;++i)assert(attempts[i].at-attempts[i-1].at>=4000);
    assert(input_metrics.send_duration.count==250 && input_metrics.send_duration.maximum_us==150);
}
static void legacy_identical(void)
{
    X4Rtc rtc;Attempt old[MAX_SENDS];unsigned n;
    fixture(&rtc,true,8,120000);blocked_until=ORIGIN+17000;fail_attempt=2;send_cost=150;
    input_sender_0747_reference(&rtc);n=attempted;memcpy(old,attempts,sizeof(old));
    uint64_t dropped=atomic_load(&rtc.input_dropped),packets=atomic_load(&rtc.input_packets);
    fixture(&rtc,true,8,120000);blocked_until=ORIGIN+17000;fail_attempt=2;send_cost=150;
    input_sender(&rtc);
    assert(attempted==n && atomic_load(&rtc.input_dropped)==dropped && atomic_load(&rtc.input_packets)==packets);
    for(unsigned i=0;i<n;++i)assert(old[i].at==attempts[i].at && old[i].rc==attempts[i].rc &&
        !memcmp(old[i].bytes,attempts[i].bytes,38));
    assert(input_metrics.legacy && input_metrics.successful==packets && priority_calls==1);
}
static void metrics_scope_and_live_guard(void)
{
    X4Rtc rtc;fixture(&rtc,false,0,22000);input_sender(&rtc);
    FILE *f=tmpfile();assert(f);x4_rtc_input_metrics_write(f);rewind(f);
    char text[4096];size_t bytes=fread(text,1,sizeof(text)-1,f);text[bytes]=0;fclose(f);
    assert(strstr(text,"\"available\":true") && strstr(text,"\"mode\":\"immediate\""));
    assert(strstr(text,"\"priority_requested\":400") && strstr(text,"\"state_age\":{"));
    assert(strstr(text,"\"p50_upper_us\":") && strstr(text,"\"send_duration\":{"));
    x4_rtc_input_metrics_reset();assert(!input_metrics.available);
    assert(input_metrics_begin(false,0));input_metrics.successful=7;
    x4_rtc_input_metrics_reset();assert(input_metrics.successful==7);
    f=tmpfile();assert(f);x4_rtc_input_metrics_write(f);rewind(f);
    bytes=fread(text,1,sizeof(text)-1,f);text[bytes]=0;fclose(f);
    assert(strstr(text,"sender_not_joined"));input_metrics_end(true);
    assert(input_metrics.successful==7);
    fixture(&rtc,false,9,22000);input_sender(&rtc);
    assert(input_metrics.state_age.count==0 && input_metrics.age_unavailable==attempted);
    fixture(&rtc,false,10,22000);input_sender(&rtc);
    assert(input_metrics.state_age.count==0 && input_metrics.age_unavailable==attempted);
}
static void rumble_snapshot_only(void)
{
    X4Rtc rtc;fixture(&rtc,false,0,22000);
    X4Rumble *rumble=x4_rumble_new();assert(rumble && x4_rumble_begin(rumble));
    uint8_t report[13]={128,0,0,0,100,50,7,9,100,0,30,0,2};
    assert(x4_rumble_receive(rumble,report,sizeof(report),now_us)==X4_RUMBLE_VALID);
    assert(x4_rumble_receive(rumble,report,3,now_us)==X4_RUMBLE_MALFORMED);
    rtc.rumble=rumble;input_sender(&rtc);
    assert(input_metrics.rumble_available && input_metrics.rumble.valid==1 &&
        input_metrics.rumble.malformed==1 && input_metrics.rumble.binary_messages==2);
    FILE *f=tmpfile();assert(f);x4_rtc_input_metrics_write(f);rewind(f);
    char text[4096];size_t bytes=fread(text,1,sizeof(text)-1,f);text[bytes]=0;fclose(f);
    assert(strstr(text,"\"send_success_scope\":\"local rtcSendMessage") &&
        strstr(text,"\"rumble_available\":true") && strstr(text,"\"malformed\":1"));
    x4_rumble_disable(rumble);assert(x4_rumble_free(rumble));
}
int main(void)
{
    changed_and_latest();mapped_normalization_and_connected();keepalive();
    backlog_retry_and_send_failure();continuous_cap_and_nonfatal_priority();
    legacy_identical();metrics_scope_and_live_guard();rumble_snapshot_only();
    puts("RTC input sender: change<=4ms, keepalive, retry, normalized state, cap, legacy golden and metrics PASS");
    return 0;
}
