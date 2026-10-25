/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/ui/catalog_browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks,cases;
#define CHECK(condition) do { ++checks; if (!(condition)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#condition); exit(1); } } while (0)
#define RUN(name) do { ++cases; name(); } while (0)
static X4CatalogSnapshot catalog;
static X4CatalogBrowser browser;
static X4RecentGames history;
static void fill(unsigned count)
{
    memset(&catalog,0,sizeof(catalog)); catalog.state=X4_CATALOG_READY; catalog.count=count;
    for (unsigned i=0;i<count&&i<X4_CATALOG_MAX;++i) {
        snprintf(catalog.titles[i].id,sizeof(catalog.titles[i].id),"TITLE_%u",i);
        snprintf(catalog.titles[i].product_id,sizeof(catalog.titles[i].product_id),"PRODUCT%u",i);
        snprintf(catalog.titles[i].name,sizeof(catalog.titles[i].name),"Game %u",i);
        catalog.titles[i].entitled=i%2==0;
    }
    x4_catalog_browser_init(&browser); x4_catalog_browser_rebuild(&browser,&catalog);
}
static unsigned selected(void)
{
    unsigned index=99999; CHECK(x4_catalog_browser_selected_index(&browser,&catalog,&index)); return index;
}
static void query(const char *text)
{
    x4_catalog_browser_query_clear(&browser);
    for (const char *p=text;*p;++p) CHECK(x4_catalog_browser_query_append(&browser,*p));
    x4_catalog_browser_rebuild(&browser,&catalog);
}
static void grid_navigation(void)
{
    fill(13); CHECK(selected()==0); CHECK(x4_catalog_browser_page_count(&browser)==6);
    CHECK(x4_catalog_browser_grid_move(&browser,&catalog,1,0)); CHECK(selected()==1);
    CHECK(x4_catalog_browser_grid_move(&browser,&catalog,1,0)); CHECK(selected()==2);
    CHECK(!x4_catalog_browser_grid_move(&browser,&catalog,1,0)); CHECK(selected()==2);
    CHECK(x4_catalog_browser_grid_move(&browser,&catalog,0,1)); CHECK(selected()==5);
    CHECK(!x4_catalog_browser_grid_move(&browser,&catalog,0,1));
    CHECK(x4_catalog_browser_grid_move(&browser,&catalog,-1,0)); CHECK(selected()==4);
    CHECK(x4_catalog_browser_grid_move(&browser,&catalog,0,-1)); CHECK(selected()==1);
    CHECK(!x4_catalog_browser_grid_move(&browser,&catalog,1,1)); CHECK(selected()==1);
    CHECK(x4_catalog_browser_page_move(&browser,&catalog,NULL,1)); CHECK(selected()==7);
    CHECK(x4_catalog_browser_page_start(&browser)==6);
    CHECK(x4_catalog_browser_page_move(&browser,&catalog,NULL,1)); CHECK(selected()==12);
    CHECK(x4_catalog_browser_page_count(&browser)==1); CHECK(!x4_catalog_browser_page_move(&browser,&catalog,NULL,1));
    CHECK(x4_catalog_browser_page_move(&browser,&catalog,NULL,-1)); CHECK(selected()==6);
    CHECK(x4_catalog_browser_page_move(&browser,&catalog,NULL,-1)); CHECK(selected()==0);
    CHECK(!x4_catalog_browser_page_move(&browser,&catalog,NULL,-1));
}

static void incomplete_pages(void)
{
    for (unsigned count=1;count<=18;++count) {
        fill(count);
        while (x4_catalog_browser_page_move(&browser,&catalog,NULL,1)) CHECK(selected()<count);
        unsigned start=x4_catalog_browser_page_start(&browser),last_count=x4_catalog_browser_page_count(&browser);
        CHECK(last_count>=1&&last_count<=6); CHECK(start+last_count==count);
        for (unsigned i=0;i<3;++i) x4_catalog_browser_grid_move(&browser,&catalog,1,0);
        CHECK(selected()<count);
        for (unsigned i=0;i<3;++i) x4_catalog_browser_grid_move(&browser,&catalog,0,1);
        CHECK(selected()<count);
        CHECK(x4_catalog_browser_page_start(&browser)==start);
    }
    fill(4); x4_catalog_browser_grid_move(&browser,&catalog,1,0); x4_catalog_browser_grid_move(&browser,&catalog,1,0);
    CHECK(selected()==2); CHECK(x4_catalog_browser_grid_move(&browser,&catalog,0,1)); CHECK(selected()==3);
}

static void filters_and_search(void)
{
    fill(X4_CATALOG_MAX); strcpy(catalog.titles[4095].name,"Forza Horizon Final");
    query("FORZA"); CHECK(browser.filtered_count==1); CHECK(selected()==4095);
    CHECK(browser.group_counts[X4_CATALOG_FILTER_ALL]==4096);
    CHECK(browser.group_counts[X4_CATALOG_FILTER_CONFIRMED_ACCESS]==2048);
    CHECK(browser.group_counts[X4_CATALOG_FILTER_UNCONFIRMED_ACCESS]==2048);
    CHECK(x4_catalog_browser_cycle_filter(&browser,1)); x4_catalog_browser_rebuild(&browser,&catalog);
    CHECK(browser.filtered_count==0);
    CHECK(x4_catalog_browser_cycle_filter(&browser,1)); x4_catalog_browser_rebuild(&browser,&catalog);
    CHECK(browser.filtered_count==1); CHECK(selected()==4095);
    query("4095"); CHECK(browser.filtered_count==1); CHECK(selected()==4095);
    CHECK(x4_catalog_browser_query_clear(&browser)); x4_catalog_browser_rebuild(&browser,&catalog);
    CHECK(browser.filtered_count==2048); CHECK(!catalog.titles[selected()].entitled);
    CHECK(!x4_catalog_browser_query_append(&browser,'_'));
}

static void utf8_names(void)
{
    fill(8);
    strcpy(catalog.titles[0].name,"\xc3\x81rbol y ni\xc3\xb1os");
    strcpy(catalog.titles[1].name,"Pok\xc3\xa9mon");
    strcpy(catalog.titles[2].name,"Ping\xc3\xbcino: \xc2\xbfQu\xc3\xa9? \xc2\xa1S\xc3\xad!");
    strcpy(catalog.titles[3].name,"Cafe\xcc\x81 con leche");
    query("ARBOL"); CHECK(browser.filtered_count==1); CHECK(selected()==0);
    query("NINOS"); CHECK(browser.filtered_count==1); CHECK(selected()==0);
    query("POKEMON"); CHECK(browser.filtered_count==1); CHECK(selected()==1);
    query("PINGUINO"); CHECK(browser.filtered_count==1); CHECK(selected()==2);
    query("CAFE"); CHECK(browser.filtered_count==1); CHECK(selected()==3);
    strcpy(catalog.titles[6].name,"Stra\xc3\x9f" "e");
    strcpy(catalog.titles[7].name,"\xc5\x92uvre");
    query("STRASSE"); CHECK(browser.filtered_count==1); CHECK(selected()==6);
    query("OEUVRE"); CHECK(browser.filtered_count==1); CHECK(selected()==7);
    memset(catalog.titles[4].name,'A',127); catalog.titles[4].name[127]=0;
    query("AAAA"); CHECK(browser.filtered_count==1); CHECK(selected()==4);
    strcpy(catalog.titles[5].name,"Bad\xc0\xaf" "FORZA");
    query("FORZA"); CHECK(browser.filtered_count==0);
}

static void preserve_id_and_refuse_stale_map(void)
{
    fill(12); x4_catalog_browser_page_move(&browser,&catalog,NULL,1); CHECK(selected()==6);
    char id[96]; strcpy(id,browser.selected_id);
    X4CatalogTitle swap=catalog.titles[6]; catalog.titles[6]=catalog.titles[1]; catalog.titles[1]=swap;
    unsigned index; CHECK(!x4_catalog_browser_selected_index(&browser,&catalog,&index));
    x4_catalog_browser_rebuild(&browser,&catalog); CHECK(selected()==1); CHECK(!strcmp(browser.selected_id,id));
    catalog.state=X4_CATALOG_LOADING; x4_catalog_browser_rebuild(&browser,&catalog);
    CHECK(!x4_catalog_browser_selected_index(&browser,&catalog,&index)); CHECK(!strcmp(browser.selected_id,id));
    catalog.state=X4_CATALOG_READY; x4_catalog_browser_rebuild(&browser,&catalog); CHECK(selected()==1);
    catalog.count=0; x4_catalog_browser_rebuild(&browser,&catalog); CHECK(!x4_catalog_browser_selected_index(&browser,&catalog,&index));
    catalog.count=X4_CATALOG_MAX+1; x4_catalog_browser_rebuild(&browser,&catalog); CHECK(browser.filtered_count==0);
    fill(1); strcpy(catalog.titles[0].id,"../not-a-title"); x4_catalog_browser_rebuild(&browser,&catalog); CHECK(browser.filtered_count==0);
}

static void same_frame_guard(void)
{
    CHECK(x4_catalog_browser_can_launch_frame(0));
    for (unsigned bit=0;bit<11;++bit) CHECK(!x4_catalog_browser_can_launch_frame(1u<<bit));
    CHECK(!x4_catalog_browser_can_launch_frame(X4_CATALOG_FRAME_LEFT|X4_CATALOG_FRAME_RIGHT));
    CHECK(!x4_catalog_browser_can_launch_frame(X4_CATALOG_FRAME_REFRESH|X4_CATALOG_FRAME_METADATA));
}
static void metadata_frame_launch_deferral(void)
{
    fill(6);x4_recent_init(&history);
    CHECK(x4_catalog_browser_grid_move(&browser,&catalog,1,0));
    char pending_id[96];strcpy(pending_id,browser.selected_id);
    /* Simulate the coordinator's completed public metadata update. The
     * actual main loop owns pending intent; this tests model/guard contracts,
     * not an integrated main loop or auth/cache scheduling. */
    strcpy(catalog.titles[1].name,"Título actualizado");
    x4_catalog_browser_rebuild(&browser,&catalog);
    const X4CatalogTitle*chosen=x4_catalog_browser_selected_title(&browser,&catalog,&history);
    CHECK(chosen&&!strcmp(chosen->id,pending_id));
    CHECK(!x4_catalog_browser_can_launch_frame(X4_CATALOG_FRAME_METADATA));
    CHECK(x4_catalog_browser_can_launch_frame(0));
    unsigned index;CHECK(x4_catalog_browser_resolved_index(&browser,&catalog,&history,&index));
    CHECK(index==1);CHECK(!strcmp(catalog.titles[index].id,pending_id));
    CHECK(!strcmp(catalog.titles[index].name,"Título actualizado"));
}


static void publish_ids(unsigned count,uint64_t account,uint64_t generation)
{
    char ids[X4_RECENT_MAX][96]; memset(ids,0,sizeof(ids));
    for(unsigned i=0;i<count;++i) strcpy(ids[i],catalog.titles[count-1-i].id);
    x4_recent_begin(&history,account,generation);
    CHECK(x4_recent_publish(&history,account,generation,ids,count,0,200,&catalog));
}
static void server_mru_order_and_stale_results(void)
{
    fill(25); x4_recent_init(&history); publish_ids(25,1,1);
    CHECK(x4_recent_count(&history)==25);
    CHECK(!strcmp(x4_recent_get(&history,0)->id,"TITLE_24"));
    CHECK(!strcmp(x4_recent_get(&history,24)->id,"TITLE_0"));
    char ids[3][96]={{0}};strcpy(ids[0],"TITLE_2");strcpy(ids[1],"title_2");strcpy(ids[2],"TITLE_1");
    x4_recent_begin(&history,1,2);CHECK(x4_recent_count(&history)==0);
    CHECK(!x4_recent_publish(&history,1,1,ids,3,0,200,&catalog)); CHECK(history.state==X4_RECENT_LOADING);
    CHECK(x4_recent_publish(&history,1,2,ids,3,0,200,&catalog)); CHECK(x4_recent_count(&history)==2);
    CHECK(!strcmp(history.entries[0].id,"TITLE_2"));CHECK(!strcmp(history.entries[1].id,"TITLE_1"));
    CHECK(!x4_recent_publish(&history,1,2,ids,3,0,200,&catalog)); /* Duplicate completion. */
    x4_recent_begin(&history,2,3);CHECK(history.count==0);
    CHECK(!x4_recent_publish(&history,1,2,ids,3,0,200,&catalog));CHECK(history.state==X4_RECENT_LOADING);
    CHECK(x4_recent_publish(&history,2,3,NULL,0,0,200,&catalog));CHECK(history.state==X4_RECENT_READY);
    CHECK(x4_recent_count(&history)==0);CHECK(history.stale_results==1);
    x4_recent_begin(&history,0,0);CHECK(history.state==X4_RECENT_IDLE);CHECK(!history.account_generation);
}
static void mru_ids_not_products_and_errors(void)
{
    fill(3); x4_recent_init(&history);
    char ids[2][96]={{0}};strcpy(ids[0],"PRODUCT0");strcpy(ids[1],"TITLE_1");
    x4_recent_begin(&history,1,1); CHECK(x4_recent_publish(&history,1,1,ids,2,0,200,&catalog));
    unsigned index; CHECK(!x4_recent_catalog_index(x4_recent_get(&history,0),&catalog,&index));
    CHECK(history.entries[0].product_id[0]==0);CHECK(history.entries[0].name[0]==0);
    CHECK(x4_recent_catalog_index(x4_recent_get(&history,1),&catalog,&index));CHECK(index==1);
    CHECK(!catalog.titles[index].entitled); /* MRU cannot establish access. */
    x4_recent_begin(&history,1,2);strcpy(ids[0],"../bad");
    CHECK(!x4_recent_publish(&history,1,2,ids,1,0,200,&catalog));CHECK(history.state==X4_RECENT_ERROR);
    CHECK(history.count==0);CHECK(x4_recent_count(&history)==0);
    x4_recent_begin(&history,1,3);CHECK(!x4_recent_publish(&history,1,3,NULL,0,-23,500,&catalog));
    CHECK(history.error==-23);CHECK(history.http_status==500);
    x4_recent_begin(&history,1,4);CHECK(!x4_recent_publish(&history,1,4,ids,26,0,200,&catalog));
    CHECK(history.state==X4_RECENT_ERROR);
}
static void recent_navigation_and_current_access(void)
{
    fill(25); x4_recent_init(&history);publish_ids(25,1,1);
    x4_catalog_browser_sync_recents(&browser,&history);CHECK(browser.section==X4_CATALOG_SECTION_RECENT);
    const X4CatalogTitle *title=x4_catalog_browser_selected_title(&browser,&catalog,&history);
    CHECK(title&&!strcmp(title->id,"TITLE_24"));CHECK(title->entitled);
    CHECK(x4_catalog_browser_page_move(&browser,&catalog,&history,1));CHECK(browser.recent_selected==6);
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,1,0));CHECK(browser.recent_selected==7);
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,1));CHECK(browser.section==X4_CATALOG_SECTION_CATALOG);CHECK(selected()==0);
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,-1));CHECK(browser.section==X4_CATALOG_SECTION_RECENT);CHECK(browser.recent_selected==6);
    unsigned source;CHECK(x4_catalog_browser_resolved_index(&browser,&catalog,&history,&source));CHECK(source==18);
    CHECK(x4_catalog_browser_page_move(&browser,&catalog,&history,1));CHECK(browser.recent_selected==12);
    CHECK(x4_catalog_browser_page_move(&browser,&catalog,&history,1));CHECK(browser.recent_selected==18);
    CHECK(x4_catalog_browser_page_move(&browser,&catalog,&history,1));CHECK(browser.recent_selected==24);
    CHECK(!x4_catalog_browser_nav(&browser,&catalog,&history,1,0));CHECK(!x4_catalog_browser_page_move(&browser,&catalog,&history,1));
    catalog.titles[0].entitled=0;title=x4_catalog_browser_selected_title(&browser,&catalog,&history);CHECK(title&&!title->entitled);
    catalog.titles[0].id[0]=0;CHECK(!x4_catalog_browser_resolved_index(&browser,&catalog,&history,&source));
    x4_recent_begin(&history,2,2);CHECK(!x4_catalog_browser_resolved_index(&browser,&catalog,&history,&source));
    x4_catalog_browser_sync_recents(&browser,&history);CHECK(browser.recent_selected==0);CHECK(browser.recent_account_generation==2);
}
static void recent_grid_visual_columns(void)
{
    fill(25);x4_recent_init(&history);publish_ids(25,1,1);x4_catalog_browser_sync_recents(&browser,&history);
    /* Preserve a nonzero catalog page while mapping all six compact cards. */
    x4_catalog_browser_move(&browser,&catalog,12,true);CHECK(selected()==12);
    for (unsigned local=0;local<6;++local) {
        browser.section=X4_CATALOG_SECTION_RECENT;browser.recent_selected=6+local;
        strcpy(browser.recent_selected_id,history.entries[browser.recent_selected].id);
        CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,1));
        CHECK(browser.section==X4_CATALOG_SECTION_CATALOG);CHECK(selected()==12+local/2);
        CHECK(x4_catalog_browser_page_start(&browser)==12);
        CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,-1));
        CHECK(browser.section==X4_CATALOG_SECTION_RECENT);CHECK(browser.recent_selected==6+2*(local/2));
    }
    browser.recent_selected=24;strcpy(browser.recent_selected_id,history.entries[24].id);
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,1));CHECK(selected()==12);
    x4_catalog_browser_grid_move(&browser,&catalog,1,0);x4_catalog_browser_grid_move(&browser,&catalog,1,0);
    CHECK(selected()==14);CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,-1));CHECK(browser.recent_selected==24);
    /* A two-card final recent page clamps catalog column two to local one. */
    publish_ids(8,1,2);x4_catalog_browser_sync_recents(&browser,&history);
    browser.recent_selected=6;strcpy(browser.recent_selected_id,history.entries[6].id);
    browser.section=X4_CATALOG_SECTION_CATALOG;
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,-1));CHECK(browser.recent_selected==7);
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,1));CHECK(selected()==12);
    /* Incomplete catalog pages also keep the selection in the current page. */
    x4_catalog_browser_move(&browser,&catalog,12,true);CHECK(selected()==24);
    browser.section=X4_CATALOG_SECTION_RECENT;browser.recent_selected=5;
    strcpy(browser.recent_selected_id,history.entries[5].id);
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,1));CHECK(selected()==24);
    CHECK(x4_catalog_browser_page_start(&browser)==24);
}
static void recent_selection_across_refresh_and_empty_catalog(void)
{
    fill(5);x4_recent_init(&history);publish_ids(5,1,1);x4_catalog_browser_sync_recents(&browser,&history);
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,1,0));CHECK(browser.recent_selected==1);
    CHECK(!strcmp(browser.recent_selected_id,"TITLE_3"));
    x4_recent_begin(&history,1,2);x4_catalog_browser_sync_recents(&browser,&history);
    CHECK(!strcmp(browser.recent_selected_id,"TITLE_3"));
    char ids[3][96]={{0}};strcpy(ids[0],"TITLE_0");strcpy(ids[1],"TITLE_4");strcpy(ids[2],"TITLE_3");
    CHECK(x4_recent_publish(&history,1,2,ids,3,0,200,&catalog));x4_catalog_browser_sync_recents(&browser,&history);
    CHECK(browser.recent_selected==2);CHECK(!strcmp(browser.recent_selected_id,"TITLE_3"));
    catalog.count=0;x4_catalog_browser_rebuild(&browser,&catalog);x4_recent_refresh_catalog(&history,&catalog);
    unsigned source;CHECK(!x4_catalog_browser_resolved_index(&browser,&catalog,&history,&source));
    CHECK(history.entries[2].product_id[0]==0);
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,1));CHECK(browser.section==X4_CATALOG_SECTION_CATALOG);
    CHECK(x4_catalog_browser_nav(&browser,&catalog,&history,0,-1));CHECK(browser.section==X4_CATALOG_SECTION_RECENT);
    CHECK(x4_catalog_browser_query_append(&browser,'A'));CHECK(browser.section==X4_CATALOG_SECTION_CATALOG);
}
int main(void)
{
    RUN(grid_navigation);RUN(incomplete_pages);RUN(filters_and_search);RUN(utf8_names);
    RUN(preserve_id_and_refuse_stale_map);RUN(same_frame_guard);RUN(metadata_frame_launch_deferral);
    RUN(server_mru_order_and_stale_results);
    RUN(mru_ids_not_products_and_errors);RUN(recent_navigation_and_current_access);
    RUN(recent_grid_visual_columns);
    RUN(recent_selection_across_refresh_and_empty_catalog);
    printf("PASS catalog browser/server MRU: %u cases, %u checks\n",cases,checks);return 0;
}
