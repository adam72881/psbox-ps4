/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/ui/catalog_assets.h"
#include "../src/auth/http_client.h"
#include "../src/auth/json.h"
#include <assert.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Atomic unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"check failed %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static void sleep_ms(unsigned ms) { struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }
static unsigned char *read_file(const char *path, size_t *length)
{
    FILE *f=fopen(path,"rb"); CHECK(f); CHECK(!fseek(f,0,SEEK_END)); long n=ftell(f); CHECK(n>0); rewind(f);
    unsigned char *p=malloc((size_t)n+1); CHECK(p); CHECK(fread(p,1,(size_t)n,f)==(size_t)n); fclose(f); p[n]=0; *length=(size_t)n; return p;
}
struct X4Http { pthread_t owner; };
static _Atomic int block_json, block_image, block_reset, ignore_cancel, entered, image_entered, reset_entered;
static _Atomic int fake_missing, fake_corrupt, fake_image_status, fake_partial_multi, fake_single_missing, fake_reset_error;
static _Atomic unsigned json_calls, image_calls, reset_calls, open_calls, active_requests, active_http, fake_batch_limit;
static _Atomic uint64_t fake_now_us, fake_retry_ms;
static char metadata_order[128][256], image_order[128][32];
static unsigned char *fake_image;
static size_t fake_image_length;
static void check_owner(X4Http *h) { CHECK(h && pthread_equal(h->owner,pthread_self())); }
X4Http *x4_http_open(int *error)
{
    if(error)*error=0;
    X4Http*h=calloc(1,sizeof(*h));CHECK(h);h->owner=pthread_self();
    CHECK(atomic_fetch_add(&active_http,1)==0);atomic_fetch_add(&open_calls,1);return h;
}
void x4_http_close(X4Http *h)
{
    if(!h)return;
    check_owner(h);CHECK(!atomic_load(&active_requests));
    CHECK(atomic_fetch_sub(&active_http,1)==1);free(h);
}
int x4_http_public_reset(X4Http*h)
{
    check_owner(h);CHECK(!atomic_load(&active_requests));atomic_fetch_add(&reset_calls,1);
    atomic_store(&reset_entered,1);while(atomic_load(&block_reset))sleep_ms(1);return atomic_load(&fake_reset_error);
}
uint64_t x4_http_public_retry_after_ms(const X4Http*h)
{CHECK(h&&pthread_equal(h->owner,pthread_self()));return atomic_load(&fake_retry_ms);}
int x4_http_public_metadata_request(X4Http *h,const char *url,
    char *body,size_t capacity,size_t *length,int *status,const _Atomic int *cancel)
{
    check_owner(h);CHECK(cancel);CHECK(atomic_fetch_add(&active_requests,1)==0);
    unsigned call=atomic_fetch_add(&json_calls,1);CHECK(call<128);
    const char*prefix="https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=";
    CHECK(!strncmp(url,prefix,strlen(prefix)));
    CHECK(strstr(url,"&market=MX&languages=es-MX&fieldsTemplate=Details"));
    const char *id=strstr(url,"bigIds=");CHECK(id);id+=7;
    size_t query_length=strcspn(id,"&");CHECK(query_length<sizeof(metadata_order[0]));
    memcpy(metadata_order[call],id,query_length);metadata_order[call][query_length]=0;
    char products[4][32];unsigned count=0;size_t offset=0;
    while(offset<query_length){size_t end=offset;while(end<query_length&&id[end]!=',')++end;
        CHECK(count<4&&end>offset&&end-offset<sizeof(products[0]));
        memcpy(products[count],id+offset,end-offset);products[count][end-offset]=0;
        CHECK(x4_product_id_valid(products[count]));++count;offset=end+1;}
    CHECK(count>0);atomic_store(&entered,1);
    while(atomic_load(&block_json) && (atomic_load(&ignore_cancel) || !atomic_load(cancel))) sleep_ms(1);
    int rc=0;*length=0;*status=0;
    if(atomic_load(cancel)&&!atomic_load(&ignore_cancel)){rc=X4_HTTP_CANCELLED;goto done;}
    if(atomic_load(&fake_batch_limit)&&count>atomic_load(&fake_batch_limit)){rc=X4_HTTP_BOUND;goto done;}
    size_t used=0;int written=snprintf(body,capacity,"{\"Products\":[");CHECK(written>0&&(size_t)written<capacity);used=(size_t)written;
    unsigned emitted=0;
    for(unsigned i=0;i<count;++i){
        if(!strcmp(products[i],"ID2")&&((count>1&&atomic_load(&fake_partial_multi))||atomic_load(&fake_single_missing)))continue;
        written=snprintf(body+used,capacity-used,
            "%s{\"ProductId\":\"%s\",\"Properties\":{\"Category\":\"Test\"},\"LocalizedProperties\":[{\"ProductTitle\":\"Edición ñü %s\",\"Images\":[{\"ImagePurpose\":\"BrandedKeyArt\",\"Uri\":\"//store-images.s-microsoft.com/image/apps.test-asset-%s\",\"Width\":80,\"Height\":120}]}]}",
            emitted?",":"",products[i],products[i],products[i]);CHECK(written>0&&(size_t)written<capacity-used);
        used+=(size_t)written;++emitted;
    }
    written=snprintf(body+used,capacity-used,"]}");CHECK(written>0&&(size_t)written<capacity-used);
    *length=used+(size_t)written;*status=atomic_load(&fake_missing)?404:200;
done:
    CHECK(atomic_fetch_sub(&active_requests,1)==1);return rc;
}
int x4_http_public_image_request(X4Http *h,const char *url,unsigned char *body,size_t capacity,size_t *length,int *status,const _Atomic int *cancel)
{
    check_owner(h);CHECK(x4_http_public_image_url(url));CHECK(cancel);CHECK(atomic_fetch_add(&active_requests,1)==0);
    unsigned call=atomic_fetch_add(&image_calls,1);CHECK(call<128);
    const char*id=strstr(url,"apps.test-asset-");CHECK(id);id+=16;
    size_t n=strcspn(id,"?");CHECK(n<sizeof(image_order[0]));memcpy(image_order[call],id,n);image_order[call][n]=0;
    atomic_store(&image_entered,1);
    while(atomic_load(&block_image)&&(atomic_load(&ignore_cancel)||!atomic_load(cancel)))sleep_ms(1);
    if(atomic_load(cancel)&&!atomic_load(&ignore_cancel)){CHECK(atomic_fetch_sub(&active_requests,1)==1);return X4_HTTP_CANCELLED;}
    if(atomic_load(&fake_corrupt)){memcpy(body,"corrupt",7);*length=7;}else{CHECK(fake_image_length<capacity);memcpy(body,fake_image,fake_image_length);*length=fake_image_length;}
    *status=atomic_load(&fake_image_status);CHECK(atomic_fetch_sub(&active_requests,1)==1);return 0;
}
static X4CatalogAssetStats stats(X4CatalogAssets *a)
{
    X4CatalogAssetStats s;
    /* A try-lock miss produces zeros; it is not an observed empty cache. */
    for(unsigned i=0;i<5000;++i){x4_catalog_assets_stats(a,&s);
        if(s.suspended||s.busy||s.requests||s.occupied||s.revision)return s;
        sleep_ms(1);
    }
    CHECK(0);return s;
}
static void wait_ready(X4CatalogAssets *a,unsigned ready)
{
    for(unsigned i=0;i<5000;++i){X4CatalogAssetStats s=stats(a);if(s.covers_ready>=ready && !s.pending && x4_catalog_assets_idle(a))return;sleep_ms(1);}
    CHECK(0);
}
static void wait_entered(void) { for(unsigned i=0;i<2000&&!atomic_load(&entered);++i)sleep_ms(1); CHECK(atomic_load(&entered)); }
static void wait_idle(X4CatalogAssets *a) { for(unsigned i=0;i<2000&&!x4_catalog_assets_idle(a);++i)sleep_ms(1);CHECK(x4_catalog_assets_idle(a)); }
static int wait_acquire(X4CatalogAssets *a,const char *id,X4CatalogAssetView *view)
{
    for(unsigned i=0;i<5000;++i){if(x4_catalog_assets_acquire(a,id,view))return 1;sleep_ms(1);}
    return 0;
}
static void wait_state(X4CatalogAssets *a,const char *id,enum X4CatalogAssetState state,X4CatalogAssetView *view)
{
    /* stats/acquire deliberately use try-lock. A zeroed unavailable stats
     * snapshot is not proof that the asynchronous request has completed. */
    for(unsigned i=0;i<5000;++i){
        if(x4_catalog_assets_acquire(a,id,view)){
            if(view->state==state)return;
            x4_catalog_assets_release(a,view);
        }
        sleep_ms(1);
    }
    CHECK(0);
}
static void wait_flag(const _Atomic int *flag)
{for(unsigned i=0;i<5000&&!atomic_load(flag);++i)sleep_ms(1);CHECK(atomic_load(flag));}
static void fake_reset(void)
{
    CHECK(!atomic_load(&active_http));CHECK(!atomic_load(&active_requests));
    atomic_store(&block_json,0);atomic_store(&block_image,0);atomic_store(&block_reset,0);
    atomic_store(&ignore_cancel,0);atomic_store(&entered,0);atomic_store(&image_entered,0);atomic_store(&reset_entered,0);
    atomic_store(&fake_missing,0);atomic_store(&fake_corrupt,0);atomic_store(&fake_partial_multi,0);atomic_store(&fake_single_missing,0);
    atomic_store(&fake_image_status,200);atomic_store(&fake_batch_limit,0);
    atomic_store(&fake_reset_error,0);
    atomic_store(&json_calls,0);atomic_store(&image_calls,0);atomic_store(&reset_calls,0);atomic_store(&open_calls,0);
    atomic_store(&fake_now_us,UINT64_C(10000000));atomic_store(&fake_retry_ms,0);
    memset(metadata_order,0,sizeof(metadata_order));memset(image_order,0,sizeof(image_order));
}
static uint64_t clock_us(void *context) {(void)context;return atomic_load(&fake_now_us);}
static X4CatalogAssets *create_assets(const char *root)
{
    X4CatalogAssetsConfig config={root,clock_us,NULL};
    X4CatalogAssets*a=x4_catalog_assets_create_config(&config);CHECK(a);CHECK(x4_catalog_assets_idle(a));return a;
}
static void close_assets(X4CatalogAssets*a)
{x4_catalog_assets_suspend(a);wait_idle(a);x4_catalog_assets_destroy(a);CHECK(!atomic_load(&active_http));}
static void request_resume(X4CatalogAssets*a,const char*const*ids,size_t count)
{x4_catalog_assets_request(a,ids,count);x4_catalog_assets_resume(a);}
static void advance_ms(uint64_t milliseconds)
{CHECK(milliseconds<=UINT64_MAX/1000);atomic_fetch_add(&fake_now_us,milliseconds*1000);}
static void test_text(void)
{
    X4JsonSpan s;char out[128];const char *j="\"Árbol, niño, pingüino ¿sí? ¡Sí!\\n\\u00f1\"";
    CHECK(!x4_json_parse(j,strlen(j),&s));CHECK(!x4_json_display_text(s,out,sizeof(out)));CHECK(!strcmp(out,"Árbol, niño, pingüino ¿sí? ¡Sí! ñ"));
    CHECK(x4_json_token(s,out,sizeof(out))<0);CHECK(!out[0]);
    CHECK(!x4_json_parse("\"ñüa\"",strlen("\"ñüa\""),&s));CHECK(!x4_json_display_text(s,out,4));CHECK(!strcmp(out,"ñ"));
    CHECK(x4_json_parse("\"\\uD800\"",8,&s)<0);CHECK(x4_json_parse("\"\xc0\xaf\"",4,&s)<0);
}
static void test_policy_metadata(const char *fixture)
{
    size_t n;unsigned char *j=read_file(fixture,&n);X4ProductMetadata m;
    CHECK(!x4_product_metadata_parse((char*)j,n,"9NKX70BBCDRN",&m));CHECK(strstr(m.name,"Edición"));CHECK(!strcmp(m.publisher,"Xbox Game Studios"));CHECK(m.category[0] && m.description[0]);CHECK(x4_http_public_image_url(m.cover_url));
    CHECK(x4_product_metadata_parse((char*)j,n,"NOTFOUND",&m)<0);free(j);
    CHECK(!x4_product_id_valid("../bad"));CHECK(!x4_product_id_valid(""));CHECK(!x4_product_id_valid("ID\r\n"));CHECK(x4_product_id_valid("9nkx70bbcdrn"));
    char url[512];CHECK(x4_product_cover_url("//store-images.s-microsoft.com/image/apps.test-asset",url));CHECK(x4_http_public_image_url(url));
    const char *bad[]={"http://store-images.s-microsoft.com/image/apps.a","//store-images.s-microsoft.com.evil/image/apps.a","//evil/image/apps.a","//store-images.s-microsoft.com/image/apps.a?secret=1","//store-images.s-microsoft.com:443/image/apps.a","//user@store-images.s-microsoft.com/image/apps.a","//store-images.s-microsoft.com/image/apps.a/../b","//store-images.s-microsoft.com/image/apps.a%2fb"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i)CHECK(!x4_product_cover_url(bad[i],url));
    CHECK(!x4_http_public_image_url("https://store-images.s-microsoft.com/image/apps.a?w=512&q=80&format=jpg&token=x"));
    const char *missing="{\"Products\":[{\"ProductId\":\"ID1\",\"LocalizedProperties\":[{}]}]}";CHECK(!x4_product_metadata_parse(missing,strlen(missing),"ID1",&m));CHECK(!m.name[0]&&!m.cover_url[0]);
    const char *duplicate="{\"Products\":[{\"ProductId\":\"ID1\"},{\"ProductId\":\"ID1\"}]}";CHECK(x4_product_metadata_parse(duplicate,strlen(duplicate),"ID1",&m)<0);
}
static void test_images(const char *png)
{
    X4CoverRasters r;CHECK(!x4_cover_decode(fake_image,fake_image_length,&r));CHECK(r.surface[0].width==320&&r.surface[0].height==224);CHECK(r.surface[1].width==512&&r.surface[1].height==288);CHECK(r.surface[2].width==96&&r.surface[2].height==96);
    CHECK(r.surface[0].rgba[0]==0xff30261du);CHECK(r.surface[0].rgba[112*320+160]!=0xff30261du);x4_cover_rasters_free(&r);
    size_t n;unsigned char *b=read_file(png,&n);CHECK(!x4_cover_decode(b,n,&r));x4_cover_rasters_free(&r);
    CHECK(x4_cover_decode((unsigned char*)"corrupt",7,&r)<0);CHECK(x4_cover_decode(b,2,&r)<0);CHECK(x4_cover_decode(b,2u*1024u*1024u+1u,&r)<0);
    /* PNG IHDR dimension preflight: decoder never allocates its large pixels. */
    b[16]=0;b[17]=0;b[18]=0x10;b[19]=0x01;CHECK(x4_cover_decode(b,n,&r)<0);free(b);
}
static void test_color_layout(const char *fixture)
{
    size_t n;unsigned char *b=read_file(fixture,&n);X4CoverRasters r;
    CHECK(!x4_cover_decode(b,n,&r));
    for(unsigned i=0;i<X4_COVER_SURFACES;++i){
        X4CoverRaster *s=&r.surface[i];unsigned y=s->height/2;
        CHECK(s->rgba[(size_t)y*s->width]==0xff0000ffu); /* opaque red */
        CHECK(s->rgba[(size_t)y*s->width+s->width-1]==0x80ff0000u); /* half-alpha blue */
        CHECK(s->rgba[0]==0xff30261du); /* letterbox RGB #1d2630 */
    }
    x4_cover_rasters_free(&r);free(b);
}
static void test_priority_metadata_before_cover(void)
{
    fake_reset();atomic_store(&block_image,1);X4CatalogAssets*a=create_assets(NULL);
    const char*ids[]={"SELECTED9","VISIBLE2","VISIBLE3","ADJACENT4"};
    request_resume(a,ids,4);wait_flag(&image_entered);
    CHECK(!strcmp(metadata_order[0],"SELECTED9,VISIBLE2,VISIBLE3,ADJACENT4"));
    CHECK(!strcmp(image_order[0],"SELECTED9"));
    X4ProductMetadata m[24];uint64_t revision=0;
    size_t count=0;for(unsigned i=0;i<2000&&count!=4;++i){count=x4_catalog_assets_copy_metadata(a,m,24,&revision);if(count!=4)sleep_ms(1);}
    CHECK(count==4&&revision);CHECK(m[0].name[0]);
    X4CatalogAssetView v;CHECK(wait_acquire(a,"SELECTED9",&v));
    CHECK(v.metadata.name[0]&&!v.surface[0].rgba&&v.state==X4_ASSET_DOWNLOADING);
    /* Borrowing a metadata-only view must not make the worker throw away a
     * newly decoded immutable cover or burn all network retries. */
    atomic_store(&block_image,0);wait_ready(a,4);
    CHECK(!v.surface[0].rgba);x4_catalog_assets_release(a,&v);
    CHECK(atomic_load(&json_calls)==1);CHECK(atomic_load(&image_calls)==4);
    CHECK(!strcmp(image_order[1],"VISIBLE2"));CHECK(!strcmp(image_order[3],"ADJACENT4"));
    X4AssetEvent events[X4_ASSET_EVENT_CAP];size_t n=0;
    for(unsigned i=0;i<2000&&!n;++i){n=x4_catalog_assets_events(a,events,X4_ASSET_EVENT_CAP);if(!n)sleep_ms(1);}
    CHECK(n&&n<=X4_ASSET_EVENT_CAP);unsigned stage_bits=0;
    for(size_t i=0;i<n;++i){CHECK(events[i].stage<X4_ASSET_STAGE_COUNT);CHECK(events[i].source<=X4_ASSET_NETWORK);
        CHECK(events[i].end_us>=events[i].start_us);CHECK(x4_product_id_valid(events[i].product_id));stage_bits|=1u<<events[i].stage;}
    CHECK(stage_bits==(1u<<X4_ASSET_STAGE_COUNT)-1u);close_assets(a);
}
static void test_batches_large_and_partial(void)
{
    const char*ids[]={"ID1","ID2","ID3","ID4"};
    fake_reset();atomic_store(&fake_batch_limit,1);X4CatalogAssets*a=create_assets(NULL);
    request_resume(a,ids,4);wait_ready(a,4);CHECK(stats(a).batch_splits>=1);CHECK(atomic_load(&json_calls)>4);
    CHECK(atomic_load(&image_calls)==4);close_assets(a);
    fake_reset();atomic_store(&fake_partial_multi,1);a=create_assets(NULL);
    request_resume(a,ids,4);wait_ready(a,4);CHECK(stats(a).partial_responses>=1);
    CHECK(atomic_load(&json_calls)>=2);CHECK(atomic_load(&image_calls)==4);close_assets(a);
    fake_reset();atomic_store(&fake_partial_multi,1);atomic_store(&fake_single_missing,1);a=create_assets(NULL);
    request_resume(a,ids,4);X4CatalogAssetView v;wait_state(a,"ID2",X4_ASSET_TEMPORARY_FAILURE,&v);
    CHECK(!v.surface[0].rgba);x4_catalog_assets_release(a,&v);
    wait_state(a,"ID1",X4_ASSET_READY,&v);CHECK(v.metadata.name[0]);x4_catalog_assets_release(a,&v);
    CHECK(atomic_load(&json_calls)<=5);close_assets(a);
}
static void test_navigation_defers_obsolete_next_stage(void)
{
    fake_reset();atomic_store(&block_json,1);X4CatalogAssets*a=create_assets(NULL);
    const char*old[]={"OLD1","SHARED"};request_resume(a,old,2);wait_entered();
    const char*now[]={"SHARED","FRESH"};x4_catalog_assets_request(a,now,2);
    atomic_store(&block_json,0);wait_ready(a,2);CHECK(!strcmp(image_order[0],"SHARED"));
    for(unsigned i=0;i<atomic_load(&image_calls);++i)CHECK(strcmp(image_order[i],"OLD1"));
    CHECK(stats(a).deferred_stages>=1);X4CatalogAssetView v;wait_state(a,"SHARED",X4_ASSET_READY,&v);
    CHECK(v.surface[0].rgba);x4_catalog_assets_release(a,&v);close_assets(a);
}
static void test_retry_after_and_manual_retry(void)
{
    fake_reset();atomic_store(&fake_image_status,503);atomic_store(&fake_retry_ms,2000);
    X4CatalogAssets*a=create_assets(NULL);const char*ids[]={"RETRY1"};request_resume(a,ids,1);
    X4CatalogAssetView v;wait_state(a,"RETRY1",X4_ASSET_TEMPORARY_FAILURE,&v);
    CHECK(v.metadata.name[0]&&!v.surface[0].rgba);x4_catalog_assets_release(a,&v);
    CHECK(atomic_load(&json_calls)==1&&atomic_load(&image_calls)==1);
    atomic_store(&fake_image_status,200);x4_catalog_assets_retry(a);advance_ms(1999);sleep_ms(50);
    CHECK(atomic_load(&image_calls)==1);advance_ms(1);wait_ready(a,1);
    CHECK(atomic_load(&image_calls)==2);CHECK(atomic_load(&json_calls)==1);CHECK(stats(a).retries>=1);close_assets(a);
    /* A corrupt image is temporary, with bounded retries. A manual retry
     * after exhaustion reuses already-published metadata. */
    fake_reset();atomic_store(&fake_corrupt,1);a=create_assets(NULL);request_resume(a,ids,1);
    for(unsigned attempt=1;attempt<=3;++attempt){
        for(unsigned i=0;i<2000&&atomic_load(&image_calls)<attempt;++i)sleep_ms(1);
        CHECK(atomic_load(&image_calls)==attempt);wait_state(a,"RETRY1",X4_ASSET_TEMPORARY_FAILURE,&v);x4_catalog_assets_release(a,&v);
        if(attempt<3)advance_ms(attempt==1?1000:4000);
    }
    advance_ms(100000);sleep_ms(50);CHECK(atomic_load(&image_calls)==3);
    atomic_store(&fake_corrupt,0);x4_catalog_assets_retry(a);wait_ready(a,1);
    CHECK(atomic_load(&image_calls)==4&&atomic_load(&json_calls)==1);close_assets(a);
    /* Invalid/not-found metadata is permanent rather than a retry storm. */
    fake_reset();atomic_store(&fake_missing,1);a=create_assets(NULL);request_resume(a,ids,1);
    wait_state(a,"RETRY1",X4_ASSET_MISSING,&v);CHECK(!v.surface[0].rgba);x4_catalog_assets_release(a,&v);
    advance_ms(100000);x4_catalog_assets_retry(a);sleep_ms(50);CHECK(atomic_load(&json_calls)==1&&!atomic_load(&image_calls));close_assets(a);
}
static void test_ram_eviction_pinning_generation(void)
{
    fake_reset();X4CatalogAssets*a=create_assets(NULL);sleep_ms(20);CHECK(!atomic_load(&json_calls));
    const char*ids[]={"ID1","ID2","../bad","ID1"};x4_catalog_assets_request(a,ids,4);
    CHECK(!atomic_load(&json_calls));x4_catalog_assets_resume(a);wait_ready(a,2);
    X4CatalogAssetView v;CHECK(wait_acquire(a,"ID1",&v));CHECK(v.surface[0].rgba);CHECK(stats(a).pinned==1);
    x4_catalog_assets_release(a,&v);CHECK(stats(a).pinned==0);
    char names[30][32];const char*many[30];for(unsigned i=0;i<30;++i){snprintf(names[i],32,"PROD%u",i);many[i]=names[i];}
    x4_catalog_assets_request(a,many,30);wait_ready(a,26);X4CatalogAssetStats s=stats(a);
    CHECK(s.occupied==24);CHECK(s.thumbnail_bytes==24u*(320u*224u+512u*288u+96u*96u)*4u);
    CHECK(s.thumbnail_bytes<X4_CATALOG_THUMBNAIL_BUDGET);CHECK(s.events_omitted>0);
    CHECK(!x4_catalog_assets_acquire(a,"PROD29",&v));
    CHECK(wait_acquire(a,"PROD0",&v));uint32_t*held=v.surface[0].rgba;
    const char*other[]={"OTHER"};x4_catalog_assets_request(a,other,1);wait_ready(a,27);
    CHECK(held[112*320+160]!=0xff30261du);x4_catalog_assets_release(a,&v);
    CHECK(wait_acquire(a,"OTHER",&v));held=v.surface[0].rgba;
    x4_catalog_assets_suspend(a);wait_idle(a);unsigned queries=atomic_load(&json_calls),images=atomic_load(&image_calls);
    x4_catalog_assets_set_generation(a,2);X4CatalogAssetView current;CHECK(!x4_catalog_assets_acquire(a,"OTHER",&current));
    x4_catalog_assets_request(a,other,1);CHECK(held[112*320+160]!=0xff30261du);
    x4_catalog_assets_release(a,&v);x4_catalog_assets_resume(a);wait_state(a,"OTHER",X4_ASSET_READY,&current);
    CHECK(current.generation==2);x4_catalog_assets_release(a,&current);
    CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);close_assets(a);
}
static void test_old_account_result_and_cancel(void)
{
    fake_reset();atomic_store(&block_json,1);atomic_store(&ignore_cancel,1);
    X4CatalogAssets*a=create_assets(NULL);const char*old[]={"OLDGEN"};request_resume(a,old,1);wait_entered();
    CHECK(!x4_catalog_assets_idle(a));x4_catalog_assets_set_generation(a,2);
    const char*fresh[]={"FRESHGEN"};x4_catalog_assets_request(a,fresh,1);atomic_store(&block_json,0);
    wait_ready(a,1);atomic_store(&ignore_cancel,0);CHECK(stats(a).stale_results>=1);
    X4CatalogAssetView v;CHECK(!x4_catalog_assets_acquire(a,"OLDGEN",&v));wait_state(a,"FRESHGEN",X4_ASSET_READY,&v);
    CHECK(v.generation==2);x4_catalog_assets_release(a,&v);
    atomic_store(&block_json,1);atomic_store(&entered,0);const char*cancelled[]={"CANCELME"};
    x4_catalog_assets_request(a,cancelled,1);wait_entered();x4_catalog_assets_suspend(a);wait_idle(a);
    CHECK(stats(a).cancelled>=1);atomic_store(&block_json,0);close_assets(a);
}
static void test_auth_boundary_waits_for_cleanup(void)
{
    fake_reset();atomic_store(&block_image,1);X4CatalogAssets*a=create_assets(NULL);
    const char*ids[]={"AUTHBOUNDARY"};request_resume(a,ids,1);wait_flag(&image_entered);
    atomic_store(&block_reset,1);x4_catalog_assets_suspend(a);wait_flag(&reset_entered);
    CHECK(!x4_catalog_assets_idle(a));CHECK(!atomic_load(&active_requests));
    sleep_ms(30);CHECK(!x4_catalog_assets_idle(a));
    /* The foreground login/session may begin only after this cleanup
     * contract. This is a local gate test, not a Microsoft login. */
    atomic_store(&block_reset,0);wait_idle(a);CHECK(atomic_load(&reset_calls)>=1);
    unsigned images=atomic_load(&image_calls);sleep_ms(30);CHECK(atomic_load(&image_calls)==images);
    atomic_store(&block_image,0);close_assets(a);
}
static void cache_directory_action(const char*root,int corrupt)
{
    CHECK(!strncmp(root,"/tmp/x4-assets-cache.",21));DIR*d=opendir(root);CHECK(d);struct dirent*entry;
    while((entry=readdir(d))){if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        CHECK(!strncmp(entry->d_name,"x4c-",4));char path[512];CHECK(snprintf(path,sizeof(path),"%s/%s",root,entry->d_name)<(int)sizeof(path));
        if(corrupt){FILE*f=fopen(path,"wb");CHECK(f);CHECK(fwrite("corrupt",1,7,f)==7);CHECK(!fclose(f));}
        else CHECK(!unlink(path));
    }
    CHECK(!closedir(d));
}
static void test_disk_reopen_and_corrupt_fallback(void)
{
    fake_reset();char root[]="/tmp/x4-assets-cache.XXXXXX";CHECK(mkdtemp(root));
    const char*ids[]={"CACHE1","CACHE2"};X4CatalogAssets*a=create_assets(root);request_resume(a,ids,2);wait_ready(a,2);close_assets(a);
    unsigned queries=atomic_load(&json_calls),images=atomic_load(&image_calls);
    atomic_store(&open_calls,0);
    a=create_assets(root);x4_catalog_assets_set_generation(a,2);request_resume(a,ids,2);wait_ready(a,2);
    CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);
    CHECK(!atomic_load(&open_calls));
    CHECK(stats(a).disk_metadata_hits==2&&stats(a).disk_image_hits==2);close_assets(a);
    cache_directory_action(root,1);a=create_assets(root);request_resume(a,ids,2);wait_ready(a,2);
    CHECK(atomic_load(&json_calls)>queries&&atomic_load(&image_calls)>images);close_assets(a);
    cache_directory_action(root,0);CHECK(!rmdir(root));
}
static X4CatalogAssets *create_realtime_assets(const char*root)
{
    X4CatalogAssetsConfig config={root,NULL,NULL};X4CatalogAssets*a=x4_catalog_assets_create_config(&config);
    CHECK(a);CHECK(x4_catalog_assets_idle(a));return a;
}
static uint64_t wall_monotonic_us(void)
{
    struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*UINT64_C(1000000)+(uint64_t)t.tv_nsec/1000u;
}
static void test_cleanup_fault_is_fail_closed_and_destroyable(void)
{
    fake_reset();X4CatalogAssets*a=create_assets(NULL);const char*ids[]={"FAULTCHECK"};
    request_resume(a,ids,1);wait_ready(a,1);CHECK(x4_catalog_assets_fault(a)==X4_ASSET_FAULT_NONE);
    unsigned queries=atomic_load(&json_calls),images=atomic_load(&image_calls),opens=atomic_load(&open_calls);
    atomic_store(&fake_reset_error,X4_HTTP_UNUSABLE);x4_catalog_assets_suspend(a);
    for(unsigned i=0;i<5000&&x4_catalog_assets_fault(a)!=X4_ASSET_FAULT_CLEANUP;++i)sleep_ms(1);
    CHECK(x4_catalog_assets_fault(a)==X4_ASSET_FAULT_CLEANUP);
    CHECK(!x4_catalog_assets_idle(a)&&stats(a).cleanup_failures==1);
    x4_catalog_assets_resume(a);const char*later[]={"AFTERFAULT"};x4_catalog_assets_request(a,later,1);sleep_ms(50);
    CHECK(x4_catalog_assets_fault(a)==X4_ASSET_FAULT_CLEANUP);
    CHECK(!x4_catalog_assets_idle(a)&&stats(a).cleanup_failures==1);
    CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images&&atomic_load(&open_calls)==opens);
    uint64_t start=wall_monotonic_us();x4_catalog_assets_destroy(a);uint64_t end=wall_monotonic_us();
    CHECK(end-start<UINT64_C(2000000));CHECK(!atomic_load(&active_http)&&!atomic_load(&active_requests));
}
static void test_lowercase_disk_reopen_preserves_job_identity(void)
{
    fake_reset();char root[]="/tmp/x4-assets-cache.XXXXXX";CHECK(mkdtemp(root));
    const char*ids[]={"lowercase1"};X4CatalogAssets*a=create_realtime_assets(root);
    request_resume(a,ids,1);wait_ready(a,1);close_assets(a);
    unsigned queries=atomic_load(&json_calls),images=atomic_load(&image_calls);
    atomic_store(&open_calls,0);
    a=create_realtime_assets(root);x4_catalog_assets_set_generation(a,2);request_resume(a,ids,1);wait_ready(a,1);
    X4CatalogAssetView v;wait_state(a,"lowercase1",X4_ASSET_READY,&v);
    CHECK(!strcmp(v.metadata.product_id,"lowercase1"));CHECK(v.generation==2&&v.surface[0].rgba);
    x4_catalog_assets_release(a,&v);CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);
    CHECK(!atomic_load(&open_calls));
    CHECK(stats(a).disk_metadata_hits==1&&stats(a).disk_image_hits==1&&!stats(a).busy);
    /* Completing a cached, normalized metadata record must clear its job's
     * busy bit. A subsequent unrelated job verifies the worker can progress. */
    const char*following[]={"following2"};x4_catalog_assets_request(a,following,1);wait_ready(a,2);
    wait_state(a,"following2",X4_ASSET_READY,&v);x4_catalog_assets_release(a,&v);
    CHECK(atomic_load(&json_calls)==queries+1&&atomic_load(&image_calls)==images+1);close_assets(a);
    cache_directory_action(root,0);CHECK(!rmdir(root));
}
static void measure_page(X4CatalogAssets*a,const char*scenario,unsigned page,
    const char*const*ids,unsigned count,unsigned covers)
{
    X4CatalogAssetStats before=stats(a);uint64_t start=wall_monotonic_us();
    unsigned open_before=atomic_load(&open_calls);
    request_resume(a,ids,count);wait_ready(a,covers);
    for(unsigned i=0;i<count;++i){X4CatalogAssetView v;wait_state(a,ids[i],X4_ASSET_READY,&v);
        CHECK(v.surface[0].rgba&&v.surface[1].rgba&&v.surface[2].rgba);x4_catalog_assets_release(a,&v);}
    uint64_t end=wall_monotonic_us();X4CatalogAssetStats after=stats(a);
    static const char*stage_names[]={"queue","metadata","image","decode_scale","publish"};
    printf("MEASUREMENT {\"scenario\":\"%s\",\"environment\":\"host_fake_public_http\",\"page\":%u,"
        "\"visible_cards\":6,\"demanded_cards\":%u,\"covers_ready_cumulative\":%llu,\"wall_monotonic_us\":%llu,"
        "\"http_open_delta\":%u,\"metadata_request_delta\":%llu,\"image_request_delta\":%llu,\"ram_hit_delta\":%llu,"
        "\"disk_metadata_hit_delta\":%llu,\"disk_image_hit_delta\":%llu,\"events_omitted_delta\":%llu,\"stage_delta\":[",
        scenario,page,count,(unsigned long long)after.covers_ready,(unsigned long long)(end-start),
        atomic_load(&open_calls)-open_before,
        (unsigned long long)(after.metadata_network_requests-before.metadata_network_requests),
        (unsigned long long)(after.image_network_requests-before.image_network_requests),
        (unsigned long long)(after.ram_hits-before.ram_hits),
        (unsigned long long)(after.disk_metadata_hits-before.disk_metadata_hits),
        (unsigned long long)(after.disk_image_hits-before.disk_image_hits),
        (unsigned long long)(after.events_omitted-before.events_omitted));
    for(unsigned i=0;i<X4_ASSET_STAGE_COUNT;++i)
        printf("%s{\"stage\":\"%s\",\"count\":%llu,\"total_us\":%llu,\"max_cumulative_us\":%llu}",i?",":"",stage_names[i],
            (unsigned long long)(after.stage[i].count-before.stage[i].count),
            (unsigned long long)(after.stage[i].total_us-before.stage[i].total_us),
            (unsigned long long)after.stage[i].max_us);
    puts("]}");
}
static void test_six_card_loading_comparison(void)
{
    fake_reset();char root[]="/tmp/x4-assets-cache.XXXXXX";CHECK(mkdtemp(root));
    char product[12][32];const char*ids[12];for(unsigned i=0;i<12;++i){snprintf(product[i],32,"CARD%u",i);ids[i]=product[i];}
    X4CatalogAssets*a=create_realtime_assets(root);
    measure_page(a,"cold_page",1,ids,6,6);CHECK(atomic_load(&json_calls)==2&&atomic_load(&image_calls)==6);
    measure_page(a,"preload_adjacent",1,ids,12,12);CHECK(atomic_load(&json_calls)==4&&atomic_load(&image_calls)==12);
    unsigned queries=atomic_load(&json_calls),images=atomic_load(&image_calls);
    measure_page(a,"preloaded_page",2,ids+6,6,12);CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);
    measure_page(a,"revisit_page",1,ids,6,12);CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);
    close_assets(a);atomic_store(&open_calls,0);a=create_realtime_assets(root);x4_catalog_assets_set_generation(a,2);
    measure_page(a,"reopen_app",1,ids,6,6);
    CHECK(atomic_load(&json_calls)==queries&&atomic_load(&image_calls)==images);
    CHECK(!atomic_load(&open_calls));
    CHECK(stats(a).disk_metadata_hits==6&&stats(a).disk_image_hits==6);close_assets(a);
    cache_directory_action(root,0);CHECK(!rmdir(root));
}
int main(int argc,char **argv)
{
    CHECK(argc==5);fake_image=read_file(argv[2],&fake_image_length);test_text();test_policy_metadata(argv[1]);test_images(argv[3]);test_color_layout(argv[4]);
    test_priority_metadata_before_cover();test_batches_large_and_partial();test_navigation_defers_obsolete_next_stage();
    test_retry_after_and_manual_retry();test_ram_eviction_pinning_generation();test_old_account_result_and_cancel();
    test_auth_boundary_waits_for_cleanup();test_disk_reopen_and_corrupt_fallback();
    test_cleanup_fault_is_fail_closed_and_destroyable();test_lowercase_disk_reopen_preserves_job_identity();
    test_six_card_loading_comparison();free(fake_image);
    printf("catalog assets PASS: 15 test groups, %u checks (fake public HTTP; no real network)\n",atomic_load(&checks));return 0;
}
