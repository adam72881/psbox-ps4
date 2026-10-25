/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ui/stream_settings.h"
#include "../src/ui/font.h"
#include "../src/video/stream_color_policy.h"

/* Interaction modes occupy existing UI padding; eight draft rows remain local. */
_Static_assert(sizeof(X4StreamSettingsUi)==24&&offsetof(X4StreamSettingsUi,row)==4&&
    offsetof(X4StreamSettingsUi,interaction_modes)==2&&
    offsetof(X4StreamSettingsUi,profile)==8&&offsetof(X4StreamSettingsUi,color)==12&&
    offsetof(X4StreamSettingsUi,cpu)==16&&offsetof(X4StreamSettingsUi,depth)==20,
    "explicit x86_64 UI draft layout");
_Static_assert((X4_INTERACTION_VSYNC_OFF<<8)==(1u<<11)&&
    !(X4_INTERACTION_VSYNC_OFF&(X4_INTERACTION_INPUT_60HZ|
        X4_INTERACTION_RUMBLE_OFF|X4_INTERACTION_AUDIO_PREVIOUS)),
    "vertical synchronization exclusively occupies settings bit 11");

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"settings check line %d: %s\n",__LINE__,#x);abort();}}while(0)
static void policy(void)
{
    for(unsigned preference=0;preference<2;++preference) {
        CHECK(x4_stream_color_resolve(1,1280,720,preference)==1);
        CHECK(x4_stream_color_resolve(1,1920,1080,preference)==1);
        CHECK(x4_stream_color_resolve(5,1920,1080,preference)==6);
        CHECK(x4_stream_color_resolve(6,1280,720,preference)==6);
        CHECK(x4_stream_color_resolve(2,960,540,preference)==6);
        CHECK(x4_stream_color_resolve(2,1280,720,preference)==(preference?6:1));
        CHECK(x4_stream_color_resolve(2,1920,1080,preference)==(preference?6:1));
        CHECK(x4_stream_color_resolve(2,1920,719,preference)==6);
        CHECK(x4_stream_color_resolve(9,1920,1080,preference)==6);
        CHECK(x4_stream_color_resolve(0,1280,720,preference)==6);
    }
    CHECK(x4_stream_color_resolve(2,1280,720,UINT_MAX)==6);
}
static void model(void)
{
    X4StreamSettingsUi ui={0};
    X4StreamRequestConfig initial=x4_stream_settings_get();
    CHECK(initial.profile==X4_REQUEST_720P_HQ&&initial.color_preference==X4_COLOR_AUTO_709);
    CHECK(initial.cpu_variant==X4_CPU_0&&initial.decoder_depth==X4_DEPTH_AUTO&&initial.interaction_modes==0);
    /* Keep the existing navigation fixture explicit; only process default changed. */
    CHECK(x4_stream_settings_set(X4_REQUEST_720P60,X4_COLOR_AUTO_709));
    CHECK(!x4_stream_settings_ui_handle(NULL,0,true));
    CHECK(!x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    CHECK(!x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true));
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN|X4_SETTINGS_CONFIRM,true));
    CHECK(ui.open&&ui.profile==X4_REQUEST_720P60);
    const unsigned visual_order[]={0,4,1,5,2,3};
    for(unsigned i=1;i<=X4_REQUEST_PROFILE_COUNT;++i){
        CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT|X4_SETTINGS_CONFIRM,true));
        CHECK(ui.open&&(unsigned)ui.profile==visual_order[i%X4_REQUEST_PROFILE_COUNT]);
        CHECK(x4_stream_settings_get().profile==X4_REQUEST_720P60);
    }
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true));
    CHECK(ui.profile==X4_REQUEST_1080P_HQ);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_DOWN,true)&&ui.row==1);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT|X4_SETTINGS_CONFIRM,true));
    CHECK(ui.open&&ui.color==X4_COLOR_FORCE_601);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    CHECK(!ui.open&&ui.applied&&ui.profile==X4_REQUEST_1080P_HQ);
    CHECK(x4_stream_settings_get().profile==X4_REQUEST_1080P_HQ);
    CHECK(x4_stream_settings_get().color_preference==X4_COLOR_FORCE_601);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    CHECK(ui.profile==X4_REQUEST_1080P_HQ&&ui.color==X4_COLOR_FORCE_601&&!ui.applied);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL|X4_SETTINGS_CONFIRM,true));
    CHECK(!ui.open&&!ui.applied&&x4_stream_settings_get().profile==X4_REQUEST_1080P_HQ);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,false));
    CHECK(!ui.open&&x4_stream_settings_get().profile==X4_REQUEST_1080P_HQ);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN|X4_SETTINGS_CONFIRM,false));
    CHECK(!ui.open);
    CHECK(x4_stream_settings_set(X4_REQUEST_720P60,X4_COLOR_AUTO_709));
    for(unsigned p=0;p<X4_REQUEST_PROFILE_COUNT;++p)for(unsigned c=0;c<2;++c){
        CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
        ui.profile=(X4StreamRequestProfile)p;ui.color=(X4StreamColorPreference)c;
        CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
        X4StreamRequestConfig cfg=x4_stream_settings_get();
        CHECK((unsigned)cfg.profile==p&&(unsigned)cfg.color_preference==c);
        CHECK(ui.applied&&ui.profile==(X4StreamRequestProfile)p&&!ui.open);
    }
}
static void one_confirmation(void)
{
    X4StreamSettingsUi ui={0};char line[96];
    CHECK(x4_stream_settings_set(X4_REQUEST_720P60,X4_COLOR_AUTO_709));
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
    CHECK(ui.open&&ui.profile==X4_REQUEST_720P_HQ);
    CHECK(x4_stream_settings_get().profile==X4_REQUEST_720P60);
    CHECK(x4_stream_settings_applied_text(&ui,line,sizeof(line))==-1&&line[0]==0);
    /* Main enters its launch branch only if the UI frame was not consumed.
     * Source contract checks pin that production branch. The single X commits
     * now; it cannot also launch. There is no second settings confirmation. */
    bool consumed=x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true);
    CHECK(consumed&&!ui.open&&ui.applied);
    CHECK(x4_stream_settings_get().profile==X4_REQUEST_720P_HQ);
    bool launch_from_this_x=!consumed;CHECK(!launch_from_this_x);
    CHECK(!x4_stream_settings_applied_text(&ui,line,sizeof(line))&&!strcmp(line,"Aplicado: 720p-HQ · CPU-0"));
    CHECK(!x4_stream_settings_ui_handle(&ui,0,true));
    /* A later, fresh X belongs to the catalog, rather than a hidden second
     * confirmation. Existing movement/refresh guards remain in main. */
    CHECK(!x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true)&&!ui.applied);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT|X4_SETTINGS_CONFIRM,true));
    CHECK(ui.open&&!ui.applied&&x4_stream_settings_get().profile==X4_REQUEST_720P_HQ);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL,true));
    CHECK(!ui.open&&!ui.applied&&x4_stream_settings_get().profile==X4_REQUEST_720P_HQ);
    for(unsigned profile=0;profile<X4_REQUEST_PROFILE_COUNT;++profile){
        CHECK(x4_stream_settings_set((X4StreamRequestProfile)profile,X4_COLOR_AUTO_709));
        x4_stream_settings_summary(line,sizeof(line));
        X4StreamRequestConfig cfg=x4_stream_settings_get();char expected[96];
        snprintf(expected,sizeof(expected),"Calidad: %s · CPU-%u · R3",cfg.label,cfg.cpu_variant);
        CHECK(!strcmp(line,expected)&&x4_text_width_px(28,1,line)<=580);
    }
    CHECK(x4_stream_settings_applied_text(NULL,line,sizeof(line))==-1);
    CHECK(x4_stream_settings_applied_text(&ui,NULL,0)==-1);
}
static void interaction_options(void)
{
    for(unsigned modes=0;modes<=X4_INTERACTION_MASK;++modes) {
        CHECK(x4_stream_settings_set_extended(X4_REQUEST_1080P_G8,X4_COLOR_FORCE_601,
            X4_CPU_2,X4_DEPTH_2,modes));
        X4StreamRequestConfig captured=x4_stream_request_capture(NULL);
        CHECK(captured.interaction_modes==modes&&captured.profile==X4_REQUEST_1080P_G8&&
            captured.color_preference==X4_COLOR_FORCE_601&&captured.cpu_variant==X4_CPU_2&&
            captured.decoder_depth==X4_DEPTH_2);
        CHECK(x4_stream_settings_set(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709));
        X4StreamRequestConfig current=x4_stream_settings_get();
        CHECK(current.interaction_modes==modes&&current.cpu_variant==X4_CPU_2&&current.decoder_depth==X4_DEPTH_2);
        CHECK(x4_stream_settings_set_cpu(X4_REQUEST_1080P_H,X4_COLOR_FORCE_601,X4_CPU_1));
        current=x4_stream_settings_get();
        CHECK(current.interaction_modes==modes&&current.cpu_variant==X4_CPU_1&&current.decoder_depth==X4_DEPTH_2);
        CHECK(x4_stream_settings_set_all(X4_REQUEST_1080P_HQ,X4_COLOR_AUTO_709,X4_CPU_3,X4_DEPTH_1));
        current=x4_stream_settings_get();
        CHECK(current.interaction_modes==modes&&current.cpu_variant==X4_CPU_3&&current.decoder_depth==X4_DEPTH_1);
        CHECK(captured.interaction_modes==modes&&captured.profile==X4_REQUEST_1080P_G8&&
            captured.color_preference==X4_COLOR_FORCE_601&&captured.cpu_variant==X4_CPU_2&&
            captured.decoder_depth==X4_DEPTH_2); /* launch snapshot is by value */
        CHECK(!x4_stream_settings_set_extended(X4_REQUEST_720P60,X4_COLOR_AUTO_709,
            X4_CPU_0,X4_DEPTH_AUTO,X4_INTERACTION_MASK+1u));
        CHECK(!x4_stream_settings_set_extended((X4StreamRequestProfile)-1,X4_COLOR_AUTO_709,
            X4_CPU_0,X4_DEPTH_AUTO,0));
        CHECK(!x4_stream_settings_set_extended(X4_REQUEST_720P60,(X4StreamColorPreference)-1,
            X4_CPU_0,X4_DEPTH_AUTO,0));
        CHECK(!x4_stream_settings_set_extended(X4_REQUEST_720P60,X4_COLOR_AUTO_709,
            (X4StreamCpuVariant)-1,X4_DEPTH_AUTO,0));
        CHECK(!x4_stream_settings_set_extended(X4_REQUEST_720P60,X4_COLOR_AUTO_709,
            X4_CPU_0,(X4StreamDecoderDepth)-1,0));
        CHECK(x4_stream_settings_get().interaction_modes==modes&&
            x4_stream_settings_get().profile==X4_REQUEST_1080P_HQ);
        for(unsigned profile=0;profile<X4_REQUEST_PROFILE_COUNT;++profile)
            for(unsigned color=0;color<2;++color)for(unsigned cpu=0;cpu<4;++cpu)
                for(unsigned depth=0;depth<3;++depth) {
                    CHECK(x4_stream_settings_set_extended((X4StreamRequestProfile)profile,
                        (X4StreamColorPreference)color,(X4StreamCpuVariant)cpu,
                        (X4StreamDecoderDepth)depth,modes));
                    current=x4_stream_settings_get();
                    CHECK((unsigned)current.profile==profile&&(unsigned)current.color_preference==color&&
                        current.cpu_variant==cpu&&current.decoder_depth==depth&&current.interaction_modes==modes);
                }
    }
    CHECK(x4_stream_settings_set_extended(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709,
        X4_CPU_0,X4_DEPTH_AUTO,0));
    X4StreamSettingsUi ui={0};
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN|X4_SETTINGS_CONFIRM,true));
    CHECK(ui.open&&ui.interaction_modes==0);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_UP,true)&&ui.row==7);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_DOWN,true)&&ui.row==0);
    for(unsigned row=1;row<=7;++row)CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_DOWN,true)&&ui.row==row);
    for(unsigned row=4;row<=7;++row) {
        ui.row=row;
        CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT|X4_SETTINGS_CONFIRM,true));
        CHECK(ui.open&&!ui.applied&&x4_stream_settings_get().interaction_modes==0);
    }
    CHECK(ui.interaction_modes==X4_INTERACTION_MASK);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL|X4_SETTINGS_CONFIRM,true));
    CHECK(!ui.open&&x4_stream_settings_get().interaction_modes==0);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true)&&ui.interaction_modes==0);
    ui.interaction_modes=X4_INTERACTION_MASK;
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    CHECK(!ui.open&&ui.applied&&x4_stream_settings_get().interaction_modes==X4_INTERACTION_MASK);
    X4StreamRequestConfig next=x4_stream_request_capture(NULL);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true)&&ui.interaction_modes==X4_INTERACTION_MASK);
    ui.interaction_modes=0;
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,false));
    CHECK(!ui.open&&x4_stream_settings_get().interaction_modes==X4_INTERACTION_MASK&&next.interaction_modes==X4_INTERACTION_MASK);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    ui.row=4;CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true));
    CHECK(ui.interaction_modes==(X4_INTERACTION_RUMBLE_OFF|X4_INTERACTION_AUDIO_PREVIOUS|X4_INTERACTION_VSYNC_OFF));
    ui.row=5;CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true));
    CHECK(ui.interaction_modes==(X4_INTERACTION_AUDIO_PREVIOUS|X4_INTERACTION_VSYNC_OFF));
    ui.row=6;CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true));
    CHECK(ui.interaction_modes==X4_INTERACTION_VSYNC_OFF);
    ui.row=7;CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true));
    CHECK(ui.interaction_modes==0);
    CHECK(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    CHECK(x4_stream_settings_get().interaction_modes==0&&next.interaction_modes==X4_INTERACTION_MASK);
}
static void write_ppm(const char *dir,const char *name,const uint32_t *p)
{
    if(!dir)return;
    char path[1024];int n=snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);
    CHECK(n>0&&(size_t)n<sizeof(path));
    FILE *f=fopen(path,"wb");CHECK(f);
    CHECK(fprintf(f,"P6\n1920 1080\n255\n")>0);
    for(unsigned i=0;i<1920u*1080u;++i){
        unsigned char rgb[3]={(unsigned char)p[i],(unsigned char)(p[i]>>8),(unsigned char)(p[i]>>16)};
        CHECK(fwrite(rgb,1,3,f)==3);
    }
    CHECK(!fclose(f));
}
static void render(const char *dir)
{
    size_t count=1920u*1080u;
    uint32_t *guarded=malloc((count+32)*sizeof(*guarded));CHECK(guarded);
    for(size_t i=0;i<count+32;++i)guarded[i]=0x5a5a5a5a;
    uint32_t *p=guarded+16;
    for(unsigned profile=0;profile<X4_REQUEST_PROFILE_COUNT;++profile){
        X4StreamSettingsUi ui={.open=true,.row=profile&1u,.profile=(X4StreamRequestProfile)profile,
            .color=(X4StreamColorPreference)(profile&1u)};
        memset(p,0,count*sizeof(*p));
        x4_stream_settings_ui_draw(&ui,p);
        char name[32];snprintf(name,sizeof(name),"settings-%u",profile);write_ppm(dir,name,p);
    }
    for(unsigned cpu=0;cpu<=X4_CPU_3;++cpu){
        X4StreamSettingsUi ui={.open=true,.row=2,.profile=X4_REQUEST_1080P_G8,
            .color=X4_COLOR_AUTO_709,.cpu=(X4StreamCpuVariant)cpu};
        memset(p,0,count*sizeof(*p));x4_stream_settings_ui_draw(&ui,p);
        char name[32];snprintf(name,sizeof(name),"settings-cpu-%u",cpu);write_ppm(dir,name,p);
    }
    for(unsigned depth=0;depth<=X4_DEPTH_2;++depth){
        X4StreamSettingsUi ui={.open=true,.row=3,.profile=X4_REQUEST_1080P_G8,
            .color=X4_COLOR_AUTO_709,.cpu=X4_CPU_0,.depth=(X4StreamDecoderDepth)depth};
        memset(p,0,count*sizeof(*p));x4_stream_settings_ui_draw(&ui,p);
        char name[32];snprintf(name,sizeof(name),"settings-depth-%u",depth);write_ppm(dir,name,p);
    }
    for(unsigned modes=0;modes<=X4_INTERACTION_MASK;++modes) {
        X4StreamSettingsUi ui={.open=true,.row=modes==4u?0u:4u+modes%4u,.profile=X4_REQUEST_1080P_G8,
            .color=X4_COLOR_AUTO_709,.interaction_modes=(unsigned char)modes};
        memset(p,0,count*sizeof(*p));x4_stream_settings_ui_draw(&ui,p);
        char name[32];snprintf(name,sizeof(name),"settings-interaction-%u",modes);write_ppm(dir,name,p);
    }
    CHECK(x4_text_width_px(26,0,"No (menos retraso, puede verse un corte)")<=1220);
    char text[224];
    CHECK(!x4_stream_received_text(text,sizeof(text),1280,720,2,1,false));
    CHECK(strstr(text,"Recibido 1280×720")&&strstr(text,"SPS 2 (sin especificar)")&&strstr(text,"BT.709")&&strstr(text,"limitado"));
    CHECK(!x4_stream_received_text(text,sizeof(text),1920,1080,1,1,true));
    CHECK(strstr(text,"1920×1080")&&strstr(text,"completo"));
    CHECK(x4_text_width_px(30,1,text)<1712);
    CHECK(!x4_stream_received_text(text,sizeof(text),UINT_MAX,UINT_MAX,UINT_MAX,6,true));
    char tiny[2];CHECK(x4_stream_received_text(tiny,sizeof(tiny),1280,720,2,1,false)==-1&&tiny[1]==0);
    CHECK(x4_stream_received_text(NULL,0,0,0,0,0,false)==-1);
    memset(p,0,count*sizeof(*p));
    x4_stream_received_draw(p,1920,1080,2,1,false);write_ppm(dir,"received-1080-auto",p);
    for(unsigned i=0;i<16;++i)CHECK(guarded[i]==0x5a5a5a5a&&guarded[count+16+i]==0x5a5a5a5a);
    free(guarded);
}
int main(int argc,char **argv)
{
    CHECK(argc==2||argc==3);CHECK(!x4_font_init(argv[1]));
    policy();model();one_confirmation();interaction_options();render(argc==3?argv[2]:NULL);x4_font_close();
    printf("stream settings/color: %u checks passed\n",checks);return 0;
}
