/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_gui.h"
#include "font.h"
#include "lang.h"
#include "../core/build_identity.h"
#include "stream_settings.h"
#include <stdio.h>
#include <string.h>

#define W 1920
#define H 1080
static const uint32_t bg=0xff1d1510, panel=0xff30271e, edge=0xff4c4135;
static const uint32_t white=0xfff5f3ee, muted=0xffc0b4a5, green=0xff90dc26;

static void rect(uint32_t *p,int x,int y,int w,int h,uint32_t c)
{
    int right=x+w,bottom=y+h;
    if(x<0)x=0;
    if(y<0)y=0;
    if(right>W)right=W;
    if(bottom>H)bottom=H;
    for(int row=y;row<bottom;++row)for(int col=x;col<right;++col)p[row*W+col]=c;
}
static void frame(uint32_t *p,int x,int y,int w,int h,int thickness,uint32_t c)
{
    rect(p,x,y,w,thickness,c); rect(p,x,y+h-thickness,w,thickness,c);
    rect(p,x,y,thickness,h,c); rect(p,x+w-thickness,y,thickness,h,c);
}
static void txt(uint32_t*p,int x,int y,int size,int bold,const char*s,int width,uint32_t c)
{ x4_text_ellipsis_px(p,x,y,size,bold,s?s:"",width,c); }
static void blit(uint32_t *p,int x,int y,const X4CoverRaster *r)
{
    if(!r||!r->rgba||x<0||y<0||x+(int)r->width>W||y+(int)r->height>H)return;
    for(unsigned row=0;row<r->height;++row)
        memcpy(p+(y+(int)row)*W+x,r->rgba+row*r->width,r->width*sizeof(uint32_t));
}
const char *x4_catalog_gui_asset_status(enum X4CatalogAssetState state)
{
    switch(state){
        case X4_ASSET_DOWNLOADING:return x4_tr(X4_TR_ASSET_DOWNLOADING);
        case X4_ASSET_READY:return x4_tr(X4_TR_ASSET_READY);
        case X4_ASSET_TEMPORARY_FAILURE:return x4_tr(X4_TR_ASSET_RETRY_PENDING);
        case X4_ASSET_MISSING:return x4_tr(X4_TR_ASSET_MISSING);
        case X4_ASSET_PENDING:default:return x4_tr(X4_TR_ASSET_PENDING);
    }
}
static void placeholder(uint32_t *p,int x,int y,int w,int h,const char*id,int small,
    enum X4CatalogAssetState state)
{
    for(int row=0;row<h;++row){
        unsigned shade=24u+(unsigned)row*15u/(unsigned)(h?h:1);
        rect(p,x,y+row,w,1,0xff000000u|((shade+8u)<<16)|((shade+2u)<<8)|shade);
    }
    int cx=x+w/2,cy=small?y+23:y+h/2-28;
    frame(p,cx-28,cy-18,56,36,2,edge);
    rect(p,cx-17,cy-2,16,3,muted);rect(p,cx-11,cy-8,3,16,muted);
    rect(p,cx+10,cy-6,4,4,muted);rect(p,cx+17,cy+2,4,4,muted);
    const char *label=x4_catalog_gui_asset_status(state);
    /* A READY entry without this raster is an absent image, not loading. */
    if(state==X4_ASSET_READY)label=x4_tr(X4_TR_ASSET_MISSING);
    if(small){
        if(state==X4_ASSET_DOWNLOADING)label=x4_tr(X4_TR_ASSET_SMALL_LOADING);
        else if(state==X4_ASSET_TEMPORARY_FAILURE)label=x4_tr(X4_TR_ASSET_SMALL_RETRY);
    }
    int size=small?18:26,width=w-(small?8:32);
    int measured=x4_text_width_px(size,0,label);
    int tx=measured<width?x+(w-measured)/2:x+(small?4:16);
    txt(p,tx,small?y+43:cy+28,size,0,label,width,muted);
    if(!small)txt(p,x+16,y+h-39,26,0,id,w-32,muted);
}
static void cover(uint32_t*p,X4CatalogAssets*a,const char*product,int surface,int x,int y,int w,int h,const char*id)
{
    X4CatalogAssetView v;
    if(a&&product&&x4_catalog_assets_acquire(a,product,&v)){
        if(v.surface[surface].rgba)blit(p,x,y,&v.surface[surface]);
        else placeholder(p,x,y,w,h,id,surface==X4_COVER_RECENT,v.state);
        x4_catalog_assets_release(a,&v);
    }else placeholder(p,x,y,w,h,id,surface==X4_COVER_RECENT,
        product&&product[0]?X4_ASSET_PENDING:X4_ASSET_MISSING);
}
/* Limited word wrapping on UTF-8 boundaries, with width measured by the
 * actual atlas. Final line uses safe ellipsis; no byte-substring clipping. */
static int wrapped(uint32_t*p,int x,int y,int size,int bold,const char*s,int width,int lines,uint32_t c)
{
    int line=0;const char*at=s?s:"";
    while(*at&&line<lines){
        if(line==lines-1){txt(p,x,y+line*(size+9),size,bold,at,width,c);++line;break;}
        char out[512];size_t n=0,last_space=0;const char*end=at;
        while(*end&&*end!='\n'){
            const char*next=end;x4_utf8_next(&next);size_t bytes=(size_t)(next-end);
            if(n+bytes>=sizeof(out))break;
            memcpy(out+n,end,bytes);out[n+bytes]=0;
            if(x4_text_width_px(size,bold,out)>width)break;
            n+=bytes;if(*end==' ')last_space=n;end=next;
        }
        if(!n){txt(p,x,y+line*(size+9),size,bold,at,width,c);++line;break;}
        if(*end&&*end!='\n'&&last_space)n=last_space;
        memcpy(out,at,n);out[n]=0;
        while(n&&out[n-1]==' ')out[--n]=0;
        txt(p,x,y+line*(size+9),size,bold,out,width,c);++line;
        at+=n;while(*at==' '||*at=='\n')++at;
    }
    return line;
}
static void pill(uint32_t*p,int x,int y,int width,const char*label,int active)
{
    rect(p,x,y,width,44,active?0xff362b18:panel);
    frame(p,x,y,width,44,active?2:1,active?green:edge);
    txt(p,x+14,y+5,26,active,label,width-28,active?green:muted);
}
static void keyboard(uint32_t*p,const X4CatalogBrowser*b)
{
    rect(p,420,290,1080,670,bg);frame(p,420,290,1080,670,3,green);
    txt(p,456,320,38,1,x4_tr(X4_TR_KB_TITLE),1008,white);
    rect(p,456,385,1008,62,panel);
    txt(p,480,397,32,0,b->query[0]?b->query:x4_tr(X4_TR_KB_PLACEHOLDER),950,b->query[0]?white:muted);
    static const char keys[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for(unsigned i=0;i<36;++i){
        int x=456+(int)(i%6)*170,y=472+(int)(i/6)*60;char label[2]={keys[i],0};
        rect(p,x,y,156,50,i==b->keyboard_cursor?green:panel);
        txt(p,x+64,y+4,32,1,label,72,i==b->keyboard_cursor?bg:white);
    }
    txt(p,456,854,26,0,x4_tr(X4_TR_KB_HELP1),1008,white);
#if X4_PUBLIC
    txt(p,456,902,26,0,x4_tr(X4_TR_KB_HELP2_PUBLIC),1008,muted);
#else
    txt(p,456,902,26,0,x4_tr(X4_TR_KB_HELP2_LAB),1008,muted);
#endif
}

void x4_catalog_gui_draw(const X4CatalogSnapshot*c,const X4CatalogBrowser*b,
    const X4RecentGames*r,X4CatalogAssets*a,const X4CatalogGuiStatus*s,uint32_t*p)
{
    rect(p,0,0,W,H,bg);
    txt(p,84,50,56,1,"XCloud",520,white);
    int logo_end=x4_text_width_px(56,1,"XCloud");txt(p,84+logo_end,50,56,1,"4",100,green);
    txt(p,1250,68,28,0,s&&s->connected?x4_tr(X4_TR_CAT_DS4_CONNECTED):x4_tr(X4_TR_CAT_DS4_CONNECT),586,s&&s->connected?white:muted);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE && !X4_PUBLIC
    char video_settings[192];
    x4_stream_settings_summary(video_settings,sizeof(video_settings));
    txt(p,650,68,28,1,video_settings,580,green);
#endif
    txt(p,84,143,32,1,x4_tr(X4_TR_CAT_TITLE),230,white);rect(p,84,187,155,4,green);
    rect(p,1260,132,576,58,panel);frame(p,1260,132,576,58,1,edge);
    txt(p,1280,143,28,0,b->query[0]?b->query:x4_tr(X4_TR_CAT_SEARCH_BTN),536,b->query[0]?white:muted);
    rect(p,84,203,1752,1,edge);
    txt(p,84,220,28,1,x4_tr(X4_TR_CAT_RECENT_TITLE),850,white);
    unsigned nr=x4_recent_count(r),recent_start=b->recent_selected/6*6;
    if(nr){
        for(unsigned i=0;i<6&&recent_start+i<nr;++i){
            const X4RecentGame*g=x4_recent_get(r,recent_start+i);int x=84+(int)i*187;
            int selected=b->section==X4_CATALOG_SECTION_RECENT&&b->recent_selected==recent_start+i;
            rect(p,x,263,176,108,panel);
            cover(p,a,g->product_id,X4_COVER_RECENT,x+40,269,96,96,g->id);
            const X4CatalogTitle*ct=NULL;unsigned index;
            if(x4_recent_catalog_index(g,c,&index))ct=&c->titles[index];
            rect(p,x+4,334,168,33,0xff15120e);
            txt(p,x+9,334,26,0,ct?(ct->name[0]?ct->name:ct->id):(g->name[0]?g->name:g->id),158,white);
            if(selected)frame(p,x-3,260,182,114,3,green);
        }
    }else{
        rect(p,84,263,1110,108,panel);
        const char*label=x4_tr(X4_TR_CAT_RECENT_EMPTY);
        if(r&&r->state==X4_RECENT_LOADING)label=x4_tr(X4_TR_CAT_RECENT_LOADING);
        else if(r&&r->state==X4_RECENT_ERROR)label=x4_tr(X4_TR_CAT_RECENT_ERROR);
        txt(p,110,281,28,0,label,1050,muted);
        if(r&&r->state==X4_RECENT_ERROR)txt(p,110,324,26,0,x4_tr(X4_TR_CAT_RECENT_RETRY),1050,white);
        if(b->section==X4_CATALOG_SECTION_RECENT)frame(p,81,260,1116,114,3,green);
    }
    txt(p,84,391,32,1,x4_tr(X4_TR_CAT_NEXT_GAME),430,white);
    pill(p,540,391,128,x4_tr(X4_TR_CAT_FILTER_ALL),b->filter==X4_CATALOG_FILTER_ALL);
    pill(p,680,391,192,x4_tr(X4_TR_CAT_FILTER_ENTITLED),b->filter==X4_CATALOG_FILTER_CONFIRMED_ACCESS);
    pill(p,884,391,326,x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED),b->filter==X4_CATALOG_FILTER_UNCONFIRMED_ACCESS);
    unsigned count=c&&c->count<=X4_CATALOG_MAX&&!b->criteria_dirty&&b->filtered_count<=c->count?
        b->filtered_count:0;
    unsigned start=count&&b->selected<count?x4_catalog_browser_page_start(b):0;
    if(c&&c->state==X4_CATALOG_READY&&count){
        for(unsigned i=0;i<6&&start+i<count;++i){
            unsigned index=b->indices[start+i];if(index>=c->count||index>=X4_CATALOG_MAX)continue;
            const X4CatalogTitle*t=&c->titles[index];int x=84+(int)(i%3)*380,y=443+(int)(i/3)*269;
            rect(p,x,y,350,259,panel);cover(p,a,t->product_id,X4_COVER_GRID,x+15,y+10,320,224,t->id);
            rect(p,x+15,y+182,320,52,0xff15120e);
            txt(p,x+26,y+185,32,1,t->name[0]?t->name:t->id,299,white);
            if(b->section==X4_CATALOG_SECTION_CATALOG&&b->selected==start+i)
                frame(p,x-4,y-4,358,267,4,green);
        }
    }else{
        rect(p,84,450,1110,531,panel);
        const char*label=count?x4_tr(X4_TR_CAT_LOADING):x4_tr(X4_TR_CAT_NO_MATCH);
        if(!c||c->state==X4_CATALOG_IDLE)label=x4_tr(X4_TR_CAT_OPEN_ACCOUNT);
        else if(c->state==X4_CATALOG_LOADING)label=x4_tr(X4_TR_CAT_LOADING);
        else if(c->state==X4_CATALOG_ERROR||c->state==X4_CATALOG_CANCELLED)label=x4_tr(X4_TR_CAT_ERROR);
        txt(p,116,610,32,0,label,1045,muted);
    }
    rect(p,1236,220,1,764,edge);
    const X4CatalogTitle*selected=x4_catalog_browser_selected_title(b,c,r);
    const X4RecentGame*historical=b->section==X4_CATALOG_SECTION_RECENT?x4_recent_get(r,b->recent_selected):NULL;
    const char*product=selected?selected->product_id:historical?historical->product_id:"";
    const char*id=selected?selected->id:historical?historical->id:"";
    X4CatalogAssetView view;int acquired=a&&x4_catalog_assets_acquire(a,product,&view);
    if(selected||historical){
        if(acquired&&view.surface[X4_COVER_DETAIL].rgba)blit(p,1284,230,&view.surface[X4_COVER_DETAIL]);
        else placeholder(p,1284,230,512,288,id,0,acquired?view.state:
            product[0]?X4_ASSET_PENDING:X4_ASSET_MISSING);
        const char*name=selected?(selected->name[0]?selected->name:selected->id):(historical->name[0]?historical->name:historical->id);
        if(acquired&&view.metadata.name[0])name=view.metadata.name;
        int lines=wrapped(p,1284,540,46,1,name,512,2,white);int y=540+lines*55+12;
        if(acquired&&view.metadata.category[0]){txt(p,1284,y,28,0,view.metadata.category,512,muted);y+=45;}
        if(acquired&&view.metadata.publisher[0]){txt(p,1284,y,26,0,view.metadata.publisher,512,muted);y+=42;}
        int description_limit=selected&&selected->entitled?828:780;
        int description_lines=(description_limit-y)/37;
        if(description_lines>3)description_lines=3;
        if(acquired&&view.metadata.description[0]&&description_lines>0)
            wrapped(p,1284,y,28,0,view.metadata.description,512,description_lines,white);
        if(selected&&!selected->entitled)
            txt(p,1284,794,26,0,x4_tr(X4_TR_CAT_PERMISSION_UNKNOWN),512,muted);
        pill(p,1284,837,selected&&selected->entitled?214:300,
            selected?(selected->entitled?x4_tr(X4_TR_CAT_FILTER_ENTITLED):x4_tr(X4_TR_CAT_FILTER_UNCONFIRMED)):x4_tr(X4_TR_CAT_NOT_AVAILABLE),selected&&selected->entitled);
        int can_start=selected&&!(s&&(s->busy||s->pending));
        rect(p,1284,900,512,62,can_start?green:panel);
        const char*play_label=!selected?x4_tr(X4_TR_CAT_REFRESH_PROMPT):s&&s->pending?x4_tr(X4_TR_CAT_PREPARING):
            s&&s->busy?x4_tr(X4_TR_CAT_QUERYING):x4_tr(X4_TR_CAT_PLAY_BTN);
        txt(p,1306,911,32,1,play_label,468,can_start?bg:muted);
    }else txt(p,1284,540,32,0,x4_tr(X4_TR_CAT_CHOOSE_GAME),512,muted);
    if(acquired)x4_catalog_assets_release(a,&view);
    rect(p,84,983,1752,1,edge);
#if X4_PUBLIC
    txt(p,84,993,26,0,x4_tr(X4_TR_CAT_FOOTER_PUBLIC),1752,white);
#else
    txt(p,84,993,26,0,x4_tr(X4_TR_CAT_FOOTER_LAB),1752,white);
#endif
    char footer[256];
    if(s&&s->pending)snprintf(footer,sizeof(footer),"%s",x4_tr(X4_TR_CAT_FINISHING_COVERS));
    else if(s&&s->notice&&s->notice[0])snprintf(footer,sizeof(footer),"%s",s->notice);
    else snprintf(footer,sizeof(footer),x4_tr(X4_TR_CAT_RESULTS_FMT),count,
        count?start/6+1:0,(count+5)/6,X4_PRODUCT_VERSION);
    txt(p,350,145,26,0,footer,850,muted);
    if(b->editing)keyboard(p,b);
}
