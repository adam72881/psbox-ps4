/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/video/probe_files.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned checks, callbacks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
static void path(char *out, const char *root, const char *leaf)
{ int n=snprintf(out,X4_PROBE_FILE_PATH_MAX,"%s/%s",root,leaf); CHECK(n>0 && n<X4_PROBE_FILE_PATH_MAX); }
static void report_path(char *out,const char *root,unsigned sequence)
{ char leaf[128]; int n=snprintf(leaf,sizeof(leaf),"xcloud4-probe-0123456789ab-n1-g45-r0-vc0-p1-im-files-%u.json",sequence); CHECK(n>0 && (size_t)n<sizeof(leaf)); path(out,root,leaf); }
static void progress(const X4ProbeFileSelfcheck *r, void *context)
{ (void)context; ++callbacks; for(unsigned i=0;i<X4_PROBE_FILES_STEP_COUNT;++i) if(r->ran[i]) CHECK(r->step[i].result || !r->step[i].error_number); }
static void index_fixture(const char *p)
{
    uint8_t h[96]={0}; memcpy(h,"X4AU1\0\0\0",8);
    FILE *f=fopen(p,"wb"); CHECK(f); CHECK(fwrite(h,1,sizeof(h),f)==sizeof(h)); CHECK(!fclose(f));
}
static size_t descriptor_count(void)
{
    DIR *dir=opendir("/proc/self/fd"); CHECK(dir); size_t count=0;
    struct dirent *entry; errno=0;
    while((entry=readdir(dir))) {
        if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) ++count;
        errno=0;
    }
    CHECK(!errno); CHECK(!closedir(dir)); return count;
}
static void read_exact_original(const char *p)
{
    FILE *f=fopen(p,"rb"); CHECK(f); char bytes[32]={0};
    CHECK(fread(bytes,1,sizeof(bytes),f)==8); CHECK(!strcmp(bytes,"original")); CHECK(!fclose(f));
}
static void check_calls(const X4ProbeFileSelfcheck *sc)
{
    CHECK(sc->call_count>0 && sc->call_count<=64); CHECK(!sc->calls_omitted);
    unsigned informational=0;
    for(unsigned i=0;i<sc->call_count;++i) {
        CHECK(sc->calls[i].call && sc->calls[i].call[0]);
        if(!strcmp(sc->calls[i].call,"openat")) ++informational;
    }
    CHECK(informational==1); /* One optional diagnostic; no functional dependency. */
}
int main(int argc,char **argv)
{
    CHECK(argc==2); const char *root=argv[1]; (void)umask(0022);
    char reports[X4_PROBE_FILE_PATH_MAX],samples[X4_PROBE_FILE_PATH_MAX],test[X4_PROBE_FILE_PATH_MAX],index[X4_PROBE_FILE_PATH_MAX],bad[X4_PROBE_FILE_PATH_MAX],linkpath[X4_PROBE_FILE_PATH_MAX];
    path(reports,root,"xcloud4-probe-reports"); path(samples,root,"xcloud4-probe");
    report_path(test,root,1); path(index,samples,"control-720p30-cb.x4au");
    CHECK(!x4_probe_files_test_root(root));
    X4ProbeFileStatus st; struct stat sb;
    /* One central path formatter: actual compile flags, bounded output and
     * decimal sequence at the end. No user filename can inject a directory. */
    char generated[X4_PROBE_FILE_PATH_MAX], tiny[8], long_id[82];
    CHECK(!x4_probe_files_report_path(generated,sizeof(generated),"files",UINT64_MAX));
    CHECK(!strncmp(generated,root,strlen(root))); CHECK(strstr(generated,"/xcloud4-probe-"));
    CHECK(strstr(generated,"-files-18446744073709551615.json"));
    CHECK(!x4_probe_files_report_path(generated,sizeof(generated),"control-720p30-cb-paced-case",UINT64_MAX));
    FILE *formatted=x4_probe_files_report_create(generated,&st); CHECK(formatted); CHECK(!x4_probe_files_report_finish(formatted,NULL));
    CHECK(x4_probe_files_report_path(tiny,sizeof(tiny),"files",1)==-1); CHECK(errno==ENAMETOOLONG); CHECK(!tiny[0]);
    CHECK(x4_probe_files_report_path(NULL,sizeof(generated),"files",1)==-1 && errno==EINVAL);
    CHECK(x4_probe_files_report_path(generated,0,"files",1)==-1 && errno==EINVAL);
    CHECK(x4_probe_files_report_path(generated,sizeof(generated)+1,"files",1)==-1 && errno==EINVAL);
    const char *invalid[]={NULL,"","../files","files/more","files.json","files\\more","files name"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
        CHECK(x4_probe_files_report_path(generated,sizeof(generated),invalid[i],1)==-1 && errno==EINVAL);
    memset(long_id,'a',80); long_id[80]=0;
    CHECK(!x4_probe_files_report_path(generated,sizeof(generated),long_id,UINT64_MAX));
    long_id[80]='a'; long_id[81]=0; CHECK(x4_probe_files_report_path(generated,sizeof(generated),long_id,1)==-1 && errno==EINVAL);
    /* The legacy inaccessible report folder must not be created, opened or
     * chmod-ed. Reports go straight into the fixed /data fixture root. */
    CHECK(!x4_probe_files_prepare(&st)); CHECK(st.result==0 && st.error_number==0);
    CHECK(access(reports,F_OK)==-1 && errno==ENOENT);
    CHECK(!mkdir(reports,0700)); CHECK(!chmod(reports,0700));
    CHECK(!x4_probe_files_prepare(&st)); CHECK(!stat(reports,&sb)); CHECK((sb.st_mode&07777)==0700);
    /* Creation mode, exact readback, collision and no symlink target write. */
    FILE *f=x4_probe_files_report_create(test,&st); CHECK(f); CHECK(!st.error_number);
    CHECK(fputs("original",f)>=0); X4ProbeFileFinish finished;
    CHECK(!x4_probe_files_report_finish(f,&finished));
    CHECK(!finished.flush.error_number && !finished.sync.error_number && !finished.close.error_number);
    CHECK(!stat(test,&sb)); CHECK((sb.st_mode&07777)==0644);
    CHECK(!x4_probe_files_report_create(test,&st)); CHECK(st.error_number==EEXIST); read_exact_original(test);
    f=x4_probe_files_report_open(test,&st); CHECK(f); char bytes[32]={0};
    CHECK(fread(bytes,1,sizeof(bytes),f)==8); CHECK(!strcmp(bytes,"original")); CHECK(!x4_probe_files_close(f,&st));
    report_path(linkpath,root,2); CHECK(!symlink(test,linkpath));
    CHECK(!x4_probe_files_report_create(linkpath,&st)); CHECK(st.error_number==EEXIST); read_exact_original(test);
    /* Exclusive creation must reject a dangling link without creating target. */
    report_path(linkpath,root,3); path(bad,root,"dangling-target"); CHECK(!symlink(bad,linkpath));
    CHECK(!x4_probe_files_report_create(linkpath,&st)); CHECK(st.error_number==EEXIST);
    CHECK(access(bad,F_OK)==-1 && errno==ENOENT);
    /* No fchmod or native umask adjustment: restrictive mode fails visibly. */
    (void)umask(0077); report_path(linkpath,root,4); size_t restricted_before=descriptor_count();
    CHECK(!x4_probe_files_report_create(linkpath,&st)); CHECK(st.error_number==EPERM);
    CHECK(!stat(linkpath,&sb)); CHECK((sb.st_mode&07777)==0600 && sb.st_size==0);
    CHECK(restricted_before==descriptor_count()); (void)umask(0022);
    CHECK(!x4_probe_files_report_create("/data/xcloud4-probe-reports/a.json",&st)); CHECK(st.error_number==EINVAL);
    path(bad,root,"../xcloud4-probe-0123456789ab-files-3.json"); CHECK(!x4_probe_files_report_create(bad,&st)); CHECK(st.error_number==EINVAL);
    path(bad,root,"sub/xcloud4-probe-0123456789ab-files-3.json"); CHECK(!x4_probe_files_report_create(bad,&st)); CHECK(st.error_number==EINVAL);
    path(bad,root,"wrong-prefix.json"); CHECK(!x4_probe_files_report_create(bad,&st)); CHECK(st.error_number==EINVAL);
    /* Only fixed, validated clip paths. Clip ownership is not required: FTP
     * may create files for another UID. fstat checks regular + nlink0/1. */
    CHECK(!mkdir(samples,0755)); index_fixture(index); size_t sample_before=descriptor_count();
    f=x4_probe_files_sample_open(index,&st); CHECK(f); CHECK(!x4_probe_files_close(f,&st));
    path(bad,samples,"1080p60-main.x4au"); CHECK(!x4_probe_files_sample_open(bad,&st)); CHECK(st.error_number==ENOENT);
    path(bad,samples,"720p60-cb.x4au"); CHECK(!mkdir(bad,0700)); CHECK(!x4_probe_files_sample_open(bad,&st)); CHECK(st.error_number==EPERM);
    char hard[X4_PROBE_FILE_PATH_MAX]; path(hard,samples,"1080p60-cb.x4au"); CHECK(!link(index,hard));
    CHECK(!x4_probe_files_sample_open(index,&st)); CHECK(st.error_number==EPERM); CHECK(!unlink(hard));
    /* Owner-fault injection is obsolete and must not affect clips. */
    x4_probe_files_test_fault(X4_PROBE_FILE_FAULT_OWNER,EPERM);
    f=x4_probe_files_sample_open(index,&st); CHECK(f); CHECK(!x4_probe_files_close(f,&st));
    x4_probe_files_test_fault(X4_PROBE_FILE_FAULT_NONE,0);
    if(geteuid()==0) { CHECK(!chown(index,12345,(gid_t)-1)); f=x4_probe_files_sample_open(index,&st); CHECK(f); CHECK(!x4_probe_files_close(f,&st)); }
    CHECK(sample_before==descriptor_count());
    X4ProbeFileSelfcheck sc; report_path(test,root,10);
    CHECK(!x4_probe_files_selfcheck(test,index,&sc,progress,NULL)); CHECK(sc.passed==1); CHECK(callbacks==X4_PROBE_FILES_STEP_COUNT+1);
    for(unsigned i=0;i<X4_PROBE_FILES_OPENAT_INFO;++i){ CHECK(sc.ran[i]); CHECK(!sc.step[i].result && !sc.step[i].error_number); CHECK(!sc.step[i].call_errno); }
    CHECK(sc.ran[X4_PROBE_FILES_OPENAT_INFO]); check_calls(&sc);
    CHECK(x4_probe_files_selfcheck(test,index,&sc,NULL,NULL)==-1); CHECK(sc.ran[X4_PROBE_FILES_CREATE] && sc.step[X4_PROBE_FILES_CREATE].error_number==EEXIST); CHECK(!sc.ran[X4_PROBE_FILES_WRITE]);
    /* EPERM in the informational openat cannot veto required checks. */
    report_path(test,root,11); size_t before=descriptor_count();
    x4_probe_files_test_fault(X4_PROBE_FILE_FAULT_OPENAT_INFO,EPERM);
    CHECK(!x4_probe_files_selfcheck(test,index,&sc,NULL,NULL)); CHECK(sc.passed);
    CHECK(sc.ran[X4_PROBE_FILES_OPENAT_INFO]); CHECK(sc.step[X4_PROBE_FILES_OPENAT_INFO].result==-1);
    CHECK(sc.step[X4_PROBE_FILES_OPENAT_INFO].error_number==EPERM && sc.step[X4_PROBE_FILES_OPENAT_INFO].call_errno==EPERM);
    CHECK(sc.step[X4_PROBE_FILES_OPENAT_INFO].raw_return==-1); check_calls(&sc);
    x4_probe_files_test_fault(X4_PROBE_FILE_FAULT_NONE,0); CHECK(before==descriptor_count());
    /* Failures retain partial files and finalize every created descriptor. */
    const int faults[]={X4_PROBE_FILE_FAULT_CREATE,X4_PROBE_FILE_FAULT_WRITE,X4_PROBE_FILE_FAULT_SHORT_WRITE,X4_PROBE_FILE_FAULT_FLUSH,X4_PROBE_FILE_FAULT_SYNC,X4_PROBE_FILE_FAULT_CLOSE,X4_PROBE_FILE_FAULT_FLUSH,X4_PROBE_FILE_FAULT_SHORT_WRITE};
    const int steps[]={X4_PROBE_FILES_CREATE,X4_PROBE_FILES_WRITE,X4_PROBE_FILES_WRITE,X4_PROBE_FILES_FLUSH,X4_PROBE_FILES_SYNC,X4_PROBE_FILES_CLOSE,X4_PROBE_FILES_FLUSH,X4_PROBE_FILES_WRITE};
    const int errors[]={ENOSPC,ENOSPC,ENOSPC,EIO,EIO,EIO,ENOSPC,EIO};
    for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);++i){
        report_path(test,root,100+i); int injected=errors[i]; before=descriptor_count();
        x4_probe_files_test_fault(faults[i],injected);
        CHECK(x4_probe_files_selfcheck(test,index,&sc,NULL,NULL)==-1); CHECK(!sc.passed); CHECK(errno==injected);
        CHECK(sc.ran[steps[i]] && sc.step[steps[i]].result==-1 && sc.step[steps[i]].error_number==injected);
        CHECK(sc.step[steps[i]].call_errno==injected);
        if(i==0) CHECK(!sc.ran[X4_PROBE_FILES_WRITE]);
        else { CHECK(sc.ran[X4_PROBE_FILES_FLUSH] && sc.ran[X4_PROBE_FILES_SYNC] && sc.ran[X4_PROBE_FILES_CLOSE]); CHECK(!sc.ran[X4_PROBE_FILES_REOPEN]); CHECK(!stat(test,&sb)); CHECK((sb.st_mode&07777)==0644); }
        x4_probe_files_test_fault(X4_PROBE_FILE_FAULT_NONE,0);
        size_t after=descriptor_count(); CHECK(before==after);
        printf("PASS failure %u errno=%d descriptors before=%zu after=%zu\n",i,injected,before,after);
    }
    report_path(test,root,200); path(bad,samples,"1080p60-main.x4au");
    CHECK(x4_probe_files_selfcheck(test,bad,&sc,NULL,NULL)==-1); CHECK(sc.step[X4_PROBE_FILES_INDEX_OPEN].error_number==ENOENT);
    CHECK(!sc.ran[X4_PROBE_FILES_INDEX_READ]); CHECK(!sc.ran[X4_PROBE_FILES_INDEX_CLOSE]);
    CHECK(!x4_probe_files_shutdown(&st)); CHECK(!x4_probe_files_shutdown(&st));
    CHECK(!stat(reports,&sb)); CHECK((sb.st_mode&07777)==0700);
    printf("PASS probe direct files: %u checks; host fixtures and simulated errors only; no console\n",checks);
    return 0;
}
