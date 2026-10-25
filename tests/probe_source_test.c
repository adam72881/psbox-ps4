/* SPDX-License-Identifier: GPL-3.0-only */
/* AU reader fixtures are synthetic bytes, not decodable video. The test hash
 * binding checks callback/identity enforcement, not SHA-256 correctness. The
 * native binding and generated samples receive separate hash verification. */
#define _POSIX_C_SOURCE 200809L
#include "probe_source.h"
#include "probe_files.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static unsigned checks;
static void check(bool value, const char *expression, unsigned line)
{
    ++checks;
    if (!value) { fprintf(stderr,"FAIL %u: %s\n",line,expression); exit(1); }
}
#define CHECK(x) check(!!(x),#x,__LINE__)

static void pacing(void)
{
    uint64_t wait=99, late=99;
    CHECK(x4_probe_schedule(100,0,100,X4_PROBE_PACED,&wait,&late)==0 && !wait && !late);
    CHECK(x4_probe_schedule(100,16667,100,X4_PROBE_PACED,&wait,&late)==X4_PROBE_WAIT && wait==16667 && !late);
    CHECK(x4_probe_schedule(100,16667,16767,X4_PROBE_PACED,&wait,&late)==0 && !wait && !late);
    CHECK(x4_probe_schedule(100,16667,16800,X4_PROBE_PACED,&wait,&late)==0 && !wait && late==33);
    CHECK(x4_probe_schedule(100,16667,100,X4_PROBE_MAX_SPEED,&wait,&late)==0 && !wait && !late);
    CHECK(x4_probe_schedule(100,16667,16800,X4_PROBE_MAX_SPEED,&wait,&late)==0 && late==33);
    CHECK(x4_probe_schedule(UINT64_MAX,1,UINT64_MAX,X4_PROBE_PACED,&wait,&late)==X4_PROBE_CLOCK);
    CHECK(x4_probe_schedule(100,0,99,X4_PROBE_PACED,&wait,&late)==X4_PROBE_CLOCK);
    CHECK(x4_probe_schedule(0,0,0,(X4ProbeMode)2,&wait,&late)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_schedule(0,0,0,X4_PROBE_PACED,NULL,&late)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_schedule(0,0,0,X4_PROBE_PACED,&wait,NULL)==X4_PROBE_ARGUMENT);
}

#if X4_VIDEO_PROBE
typedef struct { char dir[80],samples[112],clip[128],index[128]; uint8_t expected[32]; unsigned count,size; } Fixture;
typedef struct { unsigned calls; int result; bool cancel_in_hash; atomic_bool *cancel; } HashContext;
static void put32(uint8_t *p,uint32_t value) { for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(value>>(i*8)); }
static void put64(uint8_t *p,uint64_t value) { for(unsigned i=0;i<8;++i) p[i]=(uint8_t)(value>>(i*8)); }
static int test_hash(FILE *file,uint64_t bytes,uint8_t actual[32],const atomic_bool *cancel,void *context)
{
    HashContext *hash=context;
    if(hash) {
        ++hash->calls;
        if(hash->cancel_in_hash) { if(hash->cancel) atomic_store(hash->cancel,true); return X4_PROBE_CANCELLED; }
        if(hash->result) return hash->result;
    }
    uint64_t value=UINT64_C(0xcbf29ce484222325),total=0;
    uint8_t block[257];
    while(total<bytes) {
        if(cancel && atomic_load(cancel)) return X4_PROBE_CANCELLED;
        size_t want=bytes-total<sizeof(block)?(size_t)(bytes-total):sizeof(block);
        size_t received=fread(block,1,want,file);
        if(received!=want) return X4_PROBE_IO;
        for(size_t i=0;i<received;++i) { value^=block[i];value*=UINT64_C(0x100000001b3); }
        total+=received;
    }
    for(unsigned i=0;i<32;++i) actual[i]=(uint8_t)(value>>((i&7u)*8));
    return 0;
}
static Fixture fixture(unsigned count,unsigned size,bool annexb)
{
    Fixture f={.dir="/tmp/x4-probe-source-test.XXXXXX",.count=count,.size=size};
    CHECK(mkdtemp(f.dir));
    CHECK(!x4_probe_files_test_root(f.dir));
    CHECK(snprintf(f.samples,sizeof(f.samples),"%s/xcloud4-probe",f.dir)>0);CHECK(!mkdir(f.samples,0700));
    CHECK(snprintf(f.clip,sizeof(f.clip),"%s/clip.h264",f.samples)>0);
    CHECK(snprintf(f.index,sizeof(f.index),"%s/clip.x4au",f.samples)>0);
    FILE *clip=fopen(f.clip,"wb");CHECK(clip);
    uint8_t block[4096];
    for(unsigned n=0;n<count;++n) {
        size_t remaining=size,position=0;
        while(remaining) {
            size_t bytes=remaining<sizeof(block)?remaining:sizeof(block);
            for(size_t i=0;i<bytes;++i) block[i]=(uint8_t)((position+i)*31u+n*17u+7u);
            if(!position && annexb && bytes>=5) { block[0]=block[1]=block[2]=0;block[3]=1;block[4]=9; }
            CHECK(fwrite(block,1,bytes,clip)==bytes);remaining-=bytes;position+=bytes;
        }
    }
    CHECK(!fclose(clip));clip=fopen(f.clip,"rb");CHECK(clip);
    CHECK(!test_hash(clip,(uint64_t)count*size,f.expected,NULL,NULL));CHECK(!fclose(clip));
    uint8_t header[96]={0};memcpy(header,"X4AU1\0\0\0",8);put32(header+8,1);put32(header+12,96);
    put64(header+16,(uint64_t)count*size);put64(header+24,count);put64(header+32,(uint64_t)count*33333+1);
    put32(header+40,1280);put32(header+44,720);put32(header+48,1280);put32(header+52,720);
    put32(header+56,30);put32(header+60,1);memcpy(header+64,f.expected,32);
    FILE *index=fopen(f.index,"wb");CHECK(index);CHECK(fwrite(header,1,sizeof(header),index)==sizeof(header));
    for(unsigned n=0;n<count;++n) {
        uint8_t record[24];put64(record,(uint64_t)n*size);put64(record+8,size);put64(record+16,(uint64_t)n*33333);
        CHECK(fwrite(record,1,sizeof(record),index)==sizeof(record));
    }
    CHECK(!fclose(index));return f;
}
static void remove_fixture(Fixture *f)
{ CHECK(!unlink(f->clip));CHECK(!unlink(f->index));CHECK(!rmdir(f->samples));CHECK(!rmdir(f->dir)); }
static void change_field(const char *path,unsigned offset,uint64_t value,unsigned bytes)
{
    FILE *file=fopen(path,"r+b");CHECK(file);CHECK(!fseek(file,offset,SEEK_SET));
    uint8_t field[8];put64(field,value);CHECK(fwrite(field,1,bytes,file)==bytes);CHECK(!fclose(file));
}
static int open_source(X4ProbeSource *source,Fixture *f,HashContext *ctx,const atomic_bool *cancel)
{ return x4_probe_source_open_verified(source,f->clip,f->index,f->expected,test_hash,ctx,cancel); }

static void normal_reader(void)
{
    Fixture f=fixture(4,19,true);X4ProbeSource source={0};HashContext hash={0};atomic_bool cancel;atomic_init(&cancel,false);
    CHECK(open_source(&source,&f,&hash,&cancel)==0 && hash.calls==1);
    CHECK(source.stats.hash_verified && source.stats.validated_records==4);
    CHECK(source.info.file_bytes==76 && source.info.visible_width==1280 && source.info.fps_num==30);
    CHECK(open_source(&source,&f,&hash,&cancel)==X4_PROBE_STATE && hash.calls==1);
    uint8_t output[35];memset(output,0xa5,sizeof(output));X4ProbeAU au;
    CHECK(x4_probe_source_next(&source,100,output,19,&au)==X4_PROBE_STATE);
    CHECK(x4_probe_source_begin(&source,100,X4_PROBE_PACED)==0);
    CHECK(x4_probe_source_begin(&source,100,X4_PROBE_PACED)==X4_PROBE_STATE);
    CHECK(x4_probe_source_next(&source,100,output,18,&au)==X4_PROBE_SIZE);
    for(size_t i=0;i<sizeof(output);++i) CHECK(output[i]==0xa5);
    CHECK(x4_probe_source_next(&source,100,output,19,&au)==0 && au.ordinal==0 && au.bytes==19 && au.pts_us==0);
    CHECK(output[0]==0 && output[3]==1 && output[4]==9);
    for(size_t i=19;i<sizeof(output);++i) CHECK(output[i]==0xa5);
    memset(output,0xa5,sizeof(output));
    CHECK(x4_probe_source_next(&source,101,output,19,&au)==X4_PROBE_WAIT && au.ordinal==1 && au.wait_us==33332);
    for(size_t i=0;i<sizeof(output);++i) CHECK(output[i]==0xa5);
    CHECK(x4_probe_source_next(&source,100,output,19,&au)==X4_PROBE_CLOCK);
    CHECK(x4_probe_source_next(&source,33433,output,19,&au)==0 && au.ordinal==1 && !au.late_us);
    CHECK(x4_probe_source_next(&source,67000,output,19,&au)==0 && au.ordinal==2 && au.late_us==234);
    CHECK(x4_probe_source_next(&source,200000,output,19,&au)==0 && au.ordinal==3 && au.late_us==99901);
    CHECK(x4_probe_source_next(&source,200000,output,19,&au)==X4_PROBE_EOF);
    CHECK(x4_probe_source_next(&source,200001,output,19,&au)==X4_PROBE_EOF);
    CHECK(source.stats.aus_read==4 && source.stats.bytes_read==76 && source.stats.read_calls==4);
    CHECK(source.stats.wait_observations==1 && source.stats.late_aus==2 && source.stats.max_late_us==99901);
    CHECK(x4_probe_source_close(&source)==0 && !source.state && source.stats.aus_read==4);
    CHECK(x4_probe_source_close(&source)==0);
    CHECK(open_source(&source,&f,&hash,&cancel)==0 && source.stats.aus_read==0 && hash.calls==2);
    CHECK(x4_probe_source_begin(&source,100,X4_PROBE_MAX_SPEED)==0);
    for(unsigned n=0;n<4;++n) CHECK(x4_probe_source_next(&source,100,output,19,&au)==0 && au.ordinal==n && !au.wait_us);
    CHECK(x4_probe_source_next(&source,100,output,19,&au)==X4_PROBE_EOF);
    CHECK(!x4_probe_source_close(&source));remove_fixture(&f);
}

static void bounded_large_clip(void)
{
    Fixture f=fixture(601,8192,true);X4ProbeSource source={0};CHECK(open_source(&source,&f,NULL,NULL)==0);
    CHECK(source.info.records==601 && source.info.file_bytes>4u*1024*1024);
    CHECK(!x4_probe_source_begin(&source,0,X4_PROBE_MAX_SPEED));
    uint8_t *output=malloc(8192+16);CHECK(output);memset(output+8192,0xa5,16);X4ProbeAU au;
    for(unsigned n=0;n<601;++n) CHECK(x4_probe_source_next(&source,0,output,8192,&au)==0 && au.ordinal==n);
    CHECK(x4_probe_source_next(&source,0,output,8192,&au)==X4_PROBE_EOF);
    CHECK(source.stats.aus_read==601 && source.stats.bytes_read==(uint64_t)601*8192);
    for(unsigned i=0;i<16;++i) CHECK(output[8192+i]==0xa5);
    CHECK(!x4_probe_source_close(&source));free(output);remove_fixture(&f);
    f=fixture(1,X4_PROBE_AU_MAX,true);source=(X4ProbeSource){0};CHECK(!open_source(&source,&f,NULL,NULL));
    CHECK(!x4_probe_source_begin(&source,0,X4_PROBE_MAX_SPEED));output=malloc(X4_PROBE_AU_MAX+16);CHECK(output);
    memset(output+X4_PROBE_AU_MAX,0xa5,16);
    CHECK(!x4_probe_source_next(&source,0,output,X4_PROBE_AU_MAX,&au));
    CHECK(source.stats.read_calls==X4_PROBE_AU_MAX/X4_PROBE_READ_CHUNK);
    for(unsigned i=0;i<16;++i) CHECK(output[X4_PROBE_AU_MAX+i]==0xa5);
    CHECK(!x4_probe_source_close(&source));free(output);remove_fixture(&f);
}

static void index_errors(void)
{
    const struct {unsigned offset,bytes;uint64_t value;int expected;} cases[] = {
        {0,1,'Y',X4_PROBE_INDEX},{8,4,2,X4_PROBE_INDEX},{12,4,64,X4_PROBE_INDEX},
        {16,8,0,X4_PROBE_INDEX},{24,8,0,X4_PROBE_INDEX},{24,8,X4_PROBE_MAX_RECORDS+1,X4_PROBE_INDEX},
        {32,8,0,X4_PROBE_INDEX},{32,8,X4_PROBE_DURATION_MAX_US+1,X4_PROBE_INDEX},
        {40,4,0,X4_PROBE_INDEX},{40,4,1922,X4_PROBE_INDEX},{44,4,719,X4_PROBE_INDEX},
        {48,4,1278,X4_PROBE_INDEX},{52,4,1089,X4_PROBE_INDEX},{56,4,0,X4_PROBE_INDEX},
        {56,4,121,X4_PROBE_INDEX},{60,4,0,X4_PROBE_INDEX},{60,4,1000001,X4_PROBE_INDEX},
        {64,1,0xff,X4_PROBE_HASH},{96,8,1,X4_PROBE_INDEX},{104,8,4,X4_PROBE_SIZE},
        {104,8,X4_PROBE_AU_MAX+1,X4_PROBE_SIZE},{112,8,1,X4_PROBE_INDEX},
        {120,8,18,X4_PROBE_INDEX},{136,8,0,X4_PROBE_INDEX},{136,8,UINT64_MAX,X4_PROBE_INDEX}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        Fixture f=fixture(2,19,true);change_field(f.index,cases[i].offset,cases[i].value,cases[i].bytes);
        X4ProbeSource source={0};HashContext hash={0};
        CHECK(open_source(&source,&f,&hash,NULL)==cases[i].expected && !source.state && !source.stats.hash_verified);
        CHECK(hash.calls==0);remove_fixture(&f);
    }
    Fixture f=fixture(2,19,true);CHECK(!truncate(f.index,95));X4ProbeSource source={0};
    CHECK(open_source(&source,&f,NULL,NULL)==X4_PROBE_SIZE);remove_fixture(&f);
    f=fixture(2,19,true);CHECK(!truncate(f.index,96+47));source=(X4ProbeSource){0};
    CHECK(open_source(&source,&f,NULL,NULL)==X4_PROBE_INDEX);remove_fixture(&f);
    f=fixture(2,19,true);CHECK(!truncate(f.clip,37));source=(X4ProbeSource){0};
    CHECK(open_source(&source,&f,NULL,NULL)==X4_PROBE_INDEX);remove_fixture(&f);
    f=fixture(2,19,true);change_field(f.clip,8,0x22,1);source=(X4ProbeSource){0};
    CHECK(open_source(&source,&f,NULL,NULL)==X4_PROBE_HASH && !source.state);remove_fixture(&f);
    f=fixture(2,19,true);uint8_t incorrect[32];memcpy(incorrect,f.expected,32);incorrect[0]^=1;
    CHECK(x4_probe_source_open_verified(&source,f.clip,f.index,incorrect,test_hash,NULL,NULL)==X4_PROBE_HASH);
    CHECK(x4_probe_source_open_verified(&source,f.clip,f.index,NULL,test_hash,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_source_open_verified(&source,f.clip,f.index,f.expected,NULL,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_source_open_verified(&source,"https://example.invalid/a",f.index,f.expected,test_hash,NULL,NULL)==X4_PROBE_ARGUMENT);
    HashContext hash={.result=X4_PROBE_IO};CHECK(open_source(&source,&f,&hash,NULL)==X4_PROBE_IO && !source.state);
    remove_fixture(&f);
}

static void cancellation_and_damage(void)
{
    Fixture f=fixture(3,19,true);atomic_bool cancel;atomic_init(&cancel,true);X4ProbeSource source={0};HashContext hash={0};
    CHECK(open_source(&source,&f,&hash,&cancel)==X4_PROBE_CANCELLED && !hash.calls && !source.state);
    CHECK(source.stats.cancel_observations==1);atomic_store(&cancel,false);
    hash=(HashContext){.cancel_in_hash=true,.cancel=&cancel};
    CHECK(open_source(&source,&f,&hash,&cancel)==X4_PROBE_CANCELLED && hash.calls==1 && !source.state);
    CHECK(source.stats.cancel_observations==1);atomic_store(&cancel,false);hash=(HashContext){0};
    CHECK(!open_source(&source,&f,&hash,&cancel));CHECK(!x4_probe_source_begin(&source,0,X4_PROBE_MAX_SPEED));
    uint8_t output[35];memset(output,0xa5,sizeof(output));X4ProbeAU au;
    CHECK(!x4_probe_source_next(&source,0,output,19,&au));atomic_store(&cancel,true);
    memset(output,0xa5,sizeof(output));
    CHECK(x4_probe_source_next(&source,0,output,19,&au)==X4_PROBE_CANCELLED);
    for(size_t i=0;i<sizeof(output);++i) CHECK(output[i]==0xa5);
    CHECK(x4_probe_source_next(&source,0,output,19,&au)==X4_PROBE_CANCELLED && source.stats.cancel_observations==1);
    CHECK(!x4_probe_source_close(&source));atomic_store(&cancel,false);
    CHECK(!open_source(&source,&f,&hash,&cancel));x4_probe_source_cancel(&source);
    CHECK(x4_probe_source_begin(&source,0,X4_PROBE_PACED)==X4_PROBE_CANCELLED);
    CHECK(!x4_probe_source_close(&source));
    /* Post-open truncation: EOF during read is an error, not end-of-clip. */
    CHECK(!open_source(&source,&f,&hash,NULL));CHECK(!x4_probe_source_begin(&source,0,X4_PROBE_MAX_SPEED));
    CHECK(!truncate(f.clip,0));CHECK(x4_probe_source_next(&source,0,output,19,&au)==X4_PROBE_IO);
    CHECK(!x4_probe_source_close(&source));remove_fixture(&f);
    f=fixture(1,19,false);source=(X4ProbeSource){0};CHECK(!open_source(&source,&f,NULL,NULL));
    CHECK(!x4_probe_source_begin(&source,0,X4_PROBE_MAX_SPEED));
    CHECK(x4_probe_source_next(&source,0,output,19,&au)==X4_PROBE_ANNEXB && !source.stats.aus_read);
    CHECK(!x4_probe_source_close(&source));remove_fixture(&f);
}
static void full_index_identity(void)
{
    Fixture f=fixture(2,19,true);uint8_t expected_index[32];
    FILE *index=fopen(f.index,"rb");CHECK(index);
    CHECK(!test_hash(index,96+2*24,expected_index,NULL,NULL));CHECK(!fclose(index));
    X4ProbeSource source={0};HashContext hash={0};
    CHECK(!x4_probe_source_open_fully_verified(&source,f.clip,f.index,f.expected,
        expected_index,test_hash,&hash,NULL));
    CHECK(hash.calls==2 && source.stats.hash_verified && source.stats.index_hash_verified);
    CHECK(!memcmp(source.info.index_sha256,expected_index,32));
    CHECK(!x4_probe_source_close(&source));
    CHECK(x4_probe_source_open_fully_verified(&source,f.clip,f.index,f.expected,
        NULL,test_hash,&hash,NULL)==X4_PROBE_ARGUMENT);
    uint8_t incorrect[32];memcpy(incorrect,expected_index,32);incorrect[0]^=1;hash=(HashContext){0};
    CHECK(x4_probe_source_open_fully_verified(&source,f.clip,f.index,f.expected,
        incorrect,test_hash,&hash,NULL)==X4_PROBE_HASH && hash.calls==1 && !source.state);
    /* Same file length/header/clip, but still syntactically valid changed PTS.
     * Binding the clip alone would allow this; the frozen index must reject. */
    change_field(f.index,136,33334,8);hash=(HashContext){0};
    CHECK(x4_probe_source_open_fully_verified(&source,f.clip,f.index,f.expected,
        expected_index,test_hash,&hash,NULL)==X4_PROBE_HASH && hash.calls==1 && !source.state);
    CHECK(!source.stats.validated_records && !source.stats.index_hash_verified);
    remove_fixture(&f);
}
#else
static void disabled(void)
{
    X4ProbeSource source={0};X4ProbeAU au;uint8_t output[8];
    CHECK(x4_probe_source_open_verified(&source,"/no-file","/no-index",NULL,NULL,NULL,NULL)==X4_PROBE_DISABLED);
    CHECK(x4_probe_source_open_fully_verified(&source,"/no-file","/no-index",NULL,NULL,NULL,NULL,NULL)==X4_PROBE_DISABLED);
    CHECK(x4_probe_source_begin(&source,0,X4_PROBE_PACED)==X4_PROBE_DISABLED);
    CHECK(x4_probe_source_next(&source,0,output,sizeof(output),&au)==X4_PROBE_DISABLED);
    x4_probe_source_cancel(&source);CHECK(!source.state);
    CHECK(!x4_probe_source_close(&source));CHECK(x4_probe_source_close(NULL)==X4_PROBE_ARGUMENT);
}
#endif

int main(void)
{
    pacing();
#if X4_VIDEO_PROBE
    normal_reader();bounded_large_clip();index_errors();cancellation_and_damage();full_index_identity();
#else
    disabled();
#endif
    printf("PASS probe source flag=%u checks=%u; local reader/pacing only, no codec/PS4/SHA algorithm claim\n",(unsigned)X4_VIDEO_PROBE,checks);
    return 0;
}
