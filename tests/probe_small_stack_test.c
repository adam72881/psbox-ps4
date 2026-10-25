/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual pinned MbedTLS hash + actual source reader, synthetic AnnexB bytes.
 * Stack size is a caller-specified POSIX reference, with its evidence origin.
 * This proves host stack/lifetime behavior, not a PS4 decoder or its timing. */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "probe_hash.h"
#include "probe_files.h"
#include "probe_metrics.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#ifndef X4_TEST_STACK_BYTES
#error Supply X4_TEST_STACK_BYTES explicitly
#endif
#ifndef X4_TEST_STACK_ORIGIN
#error Supply X4_TEST_STACK_ORIGIN explicitly
#endif
static atomic_uint checks;
#define CHECK(x) do { atomic_fetch_add(&checks,1); assert(x); } while (0)
enum { AU_BYTES=98321, AU_COUNT=3 };
typedef struct {
    X4ProbeHashContext hash;
    X4ProbeMetrics *metrics;
    char root[80], samples[112], clip[160], index[160];
    char report[X4_PROBE_FILE_PATH_MAX];
    uint8_t clip_hash[32], index_hash[32];
    uint8_t *au;
    atomic_bool cancel;
} Fixture;
static void put32(uint8_t *p,uint32_t value)
{ for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(i*8)); }
static void put64(uint8_t *p,uint64_t value)
{ for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(value>>(i*8)); }
static void digest(Fixture *f,const char *path,uint64_t bytes,uint8_t out[32])
{
    FILE *file=fopen(path,"rb");CHECK(file);
    CHECK(!x4_probe_hash_file(file,bytes,out,&f->cancel,&f->hash));CHECK(!fclose(file));
}
static void prepare(Fixture *f)
{
    memset(f,0,sizeof(*f));atomic_init(&f->cancel,false);
    strcpy(f->root,"/tmp/x4-probe-stack-test.XXXXXX");CHECK(mkdtemp(f->root));
    CHECK(!x4_probe_files_test_root(f->root));
    CHECK(snprintf(f->samples,sizeof(f->samples),"%s/xcloud4-probe",f->root)>0);
    CHECK(!mkdir(f->samples,0700));
    CHECK(snprintf(f->clip,sizeof(f->clip),"%s/control-720p30-cb.h264",f->samples)>0);
    CHECK(snprintf(f->index,sizeof(f->index),"%s/control-720p30-cb.x4au",f->samples)>0);
    CHECK(!x4_probe_hash_context_init(&f->hash));
    f->metrics=x4_probe_metrics_create(1);CHECK(f->metrics);
    CHECK(!x4_probe_files_report_path(f->report,sizeof(f->report),"stack-metrics",1));
    f->au=malloc(AU_BYTES);CHECK(f->au);
    FILE *clip=fopen(f->clip,"wb");CHECK(clip);
    for(unsigned n=0;n<AU_COUNT;++n) {
        for(unsigned i=0;i<AU_BYTES;++i)f->au[i]=(uint8_t)(i*31u+n*17u+7u);
        f->au[0]=f->au[1]=f->au[2]=0;f->au[3]=1;f->au[4]=9;
        CHECK(fwrite(f->au,1,AU_BYTES,clip)==AU_BYTES);
    }
    CHECK(!fclose(clip));digest(f,f->clip,(uint64_t)AU_COUNT*AU_BYTES,f->clip_hash);
    uint8_t header[96]={0};memcpy(header,"X4AU1\0\0\0",8);
    put32(header+8,1);put32(header+12,96);put64(header+16,(uint64_t)AU_COUNT*AU_BYTES);
    put64(header+24,AU_COUNT);put64(header+32,100000);
    put32(header+40,1280);put32(header+44,720);put32(header+48,1280);put32(header+52,720);
    put32(header+56,30);put32(header+60,1);memcpy(header+64,f->clip_hash,32);
    FILE *index=fopen(f->index,"wb");CHECK(index);CHECK(fwrite(header,1,96,index)==96);
    for(unsigned n=0;n<AU_COUNT;++n) {
        uint8_t record[24];put64(record,(uint64_t)n*AU_BYTES);put64(record+8,AU_BYTES);put64(record+16,n*33333u);
        CHECK(fwrite(record,1,24,index)==24);
    }
    CHECK(!fclose(index));digest(f,f->index,96+AU_COUNT*24,f->index_hash);
    /* Real console FTP observations: metadata cannot establish byte length.
     * The untouched source uses fseeko/ftello and complete hash verification. */
    x4_probe_files_test_stat(1,S_IFREG|0777,0,16);
    x4_probe_files_test_manifest(1);
}
static void *worker(void *opaque)
{
    Fixture *f=opaque;uint8_t digest_value[32];uint8_t *reserved=f->hash.buffer;
    FILE *file=fopen(f->clip,"rb");CHECK(file);
    for(unsigned repeat=0;repeat<4;++repeat) {
        CHECK(!fseeko(file,0,SEEK_SET));
        CHECK(!x4_probe_hash_file(file,(uint64_t)AU_COUNT*AU_BYTES,digest_value,&f->cancel,&f->hash));
        CHECK(!memcmp(digest_value,f->clip_hash,32));CHECK(f->hash.buffer==reserved);
    }
    CHECK(!fseeko(file,0,SEEK_SET));atomic_store(&f->cancel,true);
    CHECK(x4_probe_hash_file(file,(uint64_t)AU_COUNT*AU_BYTES,digest_value,&f->cancel,&f->hash)==X4_PROBE_CANCELLED);
    atomic_store(&f->cancel,false);CHECK(!fseeko(file,0,SEEK_SET));
    CHECK(x4_probe_hash_file(file,(uint64_t)AU_COUNT*AU_BYTES+1,digest_value,&f->cancel,&f->hash)==X4_PROBE_IO);
    CHECK(!fclose(file));
    X4ProbeSource source={0};
    CHECK(!x4_probe_source_open_fully_verified(&source,f->clip,f->index,f->clip_hash,f->index_hash,
        x4_probe_hash_file,&f->hash,&f->cancel));
    CHECK(source.stats.hash_verified&&source.stats.index_hash_verified);
    CHECK(source.info.file_bytes==(uint64_t)AU_BYTES*AU_COUNT);
    CHECK(!x4_probe_source_begin(&source,100,X4_PROBE_MAX_SPEED));
    for(unsigned n=0;n<AU_COUNT;++n) {
        X4ProbeAU au={0};CHECK(!x4_probe_source_next(&source,100+n*33333u,f->au,AU_BYTES,&au));
        CHECK(au.ordinal==n&&au.bytes==AU_BYTES);
    }
    X4ProbeAU au={0};CHECK(x4_probe_source_next(&source,100000,f->au,AU_BYTES,&au)==X4_PROBE_EOF);
    CHECK(!x4_probe_source_close(&source));CHECK(f->hash.buffer==reserved);
    file=fopen(f->clip,"r+b");CHECK(file);CHECK(!fseeko(file,AU_BYTES+17,SEEK_SET));CHECK(fputc(0xee,file)!=EOF);CHECK(!fclose(file));
    CHECK(x4_probe_source_open_fully_verified(&source,f->clip,f->index,f->clip_hash,f->index_hash,
        x4_probe_hash_file,&f->hash,&f->cancel)==X4_PROBE_HASH);
    CHECK(!source.state);CHECK(f->hash.buffer==reserved);
    /* Exercise the actual heap-backed summary and export on the same small
     * host-reference stack. Reports keep their real regular/nlink1 policy. */
    x4_probe_files_test_stat(0,0,0,0);
    x4_probe_metrics_end(f->metrics,2,0);
    X4ProbeMetricsSummary *summary=x4_probe_metrics_summary_storage(f->metrics);
    CHECK(summary);CHECK(!x4_probe_metrics_summary(f->metrics,summary));
    CHECK(summary==x4_probe_metrics_summary_storage(f->metrics));
    CHECK(!x4_probe_metrics_dump_file(f->metrics,f->report));
    return NULL;
}
int main(void)
{
    /* Context and AU reside outside the limited worker stack before creation. */
    Fixture *f=calloc(1,sizeof(*f));CHECK(f);prepare(f);
    pthread_attr_t attr;CHECK(!pthread_attr_init(&attr));
    CHECK(!pthread_attr_setstacksize(&attr,X4_TEST_STACK_BYTES));
    size_t configured=0;CHECK(!pthread_attr_getstacksize(&attr,&configured));CHECK(configured==X4_TEST_STACK_BYTES);
    pthread_t thread;CHECK(!pthread_create(&thread,&attr,worker,f));CHECK(!pthread_join(thread,NULL));CHECK(!pthread_attr_destroy(&attr));
    x4_probe_hash_context_close(&f->hash);CHECK(!f->hash.buffer&&!f->hash.capacity);
    x4_probe_metrics_free(f->metrics);
    CHECK(!unlink(f->report));CHECK(!unlink(f->clip));CHECK(!unlink(f->index));CHECK(!rmdir(f->samples));CHECK(!rmdir(f->root));free(f->au);free(f);
    printf("PASS HOST real hash/source small-stack bytes=%zu origin=%s checks=%u; synthetic data, no native decoder, not PS4\n",configured,X4_TEST_STACK_ORIGIN,atomic_load(&checks));
    return 0;
}
