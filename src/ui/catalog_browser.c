/* SPDX-License-Identifier: GPL-3.0-only */
#include "catalog_browser.h"
#include <stddef.h>
#include <string.h>

_Static_assert(X4_CATALOG_MAX <= UINT16_MAX + 1u, "catalog index must fit uint16_t");
_Static_assert(sizeof(X4CatalogBrowser)<=10u*1024,"browser RAM budget is 10 KiB");
static const char keyboard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

static unsigned folded(unsigned char c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

/* Validated bounded UTF-8 presentation search. The existing on-screen query
 * is ASCII; Latin accents/case are folded only for matching, never rewritten
 * in the displayed name, provider ID, launch ID or credential. */
static bool next_codepoint(const char *text, size_t n, size_t *offset, unsigned *value)
{
    if (*offset >= n) return false;
    unsigned c=(unsigned char)text[(*offset)++];
    if (c<0x80) { *value=c; return true; }
    unsigned extra,minimum;
    if (c>=0xc2&&c<=0xdf) { extra=1; *value=c&31; minimum=0x80; }
    else if (c>=0xe0&&c<=0xef) { extra=2; *value=c&15; minimum=0x800; }
    else if (c>=0xf0&&c<=0xf4) { extra=3; *value=c&7; minimum=0x10000; }
    else return false;
    if (extra>n-*offset) return false;
    while (extra--) { c=(unsigned char)text[(*offset)++]; if ((c&0xc0)!=0x80) return false; *value=(*value<<6)|(c&63); }
    return *value>=minimum && *value<=0x10ffff && !(*value>=0xd800&&*value<=0xdfff);
}

static unsigned latin_fold(unsigned c)
{
    if (c<0x80) return folded((unsigned char)c);
    if ((c>=0xc0&&c<=0xc5)||(c>=0xe0&&c<=0xe5)||c==0x100||c==0x101) return 'A';
    if (c==0xc7||c==0xe7||c==0x106||c==0x107) return 'C';
    if ((c>=0xc8&&c<=0xcb)||(c>=0xe8&&c<=0xeb)||c==0x112||c==0x113) return 'E';
    if ((c>=0xcc&&c<=0xcf)||(c>=0xec&&c<=0xef)||c==0x130||c==0x131) return 'I';
    if (c==0xd1||c==0xf1) return 'N';
    if ((c>=0xd2&&c<=0xd6)||(c>=0xf2&&c<=0xf6)||c==0xd8||c==0xf8) return 'O';
    if ((c>=0xd9&&c<=0xdc)||(c>=0xf9&&c<=0xfc)) return 'U';
    if (c==0xdd||c==0xfd||c==0xff) return 'Y';
    if (c==0x160||c==0x161) return 'S';
    if (c==0x17d||c==0x17e) return 'Z';
    return c;
}

static bool query_length(const X4CatalogBrowser *b, size_t *length)
{
    for (size_t i = 0; i <= X4_CATALOG_QUERY_MAX; ++i) {
        unsigned c = (unsigned char)b->query[i];
        if (!c) { *length = i; return true; }
        if (i == X4_CATALOG_QUERY_MAX ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ')) return false;
    }
    return false;
}

static bool contains(const char *text, size_t capacity, const char *query, size_t length)
{
    if (!length) return true;
    size_t n=strnlen(text,capacity);
    if (n==capacity) return false;
    unsigned normalized[128];
    size_t offset=0,count=0;
    while (offset<n) {
        unsigned code;
        if (!next_codepoint(text,n,&offset,&code)) return false;
        /* Combining accents do not change the ASCII keyboard search. */
        if (code>=0x300&&code<=0x36f) continue;
        if (code==0xc6||code==0xe6||code==0x152||code==0x153||code==0xdf||code==0x1e9e) {
            if (count+2>sizeof(normalized)/sizeof(normalized[0])) return false;
            normalized[count++]=(code==0xc6||code==0xe6)?'A':(code==0x152||code==0x153)?'O':'S';
            normalized[count++]=(code==0xdf||code==0x1e9e)?'S':'E';
            continue;
        }
        if (count==sizeof(normalized)/sizeof(normalized[0])) return false;
        normalized[count++]=latin_fold(code);
    }
    for (size_t i = 0; i < count; ++i) {
        size_t matched = 0;
        while (matched < length && matched < count - i &&
            normalized[i + matched] == folded((unsigned char)query[matched]))
            ++matched;
        if (matched == length) return true;
    }
    return false;
}

static bool matches(const X4CatalogBrowser *b, const X4CatalogTitle *title, size_t length)
{
    if (!x4_recent_title_id_valid(title->id,sizeof(title->id)) ||
        strnlen(title->name, sizeof(title->name)) == sizeof(title->name)) return false;
    if (b->filter == X4_CATALOG_FILTER_CONFIRMED_ACCESS && !title->entitled) return false;
    if (b->filter == X4_CATALOG_FILTER_UNCONFIRMED_ACCESS && title->entitled) return false;
    return contains(title->name, sizeof(title->name), b->query, length) ||
        contains(title->id, sizeof(title->id), b->query, length);
}

static void remember_selected(X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    b->selected_id[0] = 0;
    if (!c || c->state!=X4_CATALOG_READY || c->count>X4_CATALOG_MAX) return;
    if (b->selected >= b->filtered_count || b->filtered_count > X4_CATALOG_MAX) return;
    unsigned index = b->indices[b->selected];
    if (index >= c->count || index >= X4_CATALOG_MAX) return;
    size_t length = strnlen(c->titles[index].id, sizeof(b->selected_id));
    if (length && length < sizeof(b->selected_id))
        memcpy(b->selected_id, c->titles[index].id, length + 1);
}

static void criteria_changed(X4CatalogBrowser *b)
{
    b->section = X4_CATALOG_SECTION_CATALOG;
    b->selected = 0;
    b->selected_id[0] = 0;
    b->criteria_dirty = true;
}

void x4_catalog_browser_init(X4CatalogBrowser *b)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->criteria_dirty = true;
}

void x4_catalog_browser_rebuild(X4CatalogBrowser *b, const X4CatalogSnapshot *c)
{
    if (!b) return;
    bool preserve = !b->criteria_dirty && b->selected_id[0];
    size_t length = 0;
    b->filtered_count = b->selected = 0;
    memset(b->group_counts, 0, sizeof(b->group_counts));
    if (!c || c->count > X4_CATALOG_MAX || c->state != X4_CATALOG_READY ||
        (unsigned)b->filter >= X4_CATALOG_FILTER_COUNT || !query_length(b, &length)) {
        if (!c || c->state != X4_CATALOG_LOADING || !preserve || c->count > X4_CATALOG_MAX)
            b->selected_id[0] = 0;
        b->criteria_dirty = false;
        return;
    }
    b->group_counts[X4_CATALOG_FILTER_ALL] = c->count;
    bool found = false;
    for (unsigned index = 0; index < c->count; ++index) {
        const X4CatalogTitle *title = &c->titles[index];
        ++b->group_counts[title->entitled ? X4_CATALOG_FILTER_CONFIRMED_ACCESS :
            X4_CATALOG_FILTER_UNCONFIRMED_ACCESS];
        if (!matches(b, title, length)) continue;
        unsigned visible = b->filtered_count++;
        b->indices[visible] = (uint16_t)index;
        if (preserve && !strncmp(b->selected_id, title->id, sizeof(b->selected_id))) {
            b->selected = visible;
            found = true;
        }
    }
    if (!found) b->selected = 0;
    b->criteria_dirty = false;
    remember_selected(b, c);
}

bool x4_catalog_browser_selected_index(const X4CatalogBrowser *b,
    const X4CatalogSnapshot *c, unsigned *source_index)
{
    size_t length;
    if (!b || !c || !source_index || c->state != X4_CATALOG_READY || c->count > X4_CATALOG_MAX ||
        b->criteria_dirty || !b->filtered_count || b->filtered_count > X4_CATALOG_MAX ||
        b->selected >= b->filtered_count || (unsigned)b->filter >= X4_CATALOG_FILTER_COUNT ||
        !query_length(b, &length)) return false;
    unsigned index = b->indices[b->selected];
    if (index >= c->count || index >= X4_CATALOG_MAX || !matches(b, &c->titles[index], length) ||
        strncmp(b->selected_id, c->titles[index].id, sizeof(b->selected_id))) return false;
    *source_index = index;
    return true;
}

void x4_catalog_browser_move(X4CatalogBrowser *b, const X4CatalogSnapshot *c,
    int delta, bool page_clamp)
{
    unsigned original;
    if (!x4_catalog_browser_selected_index(b, c, &original)) return;
    int64_t next = (int64_t)b->selected + delta;
    if (page_clamp) {
        if (next < 0) next = 0;
        if (next >= b->filtered_count) next = b->filtered_count - 1;
    } else {
        next %= b->filtered_count;
        if (next < 0) next += b->filtered_count;
    }
    b->selected = (unsigned)next;
    remember_selected(b, c);
}

unsigned x4_catalog_browser_page_start(const X4CatalogBrowser *b)
{
    return b ? b->selected/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE : 0;
}

unsigned x4_catalog_browser_page_count(const X4CatalogBrowser *b)
{
    if (!b || b->filtered_count>X4_CATALOG_MAX) return 0;
    unsigned start=x4_catalog_browser_page_start(b);
    if (start>=b->filtered_count) return 0;
    unsigned count=b->filtered_count-start;
    return count<X4_CATALOG_PAGE_SIZE?count:X4_CATALOG_PAGE_SIZE;
}

bool x4_catalog_browser_grid_move(X4CatalogBrowser *b,const X4CatalogSnapshot *c,int dx,int dy)
{
    unsigned original;
    if (!x4_catalog_browser_selected_index(b,c,&original) || (dx && dy)) return false;
    unsigned old=b->selected,start=x4_catalog_browser_page_start(b),local=old-start;
    unsigned count=x4_catalog_browser_page_count(b),row=local/X4_CATALOG_COLUMNS,col=local%X4_CATALOG_COLUMNS;
    int next=(int)local;
    if (dx) {
        int next_col=(int)col+(dx<0?-1:1);
        if (next_col<0 || next_col>=X4_CATALOG_COLUMNS) return false;
        next=(int)(row*X4_CATALOG_COLUMNS)+next_col;
        if ((unsigned)next>=count) return false;
    } else if (dy) {
        int next_row=(int)row+(dy<0?-1:1);
        if (next_row<0 || next_row>=X4_CATALOG_ROWS || (unsigned)(next_row*X4_CATALOG_COLUMNS)>=count) return false;
        next=next_row*X4_CATALOG_COLUMNS+(int)col;
        if ((unsigned)next>=count) next=(int)count-1;
    }
    b->selected=start+(unsigned)next;
    remember_selected(b,c);
    return old!=b->selected;
}

static void remember_recent(X4CatalogBrowser *b,const X4RecentGames *h)
{
    const X4RecentGame *game=x4_recent_get(h,b->recent_selected);
    b->recent_selected_id[0]=0;
    if (game) memcpy(b->recent_selected_id,game->id,sizeof(b->recent_selected_id));
}

void x4_catalog_browser_sync_recents(X4CatalogBrowser *b,const X4RecentGames *h)
{
    if (!b) return;
    unsigned count=x4_recent_count(h);
    uint64_t account=h?h->account_generation:0;
    bool changed=b->recent_account_generation!=account;
    if (changed) {
        b->recent_account_generation=account;
        b->recent_initialized=false;
        b->recent_selected=0; b->recent_selected_id[0]=0;
        b->section=count?X4_CATALOG_SECTION_RECENT:X4_CATALOG_SECTION_CATALOG;
    } else if (h&&h->state==X4_RECENT_LOADING) return;
    else if (b->recent_selected_id[0]) {
        bool found=false;
        for (unsigned i=0;i<count;++i) if (!strcmp(b->recent_selected_id,h->entries[i].id)) {
            b->recent_selected=i; found=true; break;
        }
        if (!found) b->recent_selected=0;
    }
    if (h&&h->state==X4_RECENT_READY&&!b->recent_initialized) {
        b->section=count?X4_CATALOG_SECTION_RECENT:X4_CATALOG_SECTION_CATALOG;
        b->recent_initialized=true;
    }
    if (b->recent_selected>=count) b->recent_selected=0;
    remember_recent(b,h);
}

bool x4_catalog_browser_nav(X4CatalogBrowser *b,const X4CatalogSnapshot *c,const X4RecentGames *h,int dx,int dy)
{
    if (!b || (dx&&dy)) return false;
    x4_catalog_browser_sync_recents(b,h);
    if (b->section==X4_CATALOG_SECTION_RECENT) {
        if (dy>0) {
            unsigned col=(b->recent_selected%X4_CATALOG_PAGE_SIZE)/2;
            b->section=X4_CATALOG_SECTION_CATALOG;
            if (b->filtered_count) {
                unsigned start=x4_catalog_browser_page_start(b),count=x4_catalog_browser_page_count(b);
                if (count) b->selected=start+(col<count?col:count-1);
                remember_selected(b,c);
            }
            return true;
        }
        unsigned count=x4_recent_count(h);
        if (!dx || !count) return false;
        unsigned start=b->recent_selected/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE;
        int next=(int)b->recent_selected+(dx<0?-1:1);
        if (next<(int)start || next>=(int)(start+X4_CATALOG_PAGE_SIZE) || next>=(int)count) return false;
        b->recent_selected=(unsigned)next; remember_recent(b,h); return true;
    }
    if (dy<0 && (!b->filtered_count || b->selected%X4_CATALOG_PAGE_SIZE<X4_CATALOG_COLUMNS)) {
        b->section=X4_CATALOG_SECTION_RECENT;
        unsigned count=x4_recent_count(h),col=b->selected%X4_CATALOG_COLUMNS;
        unsigned start=b->recent_selected/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE;
        if (count) {
            unsigned end=start+X4_CATALOG_PAGE_SIZE;
            if (end>count) end=count;
            unsigned local=col*2;
            b->recent_selected=start+local<end?start+local:end-1;
        }
        remember_recent(b,h); return true;
    }
    return x4_catalog_browser_grid_move(b,c,dx,dy);
}

bool x4_catalog_browser_page_move(X4CatalogBrowser *b,const X4CatalogSnapshot *c,const X4RecentGames *h,int direction)
{
    if (!b || !direction) return false;
    if (b->section==X4_CATALOG_SECTION_RECENT) {
        unsigned count=x4_recent_count(h);
        if (!count) return false;
        unsigned old=b->recent_selected,start=old/X4_CATALOG_PAGE_SIZE*X4_CATALOG_PAGE_SIZE;
        int next=(int)start+(direction<0?-X4_CATALOG_PAGE_SIZE:X4_CATALOG_PAGE_SIZE);
        if (next<0 || next>=(int)count) return false;
        unsigned offset=old%X4_CATALOG_PAGE_SIZE;
        b->recent_selected=(unsigned)next+offset;
        if (b->recent_selected>=count) b->recent_selected=count-1;
        remember_recent(b,h); return old!=b->recent_selected;
    }
    unsigned source;
    if (!x4_catalog_browser_selected_index(b,c,&source)) return false;
    unsigned old=b->selected,start=x4_catalog_browser_page_start(b);
    int next=(int)start+(direction<0?-X4_CATALOG_PAGE_SIZE:X4_CATALOG_PAGE_SIZE);
    if (next<0 || next>=(int)b->filtered_count) return false;
    b->selected=(unsigned)next+old%X4_CATALOG_PAGE_SIZE;
    if (b->selected>=b->filtered_count) b->selected=b->filtered_count-1;
    remember_selected(b,c); return old!=b->selected;
}

bool x4_catalog_browser_resolved_index(const X4CatalogBrowser *b,const X4CatalogSnapshot *c,
    const X4RecentGames *h,unsigned *source_index)
{
    if (!b) return false;
    if (b->section==X4_CATALOG_SECTION_CATALOG) return x4_catalog_browser_selected_index(b,c,source_index);
    if (b->section!=X4_CATALOG_SECTION_RECENT || !h || !h->account_generation ||
        b->recent_account_generation!=h->account_generation) return false;
    const X4RecentGame *game=x4_recent_get(h,b->recent_selected);
    if (!game || strcmp(b->recent_selected_id,game->id)) return false;
    return x4_recent_catalog_index(game,c,source_index);
}

const X4CatalogTitle *x4_catalog_browser_selected_title(const X4CatalogBrowser *b,const X4CatalogSnapshot *c,
    const X4RecentGames *h)
{
    unsigned index;
    return x4_catalog_browser_resolved_index(b,c,h,&index)?&c->titles[index]:NULL;
}

bool x4_catalog_browser_can_launch_frame(uint32_t action_flags) { return action_flags==0; }

bool x4_catalog_browser_cycle_filter(X4CatalogBrowser *b, int delta)
{
    if (!b || (unsigned)b->filter >= X4_CATALOG_FILTER_COUNT) return false;
    int64_t next = ((int64_t)b->filter + delta) % X4_CATALOG_FILTER_COUNT;
    if (next < 0) next += X4_CATALOG_FILTER_COUNT;
    if (next == b->filter) return false;
    b->filter = (enum X4CatalogFilter)next;
    criteria_changed(b);
    return true;
}

bool x4_catalog_browser_query_append(X4CatalogBrowser *b, char character)
{
    size_t length;
    unsigned c = folded((unsigned char)character);
    if (!b || !query_length(b, &length) || length >= X4_CATALOG_QUERY_MAX ||
        !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ')) return false;
    b->query[length] = (char)c;
    b->query[length + 1] = 0;
    criteria_changed(b);
    return true;
}

bool x4_catalog_browser_query_delete(X4CatalogBrowser *b)
{
    size_t length;
    if (!b || !query_length(b, &length) || !length) return false;
    b->query[length - 1] = 0;
    criteria_changed(b);
    return true;
}

bool x4_catalog_browser_query_clear(X4CatalogBrowser *b)
{
    size_t length;
    if (!b || !query_length(b, &length) || !length) return false;
    b->query[0] = 0;
    criteria_changed(b);
    return true;
}

void x4_catalog_browser_keyboard_move(X4CatalogBrowser *b, int dx, int dy)
{
    if (!b) return;
    unsigned cursor = b->keyboard_cursor % X4_CATALOG_KEYBOARD_KEYS;
    int64_t x = ((int64_t)(cursor % 6) + dx) % 6;
    int64_t y = ((int64_t)(cursor / 6) + dy) % 6;
    if (x < 0) x += 6;
    if (y < 0) y += 6;
    b->keyboard_cursor = (unsigned)(y * 6 + x);
}

char x4_catalog_browser_keyboard_char(const X4CatalogBrowser *b)
{
    return b && b->keyboard_cursor < X4_CATALOG_KEYBOARD_KEYS ? keyboard[b->keyboard_cursor] : 0;
}
