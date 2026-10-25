/* SPDX-License-Identifier: GPL-3.0-only */
#include "xbox_recent.h"
#include "json.h"
#include <string.h>
static int valid_id(const char *id)
{
    if (!id[0]) return 0;
    for (const unsigned char *p=(const unsigned char *)id; *p; ++p)
        if (!((*p>='a'&&*p<='z') || (*p>='A'&&*p<='Z') || (*p>='0'&&*p<='9') ||
            *p=='-' || *p=='_' || *p=='.')) return 0;
    return 1;
}
int x4_xbox_recent_parse(const char *json, size_t length, X4RecentSnapshot *out)
{
    if (!out) return -1;
    memset(out,0,sizeof(*out));
    out->state=X4_CATALOG_ERROR;
    if (!json || length>X4_RECENT_RESPONSE_MAX) return -1;
    X4JsonSpan root,results,item,v;
    if (x4_json_parse(json,length,&root) || x4_json_member(root,"results",&results)!=1 ||
        x4_json_type(results)!=X4_JSON_T_ARRAY) return -1;
    size_t cursor=0;
    unsigned examined=0;
    int rc;
    while ((rc=x4_json_item(results,&cursor,&item))==1) {
        char id[96];
        if (++examined>X4_RECENT_MAX || x4_json_type(item)!=X4_JSON_T_OBJECT ||
            x4_json_member(item,"titleId",&v)!=1 || x4_json_token(v,id,sizeof(id)) || !valid_id(id)) goto invalid;
        int duplicate=0;
        for (unsigned i=0;i<out->count;++i) if (!strcmp(out->title_ids[i],id)) duplicate=1;
        if (!duplicate) memcpy(out->title_ids[out->count++],id,strlen(id)+1);
    }
    if (rc<0) goto invalid;
    out->state=X4_CATALOG_READY;
    return 0;
invalid:
    memset(out,0,sizeof(*out));out->state=X4_CATALOG_ERROR;return -1;
}
