/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_presentation.h"
#include <string.h>

_Static_assert(sizeof(X4CatalogPresentation)<=704u*1024,"public name overlay budget is 704 KiB");
enum { TOMBSTONE=UINT16_MAX };
static unsigned fold(unsigned c) { return c>='a'&&c<='z'?c-32:c; }
static bool product_valid(const char *id)
{
    if(!id||!id[0])return false;
    for(unsigned i=0;i<X4_PRODUCT_ID_CAP;++i){unsigned c=(unsigned char)id[i];if(!c)return true;
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')))return false;}
    return false;
}
static bool same(const char *a,const char *b)
{
    for(;*a&&*b;++a,++b)if(fold((unsigned char)*a)!=fold((unsigned char)*b))return false;
    return *a==*b;
}
static unsigned hash(const char *id)
{
    uint32_t h=2166136261u;
    while(*id){h^=fold((unsigned char)*id++);h*=16777619u;}
    return h&(X4_PRESENTATION_HASH_SLOTS-1);
}
static unsigned find(const X4CatalogPresentation *p,const char *id,unsigned *empty)
{
    unsigned at=hash(id),first=TOMBSTONE;
    for(unsigned step=0;step<X4_PRESENTATION_HASH_SLOTS;++step,at=(at+1)&(X4_PRESENTATION_HASH_SLOTS-1)){
        unsigned slot=p->lookup[at];
        if(!slot){if(empty)*empty=first==TOMBSTONE?at:first;return X4_CATALOG_MAX;}
        if(slot==TOMBSTONE){if(first==TOMBSTONE)first=at;continue;}
        if(slot<=p->count&&same(p->names[slot-1].product_id,id)){if(empty)*empty=at;return slot-1;}
    }
    if(empty)*empty=first;
    return X4_CATALOG_MAX;
}
void x4_catalog_presentation_init(X4CatalogPresentation *p){if(p)memset(p,0,sizeof(*p));}
void x4_catalog_presentation_set_generation(X4CatalogPresentation *p,uint64_t generation)
{
    if(!p||p->generation==generation)return;
    p->generation=generation;p->last_revision=0;
}
size_t x4_catalog_presentation_ingest(X4CatalogPresentation *p,uint64_t generation,uint64_t revision,
    const X4ProductMetadata *metadata,size_t count)
{
    if(!p||(!metadata&&count)||count>X4_PRESENTATION_DEMAND_MAX)return 0;
    if(!generation||generation!=p->generation){++p->stale_results;return 0;}
    if(!revision||revision<=p->last_revision)return 0;
    p->last_revision=revision;size_t changed=0;
    for(size_t i=0;i<count;++i){
        const X4ProductMetadata *m=&metadata[i];
        if(!product_valid(m->product_id)||!m->name[0]||!x4_recent_display_name_valid(m->name)){++p->rejected_metadata;continue;}
        unsigned empty,index=find(p,m->product_id,&empty);
        if(index==X4_CATALOG_MAX){
            if(p->count<X4_CATALOG_MAX)index=p->count++;
            else {
                for(index=0;index<p->count;++index)if(p->names[index].seen_generation!=generation)break;
                if(index==p->count){++p->capacity_rejected;continue;}
                unsigned old_slot;find(p,p->names[index].product_id,&old_slot);p->lookup[old_slot]=TOMBSTONE;
                find(p,m->product_id,&empty);
            }
            if(empty==TOMBSTONE){++p->capacity_rejected;continue;}
            memset(&p->names[index],0,sizeof(p->names[index]));
            memcpy(p->names[index].product_id,m->product_id,strlen(m->product_id)+1);
            p->lookup[empty]=(uint16_t)(index+1);
        }
        p->names[index].seen_generation=generation;
        if(strcmp(p->names[index].name,m->name)){
            memset(p->names[index].name,0,sizeof(p->names[index].name));
            memcpy(p->names[index].name,m->name,strlen(m->name)+1);++changed;
        }
    }
    return changed;
}
bool x4_catalog_presentation_apply_names(X4CatalogPresentation *p,X4CatalogSnapshot *c)
{
    if(!p||!c||c->state!=X4_CATALOG_READY||c->count>X4_CATALOG_MAX)return false;
    bool changed=false;
    for(unsigned i=0;i<c->count;++i){
        X4CatalogTitle *t=&c->titles[i];
        if(!x4_recent_title_id_valid(t->id,sizeof(t->id))||!product_valid(t->product_id))continue;
        unsigned index=find(p,t->product_id,NULL);if(index==X4_CATALOG_MAX)continue;
        p->names[index].seen_generation=p->generation;
        if(strncmp(t->name,p->names[index].name,sizeof(t->name))){memcpy(t->name,p->names[index].name,sizeof(t->name));changed=true;}
    }
    return changed;
}
static void append(char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],size_t *count,const char *id)
{
    if(*count>=X4_PRESENTATION_DEMAND_MAX||!product_valid(id))return;
    for(size_t i=0;i<*count;++i)if(same(ids[i],id))return;
    memcpy(ids[(*count)++],id,strlen(id)+1);
}
static void catalog_page(char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],size_t *count,
    const X4CatalogBrowser *b,const X4CatalogSnapshot *c,unsigned start)
{
    for(unsigned i=start;i<b->filtered_count&&i<start+X4_CATALOG_PAGE_SIZE;++i){
        unsigned source=b->indices[i];
        if(source<c->count&&source<X4_CATALOG_MAX&&x4_recent_title_id_valid(c->titles[source].id,sizeof(c->titles[source].id)))
            append(ids,count,c->titles[source].product_id);
    }
}
static void recent_page(char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP],size_t *count,
    const X4RecentGames *h,const X4CatalogSnapshot *c,unsigned start)
{
    unsigned recent_count=x4_recent_count(h);
    for(unsigned i=start;i<recent_count&&i<start+X4_CATALOG_PAGE_SIZE;++i){
        unsigned source;
        if(x4_recent_catalog_index(x4_recent_get(h,i),c,&source))append(ids,count,c->titles[source].product_id);
    }
}
static unsigned adjacent_page(unsigned start,unsigned count)
{
    if(start>=count)return count;
    return start+X4_CATALOG_PAGE_SIZE<count?start+X4_CATALOG_PAGE_SIZE:
        start>=X4_CATALOG_PAGE_SIZE?start-X4_CATALOG_PAGE_SIZE:start;
}
size_t x4_catalog_presentation_demand(const X4CatalogPresentation *p,const X4CatalogBrowser *b,
    const X4CatalogSnapshot *c,const X4RecentGames *h,char ids[X4_PRESENTATION_DEMAND_MAX][X4_PRODUCT_ID_CAP])
{
    if(!ids)return 0;
    memset(ids,0,X4_PRESENTATION_DEMAND_MAX*X4_PRODUCT_ID_CAP);
    if(!p||!p->generation||!b||!c||c->state!=X4_CATALOG_READY||c->count>X4_CATALOG_MAX||
        b->filtered_count>X4_CATALOG_MAX||b->criteria_dirty)return 0;
    size_t count=0;
    const X4CatalogTitle *selected=x4_catalog_browser_selected_title(b,c,h);
    if(selected)append(ids,&count,selected->product_id);
    unsigned catalog_start=x4_catalog_browser_page_start(b);
    bool current_recent=h&&h->account_generation&&b->recent_account_generation==h->account_generation;
    unsigned recent_count=current_recent?x4_recent_count(h):0;
    unsigned recent_start=b->recent_selected/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE;
    /* Match the renderer's visible page, not MRU index zero. A changed
     * generation is never used to populate this account's demand. */
    if(b->section==X4_CATALOG_SECTION_RECENT&&current_recent)
        recent_page(ids,&count,h,c,recent_start);
    catalog_page(ids,&count,b,c,catalog_start);
    if(b->section!=X4_CATALOG_SECTION_RECENT&&current_recent)
        recent_page(ids,&count,h,c,recent_start);
    /* No speculative item precedes an actually visible item. */
    if(b->section==X4_CATALOG_SECTION_RECENT&&recent_count>X4_CATALOG_PAGE_SIZE)
        recent_page(ids,&count,h,c,adjacent_page(recent_start,recent_count));
    if(b->filtered_count>X4_CATALOG_PAGE_SIZE)
        catalog_page(ids,&count,b,c,adjacent_page(catalog_start,b->filtered_count));
    if(b->section!=X4_CATALOG_SECTION_RECENT&&recent_count>X4_CATALOG_PAGE_SIZE)
        recent_page(ids,&count,h,c,adjacent_page(recent_start,recent_count));
    return count;
}
