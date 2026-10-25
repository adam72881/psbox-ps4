/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only reproduction of observed PS4 FTP stat fields. The file contents
 * below are tiny synthetic fixtures, NOT valid video or console evidence.
 * Integrity checks execute production source/reference paths and the actual
 * pinned MbedTLS SHA-256 adapter; no substitute digest is used. */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "../src/video/probe_files.h"
#include "../src/video/probe_hash.h"
#include "../src/video/probe_reference.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks, report_sequence = 500;
static X4ProbeHashContext hash_context;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
typedef struct {
    char root[100], samples[140], clip[200], index[200], reference[200];
    uint8_t clip_sha[32], index_sha[32], reference_sha[32];
} Fixture;
static void path(char *out, size_t size, const char *root, const char *leaf)
{ int n=snprintf(out,size,"%s/%s",root,leaf); CHECK(n>0 && (size_t)n<size); }
static void put32(uint8_t *p,uint32_t value)
{ for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(value>>(8*i)); }
static void put64(uint8_t *p,uint64_t value)
{ for(unsigned i=0;i<8;++i) p[i]=(uint8_t)(value>>(8*i)); }
static void write_bytes(const char *name, const uint8_t *bytes, size_t count)
{ FILE *f=fopen(name,"wb"); CHECK(f); CHECK(fwrite(bytes,1,count,f)==count); CHECK(!fclose(f)); }
static void digest(const char *name,size_t bytes,uint8_t sha[32])
{
    FILE *f=fopen(name,"rb"); CHECK(f);
    CHECK(!x4_probe_hash_file(f,bytes,sha,NULL,&hash_context)); CHECK(!fclose(f));
}
static void alter_byte(const char *name,long position)
{
    FILE *f=fopen(name,"r+b"); CHECK(f); CHECK(!fseek(f,position,SEEK_SET));
    int byte=fgetc(f); CHECK(byte!=EOF); CHECK(!fseek(f,position,SEEK_SET));
    CHECK(fputc(byte^1,f)!=EOF); CHECK(!fclose(f));
}
static size_t descriptor_count(void)
{
    DIR *d=opendir("/proc/self/fd"); CHECK(d); struct dirent *e; size_t n=0;
    errno=0; while((e=readdir(d))) { if(strcmp(e->d_name,".") && strcmp(e->d_name,"..")) ++n; errno=0; }
    CHECK(!errno); CHECK(!closedir(d)); return n;
}
static void ftp_stat(int64_t size)
{ x4_probe_files_test_stat(1,S_IFREG|0777,0,size); }
static void clear_stat(void)
{ x4_probe_files_test_stat(0,0,0,0); }

static void manifest_paths(Fixture *f)
{
    static const char *cases[]={"control-720p30-cb","720p60-cb","1080p60-cb",
        "1080p60-main","1080p60-high","S720-CB","S1080-CB","S1080-HIGH"};
    static const char *extensions[]={"h264","x4au","rgba"};
    X4ProbeFileStatus status; char name[220], leaf[100]; uint8_t value=1;
    ftp_stat(0); x4_probe_files_test_manifest(1);
    for(unsigned c=0;c<8;++c) for(unsigned e=0;e<3;++e) {
        int n=snprintf(leaf,sizeof(leaf),"%s.%s",cases[c],extensions[e]); CHECK(n>0 && (size_t)n<sizeof(leaf));
        path(name,sizeof(name),f->samples,leaf); write_bytes(name,&value,1);
        FILE *file=x4_probe_files_sample_open(name,&status); CHECK(file); CHECK(!status.result && !status.error_number);
        CHECK(!x4_probe_files_close(file,NULL)); CHECK(!unlink(name));
    }
    const char *invalid[]={"not-in-manifest.h264","control-720p30-cb.json","control-720p30-cb.H264",
        "control-720p30-cb.h264.extra","control-720p30-cb-duplicate.x4au"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        path(name,sizeof(name),f->samples,invalid[i]); write_bytes(name,&value,1);
        CHECK(!x4_probe_files_sample_open(name,&status)); CHECK(status.error_number==EINVAL); CHECK(!unlink(name));
    }
    path(name,sizeof(name),f->root,"control-720p30-cb.h264"); write_bytes(name,&value,1);
    CHECK(!x4_probe_files_sample_open(name,&status)); CHECK(status.error_number==EINVAL); CHECK(!unlink(name));
    path(name,sizeof(name),f->samples,"../control-720p30-cb.h264");
    CHECK(!x4_probe_files_sample_open(name,&status)); CHECK(status.error_number==EINVAL);
    path(name,sizeof(name),f->samples,"subdir/control-720p30-cb.h264");
    CHECK(!x4_probe_files_sample_open(name,&status)); CHECK(status.error_number==EINVAL);
    clear_stat();
}
static void prepare_content(Fixture *f)
{
    uint8_t clip[38];
    for(unsigned i=0;i<sizeof(clip);++i) clip[i]=(uint8_t)(i*17+3);
    for(unsigned i=0;i<2;++i) { unsigned n=19*i; clip[n]=clip[n+1]=clip[n+2]=0; clip[n+3]=1; clip[n+4]=9; }
    write_bytes(f->clip,clip,sizeof(clip)); digest(f->clip,sizeof(clip),f->clip_sha);
    uint8_t index[144]={0}; memcpy(index,"X4AU1\0\0\0",8);
    put32(index+8,1); put32(index+12,96); put64(index+16,sizeof(clip)); put64(index+24,2);
    put64(index+32,66667); put32(index+40,1280); put32(index+44,720);
    put32(index+48,1280); put32(index+52,720); put32(index+56,30); put32(index+60,1);
    memcpy(index+64,f->clip_sha,32);
    for(unsigned i=0;i<2;++i) { put64(index+96+24*i,19*i); put64(index+104+24*i,19); put64(index+112+24*i,33333*i); }
    write_bytes(f->index,index,sizeof(index)); digest(f->index,sizeof(index),f->index_sha);
    uint8_t rgba[16]; for(unsigned i=0;i<4;++i) { rgba[4*i]=20;rgba[4*i+1]=40;rgba[4*i+2]=60;rgba[4*i+3]=255; }
    write_bytes(f->reference,rgba,sizeof(rgba)); digest(f->reference,sizeof(rgba),f->reference_sha);
}
static int source_open(X4ProbeSource *source,Fixture *f)
{ return x4_probe_source_open_fully_verified(source,f->clip,f->index,f->clip_sha,f->index_sha,x4_probe_hash_file,&hash_context,NULL); }
static int reference_open(X4ProbeReference *reference,Fixture *f)
{ return x4_probe_reference_open_verified(reference,f->reference,2,2,1,f->reference_sha,x4_probe_hash_file,&hash_context,NULL); }
static void verified_content(Fixture *f)
{
    /* st_size is not trusted: byte lengths are obtained by the production
     * seek/read path and SHA-256 checks the retained file before any AU use. */
    const int64_t observed_sizes[]={4120327,38,7296,0,-1,INT64_MAX};
    for(unsigned i=0;i<sizeof(observed_sizes)/sizeof(observed_sizes[0]);++i) {
        ftp_stat(observed_sizes[i]); X4ProbeSource source={0}; X4ProbeReference reference={0};
        CHECK(!source_open(&source,f)); CHECK(source.stats.hash_verified && source.stats.index_hash_verified);
        CHECK(source.info.file_bytes==38 && source.stats.validated_records==2 && !source.stats.aus_read);
        CHECK(!x4_probe_source_begin(&source,100,X4_PROBE_PACED)); X4ProbeAU au; uint8_t out[19];
        CHECK(!x4_probe_source_next(&source,100,out,sizeof(out),&au)); CHECK(au.ordinal==0 && source.stats.aus_read==1);
        CHECK(!x4_probe_source_close(&source) && !source.state);
        CHECK(!reference_open(&reference,f)); CHECK(reference.stats.hash_verified && !reference.stats.references_read);
        uint32_t rgba[4]={0xff3c2814,0xff3c2814,0xff3c2814,0xff3c2814}; X4ProbeReferenceScore score;
        CHECK(!x4_probe_reference_match(&reference,rgba,4,2,2,&score)); CHECK(score.accepted_references==1 && !score.sum_abs);
        CHECK(!x4_probe_reference_close(&reference) && !reference.state);
    }
    ftp_stat(0);
    /* Altered clip is HASH-rejected with no AU consumption, not excused by
     * the permissive FTP metadata. Expected hashes stay pristine. */
    alter_byte(f->clip,10); X4ProbeSource source={0};
    CHECK(source_open(&source,f)==X4_PROBE_HASH); CHECK(!source.state && !source.stats.hash_verified && !source.stats.aus_read);
    uint8_t out[19]; memset(out,0xa5,sizeof(out)); X4ProbeAU au;
    CHECK(x4_probe_source_next(&source,0,out,sizeof(out),&au)==X4_PROBE_ARGUMENT);
    for(unsigned i=0;i<sizeof(out);++i) CHECK(out[i]==0xa5);
    alter_byte(f->clip,10);
    /* A well-formed but altered index is rejected by its own SHA before its
     * timestamps or record boundaries are accepted. */
    alter_byte(f->index,138); source=(X4ProbeSource){0};
    CHECK(source_open(&source,f)==X4_PROBE_HASH); CHECK(!source.state && !source.stats.index_hash_verified && !source.stats.validated_records && !source.stats.aus_read);
    alter_byte(f->index,138);
    alter_byte(f->reference,1); X4ProbeReference reference={0};
    CHECK(reference_open(&reference,f)==X4_PROBE_HASH); CHECK(!reference.state && !reference.stats.hash_verified && !reference.stats.references_read && !reference.stats.comparisons);
    alter_byte(f->reference,1);
    source=(X4ProbeSource){0}; CHECK(!source_open(&source,f)); CHECK(!x4_probe_source_close(&source));
    reference=(X4ProbeReference){0}; CHECK(!reference_open(&reference,f)); CHECK(!x4_probe_reference_close(&reference));
    clear_stat();
}
static void metadata_policy(Fixture *f)
{
    X4ProbeFileStatus status;
    const unsigned permissions[]={0777,0644,0000,0600,04777};
    for(unsigned i=0;i<sizeof(permissions)/sizeof(permissions[0]);++i) for(unsigned links=0;links<=1;++links) {
        x4_probe_files_test_stat(1,S_IFREG|permissions[i],links,0);
        FILE *file=x4_probe_files_sample_open(f->index,&status); CHECK(file); CHECK(!status.result); CHECK(!x4_probe_files_close(file,NULL));
    }
    const uint32_t modes[]={S_IFREG|0777,S_IFDIR|0777,S_IFIFO|0777,S_IFSOCK|0777};
    for(unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);++i) {
        x4_probe_files_test_stat(1,modes[i],i?0:2,0);
        CHECK(!x4_probe_files_sample_open(f->index,&status)); CHECK(status.error_number==EPERM);
    }
    x4_probe_files_test_stat(1,S_IFREG|0777,UINT64_MAX,0);
    CHECK(!x4_probe_files_sample_open(f->index,&status)); CHECK(status.error_number==EPERM);
    clear_stat();
    /* A real directory under a manifest-allowed filename is not a clip. */
    char directory[220]; path(directory,sizeof(directory),f->samples,"720p60-cb.x4au"); CHECK(!mkdir(directory,0777));
    CHECK(!x4_probe_files_sample_open(directory,&status)); CHECK(status.error_number==EPERM); CHECK(!rmdir(directory));
    char report[X4_PROBE_FILE_PATH_MAX]; CHECK(!x4_probe_files_report_path(report,sizeof(report),"files",report_sequence++));
    x4_probe_files_test_stat(1,S_IFREG|0644,1,0);
    FILE *file=x4_probe_files_report_create(report,&status); CHECK(file); CHECK(!x4_probe_files_report_finish(file,NULL));
    file=x4_probe_files_report_open(report,&status); CHECK(file); CHECK(!x4_probe_files_close(file,NULL));
    x4_probe_files_test_stat(1,S_IFREG|0644,0,0);
    CHECK(!x4_probe_files_report_open(report,&status)); CHECK(status.error_number==EPERM);
    CHECK(!unlink(report)); CHECK(!x4_probe_files_report_path(report,sizeof(report),"files",report_sequence++));
    CHECK(!x4_probe_files_report_create(report,&status)); CHECK(status.error_number==EPERM); CHECK(!unlink(report));
    CHECK(!x4_probe_files_report_path(report,sizeof(report),"files",report_sequence++));
    x4_probe_files_test_stat(1,S_IFREG|0777,1,0);
    CHECK(!x4_probe_files_report_create(report,&status)); CHECK(status.error_number==EPERM); CHECK(!unlink(report));
    clear_stat();
}
static void before_index(const X4ProbeFileSelfcheck *sc,void *context)
{
    const int64_t *size=context;
    if(sc->ran[X4_PROBE_FILES_COMPARE]) ftp_stat(*size);
}
static unsigned find_field(const X4ProbeFileSelfcheck *sc,const char *field,int64_t expected)
{
    unsigned found=0;
    for(unsigned i=0;i<sc->call_count;++i) if(!strcmp(sc->calls[i].call,field) && sc->calls[i].raw_return==expected) ++found;
    return found;
}
static void selfchecks(Fixture *f)
{
    const int64_t sizes[]={7296,144,0,-1,INT64_MAX};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        clear_stat(); char report[X4_PROBE_FILE_PATH_MAX]; CHECK(!x4_probe_files_report_path(report,sizeof(report),"files",report_sequence++));
        x4_probe_files_test_fault(X4_PROBE_FILE_FAULT_OPENAT_INFO,EPERM); X4ProbeFileSelfcheck sc;
        CHECK(!x4_probe_files_selfcheck(report,f->index,&sc,before_index,(void *)&sizes[i])); CHECK(sc.passed && !sc.cleanup_failed && !sc.calls_omitted);
        for(unsigned step=0;step<X4_PROBE_FILES_OPENAT_INFO;++step) { CHECK(sc.ran[step]); CHECK(!sc.step[step].result && !sc.step[step].error_number && !sc.step[step].call_errno); }
        CHECK(sc.ran[X4_PROBE_FILES_OPENAT_INFO] && sc.step[X4_PROBE_FILES_OPENAT_INFO].result==-1);
        CHECK(sc.step[X4_PROBE_FILES_OPENAT_INFO].raw_return==-1 && sc.step[X4_PROBE_FILES_OPENAT_INFO].call_errno==EPERM);
        /* Report metadata is still 0644/1; only the FTP index is 0777/0. */
        CHECK(find_field(&sc,"fstat.st_mode (observed field)",0644)==2);
        CHECK(find_field(&sc,"fstat.st_nlink (observed field)",1)==2);
        CHECK(find_field(&sc,"fstat.st_mode (observed field)",0777)==1);
        CHECK(find_field(&sc,"fstat.st_nlink (observed field)",0)==1);
        CHECK(find_field(&sc,"fstat.st_size (informational field)",sizes[i])==1);
        CHECK(find_field(&sc,"manifest.bytes (informational field)",7296)==1);
        CHECK(find_field(&sc,"stat_size_matches_manifest (informational field)",sizes[i]==7296)==1);
        clear_stat(); x4_probe_files_test_fault(X4_PROBE_FILE_FAULT_NONE,0); CHECK(!unlink(report));
    }
}
int main(void)
{
    (void)umask(0022); Fixture f={.root="/tmp/x4-probe-ftp-stat-test.XXXXXX"}; CHECK(mkdtemp(f.root));
    CHECK(!x4_probe_hash_context_init(&hash_context));
    CHECK(!x4_probe_files_test_root(f.root)); x4_probe_files_test_manifest(1);
    path(f.samples,sizeof(f.samples),f.root,"xcloud4-probe"); CHECK(!mkdir(f.samples,0755));
    path(f.clip,sizeof(f.clip),f.samples,"control-720p30-cb.h264");
    path(f.index,sizeof(f.index),f.samples,"control-720p30-cb.x4au");
    path(f.reference,sizeof(f.reference),f.samples,"control-720p30-cb.rgba");
    size_t before=descriptor_count(); manifest_paths(&f); prepare_content(&f);
    verified_content(&f); metadata_policy(&f); selfchecks(&f); CHECK(before==descriptor_count());
    clear_stat(); x4_probe_files_test_manifest(0); CHECK(!x4_probe_files_shutdown(NULL));
    CHECK(!unlink(f.clip) && !unlink(f.index) && !unlink(f.reference)); CHECK(!rmdir(f.samples) && !rmdir(f.root));
    x4_probe_hash_context_close(&hash_context); CHECK(!hash_context.buffer&&!hash_context.capacity);
    printf("PASS FTP stat + actual pinned SHA-256: %u checks; synthetic host files, no decoder or console\n",checks);
    return 0;
}
