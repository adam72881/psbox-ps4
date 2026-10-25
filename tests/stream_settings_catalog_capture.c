/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual catalog + settings renderer, synthetic catalog/access only. No
 * downloads, tokens, image decoder, runtime clock or console calls. */
#include "../src/ui/catalog_gui.h"
#include "../src/ui/stream_settings.h"
#include "../src/ui/font.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int x4_catalog_assets_acquire(X4CatalogAssets *a,const char *id,X4CatalogAssetView *out)
{ (void)a;(void)id;(void)out;return 0; }
void x4_catalog_assets_release(X4CatalogAssets *a,X4CatalogAssetView *view)
{ (void)a;(void)view; }
static void capture(const char *dir,const char *name,const uint32_t *pixels)
{
    if(!dir)return;
    char path[1024];int length=snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);
    assert(length>0&&(size_t)length<sizeof(path));FILE *f=fopen(path,"wb");assert(f);
    assert(fprintf(f,"P6\n1920 1080\n255\n")>0);
    for(unsigned i=0;i<1920u*1080u;++i){
        unsigned char rgb[3]={(unsigned char)pixels[i],(unsigned char)(pixels[i]>>8),(unsigned char)(pixels[i]>>16)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==2||argc==3);assert(!x4_font_init(argv[1]));
    const char *dir=argc==3?argv[2]:NULL;
    static X4CatalogSnapshot catalog;
    catalog.state=X4_CATALOG_READY;catalog.count=catalog.total=1;
    snprintf(catalog.titles[0].id,sizeof(catalog.titles[0].id),"fixture-game");
    snprintf(catalog.titles[0].name,sizeof(catalog.titles[0].name),"Juego de prueba · sin portada");
    X4CatalogBrowser browser;X4RecentGames recent;
    x4_catalog_browser_init(&browser);x4_recent_init(&recent);x4_catalog_browser_rebuild(&browser,&catalog);
    X4CatalogGuiStatus status={1,0,0,"Captura host · catálogo y acceso de prueba"};
    size_t count=1920u*1080u;uint32_t *owned=calloc(count+2,sizeof(*owned));assert(owned);
    uint32_t *pixels=owned+1;owned[0]=pixels[count]=0x1234abcd;
    for(unsigned profile=0;profile<X4_REQUEST_PROFILE_COUNT;++profile){
        assert(x4_stream_settings_set(X4_REQUEST_720P60,X4_COLOR_AUTO_709));
        X4StreamSettingsUi ui={0};assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
        unsigned moves=0;
        while((unsigned)ui.profile!=profile){
            assert(moves++<X4_REQUEST_PROFILE_COUNT);
            assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
        }
        /* Exactly one confirmation X applies and closes. Drawing must show
         * both current quality and the receipt without another confirmation. */
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
        assert(!ui.open&&ui.applied&&(unsigned)x4_stream_settings_get().profile==profile);
        char applied[96],summary[96],expected[96];
        assert(!x4_stream_settings_applied_text(&ui,applied,sizeof(applied)));
        X4StreamRequestConfig cfg=x4_stream_settings_get();
        snprintf(expected,sizeof(expected),"Aplicado: %s · CPU-%u",cfg.label,cfg.cpu_variant);assert(!strcmp(expected,applied));
        assert(x4_text_width_px(26,1,applied)<=552);
        x4_stream_settings_summary(summary,sizeof(summary));
        snprintf(expected,sizeof(expected),"Calidad: %s · CPU-%u · R3",cfg.label,cfg.cpu_variant);assert(!strcmp(summary,expected));
        assert(x4_text_width_px(28,1,summary)<=580);
        x4_catalog_gui_draw(&catalog,&browser,&recent,NULL,&status,pixels);
        x4_stream_settings_ui_draw(&ui,pixels);
        char name[48];snprintf(name,sizeof(name),"catalog-quality-applied-%u",profile);capture(dir,name,pixels);
        assert(owned[0]==0x1234abcd&&pixels[count]==0x1234abcd);
    }
    assert(x4_stream_settings_set(X4_REQUEST_720P60,X4_COLOR_AUTO_709));
    X4StreamSettingsUi cancelled={0};assert(x4_stream_settings_ui_handle(&cancelled,X4_SETTINGS_OPEN,true));
    assert(x4_stream_settings_ui_handle(&cancelled,X4_SETTINGS_RIGHT,true));
    assert(x4_stream_settings_ui_handle(&cancelled,X4_SETTINGS_CANCEL,true));
    assert(!cancelled.open&&!cancelled.applied&&x4_stream_settings_get().profile==X4_REQUEST_720P60);
    x4_catalog_gui_draw(&catalog,&browser,&recent,NULL,&status,pixels);
    x4_stream_settings_ui_draw(&cancelled,pixels);capture(dir,"catalog-cancelled-keeps-720",pixels);
    assert(owned[0]==0x1234abcd&&pixels[count]==0x1234abcd);
    free(owned);x4_font_close();puts("PASS actual catalog quality/one-X application/cancel renderer; six synthetic captures, no PS4");return 0;
}
