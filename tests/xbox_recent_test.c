/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/auth/xbox_recent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"MRU check failed line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void invalid(const char *s) { X4RecentSnapshot r; CHECK(x4_xbox_recent_parse(s,strlen(s),&r)<0);CHECK(r.state==X4_CATALOG_ERROR&&!r.count&&!r.title_ids[0][0]); }
int main(void)
{
    X4RecentSnapshot r;
    const char *s="{\"results\":[{\"titleId\":\"FIRST\",\"details\":{\"hasEntitlement\":false}},{\"titleId\":\"SECOND\"},{\"titleId\":\"FIRST\"}]}";
    CHECK(!x4_xbox_recent_parse(s,strlen(s),&r));CHECK(r.state==X4_CATALOG_READY&&r.count==2);CHECK(!strcmp(r.title_ids[0],"FIRST")&&!strcmp(r.title_ids[1],"SECOND"));
    CHECK(!x4_xbox_recent_parse("{\"results\":[]}",14,&r));CHECK(!r.count&&r.state==X4_CATALOG_READY);
    invalid("{}");invalid("{\"results\":null}");invalid("{\"results\":{}}");invalid("{\"results\":[{}]}");invalid("{\"results\":[null]}");
    invalid("{\"results\":[{\"titleId\":\"\"}]}");invalid("{\"results\":[{\"titleId\":\"ID\\r\\n\"}]}");invalid("{\"results\":[{\"titleId\":\"ID/escape\"}]}");
    invalid("{\"results\":[{\"titleId\":\"ñ\"}]}");invalid("{\"results\":[{\"titleId\":3}]}");invalid("{\"results\":[{\"titleId\":\"A\",\"titleId\":\"B\"}]}");
    invalid("{\"results\":[],\"results\":[]}");invalid("{\"results\":[]}trailing");
    char response[4096], *p=response;size_t left=sizeof(response);int n=snprintf(p,left,"{\"results\":[");p+=n;left-=(size_t)n;
    for(unsigned i=0;i<25;++i){n=snprintf(p,left,"%s{\"titleId\":\"TITLE_%u\"}",i?",":"",i);p+=n;left-=(size_t)n;}
    snprintf(p,left,"]}");CHECK(!x4_xbox_recent_parse(response,strlen(response),&r));CHECK(r.count==25);CHECK(!strcmp(r.title_ids[24],"TITLE_24"));
    snprintf(p,left,",{\"titleId\":\"EXTRA\"}]}");invalid(response);
    CHECK(x4_xbox_recent_parse(response,X4_RECENT_RESPONSE_MAX+1,&r)<0);CHECK(x4_xbox_recent_parse(NULL,0,&r)<0);CHECK(x4_xbox_recent_parse("{}",2,NULL)<0);
    char longid[160];memset(longid,'A',sizeof(longid)-1);longid[sizeof(longid)-1]=0;snprintf(response,sizeof(response),"{\"results\":[{\"titleId\":\"%s\"}]}",longid);invalid(response);
    puts("server MRU parser: source-derived synthetic fixtures, no live account response measured");printf("server MRU PASS: %u checks\n",checks);return 0;
}
