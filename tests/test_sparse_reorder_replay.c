/* SPDX-License-Identifier: GPL-3.0-only */
/* Real portable C reorder/grace/ticket helpers, recorded sanitized clocks and
 * lengths, synthetic payloads. No native worker, SRTP, H264 AU or decoder runs.
 * Known existing-stream state: empty reorder, expected relative sequence 0.
 * Ordered draining costs zero in this fixture; 1 ms polling is synthetic.
 * Changing policy would change scheduling in a real worker. */
#include "live_media.h"
#include "nack_grace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/sparse_reorder_0735_tapes.h"

static unsigned long long checks;
static unsigned cases;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define CASE(x) do { x(); ++cases; printf("PASS %s\n",#x); } while (0)
enum { CLOCK_CALLBACK, CLOCK_WORKER_PROXY, CLOCK_MISSING_DECISION };
typedef struct {
    X4LiveReorder q;
    X4NackGraceState grace;
    X4IngressTicket tickets[128];
    unsigned missing, accepted, emitted, late, full, direct, peak, skipped;
    unsigned next_output;
    bool sparse;
} Fixture;
typedef struct { const X4ReplayPacket *packet; uint32_t now; unsigned sequence, bytes, ordinal; } Arrival;

static X4IngressTicket ticket_for(unsigned sequence)
{
    X4IngressTicket t;memset(&t,0,sizeof(t));
    t.identity=UINT64_C(5)<<32|1; t.source=7;
    t.extended_sequence=UINT64_C(65536)+sequence;
    t.rtp_timestamp=100; t.authenticated_us=1;
    t.sequence.transport_epoch=5; t.sequence.sequence_epoch=1;
    return t;
}
static void payload_for(uint8_t *p,unsigned size,unsigned sequence)
{
    for(unsigned i=0;i<size;++i) p[i]=(uint8_t)((sequence*13u+i)%251u);
    p[0]=(uint8_t)sequence; p[1]=(uint8_t)(sequence>>8);
}
static void emitted(Fixture *f,const uint8_t *p,size_t size,const X4IngressTicket *ticket)
{
    CHECK(size>=2 && size<=X4_LIVE_RTP_MAX);
    unsigned sequence=(unsigned)p[0]|((unsigned)p[1]<<8);
    CHECK(sequence==f->next_output);
    uint8_t expected[X4_LIVE_RTP_MAX]; payload_for(expected,(unsigned)size,sequence);
    CHECK(memcmp(expected,p,size)==0);
    if(f->sparse) {
        X4IngressTicket expected_ticket=ticket_for(sequence);
        CHECK(memcmp(&expected_ticket,ticket,sizeof(*ticket))==0);
    }
    if(sequence<f->missing) ++f->emitted;
    ++f->next_output;
}
static void invariant(const Fixture *f)
{
    unsigned resident=0,bytes=0;
    for(unsigned i=0;i<f->q.window;++i) if(f->q.slots[i].used) {
        ++resident;bytes+=f->q.slots[i].size;
        CHECK(f->q.slots[i].size>0 && f->q.slots[i].size<=X4_LIVE_RTP_MAX);
        if(f->sparse) {
            CHECK((uint16_t)f->tickets[i].extended_sequence==f->q.slots[i].sequence);
            CHECK(f->tickets[i].source==7 && f->tickets[i].sequence.transport_epoch==5 &&
                f->tickets[i].sequence.sequence_epoch==1);
        }
    }
    CHECK(resident==f->q.buffered && resident<=128 && bytes<=128*X4_LIVE_RTP_MAX);
    if(f->sparse) CHECK(bytes==f->q.resident_bytes);
}
static void init(Fixture *f,bool sparse,unsigned missing)
{
    memset(f,0,sizeof(*f)); f->sparse=sparse; f->missing=missing;
    CHECK((sparse?x4_live_reorder_init_video(&f->q,128,32,25000,f->tickets):
        x4_live_reorder_init(&f->q,128,32,25000))==0);
    uint8_t p[2]={255,255};
    CHECK(x4_live_reorder_insert(&f->q,p,sizeof(p),65535,0)==X4_LIVE_REORDER_STORED);
    size_t size;uint32_t lost;
    CHECK(x4_live_reorder_next(&f->q,0,&size,&lost)!=NULL && lost==0);
    CHECK(f->q.started && f->q.expected==0 && f->q.buffered==0);
    x4_nack_grace_init(&f->grace,9,true);
    (void)x4_nack_grace_bind_source(&f->grace,7,1);
    X4NackGraceRequest request={9,UINT64_C(5)<<32|1,65536,
        missing==64?UINT64_MAX:(UINT64_C(1)<<missing)-1,0,7,1};
    X4NackGraceEffects effects;
    CHECK(x4_nack_grace_offer(&f->grace,&request,0,&effects)==X4_GRACE_OFFER_ADMITTED);
    CHECK(f->grace.ranges[0].deadline_us==45000);
}
static X4NackGraceDecision query(Fixture *f,unsigned extra,bool capacity,uint64_t now)
{
    unsigned span=x4_live_reorder_span(&f->q); if(extra>span) span=extra;
    if(f->sparse) return x4_nack_grace_query_video(&f->grace,f->q.expected,
        f->q.buffered,span,4096,capacity,now);
    return x4_nack_grace_query(&f->grace,f->q.expected,f->q.buffered,span,now);
}
static bool release_forces(const X4NackGraceDecision *d)
{ return d->reason==X4_GRACE_DEADLINE || d->reason==X4_GRACE_CAPACITY || d->reason==X4_GRACE_DISTANCE; }
static void abandon_if_released(Fixture *f,const X4NackGraceDecision *d)
{
    if(!release_forces(d) || x4_live_reorder_has(&f->q,f->q.expected)) return;
    uint32_t limit=x4_nack_grace_advance_limit(&f->grace,f->q.expected,
        x4_live_reorder_advance_bound(&f->q));
    uint32_t lost=x4_live_reorder_abandon_gap_limited(&f->q,limit);
    f->skipped+=lost; f->next_output+=lost;
}
static void drain(Fixture *f,uint64_t now)
{
    for(unsigned n=0;n<1024;++n) {
        X4NackGraceDecision d=query(f,0,false,now);
        bool hole=!x4_live_reorder_has(&f->q,f->q.expected);
        if(hole && d.reason==X4_GRACE_HOLD) return;
        size_t size=0;uint32_t lost=0;unsigned reason=0;X4IngressTicket ticket={0};
        uint32_t limit=x4_nack_grace_advance_limit(&f->grace,f->q.expected,
            x4_live_reorder_advance_bound(&f->q));
        const uint8_t *p=x4_live_reorder_next_ticket_limited(&f->q,now,&size,&lost,
            &reason,hole && release_forces(&d),limit,&ticket);
        f->skipped+=lost;f->next_output+=lost;
        if(!p) return;
        emitted(f,p,size,&ticket);
    }
    CHECK(false); /* A bounded fixture must never spin through a discontinuity. */
}
static int insert(Fixture *f,unsigned sequence,unsigned bytes,uint64_t now)
{
    uint8_t p[X4_LIVE_RTP_MAX];payload_for(p,bytes,sequence);
    X4IngressTicket t=ticket_for(sequence);
    int offset=(int16_t)((uint16_t)sequence-f->q.expected);
    X4NackGraceDecision d=query(f,offset>=0?(unsigned)offset:0,false,now);
    abandon_if_released(f,&d);
    int rc=f->sparse?x4_live_reorder_insert_ticket(&f->q,p,bytes,(uint16_t)sequence,now,&t):
        x4_live_reorder_insert(&f->q,p,bytes,(uint16_t)sequence,now);
    if(rc==X4_LIVE_REORDER_FULL) {
        unsigned before=f->q.buffered; uint16_t expected=f->q.expected;
        CHECK(before==128 && sequence!=expected);++f->full;
        d=query(f,offset>=0?(unsigned)offset:0,true,now);
        CHECK(d.reason==X4_GRACE_CAPACITY || f->grace.active_count==0);
        abandon_if_released(f,&d);drain(f,now);
        CHECK(f->q.buffered<before);
        /* Match the owner: this rejected packet is dropped, never retried or
         * fabricated as STORED after existing residents have been drained. */
    }
    if(rc==X4_LIVE_REORDER_STORED || rc==X4_LIVE_REORDER_DIRECT) {
        if(sequence<f->missing) ++f->accepted;
        (void)x4_nack_grace_accepted_extended(&f->grace,7,1,65536u+sequence,now);
        if(rc==X4_LIVE_REORDER_DIRECT) { ++f->direct;emitted(f,p,bytes,&t); }
    } else if(rc==X4_LIVE_REORDER_LATE) ++f->late;
    else if(rc==X4_LIVE_REORDER_FULL) CHECK(f->sparse && f->full>0);
    else CHECK(false);
    if(f->q.buffered>f->peak) f->peak=f->q.buffered;
    CHECK(f->q.buffered<=128 && f->grace.ranges[0].deadline_us==45000);
    invariant(f);
    drain(f,now);
    invariant(f);
    return rc;
}
static int compare_arrival(const void *pa,const void *pb)
{
    const Arrival *a=pa,*b=pb;
    return a->now<b->now?-1:a->now>b->now?1:a->ordinal<b->ordinal?-1:a->ordinal>b->ordinal;
}
static void replay(const char *name,const X4ReplayPacket *tape,unsigned count,
    unsigned missing,bool sparse,unsigned clock,bool reverse)
{
    CHECK(count<=256);
    Arrival arrivals[256];
    for(unsigned i=0;i<count;++i) {
        uint32_t now=clock==CLOCK_CALLBACK?tape[i].callback_us:tape[i].worker_us;
        if(clock==CLOCK_MISSING_DECISION && tape[i].missing_decision_us!=UINT32_MAX)
            now=tape[i].missing_decision_us;
        arrivals[i]=(Arrival){&tape[i],now,tape[i].sequence,tape[i].bytes,i};
    }
    qsort(arrivals,count,sizeof(*arrivals),compare_arrival);
    if(reverse) {
        unsigned next=missing;
        for(unsigned i=0;i<count;++i) if(arrivals[i].sequence<missing) {
            unsigned target=--next;arrivals[i].sequence=target;
            for(unsigned j=0;j<count;++j) if(tape[j].sequence==target) arrivals[i].bytes=tape[j].bytes;
        }
        CHECK(next==0);
    }
    Fixture f;init(&f,sparse,missing);
    uint64_t poll=0;unsigned at=0;
    while(at<count || poll<=46000) {
        if(poll<=46000 && (at==count || poll<=arrivals[at].now)) {
            drain(&f,poll);poll+=1000;
        } else {
            insert(&f,arrivals[at].sequence,arrivals[at].bytes,arrivals[at].now);++at;
        }
    }
    CHECK(f.q.buffered==0 && f.q.expected==count);
    if(!reverse) {
        CHECK(f.accepted==(sparse?missing:0));CHECK(f.emitted==(sparse?missing:0));
        CHECK(f.grace.counters.accepted_positions==(sparse?missing:0));
        CHECK(f.full==0 && f.direct==0 && f.peak<=128);
        if(sparse && clock!=CLOCK_MISSING_DECISION) CHECK(f.peak==(missing==16?125:127));
    } else {
        CHECK(f.full>0 && f.grace.counters.capacity_ranges>0);
        CHECK(f.grace.active_count==0); /* Safe capacity fallback, not model failure hidden. */
    }
    printf("REPLAY {\"case\":\"%s\",\"policy\":\"%s\",\"clock\":%u,"
        "\"synthetic_reverse\":%s,\"accepted\":%u,\"emitted\":%u,\"late\":%u,"
        "\"peak\":%u,\"full\":%u,\"direct\":%u,\"skipped\":%u}\n",
        name,sparse?"real_C_sparse":"real_C_legacy",clock,reverse?"true":"false",
        f.accepted,f.emitted,f.late,f.peak,f.full,f.direct,f.skipped);
    x4_live_reorder_free(&f.q);
}
static void recorded_normal_order(void)
{
    for(unsigned clock=0;clock<3;++clock) for(unsigned sparse=0;sparse<2;++sparse) {
        replay("R2",r2_tape,sizeof(r2_tape)/sizeof(r2_tape[0]),16,sparse!=0,clock,false);
        replay("R4",r4_tape,sizeof(r4_tape)/sizeof(r4_tape[0]),21,sparse!=0,clock,false);
    }
}
static void synthetic_reverse_exercises_real_full(void)
{
    replay("R2",r2_tape,sizeof(r2_tape)/sizeof(r2_tape[0]),16,true,CLOCK_WORKER_PROXY,true);
    replay("R4",r4_tape,sizeof(r4_tape)/sizeof(r4_tape[0]),21,true,CLOCK_WORKER_PROXY,true);
}
static void full_expected_is_direct_and_retains_owned_packets(void)
{
    Fixture f;init(&f,true,1);
    for(unsigned seq=1;seq<=128;++seq) CHECK(insert(&f,seq,100,seq)==X4_LIVE_REORDER_STORED);
    CHECK(f.q.buffered==128 && f.q.expected==0 && f.grace.active_count==1);
    CHECK(insert(&f,0,100,1000)==X4_LIVE_REORDER_DIRECT);
    CHECK(f.direct==1 && f.full==0 && f.emitted==1 && f.accepted==1);
    CHECK(f.q.buffered==0 && f.q.expected==129 && f.grace.counters.completed_ranges==1);
    x4_live_reorder_free(&f.q);
}
static void exact_deadline_uses_storage_clock_not_auth(void)
{
    const uint64_t times[]={44999,45000,45001};
    for(unsigned i=0;i<3;++i) {
        Fixture f;init(&f,true,1);CHECK(insert(&f,1,100,1000)==0);
        /* AUTH at1us in synthetic ticket cannot authorize storing at/after45ms. */
        int rc=insert(&f,0,100,times[i]);
        CHECK(rc==(i==0?X4_LIVE_REORDER_STORED:X4_LIVE_REORDER_LATE));
        CHECK(f.grace.counters.accepted_positions==(i==0?1:0));
        CHECK(f.grace.counters.deadline_ranges==(i==0?0:1));
        CHECK(f.grace.ranges[0].deadline_us==45000);
        x4_live_reorder_free(&f.q);
    }
}
int main(void)
{
    CASE(recorded_normal_order);CASE(synthetic_reverse_exercises_real_full);
    CASE(full_expected_is_direct_and_retains_owned_packets);CASE(exact_deadline_uses_storage_clock_not_auth);
    printf("PASS sparse reorder replay: %u groups, %llu checks\n",cases,checks);
    return 0;
}
