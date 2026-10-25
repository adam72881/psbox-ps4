/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual export_pipeline + actual files/metrics implementations. Only the
 * already-quiescent Pipeline state is synthetic. No native/video work runs.
 * Unreachable native runner sections are removed by the host linker. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef X4_PIPELINE_EXPORT_SOURCE
#define X4_PIPELINE_EXPORT_SOURCE "../src/video/probe_pipeline.c"
#endif
#include X4_PIPELINE_EXPORT_SOURCE
#include "../src/video/probe_case_catalog.h"

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr,"FAIL export fixture line=%d expression=%s errno=%d\n",__LINE__,#value,errno); \
    exit(1); } } while (0)

static Pipeline *fixture(const X4ProbeCase *sample, X4ProbeMode mode,
    unsigned workers, uint64_t sequence)
{
    Pipeline *p=calloc(1,sizeof(*p));CHECK(p);
    p->sample=sample;p->mode=mode;p->converters=workers;
    p->result.started_us=sequence;p->result.ended_us=sequence+sample->duration_us+20000;
    p->input_begin_us=sequence+100;p->decode_finished_us=p->result.ended_us-1000;
    atomic_init(&p->cancel,false);atomic_init(&p->measurement_start,sequence+10000);
    p->metrics=x4_probe_metrics_create(sequence);CHECK(p->metrics);
    CHECK(x4_video_pipeline_display_init(&p->display.ledger,sequence)==0);
    if(workers) {
        p->display.ledger.peak_occupied=2;p->display.ledger.peak_in_flight=1;
        p->scanout_observations=3;p->scanout_occupied_samples[0]=1;p->scanout_occupied_samples[2]=2;
        p->scanout_inflight_samples[0]=1;p->scanout_inflight_samples[1]=2;
    }
    X4ProbeStatus *s=&p->video.probe_status;
    s->input_aus=s->native_outputs=sample->au_count;
    s->source_eof=true;s->layout_verdict=X4_PROBE_LAYOUT_VERIFIED;
    s->layout_checks=sample->au_count;s->layout_reference_matches=3;
    p->measured_decode_outputs=sample->au_count-60;
    p->ring.discarded_ready=workers?3:0;
    p->ring.discarded_latency=workers?7:0;
    p->convert_observed=p->presented=workers?sample->au_count-12:0;p->no_output_calls=1;
    size_t y=(size_t)sample->coded_width*sample->coded_height;
    p->last_image.native_width=sample->coded_width;
    p->last_image.native_height=sample->coded_height;p->last_image.pitch=sample->coded_width;
    p->last_image.layout.storage_height=sample->coded_height;
    p->last_image.layout.uv_plane_offset=y;p->last_image.returned_bytes=y*3/2;
    p->last_image.owned_bytes=(p->last_image.returned_bytes+16383)&~(size_t)16383;
    s->queried_compute_bytes=s->queried_cpu_bytes=s->queried_gpu_bytes=s->queried_shared_bytes=16384;
    s->queried_frame_bytes=p->last_image.returned_bytes;
    s->rounded_native_bytes=65536+4*p->last_image.owned_bytes;
    s->rgb_bytes=(uint64_t)sample->visible_width*sample->visible_height*4;
    p->ring.peak_occupied=4;p->ring.peak_ready=workers?3:1;
    p->display.bytes=workers?3*SCANOUT_BYTES:0;
    if(!workers) {
        p->display.allocate_rc=p->display.map_rc=p->display.register_rc=p->display.rate_rc=
            p->display.close_rc=p->display.unmap_rc=p->display.release_rc=X4_PROBE_RC_NOT_RUN;
    }
    for(unsigned i=0;i<3;++i) {
        p->video.probe_raw_score_counts[i]=1;
        p->video.probe_raw_scores[i][0].accepted_references=1;
    }
    for(unsigned slot=0;slot<=workers;++slot) {
        X4ProbeStackReport stack={.configured_bytes=262144,.reported_bytes=262144,.guard_bytes=16384,
            .painted_bytes=241664,.untouched_bytes=233472,.high_water_bytes=28672,
            .low_excluded_bytes=16384,.top_excluded_bytes=4096,.entry_used_bytes=4096,
            .started=true,.finished=true,.pattern_ready=true,.pattern_touched=true};
        if(slot)snprintf(stack.name,sizeof(stack.name),"x4-convert%u",slot-1);
        else snprintf(stack.name,sizeof(stack.name),"x4-decode");
        x4_probe_metrics_record_stack(p->metrics,slot,&stack);
    }
    for(uint64_t i=0;i<sample->au_count;++i) {
        if(i==60)CHECK(x4_probe_metrics_begin_measurement(p->metrics,sequence+10000));
        uint64_t now=sequence+(i<60?100+i*100:10100+(i-60)*100);
        x4_probe_metrics_note_at(p->metrics,now,X4_PROBE_COUNT_INPUT_AU,1);
        x4_probe_metrics_note_at(p->metrics,now,X4_PROBE_COUNT_NATIVE_OUTPUT,1);
        if(workers && i>=12) {
            x4_probe_metrics_note_at(p->metrics,now,X4_PROBE_COUNT_RGB_PUBLICATION,1);
            x4_probe_metrics_note_at(p->metrics,now,X4_PROBE_COUNT_NEW_PRESENT,1);
        }
        if(workers && i>=2 && i<5)x4_probe_metrics_note_at(p->metrics,now,X4_PROBE_COUNT_OUTPUT_SUPERSEDE,1);
        if(workers && i>=5 && i<12)x4_probe_metrics_note_at(p->metrics,now,X4_PROBE_COUNT_LATENCY_SUPERSEDE,1);
    }
    for(unsigned stage=0;stage<X4_PROBE_STAGE_COUNT;++stage)
        for(unsigned i=1;i<=3;++i)
            x4_probe_metrics_record_duration(p->metrics,(X4ProbeStage)stage,sequence+10000+i*100,i*10);
    x4_probe_metrics_end(p->metrics,p->result.ended_us,X4_PROBE_END_CLEAN_EOF);
    return p;
}

static void close_fixture(Pipeline *p)
{ x4_probe_metrics_free(p->metrics);free(p); }

static void report_name(Pipeline *p,const char *kind,char path[X4_PROBE_FILE_PATH_MAX])
{
    char key[128];int n=snprintf(key,sizeof(key),"%s-%s-p%u-%s",p->sample->id,
        p->mode==X4_PROBE_PACED?"paced":"maximum-speed",p->converters,kind);
    CHECK(n>0 && (size_t)n<sizeof(key));
    CHECK(!x4_probe_files_report_path(path,X4_PROBE_FILE_PATH_MAX,key,p->result.started_us));
}

static uint64_t checksum(const char *path)
{
    FILE *f=fopen(path,"rb");CHECK(f);uint64_t result=UINT64_C(1469598103934665603);
    int c;while((c=fgetc(f))!=EOF){result^=(unsigned)c;result*=UINT64_C(1099511628211);}
    CHECK(!ferror(f));CHECK(!fclose(f));return result;
}

static unsigned file_count(const char *root)
{
    DIR *dir=opendir(root);CHECK(dir);unsigned count=0;struct dirent *entry;
    while((entry=readdir(dir)))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,".."))++count;
    CHECK(!closedir(dir));return count;
}

static void real_path_contract(void)
{
    char oversized[384]={0};errno=0;
    CHECK(x4_probe_files_report_path(oversized,sizeof(oversized),"files",1)==-1);
    CHECK(errno==EINVAL && !oversized[0]);
    char bounded[X4_PROBE_FILE_PATH_MAX];
    for(unsigned i=0;i<X4_PROBE_ALL_CASE_COUNT;++i)
        for(unsigned mode=0;mode<2;++mode)
            for(unsigned route=0;route<3;++route) {
                unsigned workers=route?route+1:0;char key[128];
                snprintf(key,sizeof(key),"%s-%s-p%u-pipeline",x4_probe_case_at(i)->id,
                    mode?"maximum-speed":"paced",workers);
                CHECK(!x4_probe_files_report_path(bounded,sizeof(bounded),key,UINT64_MAX));
                CHECK(strlen(bounded)<sizeof(bounded));
            }
}

static void collision_and_faults(const char *root)
{
    char directory[X4_PROBE_FILE_PATH_MAX];int n=snprintf(directory,sizeof(directory),"%s/faults",root);
    CHECK(n>0 && (size_t)n<sizeof(directory));CHECK(!mkdir(directory,0700));
    CHECK(!x4_probe_files_test_root(directory));
    Pipeline *p=fixture(x4_probe_case_at(8),X4_PROBE_MAX_SPEED,3,900000000);
    char path[X4_PROBE_FILE_PATH_MAX];report_name(p,"pipeline",path);
    FILE *f=x4_probe_files_report_create(path,NULL);CHECK(f);
    CHECK(fputs("keep-exclusive-report\n",f)>=0);CHECK(!x4_probe_files_report_finish(f,NULL));
    uint64_t before=checksum(path);errno=0;
    CHECK(export_pipeline(p)!=0);CHECK(errno==EEXIST);CHECK(checksum(path)==before);
    close_fixture(p);
    /* These are the real file layer's bounded offline fault hooks. They do
     * not enter PS4 builds, and never substitute the path formatter/writer. */
    const int faults[]={X4_PROBE_FILE_FAULT_CREATE,X4_PROBE_FILE_FAULT_FLUSH,
        X4_PROBE_FILE_FAULT_SYNC,X4_PROBE_FILE_FAULT_CLOSE};
    for(unsigned i=0;i<sizeof(faults)/sizeof(faults[0]);++i) {
        p=fixture(x4_probe_case_at(8),X4_PROBE_MAX_SPEED,3,910000000+i);
        x4_probe_files_test_fault(faults[i],ENOSPC);errno=0;
        CHECK(export_pipeline(p)!=0);CHECK(errno==ENOSPC);
        x4_probe_files_test_fault(X4_PROBE_FILE_FAULT_NONE,0);
        report_name(p,"pipeline",path);CHECK(access(path,F_OK)==-1);
        close_fixture(p);
    }
    CHECK(!x4_probe_files_test_root(root));
}

int main(int argc,char **argv)
{
    CHECK(argc==2 || (argc==3 && !strcmp(argv[2],"--expect-path-rejection")));
    CHECK(!x4_probe_files_test_root(argv[1]));CHECK(X4_PROBE_ALL_CASE_COUNT==9);
    real_path_contract();
    if(argc==3) {
        Pipeline *p=fixture(x4_probe_case_at(8),X4_PROBE_MAX_SPEED,3,123456789);
        errno=0;int rc=export_pipeline(p),saved=errno;
        CHECK(rc!=0 && saved==EINVAL);CHECK(file_count(argv[1])==0);close_fixture(p);
        puts("PASS negative original path[384]: actual export rejected with EINVAL; zero reports");
        return 0;
    }
    unsigned cases=0;
    for(unsigned i=0;i<X4_PROBE_ALL_CASE_COUNT;++i)
        for(unsigned mode=0;mode<2;++mode)
            for(unsigned route=0;route<3;++route) {
                Pipeline *p=fixture(x4_probe_case_at(i),mode?X4_PROBE_MAX_SPEED:X4_PROBE_PACED,
                    route?route+1:0,UINT64_C(100000000)+(uint64_t)cases*100000000);
                errno=0;CHECK(export_pipeline(p)==0);CHECK(errno==0);
                char metrics[X4_PROBE_FILE_PATH_MAX],pipeline[X4_PROBE_FILE_PATH_MAX];
                report_name(p,"metrics",metrics);report_name(p,"pipeline",pipeline);
                struct stat st;CHECK(!stat(metrics,&st) && S_ISREG(st.st_mode) && st.st_size>0);
                CHECK(!stat(pipeline,&st) && S_ISREG(st.st_mode) && st.st_size>0);
                uint64_t a=checksum(metrics),b=checksum(pipeline);errno=0;
                CHECK(export_pipeline(p)!=0);CHECK(errno==EEXIST);
                CHECK(checksum(metrics)==a && checksum(pipeline)==b);
                close_fixture(p);++cases;
            }
    CHECK(cases==54 && file_count(argv[1])==108);
    collision_and_faults(argv[1]);
    printf("PASS actual export/files/metrics: %u synthetic pairs; no native decoder or console execution\n",cases);
    return 0;
}
