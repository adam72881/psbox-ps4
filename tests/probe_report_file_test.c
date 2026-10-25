/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/video/probe_report_file.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc,char **argv)
{
    assert(argc==2);char path[512],link[512],directory[512]; (void)umask(0022);
    assert(!x4_probe_files_test_root(argv[1]));assert(!x4_probe_files_prepare(NULL));
    snprintf(directory,sizeof(directory),"%s/xcloud4-probe-reports",argv[1]);
    snprintf(path,sizeof(path),"%s/xcloud4-probe-0123456789ab-n1-g45-r0-vc0-p1-im-control-720p30-cb-paced-case-1.json",argv[1]);
    snprintf(link,sizeof(link),"%s/xcloud4-probe-0123456789ab-n1-g45-r0-vc0-p1-im-control-720p30-cb-paced-case-2.json",argv[1]);
    assert(access(directory,F_OK)==-1);
    FILE *file=x4_probe_report_create(path);assert(file);assert(fputs("original",file)>=0);assert(!x4_probe_report_finish(file));
    struct stat st;assert(!stat(path,&st));assert((st.st_mode&0777)==0644);
    assert(!x4_probe_report_create(path));
    file=fopen(path,"rb");assert(file);char bytes[32]={0};assert(fread(bytes,1,sizeof(bytes),file)==8);assert(!strcmp(bytes,"original"));assert(!fclose(file));
    assert(!symlink(path,link));assert(!x4_probe_report_create(link));
    file=fopen(path,"rb");assert(file);memset(bytes,0,sizeof(bytes));assert(fread(bytes,1,sizeof(bytes),file)==8);assert(!strcmp(bytes,"original"));assert(!fclose(file));
    char missing[512];snprintf(missing,sizeof(missing),"%s/missing/report.json",argv[1]);
    assert(!x4_probe_report_create(missing));
    assert(!unlink(link));assert(!unlink(path));assert(!x4_probe_files_shutdown(NULL));assert(access(directory,F_OK)==-1);
    /* Real host ENOSPC on buffered flush; later fsync EINVAL must not replace it.
     * This is /dev/full, not a physical disk fill and never part of a PS4 build. */
    FILE *full = fopen("/dev/full", "wb");
    assert(full); assert(fputs("buffered disk-full fixture", full) >= 0);
    X4ProbeFileFinish finish;
    assert(x4_probe_files_report_finish(full, &finish) == -1);
    assert(finish.flush.result == -1 && finish.flush.error_number == ENOSPC);
    assert(finish.sync.result == -1 && finish.sync.error_number == EINVAL);
    assert(finish.close.result == 0 && errno == ENOSPC);
    puts("PASS real host /dev/full ENOSPC + later fsync EINVAL preserves first error; file closed");
    puts("PASS direct exclusive probe reports: mode0644, durable finish, collision, symlink, unchanged bytes, path rejection, legacy folder untouched; host only");
}
