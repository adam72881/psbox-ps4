/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "probe_reference.h"
#if X4_VIDEO_PROBE
#include "probe_files.h"
#endif
#include "video_capacity.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#if X4_VIDEO_PROBE
typedef struct {
    FILE *file;
    uint8_t *frame;
    const atomic_bool *cancel;
    bool cancelled;
} ReferenceState;
static bool cancellation(X4ProbeReference *reference, ReferenceState *state)
{
    if (!state->cancelled && state->cancel && atomic_load_explicit(state->cancel,memory_order_relaxed)) {
        state->cancelled=true; ++reference->stats.cancel_observations;
    }
    return state->cancelled;
}
static int failure(X4ProbeReference *reference,int result) { ++reference->stats.errors;return result; }

int x4_probe_reference_close(X4ProbeReference *reference)
{
    if(!reference) return X4_PROBE_ARGUMENT;
    ReferenceState *state=reference->state;
    if(!state) return X4_PROBE_OK;
    int result=0;
    if(state->file && fclose(state->file)) result=X4_PROBE_IO;
    free(state->frame);free(state);reference->state=NULL;
    if(result) ++reference->stats.errors;
    return result;
}

int x4_probe_reference_open_verified(X4ProbeReference *reference,const char *path,
    unsigned width,unsigned height,unsigned count,const uint8_t expected[32],
    X4ProbeHashFile hash,void *context,const atomic_bool *cancel)
{
    if(!reference || !path || !expected || !hash || !width || !height ||
        width>1920 || height>1080 || !count || count>X4_PROBE_REFERENCE_MAX ||
        !strnlen(path,X4_PROBE_PATH_MAX+1) || strnlen(path,X4_PROBE_PATH_MAX+1)>X4_PROBE_PATH_MAX || strstr(path,"://"))
        return X4_PROBE_ARGUMENT;
    if(reference->state) return X4_PROBE_STATE;
    memset(reference,0,sizeof(*reference));
    size_t pixels,frame_bytes,file_bytes;
    if(x4_video_size_mul(width,height,&pixels) || x4_video_size_mul(pixels,4,&frame_bytes) ||
        x4_video_size_mul(frame_bytes,count,&file_bytes)) return failure(reference,X4_PROBE_SIZE);
    ReferenceState *state=calloc(1,sizeof(*state));
    if(!state) return failure(reference,X4_PROBE_MEMORY);
    reference->state=state;state->cancel=cancel;
    int result=X4_PROBE_IO;
    if(cancellation(reference,state)) { result=X4_PROBE_CANCELLED;goto reject; }
    #if X4_VIDEO_PROBE
    state->file=x4_probe_files_sample_open(path,NULL);
#else
    state->file=fopen(path,"rb");
#endif
    if(!state->file || setvbuf(state->file,NULL,_IONBF,0) || fseeko(state->file,0,SEEK_END)) goto reject;
    off_t actual_bytes=ftello(state->file);
    if(actual_bytes<0 || fseeko(state->file,0,SEEK_SET)) goto reject;
    if((uint64_t)actual_bytes!=file_bytes) { result=X4_PROBE_SIZE;goto reject; }
    uint8_t actual[32]={0};result=hash(state->file,file_bytes,actual,cancel,context);
    if(result==X4_PROBE_CANCELLED && !state->cancelled) {
        state->cancelled=true;++reference->stats.cancel_observations;
    }
    if(result) goto reject;
    if(cancellation(reference,state)) { result=X4_PROBE_CANCELLED;goto reject; }
    if(memcmp(actual,expected,32)) { result=X4_PROBE_HASH;goto reject; }
    if(fseeko(state->file,0,SEEK_SET)) { result=X4_PROBE_IO;goto reject; }
    state->frame=malloc(frame_bytes);
    if(!state->frame) { result=X4_PROBE_MEMORY;goto reject; }
    reference->width=width;reference->height=height;reference->count=count;reference->frame_bytes=frame_bytes;
    reference->stats.hash_verified=true;
    return 0;
reject:
    x4_probe_reference_close(reference);
    if(result!=X4_PROBE_CANCELLED) ++reference->stats.errors;
    return result;
}

int x4_probe_reference_match(X4ProbeReference *reference,const uint32_t *rgba,
    size_t pixel_capacity,unsigned width,unsigned height,X4ProbeReferenceScore *out)
{
    if(!reference || !reference->state || !rgba || !out) return X4_PROBE_ARGUMENT;
    memset(out,0,sizeof(*out));
    ReferenceState *state=reference->state;
    if(cancellation(reference,state)) return X4_PROBE_CANCELLED;
    if(width!=reference->width || height!=reference->height || pixel_capacity<(size_t)width*height)
        return failure(reference,X4_PROBE_SIZE);
    if(fseeko(state->file,0,SEEK_SET)) return failure(reference,X4_PROBE_IO);
    unsigned accepted=0;bool best=false,best_acceptable=false;
    uint64_t samples=(uint64_t)width*height*3;
    for(unsigned index=0;index<reference->count;++index) {
        size_t remaining=reference->frame_bytes,position=0;
        while(remaining) {
            if(cancellation(reference,state)) return X4_PROBE_CANCELLED;
            size_t want=remaining>X4_PROBE_READ_CHUNK?X4_PROBE_READ_CHUNK:remaining;
            if(fread(state->frame+position,1,want,state->file)!=want) return failure(reference,X4_PROBE_IO);
            position+=want;remaining-=want;
        }
        ++reference->stats.references_read;
        uint64_t sum=0,alpha=0;unsigned maximum=0;
        for(unsigned y=0;y<height;++y) {
            if(!(y&63u) && cancellation(reference,state)) return X4_PROBE_CANCELLED;
            for(unsigned x=0;x<width;++x) {
                size_t pixel=(size_t)y*width+x,offset=pixel*4;
                uint32_t value=rgba[pixel];
                if((value>>24)!=255 || state->frame[offset+3]!=255) ++alpha;
                for(unsigned channel=0;channel<3;++channel) {
                    int difference=(int)((value>>(channel*8))&255u)-state->frame[offset+channel];
                    unsigned absolute=(unsigned)(difference<0?-difference:difference);
                    sum+=absolute;if(absolute>maximum) maximum=absolute;
                }
            }
        }
        reference->stats.pixels_compared+=(uint64_t)width*height;
        bool acceptable=!alpha && sum<=X4_PROBE_REFERENCE_MAE_MAX*samples && maximum<=X4_PROBE_REFERENCE_CHANNEL_MAX;
        if(acceptable) ++accepted;
        /* Prefer a valid-alpha candidate when RGB sums tie; never treat a
         * descriptive best score as accepted without the criteria above. */
        if(!best || (acceptable && !best_acceptable) ||
            (acceptable==best_acceptable && (sum<out->sum_abs || (sum==out->sum_abs && alpha<out->alpha_failures)))) {
            best=true;best_acceptable=acceptable;out->best_reference_index=index;out->sum_abs=sum;out->samples=samples;
            out->max_channel_delta=maximum;out->alpha_failures=alpha;
            out->mean_abs=(double)sum/(double)samples;
        }
    }
    ++reference->stats.comparisons;out->accepted_references=accepted;out->frame_identity_ambiguous=accepted>1;
    return 0;
}
#else
int x4_probe_reference_open_verified(X4ProbeReference *r,const char *p,unsigned w,unsigned h,unsigned n,
    const uint8_t sha[32],X4ProbeHashFile hash,void *ctx,const atomic_bool *cancel)
{ (void)r;(void)p;(void)w;(void)h;(void)n;(void)sha;(void)hash;(void)ctx;(void)cancel;return X4_PROBE_DISABLED; }
int x4_probe_reference_match(X4ProbeReference *r,const uint32_t *p,size_t c,unsigned w,unsigned h,X4ProbeReferenceScore *o)
{ (void)r;(void)p;(void)c;(void)w;(void)h;(void)o;return X4_PROBE_DISABLED; }
int x4_probe_reference_close(X4ProbeReference *r) { return r?0:X4_PROBE_ARGUMENT; }
#endif
