/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_assets.h"
#include "catalog_disk_cache.h"
#include "lang.h"
#include "../auth/http_client.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef X4_ASSETS_HOST_TEST
#include <pthread.h>
#include <time.h>
typedef pthread_t AssetThread;
typedef pthread_mutex_t AssetMutex;
static int mutex_init(AssetMutex *m) { return pthread_mutex_init(m, NULL);
}
static void lock(AssetMutex *m) { pthread_mutex_lock(m);
}
static int trylock(AssetMutex *m) { return pthread_mutex_trylock(m);
}
static void unlock(AssetMutex *m) { pthread_mutex_unlock(m);
}
static void mutex_destroy(AssetMutex *m) { pthread_mutex_destroy(m);
}
static void pause_worker(void) { struct timespec t={0,10000000};
nanosleep(&t,NULL);
}
static uint64_t platform_clock(void *p) { (void)p;
struct timespec t;
clock_gettime(CLOCK_MONOTONIC,&t);
return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
#else
#include <orbis/libkernel.h>
typedef OrbisPthread AssetThread;
typedef OrbisPthreadMutex AssetMutex;
static int mutex_init(AssetMutex *m) { return scePthreadMutexInit(m,NULL,"x4-ui-assets");
}
static void lock(AssetMutex *m) { scePthreadMutexLock(m);
}
static int trylock(AssetMutex *m) { return scePthreadMutexTrylock(m);
}
static void unlock(AssetMutex *m) { scePthreadMutexUnlock(m);
}
static void mutex_destroy(AssetMutex *m) { scePthreadMutexDestroy(m);
}
static void pause_worker(void) { sceKernelUsleep(10000);
}
static uint64_t platform_clock(void *p) { (void)p;
return sceKernelGetProcessTime();
}
#endif
#define METADATA_CAP (256u*1024u+1u)
#define METADATA_BATCH 4u
#define ATTEMPTS_MAX 3u
#define MARKET x4_lang_market()
#define LANGUAGE x4_lang_locale()
typedef struct {
    X4ProductMetadata metadata;
    X4CoverRasters cover;
    uint64_t generation, used, queued_us, metadata_due, image_due;
    unsigned pins, metadata_attempts, image_attempts;
    int occupied, wanted, busy, metadata_valid, metadata_missing, image_missing, disk_image_bad;
    enum X4CatalogAssetState state;
} Slot;
typedef struct { unsigned slot;
uint64_t generation;
char id[X4_PRODUCT_ID_CAP];
} Job;
struct X4CatalogAssets {
    AssetThread thread;
    AssetMutex mutex;
    _Atomic int stop, suspended, cancel, busy, unavailable;
    /* 0 cleaned/suspended, 1 enabled idle, 2 active, 3 cancelled active,
     * 4 cleanup pending. Auth handoff is permitted only in 0 or 1. */
    _Atomic int run_gate;
    _Atomic uint64_t generation;
    X4AssetClock clock;
    void *clock_context;
    char disk_root[256];
    uint64_t use_clock, revision;
    Slot slots[X4_CATALOG_ASSET_SLOTS];
    char demand[X4_CATALOG_ASSET_SLOTS][X4_PRODUCT_ID_CAP];
    size_t demand_count;
    X4CatalogAssetStats stats;
    X4AssetEvent events[X4_ASSET_EVENT_CAP];
    unsigned event_next, event_count;
};
static uint64_t now(X4CatalogAssets *a) { return a->clock(a->clock_context);
}
static uint64_t add_sat(uint64_t a,uint64_t b) { return UINT64_MAX-a<b?UINT64_MAX:a+b;
}
static void record(X4CatalogAssets *a,const Job *j,enum X4AssetStage stage,enum X4AssetSource source,uint64_t start,uint64_t end)
{
    lock(&a->mutex);
    uint64_t elapsed=end>=start?end-start:0;
    X4AssetTiming *t=&a->stats.stage[stage];
    ++t->count;
    t->total_us+=elapsed;
    if(elapsed>t->max_us)t->max_us=elapsed;
    X4AssetEvent *e=&a->events[a->event_next];
    *e=(X4AssetEvent){.start_us=start,.end_us=end,.generation=j->generation,.stage=stage,.source=source};
    memcpy(e->product_id,j->id,sizeof(e->product_id));
    a->event_next=(a->event_next+1u)%X4_ASSET_EVENT_CAP;
    if(a->event_count<X4_ASSET_EVENT_CAP)++a->event_count;
    else ++a->stats.events_omitted;
    unlock(&a->mutex);
}
static size_t raster_bytes(const X4CoverRasters *r)
{
    size_t n=0;
    for(unsigned i=0;i<X4_COVER_SURFACES;++i)
        if(r->surface[i].rgba)n+=(size_t)r->surface[i].width*r->surface[i].height*4u;
    return n;
}
static void evict(X4CatalogAssets *a,Slot *s)
{
    a->stats.thumbnail_bytes-=raster_bytes(&s->cover);
    x4_cover_rasters_free(&s->cover);
    memset(s,0,sizeof(*s));
}
static int find(X4CatalogAssets *a,const char *id,uint64_t generation)
{
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)
        if(a->slots[i].occupied&&a->slots[i].generation==generation&&!strcmp(a->slots[i].metadata.product_id,id))return (int)i;
    return -1;
}
static int allocate_slot(X4CatalogAssets *a)
{
    int candidate=-1;
    uint64_t oldest=UINT64_MAX;
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i){
        Slot *s=&a->slots[i];
        if(!s->occupied)return (int)i;
        if(!s->busy&&!s->pins&&!s->wanted&&s->used<oldest){oldest=s->used;
        candidate=(int)i;
        }
    }
    return candidate;
}
static void fill_demand(X4CatalogAssets *a,uint64_t generation)
{
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)a->slots[i].wanted=0;
    /* Protect requested public products before choosing an eviction. Rebind
     * generation only when no borrower or old worker owns the slot. */
    for(size_t d=0;d<a->demand_count;++d){
        int i=find(a,a->demand[d],generation);
        if(i<0)for(unsigned k=0;k<X4_CATALOG_ASSET_SLOTS;++k){
            Slot *s=&a->slots[k];
            if(s->occupied&&!s->busy&&!s->pins&&!strcmp(s->metadata.product_id,a->demand[d])){
                i=(int)k;
                s->generation=generation;
                s->queued_us=now(a);
                if(s->metadata_valid||s->cover.surface[0].rgba)++a->stats.ram_hits;
                ++a->revision;
                break;
            }
        }
        if(i>=0)a->slots[i].wanted=1;
    }
    for(size_t d=0;d<a->demand_count;++d){
        int i=find(a,a->demand[d],generation);
        if(i<0){
            int owned=0;
            for(unsigned k=0;k<X4_CATALOG_ASSET_SLOTS;++k)
                if(a->slots[k].occupied&&!strcmp(a->slots[k].metadata.product_id,a->demand[d])&&
                   (a->slots[k].pins||a->slots[k].busy))owned=1;
            if(owned)continue;
            i=allocate_slot(a);
            if(i<0)continue;
            Slot *s=&a->slots[i];
            evict(a,s);
            s->occupied=1;
            if(atomic_load(&a->unavailable)){s->metadata_missing=1;
            s->state=X4_ASSET_MISSING;
            }
            s->generation=generation;
            if(!s->metadata_missing)s->state=X4_ASSET_PENDING;
            s->queued_us=now(a);
            memcpy(s->metadata.product_id,a->demand[d],strlen(a->demand[d])+1u);
            ++a->stats.requests;
        }
        a->slots[i].wanted=1;
        a->slots[i].used=++a->use_clock;
    }
}
static Slot *job_slot(X4CatalogAssets *a,const Job *j)
{
    Slot *s=&a->slots[j->slot];
    return s->occupied&&s->generation==j->generation&&!strcmp(s->metadata.product_id,j->id)?s:NULL;
}
static int needed(X4CatalogAssets *a,const Job *j)
{
    lock(&a->mutex);
    Slot *s=job_slot(a,j);
    int yes=s&&s->wanted&&j->generation==atomic_load(&a->generation)&&!atomic_load(&a->cancel)&&!atomic_load(&a->suspended)&&!atomic_load(&a->stop);
    if(!yes)++a->stats.deferred_stages;
    unlock(&a->mutex);
    return yes;
}
static int temporary(int rc,int status)
{
    if(rc==X4_HTTP_CANCELLED)return 0;
    if(rc==X4_HTTP_URL||rc==X4_HTTP_ARGUMENT||rc==X4_HTTP_BOUND)return 0;
    return rc!=0||status==200||status==408||status==425||status==429||status>=500;
}
static void failed(X4CatalogAssets *a,const Job *j,int image,int rc,int status,uint64_t retry_ms)
{
    lock(&a->mutex);
    Slot *s=job_slot(a,j);
    if(s&&j->generation==atomic_load(&a->generation)&&!atomic_load(&a->cancel)){
        unsigned attempts=image?s->image_attempts:s->metadata_attempts;
        uint64_t backoff=attempts>1?4000000u:1000000u;
        uint64_t delay=retry_ms>UINT64_MAX/1000u?UINT64_MAX:retry_ms*1000u;
        if(delay<backoff)delay=backoff;
        if(image)s->image_due=add_sat(now(a),delay);
        else s->metadata_due=add_sat(now(a),delay);
        s->queued_us=image?s->image_due:s->metadata_due;
        if(temporary(rc,status))s->state=X4_ASSET_TEMPORARY_FAILURE;
        else{s->state=X4_ASSET_MISSING;
        if(image)s->image_missing=1;
        else s->metadata_missing=1;
        }
        ++a->stats.failures;
        ++a->revision;
    }else if(j->generation!=atomic_load(&a->generation))++a->stats.stale_results;
    else ++a->stats.cancelled;
    unlock(&a->mutex);
}
static int publish_metadata(X4CatalogAssets *a,const Job *j,const X4ProductMetadata *m,enum X4AssetSource source)
{
    uint64_t start=now(a);
    int ok=0;
    lock(&a->mutex);
    Slot *s=job_slot(a,j);
    if(s&&j->generation==atomic_load(&a->generation)&&!atomic_load(&a->cancel)){
        s->metadata=*m;
        s->metadata_valid=1;
        s->metadata_missing=0;
        /* Disk keys are canonicalized, but the outstanding borrow/job identity
         * remains the validated caller ID until that job is retired. */
        memcpy(s->metadata.product_id,j->id,sizeof(s->metadata.product_id));
        s->queued_us=now(a);
        s->state=m->cover_url[0]?X4_ASSET_PENDING:X4_ASSET_MISSING;
        s->image_missing=!m->cover_url[0];
        ++a->stats.metadata_ready;
        if(source==X4_ASSET_DISK)++a->stats.disk_metadata_hits;
        ++a->revision;
        ok=1;
    }else if(j->generation!=atomic_load(&a->generation))++a->stats.stale_results;
    else ++a->stats.cancelled;
    unlock(&a->mutex);
    record(a,j,X4_ASSET_PUBLISH,source,start,now(a));
    return ok;
}
static void metadata_batch(X4CatalogAssets *a,X4Http **http,X4CatalogDiskCache *disk,Job *jobs,unsigned count,char *json)
{
    Job active[METADATA_BATCH];
    unsigned n=0;
    for(unsigned i=0;i<count;++i)if(needed(a,&jobs[i]))active[n++]=jobs[i];
    if(!n)return;
    char url[512];
    size_t used=(size_t)snprintf(url,sizeof(url),"https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=");
    for(unsigned i=0;i<n;++i)used+=(size_t)snprintf(url+used,sizeof(url)-used,"%s%s",i?",":"",active[i].id);
    snprintf(url+used,sizeof(url)-used,"&market=%s&languages=%s&fieldsTemplate=Details",MARKET,LANGUAGE);
    lock(&a->mutex);
    for(unsigned i=0;i<n;++i){Slot *s=job_slot(a,&active[i]);
    if(s){if(s->metadata_attempts)++a->stats.retries;
    ++s->metadata_attempts;
    }}
    ++a->stats.metadata_network_requests;
    unlock(&a->mutex);
    int status=0;
    size_t length=0;
    uint64_t start=now(a);
    int open_error=0;
    if(!*http)*http=x4_http_open(&open_error);
    int rc=*http?x4_http_public_metadata_request(*http,url,json,METADATA_CAP,&length,&status,&a->cancel):open_error?open_error:X4_HTTP_ALLOCATION;
    uint64_t retry_ms=*http?x4_http_public_retry_after_ms(*http):0;
    uint64_t end=now(a);
    record(a,&active[0],X4_ASSET_METADATA,X4_ASSET_NETWORK,start,end);
    if(rc==X4_HTTP_BOUND&&n>1){
        lock(&a->mutex);
        ++a->stats.batch_splits;
        for(unsigned i=0;i<n;++i){Slot *s=job_slot(a,&active[i]);
        if(s&&s->metadata_attempts)--s->metadata_attempts;
        }
        unlock(&a->mutex);
        metadata_batch(a,http,disk,active,n/2u,json);
        metadata_batch(a,http,disk,active+n/2u,n-n/2u,json);
        return;
    }
    Job missing[METADATA_BATCH];
    unsigned missing_count=0;
    for(unsigned i=0;i<n;++i){X4ProductMetadata m;
        if(!rc&&status==200&&!x4_product_metadata_parse(json,length,active[i].id,&m)){
            publish_metadata(a,&active[i],&m,X4_ASSET_NETWORK);
            if(disk&&!atomic_load(&a->cancel))x4_catalog_disk_cache_put_metadata(disk,active[i].id,MARKET,LANGUAGE,&m,&a->cancel);
        }else if(!rc&&status==200&&n>1)missing[missing_count++]=active[i];
        else failed(a,&active[i],0,rc,status,retry_ms);
    }
    if(missing_count){lock(&a->mutex);
    ++a->stats.partial_responses;
    unlock(&a->mutex);
        /* Retry only absent products, at most one immediate singleton each. */
        for(unsigned i=0;i<missing_count;++i)metadata_batch(a,http,disk,&missing[i],1,json);
    }
}
static void process_image(X4CatalogAssets *a,X4Http **http,X4CatalogDiskCache *disk,const Job *j,unsigned char *encoded)
{
    X4ProductMetadata m;
    lock(&a->mutex);
    Slot *s=job_slot(a,j);
    m=s?s->metadata:(X4ProductMetadata){0};
    unlock(&a->mutex);
    if(!needed(a,j)||!m.cover_url[0])return;
    size_t length=0;
    int status=0,rc=0;
    enum X4AssetSource source=X4_ASSET_DISK;
    uint64_t start=now(a);
    lock(&a->mutex);
    s=job_slot(a,j);
    int bypass=s&&s->disk_image_bad;
    unlock(&a->mutex);
    int hit=disk&&!bypass?x4_catalog_disk_cache_get_image(disk,j->id,MARKET,LANGUAGE,m.cover_url,encoded,X4_HTTP_IMAGE_MAX+1u,&length,&a->cancel):0;
    if(hit!=1){
        if(!needed(a,j))return;
        source=X4_ASSET_NETWORK;
        lock(&a->mutex);
        s=job_slot(a,j);
        if(s){if(s->image_attempts)++a->stats.retries;
        ++s->image_attempts;
        s->state=X4_ASSET_DOWNLOADING;
        ++a->revision;
        }
        ++a->stats.image_network_requests;
        unlock(&a->mutex);
        start=now(a);
        int open_error=0;
        if(!*http)*http=x4_http_open(&open_error);
        rc=*http?x4_http_public_image_request(*http,m.cover_url,encoded,X4_HTTP_IMAGE_MAX+1u,&length,&status,&a->cancel):open_error?open_error:X4_HTTP_ALLOCATION;
    }else status=200;
    record(a,j,X4_ASSET_IMAGE,source,start,now(a));
    uint64_t retry_ms=source==X4_ASSET_NETWORK&&*http?x4_http_public_retry_after_ms(*http):0;
    if(rc||status!=200){failed(a,j,1,rc,status,retry_ms);
    return;
    }
    if(!needed(a,j))return;
    X4CoverRasters cover={0};
    start=now(a);
    rc=x4_cover_decode(encoded,length,&cover);
    record(a,j,X4_ASSET_DECODE,source,start,now(a));
    if(rc){
        if(source==X4_ASSET_DISK&&disk){
            x4_catalog_disk_cache_remove_image(disk,j->id,MARKET,LANGUAGE,m.cover_url);
            lock(&a->mutex);
            s=job_slot(a,j);
            if(s)s->disk_image_bad=1;
            unlock(&a->mutex);
        }
        failed(a,j,1,X4_HTTP_PROTOCOL,200,retry_ms);
        return;
    }
    /* Cache only successfully decoded public bytes; never failed responses. */
    if(disk&&source==X4_ASSET_NETWORK&&!atomic_load(&a->cancel))
        x4_catalog_disk_cache_put_image(disk,j->id,MARKET,LANGUAGE,m.cover_url,encoded,length,&a->cancel);
    start=now(a);
    lock(&a->mutex);
    s=job_slot(a,j);
    if(s&&s->wanted&&j->generation==atomic_load(&a->generation)&&!atomic_load(&a->cancel)){
        /* A pinned placeholder owns no pixels. Publishing its first raster
         * cannot invalidate that borrow; existing rasters are never replaced. */
        if(!s->cover.surface[0].rgba&&a->stats.thumbnail_bytes+raster_bytes(&cover)<=X4_CATALOG_THUMBNAIL_BUDGET){
            s->cover=cover;
            memset(&cover,0,sizeof(cover));
            a->stats.thumbnail_bytes+=raster_bytes(&s->cover);
            s->state=X4_ASSET_READY;
            ++a->stats.covers_ready;
            if(source==X4_ASSET_DISK)++a->stats.disk_image_hits;
            ++a->revision;
        } else {
            /* Future raster dimensions must not create an unthrottled disk
             * decode loop when the bounded RAM budget refuses publication. */
            if(source==X4_ASSET_DISK)++s->image_attempts;
            s->state=X4_ASSET_TEMPORARY_FAILURE;
            s->image_due=add_sat(now(a),1000000u);
            s->queued_us=s->image_due;
            ++a->stats.failures;
            ++a->revision;
        }
    }else if(j->generation!=atomic_load(&a->generation))++a->stats.stale_results;
    else ++a->stats.cancelled;
    unlock(&a->mutex);
    record(a,j,X4_ASSET_PUBLISH,source,start,now(a));
    x4_cover_rasters_free(&cover);
}
static int runnable(Slot *s,uint64_t t)
{
    if(!s->wanted||s->busy||s->cover.surface[0].rgba||s->metadata_missing||s->image_missing)return 0;
    return s->metadata_valid?(s->image_attempts<ATTEMPTS_MAX&&t>=s->image_due):
        (s->metadata_attempts<ATTEMPTS_MAX&&t>=s->metadata_due);
}
static void finish_jobs(X4CatalogAssets *a,Job *jobs,unsigned n)
{
    lock(&a->mutex);
    for(unsigned i=0;i<n;++i){Slot *s=job_slot(a,&jobs[i]);
    if(s){
    if(!s->wanted&&s->metadata_valid&&!s->cover.surface[0].rgba)++a->stats.deferred_stages;
    s->busy=0;
    if(s->state==X4_ASSET_DOWNLOADING)s->state=X4_ASSET_PENDING;
    }}
    unlock(&a->mutex);
    atomic_store(&a->busy,0);
    int active=2;
    if(!atomic_compare_exchange_strong(&a->run_gate,&active,1)){
        active=3;
        atomic_compare_exchange_strong(&a->run_gate,&active,4);
    }
}
static void diagnostic_summary(X4CatalogAssets *a)
{
#ifndef X4_ASSETS_HOST_TEST
    X4CatalogAssetStats s;
    lock(&a->mutex);
    s=a->stats;
    unlock(&a->mutex);
    printf("XCloud4: COVER_METRICS ram=%llu disk_metadata=%llu disk_image=%llu metadata_get=%llu image_get=%llu splits=%llu partial=%llu retry=%llu deferred=%llu omitted=%llu\n",
        (unsigned long long)s.ram_hits,(unsigned long long)s.disk_metadata_hits,(unsigned long long)s.disk_image_hits,
        (unsigned long long)s.metadata_network_requests,(unsigned long long)s.image_network_requests,
        (unsigned long long)s.batch_splits,(unsigned long long)s.partial_responses,(unsigned long long)s.retries,
        (unsigned long long)s.deferred_stages,(unsigned long long)s.events_omitted);
    for(unsigned i=0;i<X4_ASSET_STAGE_COUNT;++i)
        printf("XCloud4: COVER_STAGE stage=%u count=%llu total_us=%llu max_us=%llu\n",i,
            (unsigned long long)s.stage[i].count,(unsigned long long)s.stage[i].total_us,(unsigned long long)s.stage[i].max_us);
#else
    (void)a;
#endif
}
static void *asset_worker(void *argument)
{
    X4CatalogAssets *a=argument;
    X4Http *http=NULL;
    X4CatalogDiskCache *disk=x4_catalog_disk_cache_open(a->disk_root[0]?a->disk_root:NULL,X4_CATALOG_DISK_BUDGET,X4_CATALOG_DISK_PRODUCTS,NULL,NULL);
    lock(&a->mutex);
    a->stats.disk_enabled=disk!=NULL;
    unlock(&a->mutex);
    char *json=malloc(METADATA_CAP);
    unsigned char *encoded=malloc(X4_HTTP_IMAGE_MAX+1u);
    if(!json||!encoded){free(json);
    free(encoded);
    x4_catalog_disk_cache_close(disk);
    atomic_store(&a->suspended,1);
    atomic_store(&a->unavailable,1);
    atomic_store(&a->run_gate,0);
    return NULL;
    }
    while(!atomic_load(&a->stop)){
        int gate=atomic_load(&a->run_gate);
        if(gate==4){
            int rc=http?x4_http_public_reset(http):0;
            if(rc){lock(&a->mutex);
            ++a->stats.cleanup_failures;
            unlock(&a->mutex);
            atomic_store(&a->suspended,1);
                atomic_store(&a->unavailable,X4_ASSET_FAULT_CLEANUP);
                /* No false idle/auth handoff on a quarantined native context. */
                while(!atomic_load(&a->stop))pause_worker();
                break;
            }
            x4_http_close(http);
            http=NULL;
            diagnostic_summary(a);
            atomic_store(&a->run_gate,atomic_load(&a->suspended)?0:1);
            continue;
        }
        if(atomic_load(&a->suspended)||gate!=1){pause_worker();
        continue;
        }
        Job jobs[METADATA_BATCH];
        unsigned n=0;
        int image=0;
        uint64_t generation=atomic_load(&a->generation),t=now(a);
        lock(&a->mutex);
        fill_demand(a,generation);
        for(size_t d=0;d<a->demand_count;++d){
            int i=find(a,a->demand[d],generation);
            if(i<0||!runnable(&a->slots[i],t))continue;
            Slot *s=&a->slots[i];
            image=s->metadata_valid;
            jobs[n]=(Job){.slot=(unsigned)i,.generation=generation};
            memcpy(jobs[n++].id,s->metadata.product_id,X4_PRODUCT_ID_CAP);
            if(!image)for(size_t q=d+1;q<a->demand_count&&n<METADATA_BATCH;++q){
                int k=find(a,a->demand[q],generation);
                if(k>=0&&!a->slots[k].metadata_valid&&runnable(&a->slots[k],t)){
                    jobs[n]=(Job){.slot=(unsigned)k,.generation=generation};
                    memcpy(jobs[n++].id,a->slots[k].metadata.product_id,X4_PRODUCT_ID_CAP);
                }
            }
            break;
        }
        if(n){int enabled=1;
        atomic_store(&a->cancel,0);
            if(atomic_load(&a->suspended)||generation!=atomic_load(&a->generation)||!atomic_compare_exchange_strong(&a->run_gate,&enabled,2))n=0;
            else{atomic_store(&a->busy,1);
            for(unsigned i=0;i<n;++i)a->slots[jobs[i].slot].busy=1;
            }
        }
        unlock(&a->mutex);
        if(!n){pause_worker();
        continue;
        }
        for(unsigned i=0;i<n;++i){lock(&a->mutex);
        uint64_t queued=a->slots[jobs[i].slot].queued_us;
        unlock(&a->mutex);
        record(a,&jobs[i],X4_ASSET_QUEUE,X4_ASSET_RAM,queued,now(a));
        }
        if(image)process_image(a,&http,disk,&jobs[0],encoded);
        else{
            Job network[METADATA_BATCH];
            unsigned count=0;
            for(unsigned i=0;i<n;++i){X4ProductMetadata m;
            uint64_t start=now(a);
                int hit=disk?x4_catalog_disk_cache_get_metadata(disk,jobs[i].id,MARKET,LANGUAGE,&m,&a->cancel):0;
                if(hit==1){record(a,&jobs[i],X4_ASSET_METADATA,X4_ASSET_DISK,start,now(a));
                publish_metadata(a,&jobs[i],&m,X4_ASSET_DISK);
                }
                else network[count++]=jobs[i];
            }
            if(count)metadata_batch(a,&http,disk,network,count,json);
        }
        finish_jobs(a,jobs,n);
    }
    if(http)x4_http_public_reset(http);
    x4_http_close(http);
    x4_catalog_disk_cache_close(disk);
    free(json);
    free(encoded);
    atomic_store(&a->busy,0);
    return NULL;
}
X4CatalogAssets *x4_catalog_assets_create_config(const X4CatalogAssetsConfig *config)
{
    X4CatalogAssets *a=calloc(1,sizeof(*a));
    if(!a)return NULL;
    a->clock=config&&config->clock_us?config->clock_us:platform_clock;
    a->clock_context=config?config->clock_context:NULL;
    if(config&&config->disk_root){size_t n=strlen(config->disk_root);
    if(n>=sizeof(a->disk_root)){free(a);
    return NULL;
    }memcpy(a->disk_root,config->disk_root,n+1u);
    }
    atomic_init(&a->generation,1);
    atomic_init(&a->suspended,1);
    atomic_init(&a->stop,0);
    atomic_init(&a->unavailable,0);
    atomic_init(&a->cancel,0);
    atomic_init(&a->busy,0);
    atomic_init(&a->run_gate,0);
    if(mutex_init(&a->mutex)){free(a);
    return NULL;
    }
#ifdef X4_ASSETS_HOST_TEST
    int rc=pthread_create(&a->thread,NULL,asset_worker,a);
#else
    int rc=scePthreadCreate(&a->thread,NULL,asset_worker,a,"x4-ui-assets");
#endif
    if(rc){mutex_destroy(&a->mutex);
    free(a);
    return NULL;
    }return a;
}
X4CatalogAssets *x4_catalog_assets_create(void)
{
#ifdef X4_ASSETS_HOST_TEST
    return x4_catalog_assets_create_config(NULL);
#else
    X4CatalogAssetsConfig c={.disk_root=X4_CATALOG_DISK_ROOT};
    return x4_catalog_assets_create_config(&c);
#endif
}
void x4_catalog_assets_suspend(X4CatalogAssets *a)
{
    if(!a)return;
    atomic_store(&a->suspended,1);
    atomic_store(&a->cancel,1);
    int gate=atomic_load(&a->run_gate);
    while(!atomic_compare_exchange_weak(&a->run_gate,&gate,gate==2||gate==3?3:gate==1?4:gate)){}
}
void x4_catalog_assets_resume(X4CatalogAssets *a)
{
    if(!a||atomic_load(&a->unavailable))return;
    atomic_store(&a->suspended,0);
    int gate=0;
    atomic_compare_exchange_strong(&a->run_gate,&gate,1);
}
int x4_catalog_assets_idle(X4CatalogAssets *a)
{ if(!a)return 1;
int gate=atomic_load(&a->run_gate);
return gate==0||gate==1;
}
enum X4AssetFault x4_catalog_assets_fault(X4CatalogAssets *a)
{ return a?(enum X4AssetFault)atomic_load(&a->unavailable):X4_ASSET_FAULT_ALLOCATION;
}
void x4_catalog_assets_set_generation(X4CatalogAssets *a,uint64_t generation)
{
    if(!a||!generation||generation==atomic_load(&a->generation))return;
    lock(&a->mutex);
    /* Serialize both stores with the worker's cancel-clear/start decision. */
    atomic_store(&a->generation,generation);
    atomic_store(&a->cancel,1);
    a->demand_count=0;
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)a->slots[i].wanted=0;
    ++a->revision;
    unlock(&a->mutex);
}
void x4_catalog_assets_request(X4CatalogAssets *a,const char *const *ids,size_t count)
{
    if(!a||(!ids&&count)||trylock(&a->mutex))return;
    char demand[X4_CATALOG_ASSET_SLOTS][X4_PRODUCT_ID_CAP]={{0}};
    size_t n=0;
    if(count>X4_CATALOG_ASSET_SLOTS)count=X4_CATALOG_ASSET_SLOTS;
    for(size_t i=0;i<count;++i){
        if(!x4_product_id_valid(ids[i]))continue;
        int duplicate=0;
        for(size_t d=0;d<n;++d)if(!strcmp(demand[d],ids[i]))duplicate=1;
        if(!duplicate)memcpy(demand[n++],ids[i],strlen(ids[i])+1u);
    }
    int changed=n!=a->demand_count;
    for(size_t i=0;i<n&&!changed;++i)if(strcmp(a->demand[i],demand[i]))changed=1;
    if(changed)for(size_t i=0;i<n;++i){
        int hit=find(a,demand[i],atomic_load(&a->generation));
        if(hit>=0&&a->slots[hit].cover.surface[0].rgba)++a->stats.ram_hits;
    }
    memcpy(a->demand,demand,sizeof(demand));
    a->demand_count=n;
    fill_demand(a,atomic_load(&a->generation));
    unlock(&a->mutex);
}
void x4_catalog_assets_retry(X4CatalogAssets *a)
{
    if(!a)return;
    lock(&a->mutex);
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i){Slot *s=&a->slots[i];
        if(s->wanted&&!s->busy&&s->state==X4_ASSET_TEMPORARY_FAILURE){
            ++a->stats.retries;
            if(now(a)<(s->metadata_valid?s->image_due:s->metadata_due))++a->stats.retry_suppressed;
            s->metadata_attempts=0;
            s->image_attempts=0;
            s->state=X4_ASSET_PENDING;
            ++a->revision;
        }
    }
    unlock(&a->mutex);
}
int x4_catalog_assets_acquire(X4CatalogAssets *a,const char *id,X4CatalogAssetView *view)
{
    if(!a||!view||!x4_product_id_valid(id)||trylock(&a->mutex))return 0;
    int i=find(a,id,atomic_load(&a->generation));
    if(i>=0){Slot *s=&a->slots[i];
    *view=(X4CatalogAssetView){.metadata=s->metadata,.state=s->state,.generation=s->generation,.slot=(unsigned)i};
        memcpy(view->surface,s->cover.surface,sizeof(view->surface));
        ++s->pins;
        s->used=++a->use_clock;
    }
    unlock(&a->mutex);
    return i>=0;
}
void x4_catalog_assets_release(X4CatalogAssets *a,X4CatalogAssetView *view)
{
    if(!a||!view||view->slot>=X4_CATALOG_ASSET_SLOTS)return;
    lock(&a->mutex);
    Slot *s=&a->slots[view->slot];
    if(s->occupied&&s->generation==view->generation&&!strcmp(s->metadata.product_id,view->metadata.product_id)&&s->pins)--s->pins;
    unlock(&a->mutex);
    memset(view,0,sizeof(*view));
    view->slot=X4_CATALOG_ASSET_SLOTS;
}
size_t x4_catalog_assets_copy_metadata(X4CatalogAssets *a,X4ProductMetadata *out,size_t capacity,uint64_t *revision)
{
    if(!a||!out||trylock(&a->mutex))return 0;
    size_t n=0;
    uint64_t generation=atomic_load(&a->generation);
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS&&n<capacity;++i){Slot *s=&a->slots[i];
    if(s->occupied&&s->generation==generation&&s->metadata_valid)out[n++]=s->metadata;
    }
    if(revision)*revision=a->revision;
    unlock(&a->mutex);
    return n;
}
void x4_catalog_assets_stats(X4CatalogAssets *a,X4CatalogAssetStats *out)
{
    if(!out)return;
    memset(out,0,sizeof(*out));
    if(!a||trylock(&a->mutex))return;
    *out=a->stats;
    out->revision=a->revision;
    out->busy=atomic_load(&a->busy);
    out->suspended=atomic_load(&a->suspended);
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i){Slot *s=&a->slots[i];
    out->occupied+=s->occupied?1u:0u;
    out->pinned+=s->pins;
        out->pending+=s->wanted&&!s->cover.surface[0].rgba&&!s->metadata_missing&&!s->image_missing&&(s->metadata_valid?s->image_attempts<ATTEMPTS_MAX:s->metadata_attempts<ATTEMPTS_MAX)?1u:0u;
    }
    unlock(&a->mutex);
}
size_t x4_catalog_assets_events(X4CatalogAssets *a,X4AssetEvent *out,size_t capacity)
{
    if(!a||!out||trylock(&a->mutex))return 0;
    size_t n=a->event_count;
    if(n>capacity)n=capacity;
    unsigned first=(a->event_next+X4_ASSET_EVENT_CAP-a->event_count)%X4_ASSET_EVENT_CAP;
    for(size_t i=0;i<n;++i)out[i]=a->events[(first+(unsigned)i)%X4_ASSET_EVENT_CAP];
    unlock(&a->mutex);
    return n;
}
void x4_catalog_assets_destroy(X4CatalogAssets *a)
{
    if(!a)return;
    x4_catalog_assets_suspend(a);
    atomic_store(&a->stop,1);
#ifdef X4_ASSETS_HOST_TEST
    pthread_join(a->thread,NULL);
#else
    scePthreadJoin(a->thread,NULL);
#endif
    for(unsigned i=0;i<X4_CATALOG_ASSET_SLOTS;++i)x4_cover_rasters_free(&a->slots[i].cover);
    mutex_destroy(&a->mutex);
    free(a);
}
