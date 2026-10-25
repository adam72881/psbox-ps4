/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/catalog_gui.h"
#include "../src/ui/font.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixture adapter supplies immutable, decoded public covers. Drawing,
 * fonts, navigation and metadata parser are the actual production code.
 * Synthetic access bits/MRU are explicit fixtures, never console results. */
struct X4CatalogAssets { X4CatalogAssetView entries[6]; unsigned count; };
int x4_catalog_assets_acquire(X4CatalogAssets*a,const char*id,X4CatalogAssetView*out)
{
    if(!a||!id||!out)return 0;
    for(unsigned i=0;i<a->count;++i)if(!strcmp(id,a->entries[i].metadata.product_id)){
        *out=a->entries[i];out->slot=i;return 1;
    }
    return 0;
}
void x4_catalog_assets_release(X4CatalogAssets*a,X4CatalogAssetView*v){(void)a;(void)v;}
static unsigned char*read_file(const char*path,size_t*n)
{
    FILE*f=fopen(path,"rb");if(!f)return NULL;
    assert(!fseek(f,0,SEEK_END));long len=ftell(f);assert(len>=0&&len<=2*1024*1024);
    rewind(f);unsigned char*p=malloc((size_t)len+1);assert(p);
    assert(fread(p,1,(size_t)len,f)==(size_t)len);fclose(f);p[len]=0;*n=(size_t)len;return p;
}
static void capture(const char*dir,const char*name,X4CatalogSnapshot*c,X4CatalogBrowser*b,
    X4RecentGames*r,X4CatalogAssets*a,uint32_t*p)
{
    X4CatalogGuiStatus status={1,0,0,"Captura host · acceso y recientes de prueba"};
    x4_catalog_gui_draw(c,b,r,a,&status,p);
    assert(p[-1]==0x1234abcd&&p[1920*1080]==0x1234abcd);
    char path[768];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);
    FILE*f=fopen(path,"wb");assert(f);fprintf(f,"P6\n1920 1080\n255\n");
    for(size_t i=0;i<1920*1080;++i){unsigned char rgb[3]={p[i]&255,(p[i]>>8)&255,(p[i]>>16)&255};assert(fwrite(rgb,1,3,f)==3);}
    assert(!fclose(f));printf("capture PASS %s\n",name);
}
static uint64_t detail_fingerprint(const uint32_t*p)
{
    uint64_t hash=UINT64_C(1469598103934665603);
    for(int y=230;y<518;++y)for(int x=1284;x<1796;++x){hash^=p[y*1920+x];hash*=UINT64_C(1099511628211);}
    return hash;
}
static void cover_states(const char*dir,X4CatalogSnapshot*c,X4CatalogBrowser*b,
    X4RecentGames*r,X4CatalogAssets*a,uint32_t*p)
{
    static const enum X4CatalogAssetState states[]={X4_ASSET_PENDING,X4_ASSET_DOWNLOADING,
        X4_ASSET_TEMPORARY_FAILURE,X4_ASSET_MISSING};
    static const char*labels[]={"Pendiente","Descargando…","Reintento pendiente","Sin imagen"};
    X4CoverRaster saved[6][X4_COVER_SURFACES];uint64_t hashes[4];
    for(unsigned i=0;i<6;++i){
        for(unsigned j=0;j<X4_COVER_SURFACES;++j){saved[i][j]=a->entries[i].surface[j];
            if(i<4)memset(&a->entries[i].surface[j],0,sizeof(a->entries[i].surface[j]));}
        if(i<4)a->entries[i].state=states[i];
    }
    X4CatalogGuiStatus status={1,0,0,"Estados de portada · prueba host"};
    for(unsigned i=0;i<4;++i){
        assert(!strcmp(x4_catalog_gui_asset_status(states[i]),labels[i]));
        a->entries[0].state=states[i];x4_catalog_gui_draw(c,b,r,a,&status,p);
        assert(p[-1]==0x1234abcd&&p[1920*1080]==0x1234abcd);
        hashes[i]=detail_fingerprint(p);for(unsigned j=0;j<i;++j)assert(hashes[i]!=hashes[j]);
    }
    assert(!strcmp(x4_catalog_gui_asset_status(X4_ASSET_READY),"Lista"));
    a->entries[0].state=X4_ASSET_PENDING;
    capture(dir,"07-cover-states",c,b,r,a,p);
    for(unsigned i=0;i<6;++i){
        for(unsigned j=0;j<X4_COVER_SURFACES;++j)a->entries[i].surface[j]=saved[i][j];
        a->entries[i].state=X4_ASSET_READY;
    }
    puts("GUI cover states PASS (distinct pending/downloading/temporary/missing pixels; clip canaries intact)");
}
int main(int argc,char**argv)
{
    assert(argc==3);assert(x4_font_init("assets/fonts/noto_sans.x4font")==0);
    static X4CatalogSnapshot c;X4CatalogBrowser b;X4RecentGames r;struct X4CatalogAssets assets={0};
    c.state=X4_CATALOG_READY;c.count=c.total=6;x4_catalog_browser_init(&b);x4_recent_init(&r);
    char path[768],id[32];snprintf(path,sizeof(path),"%s/products.txt",argv[1]);FILE*list=fopen(path,"r");assert(list);
    for(unsigned i=0;i<6;++i){
        assert(fscanf(list,"%31s",id)==1);size_t n;
        snprintf(path,sizeof(path),"%s/%u.json",argv[1],i);unsigned char*json=read_file(path,&n);assert(json);
        assert(!x4_product_metadata_parse((const char*)json,n,id,&assets.entries[i].metadata));free(json);
        snprintf(path,sizeof(path),"%s/%u.jpg",argv[1],i);unsigned char*image=read_file(path,&n);
        if(image){X4CoverRasters rasters={0};assert(!x4_cover_decode(image,n,&rasters));
            for(unsigned j=0;j<X4_COVER_SURFACES;++j)assets.entries[i].surface[j]=rasters.surface[j];
            free(image);}
        assets.entries[i].state=X4_ASSET_READY;assets.entries[i].generation=1;++assets.count;
        snprintf(c.titles[i].id,sizeof(c.titles[i].id),"fixture-title-%u",i);
        snprintf(c.titles[i].product_id,sizeof(c.titles[i].product_id),"%s",id);
        snprintf(c.titles[i].name,sizeof(c.titles[i].name),"%s",assets.entries[i].metadata.name);
        c.titles[i].entitled=i%3!=2;
    }fclose(list);
    x4_catalog_browser_rebuild(&b,&c);
    uint32_t*owned=calloc(1920*1080+2,sizeof(uint32_t));assert(owned);uint32_t*p=owned+1;
    owned[0]=p[1920*1080]=0x1234abcd;
    capture(argv[2],"01-catalog",&c,&b,&r,&assets,p);
    cover_states(argv[2],&c,&b,&r,&assets,p);
    char ids[6][96];for(unsigned i=0;i<6;++i)snprintf(ids[i],sizeof(ids[i]),"%s",c.titles[5-i].id);
    x4_recent_begin(&r,1,1);assert(x4_recent_publish(&r,1,1,ids,6,0,200,&c));x4_catalog_browser_sync_recents(&b,&r);
    capture(argv[2],"02-server-recents",&c,&b,&r,&assets,p);
    b.editing=true;snprintf(b.query,sizeof(b.query),"FORZA");capture(argv[2],"03-search",&c,&b,&r,&assets,p);b.editing=false;
    x4_catalog_browser_init(&b);c.count=c.total=4;x4_catalog_browser_rebuild(&b,&c);
    capture(argv[2],"04-incomplete",&c,&b,&r,&assets,p);
    snprintf(c.titles[0].name,sizeof(c.titles[0].name),"¿El pingüino soñó? ¡Sí! Edición extraordinariamente larga: aventuras de mañana");
    assets.entries[0].metadata.name[0]=0;assets.entries[0].metadata.description[0]=0;
    c.titles[0].entitled=0;x4_catalog_browser_rebuild(&b,&c);
    capture(argv[2],"05-utf8-missing-cover",&c,&b,&r,NULL,p);
    c.count=c.total=0;x4_catalog_browser_rebuild(&b,&c);x4_recent_begin(&r,1,2);x4_recent_publish(&r,1,2,NULL,0,-1,503,&c);
    b.section=X4_CATALOG_SECTION_RECENT;capture(argv[2],"06-empty-error",&c,&b,&r,NULL,p);
    for(unsigned i=0;i<assets.count;++i){X4CoverRasters rasters={0};for(unsigned j=0;j<X4_COVER_SURFACES;++j)rasters.surface[j]=assets.entries[i].surface[j];x4_cover_rasters_free(&rasters);}
    free(owned);x4_font_close();puts("GUI production-renderer fixtures PASS (7 captures; clip canaries intact)");return 0;
}
