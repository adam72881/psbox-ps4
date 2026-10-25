/* SPDX-License-Identifier: GPL-3.0-only */
#include "stream_diagnostics.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t bins[X4_STREAM_METRIC_BINS], count, sum, maximum, overflow;
} Stage;
typedef struct { uint64_t ordinal, begin_us, end_us; unsigned evidence, classification; } Pause;
struct X4StreamDiagnostics {
    uint64_t started, first_present, last_present, presentations;
    uint64_t intervals_over_100ms, longest_interval;
    Stage stage[X4_STREAM_STAGE_COUNT];
    atomic_uint_fast64_t first_receive, last_receive, received_bytes, received_packets;
    atomic_uint_fast64_t seconds[X4_STREAM_BITRATE_SECONDS];
    atomic_uint_fast64_t bitrate_omitted_packets, bitrate_omitted_bytes;
    uint16_t au_seconds[X4_STREAM_BITRATE_SECONDS],present_seconds[X4_STREAM_BITRATE_SECONDS];
    uint16_t idr_seconds[X4_STREAM_BITRATE_SECONDS];
    uint64_t au_count,au_omitted,present_bucket_omitted,idr_bucket_omitted;
    /* Auth-grant observer producer, assembly consumer. No allocation/wait. */
    uint64_t requests[X4_STREAM_KEYFRAME_REQUESTS];
    atomic_uint_fast64_t request_written,request_read,request_total,request_omitted;
    uint32_t idr_latency_bins[X4_STREAM_IDR_LATENCY_BINS];
    uint64_t idr_latency_count,idr_latency_max,idr_latency_overflow,observed_idrs,last_idr_us,requests_ambiguous;
    atomic_uint interval_evidence;
    uint64_t pause_counts[6],pause_omitted;
    Pause pauses[X4_STREAM_PAUSE_RECORDS];
    /* Reserved once; export is single-owner after recording quiesces. */
    uint64_t sort[X4_STREAM_BITRATE_SECONDS];
};
static const char *const names[X4_STREAM_STAGE_COUNT] = {
    "decode", "conversion", "stripe0", "stripe1", "decode_end_to_flip_observation",
    "conversion_end_to_submit", "flip_call", "submit_return_to_flip_observation",
    "new_presentation_interval", "decode_IDR", "decode_non_IDR",
    "flip_end_to_conversion_begin", "controller_read_interval",
    "decode_with_output", "decode_without_output", "native_output_interval"
};
X4StreamDiagnostics *x4_stream_diagnostics_create(uint64_t now)
{
    X4StreamDiagnostics *d = calloc(1, sizeof(*d)); if (!d) return NULL;
    d->started = now; atomic_init(&d->first_receive, 0); atomic_init(&d->last_receive, 0);
    atomic_init(&d->received_bytes, 0); atomic_init(&d->received_packets, 0);
    atomic_init(&d->bitrate_omitted_packets, 0); atomic_init(&d->bitrate_omitted_bytes, 0);
    atomic_init(&d->request_written,0);atomic_init(&d->request_read,0);
    atomic_init(&d->request_total,0);atomic_init(&d->request_omitted,0);
    atomic_init(&d->interval_evidence,0);
    for (unsigned i=0;i<X4_STREAM_BITRATE_SECONDS;++i) atomic_init(&d->seconds[i],0);
    return d;
}
void x4_stream_diagnostics_free(X4StreamDiagnostics *d) { free(d); }
size_t x4_stream_diagnostics_storage_bytes(void) { return sizeof(X4StreamDiagnostics); }
void x4_stream_diagnostics_record(X4StreamDiagnostics *d, X4StreamStage id, uint64_t us)
{
    if (!d || (unsigned)id>=X4_STREAM_STAGE_COUNT) return;
    Stage *s=&d->stage[id]; uint64_t b=us/X4_STREAM_METRIC_QUANTUM_US;
    if (b>=X4_STREAM_METRIC_BINS-1) {b=X4_STREAM_METRIC_BINS-1;++s->overflow;}
    ++s->bins[b]; ++s->count; s->sum+=us; if(us>s->maximum)s->maximum=us;
}
void x4_stream_diagnostics_received(X4StreamDiagnostics *d,size_t bytes,uint64_t now)
{
    if(!d || !bytes || now<d->started) return;
    uint64_t first=0;
    (void)atomic_compare_exchange_strong_explicit(&d->first_receive,&first,now,memory_order_relaxed,memory_order_relaxed);
    uint64_t last=atomic_load_explicit(&d->last_receive,memory_order_relaxed);
    /* No retry loop in the callback. A contended update may understate the last
     * timestamp; bitrate uses fixed bucket boundaries rather than this field. */
    if(now>last)(void)atomic_compare_exchange_strong_explicit(&d->last_receive,&last,now,memory_order_relaxed,memory_order_relaxed);
    atomic_fetch_add_explicit(&d->received_bytes,bytes,memory_order_relaxed);
    atomic_fetch_add_explicit(&d->received_packets,1,memory_order_relaxed);
    uint64_t second=(now-d->started)/1000000;
    if(second<X4_STREAM_BITRATE_SECONDS)atomic_fetch_add_explicit(&d->seconds[second],bytes,memory_order_relaxed);
    else {atomic_fetch_add_explicit(&d->bitrate_omitted_packets,1,memory_order_relaxed);atomic_fetch_add_explicit(&d->bitrate_omitted_bytes,bytes,memory_order_relaxed);}
}
void x4_stream_diagnostics_evidence(X4StreamDiagnostics *d,unsigned flags)
{if(d)atomic_fetch_or_explicit(&d->interval_evidence,flags&15u,memory_order_relaxed);}
void x4_stream_diagnostics_keyframe_request(X4StreamDiagnostics *d,uint64_t now)
{
    if(!d)return;
    atomic_fetch_add_explicit(&d->request_total,1,memory_order_relaxed);
    uint64_t sent=atomic_load_explicit(&d->request_written,memory_order_relaxed);
    uint64_t read=atomic_load_explicit(&d->request_read,memory_order_acquire);
    if(now<d->started||sent==UINT64_MAX||sent-read>=X4_STREAM_KEYFRAME_REQUESTS){
        atomic_fetch_add_explicit(&d->request_omitted,1,memory_order_relaxed);return;}
    d->requests[sent%X4_STREAM_KEYFRAME_REQUESTS]=now;
    atomic_store_explicit(&d->request_written,sent+1,memory_order_release);
}
void x4_stream_diagnostics_au_received(X4StreamDiagnostics *d,bool idr,uint64_t now)
{
    if(!d)return;
    ++d->au_count;
    uint64_t second=now>=d->started?(now-d->started)/1000000:UINT64_MAX;
    if(second<X4_STREAM_BITRATE_SECONDS&&d->au_seconds[second]!=UINT16_MAX)++d->au_seconds[second];
    else ++d->au_omitted;
    if(!idr)return;
    ++d->observed_idrs;
    if(second<X4_STREAM_BITRATE_SECONDS&&d->idr_seconds[second]!=UINT16_MAX)++d->idr_seconds[second];
    else ++d->idr_bucket_omitted;
    uint64_t read=atomic_load_explicit(&d->request_read,memory_order_relaxed);
    uint64_t sent=atomic_load_explicit(&d->request_written,memory_order_acquire);
    while(read<sent){
        uint64_t request=d->requests[read%X4_STREAM_KEYFRAME_REQUESTS];if(request>now)break;
        /* Producer publication can race the preceding complete IDR. Such a
         * late record cannot be attributed to this next IDR without inventing
         * an interval. Retire it explicitly as ambiguous instead. */
        if(d->last_idr_us&&request<=d->last_idr_us){++d->requests_ambiguous;++read;continue;}
        uint64_t elapsed=now-request,bin=elapsed/1000;
        ++d->idr_latency_count;if(elapsed>d->idr_latency_max)d->idr_latency_max=elapsed;
        if(bin>=X4_STREAM_IDR_LATENCY_BINS-1){bin=X4_STREAM_IDR_LATENCY_BINS-1;++d->idr_latency_overflow;}
        if(d->idr_latency_bins[bin]!=UINT32_MAX)++d->idr_latency_bins[bin];
        ++read;
    }
    d->last_idr_us=now;
    atomic_store_explicit(&d->request_read,read,memory_order_release);
}
static unsigned pause_class(unsigned flags)
{
    if(!flags)return 0;
    if(flags&(flags-1))return 5;
    return flags==1?1:flags==2?2:flags==4?3:4;
}
void x4_stream_diagnostics_present(X4StreamDiagnostics *d,uint64_t now)
{
    if(!d)return;
    unsigned evidence=atomic_exchange_explicit(&d->interval_evidence,0,memory_order_acq_rel);
    if(d->last_present && now>=d->last_present) {
        uint64_t gap=now-d->last_present;
        x4_stream_diagnostics_record(d,X4_STREAM_PRESENT_INTERVAL,gap);
        if(gap>100000){
            uint64_t ordinal=++d->intervals_over_100ms;
            unsigned classification=pause_class(evidence);++d->pause_counts[classification];
            if(ordinal>X4_STREAM_PAUSE_RECORDS)++d->pause_omitted;
            d->pauses[(ordinal-1)%X4_STREAM_PAUSE_RECORDS]=(Pause){ordinal,d->last_present,now,evidence,classification};
        }
        if(gap>d->longest_interval)d->longest_interval=gap;
    }
    if(!d->presentations)d->first_present=now;
    d->last_present=now; ++d->presentations;
    uint64_t second=now>=d->started?(now-d->started)/1000000:UINT64_MAX;
    if(second<X4_STREAM_BITRATE_SECONDS&&d->present_seconds[second]!=UINT16_MAX)++d->present_seconds[second];
    else ++d->present_bucket_omitted;
}
uint64_t x4_stream_diagnostics_presentations(const X4StreamDiagnostics *d){return d?d->presentations:0;}
uint64_t x4_stream_diagnostics_first_present(const X4StreamDiagnostics *d){return d?d->first_present:0;}
static bool percentile(const Stage *s,unsigned p,uint64_t *upper)
{
    if(!s->count)return false;
    uint64_t wanted=(s->count*p+99)/100, count=0;
    for(unsigned i=0;i<X4_STREAM_METRIC_BINS;++i) {
        count+=s->bins[i];if(count>=wanted) {
            if(i==X4_STREAM_METRIC_BINS-1)return false;
            *upper=(uint64_t)(i+1)*X4_STREAM_METRIC_QUANTUM_US;return true;
        }
    }
    return false;
}
static int compare_u64(const void *a,const void *b)
{ uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y); }
static const char *const pause_names[]={"unknown","rtp_gap","waiting_idr","decoder","presentation","mixed"};
static void write_cadence(FILE *f,const uint16_t *seconds,uint64_t complete,uint64_t duration,uint64_t total,uint64_t omitted)
{
    uint64_t zero=0,near30=0,near60=0,other=0;
    for(unsigned i=0;i<complete;++i){unsigned n=seconds[i];if(!n)++zero;else if(n>=27&&n<=33)++near30;else if(n>=55&&n<=65)++near60;else ++other;}
    fprintf(f,"{\"total\":%llu,\"omitted_events\":%llu,\"complete_seconds\":%llu,\"zero_seconds\":%llu,\"near_30_seconds\":%llu,\"near_60_seconds\":%llu,\"other_seconds\":%llu,\"near_30_bounds\":[27,33],\"near_60_bounds\":[55,65],\"counts_by_complete_second\":[",
        (unsigned long long)total,(unsigned long long)omitted,(unsigned long long)complete,(unsigned long long)zero,(unsigned long long)near30,(unsigned long long)near60,(unsigned long long)other);
    for(unsigned i=0;i<complete;++i)fprintf(f,"%s%u",i?",":"",seconds[i]);
    uint64_t trailing=duration/1000000;
    fprintf(f,"],\"trailing_partial_duration_us\":%llu,\"trailing_partial_count\":",(unsigned long long)(duration%1000000));
    if(trailing<X4_STREAM_BITRATE_SECONDS)fprintf(f,"%u",seconds[trailing]);else fputs("null",f);
    fprintf(f,",\"seconds_beyond_capacity\":%llu}",(unsigned long long)(trailing>X4_STREAM_BITRATE_SECONDS?trailing-X4_STREAM_BITRATE_SECONDS:0));
}
static void write_new_diagnostics(FILE *f,const X4StreamDiagnostics *d,uint64_t ended,uint64_t complete)
{
    uint64_t duration=ended-d->started;
    fputs(",\"cadence\":{\"bucket_origin_us\":",f);fprintf(f,"%llu",(unsigned long long)d->started);
    fputs(",\"scope\":\"complete one-second buckets from pipeline creation; initialization/loading zeros included; trailing partial excluded from histogram; no claim about game-generated cadence\",\"received_complete_AU\":",f);
    write_cadence(f,d->au_seconds,complete,duration,d->au_count,d->au_omitted);
    fputs(",\"presented\":",f);write_cadence(f,d->present_seconds,complete,duration,d->presentations,d->present_bucket_omitted);fputc('}',f);
    /* Complete IDRs at the assembly frontier, including deliberately filtered
     * work. Native validated IDRs are a separate recovery observation. */
    fputs(",\"received_idr\":",f);
    write_cadence(f,d->idr_seconds,complete,duration,d->observed_idrs,d->idr_bucket_omitted);
    uint64_t idrs=0;for(unsigned i=0;i<complete;++i)idrs+=d->idr_seconds[i];
    fprintf(f,",\"idr_per_minute\":{\"complete_seconds\":%llu,\"count\":%llu,\"rate\":%.6f,\"scope\":\"complete structurally assembled IDR AUs per complete one-second bucket; pipeline creation origin, loading included; not validated Decode outputs\",\"complete_minute_counts\":[",
        (unsigned long long)complete,(unsigned long long)idrs,complete?(double)idrs*60./(double)complete:0.);
    for(unsigned minute=0;minute<complete/60;++minute){uint64_t n=0;for(unsigned i=0;i<60;++i)n+=d->idr_seconds[minute*60+i];fprintf(f,"%s%llu",minute?",":"",(unsigned long long)n);}
    fputs("]}",f);
    uint64_t wanted=(d->idr_latency_count+1)/2,seen=0,median=0;bool finite=false;
    for(unsigned i=0;d->idr_latency_count&&i<X4_STREAM_IDR_LATENCY_BINS;++i){seen+=d->idr_latency_bins[i];if(seen>=wanted){finite=i<X4_STREAM_IDR_LATENCY_BINS-1;median=(uint64_t)(i+1)*1000;break;}}
    uint64_t written=atomic_load(&d->request_written),read=atomic_load(&d->request_read);
    fprintf(f,",\"keyframe_to_idr\":{\"scope\":\"common limiter logical grant before local transport attempts to next complete IDR assembled before filtering; multiple grants may share an IDR; no remote receipt or send causality\",\"requests\":%llu,\"n\":%llu,\"p50_upper_us\":",(unsigned long long)atomic_load(&d->request_total),(unsigned long long)d->idr_latency_count);
    if(finite)fprintf(f,"%llu",(unsigned long long)median);else fputs("null",f);
    fprintf(f,",\"maximum_us\":%llu,\"histogram_quantum_us\":1000,\"overflow_count\":%llu,\"observed_complete_idrs\":%llu,\"requests_pending\":%llu,\"requests_omitted\":%llu,\"requests_ambiguous_publication\":%llu,\"oldest_pending_age_us\":",
        (unsigned long long)d->idr_latency_max,(unsigned long long)d->idr_latency_overflow,(unsigned long long)d->observed_idrs,(unsigned long long)(written-read),(unsigned long long)atomic_load(&d->request_omitted),(unsigned long long)d->requests_ambiguous);
    if(written>read){uint64_t at=d->requests[read%X4_STREAM_KEYFRAME_REQUESTS];fprintf(f,"%llu",(unsigned long long)(ended>=at?ended-at:0));}else fputs("null",f);fputc('}',f);
    fprintf(f,",\"pause_evidence\":{\"scope\":\"temporal evidence since preceding observed flip, not exclusive causal attribution; rtp_gap means local reorder-gap abandonment or deliberate RTP omission, never proof of network loss; decoder means a Decode call over 100ms, presentation means submit-to-observation over 100ms; unknown and mixed remain explicit\",\"capacity\":%u,\"record_count\":%llu,\"omitted_records\":%llu,\"counts\":{",X4_STREAM_PAUSE_RECORDS,(unsigned long long)d->intervals_over_100ms,(unsigned long long)d->pause_omitted);
    for(unsigned i=0;i<6;++i)fprintf(f,"%s\"%s\":%llu",i?",":"",pause_names[i],(unsigned long long)d->pause_counts[i]);
    fputs("},\"records\":[",f);uint64_t first=d->intervals_over_100ms>X4_STREAM_PAUSE_RECORDS?d->intervals_over_100ms-X4_STREAM_PAUSE_RECORDS:0;
    for(uint64_t i=first;i<d->intervals_over_100ms;++i){const Pause *p=&d->pauses[i%X4_STREAM_PAUSE_RECORDS];fprintf(f,"%s{\"ordinal\":%llu,\"begin_us\":%llu,\"end_us\":%llu,\"duration_us\":%llu,\"evidence_mask\":%u,\"classification\":\"%s\"}",i==first?"":",",(unsigned long long)p->ordinal,(unsigned long long)p->begin_us,(unsigned long long)p->end_us,(unsigned long long)(p->end_us-p->begin_us),p->evidence,pause_names[p->classification]);}
    uint64_t open=d->last_present&&ended>d->last_present?ended-d->last_present:0;unsigned flags=atomic_load(&d->interval_evidence);
    fprintf(f,"],\"terminal_open_interval\":{\"duration_us\":%llu,\"over_100ms\":%s,\"evidence_mask\":%u,\"classification\":\"%s\",\"scope\":\"no following flip; may include intentional close time; excluded from closed interval counts\"}}",(unsigned long long)open,open>100000?"true":"false",flags,pause_names[pause_class(flags)]);
}
int x4_stream_diagnostics_write(FILE *f,const X4StreamDiagnostics *constant,uint64_t ended)
{
    if(!f || !constant || ended<constant->started)return -1;
    X4StreamDiagnostics *d=(X4StreamDiagnostics*)constant;
    uint64_t duration=ended-d->started, complete=duration/1000000;
    if(complete>X4_STREAM_BITRATE_SECONDS)complete=X4_STREAM_BITRATE_SECONDS;
    uint64_t sum=0;for(unsigned i=0;i<complete;++i){d->sort[i]=atomic_load(&d->seconds[i]);sum+=d->sort[i];}
    qsort(d->sort,(size_t)complete,sizeof(uint64_t),compare_u64);
    uint64_t p95=complete?d->sort[(complete*95+99)/100-1]*8:0;
    fprintf(f,"{\"storage_bytes\":%llu,\"started_us\":%llu,\"ended_us\":%llu,\"first_present_us\":%llu,\"last_present_us\":%llu,\"presentations\":%llu,",
        (unsigned long long)sizeof(*d),(unsigned long long)d->started,(unsigned long long)ended,(unsigned long long)d->first_present,(unsigned long long)d->last_present,(unsigned long long)d->presentations);
    uint64_t interval=d->presentations>1&&d->last_present>=d->first_present?d->last_present-d->first_present:0;
    fprintf(f,"\"presentation_interval_us\":%llu,\"images_per_second\":%.6f,\"rate_scope\":\"(observed presentations minus one)/(last minus first observed flip); excludes before-first/after-last time, includes game loading/transitions within; local output identity only\",\"intervals_over_100ms\":%llu,\"longest_interval_us\":%llu,",
        (unsigned long long)interval,interval?(double)(d->presentations-1)*1e6/(double)interval:0.,(unsigned long long)d->intervals_over_100ms,(unsigned long long)d->longest_interval);
    fprintf(f,"\"received_video_rtp\":{\"packets\":%llu,\"bytes\":%llu,\"bucket_origin_us\":%llu,\"complete_seconds\":%llu,\"mean_bps\":%.3f,\"p95_bps\":%llu,\"scope\":\"complete one-second callback-RTP buckets from session start, zeros included; trailing partial second excluded\",\"omitted_packets\":%llu,\"omitted_bytes\":%llu},\"stages\":{",
        (unsigned long long)atomic_load(&d->received_packets),(unsigned long long)atomic_load(&d->received_bytes),(unsigned long long)d->started,(unsigned long long)complete,complete?(double)sum*8/(double)complete:0.,(unsigned long long)p95,(unsigned long long)atomic_load(&d->bitrate_omitted_packets),(unsigned long long)atomic_load(&d->bitrate_omitted_bytes));
    for(unsigned i=0;i<X4_STREAM_STAGE_COUNT;++i) {
        const Stage *s=&d->stage[i];if(i)fputc(',',f);
        fprintf(f,"\"%s\":{\"count\":%llu,\"sum_us\":%llu,\"maximum_us\":%llu,\"histogram_quantum_us\":100,\"overflow_count\":%llu,\"percentile_scope\":\"whole session cumulative histogram; finite values are upper bounds\"",names[i],(unsigned long long)s->count,(unsigned long long)s->sum,(unsigned long long)s->maximum,(unsigned long long)s->overflow);
        fputs(",\"mean_us\":",f);if(s->count)fprintf(f,"%.6f",(double)s->sum/(double)s->count);else fputs("null",f);
        const unsigned ps[]={50,95,99};for(unsigned k=0;k<3;++k){uint64_t value=0;fprintf(f,",\"p%u_upper_us\":",ps[k]);if(percentile(s,ps[k],&value))fprintf(f,"%llu",(unsigned long long)value);else fputs("null",f);}
        fputc('}',f);
    }
    fputc('}',f);write_new_diagnostics(f,d,ended,complete);fputc('}',f);return ferror(f)?-1:0;
}
