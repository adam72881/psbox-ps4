/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "catalog_disk_cache.h"
#include "../auth/http_client.h"
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* File names are derived from complete, validated public keys. Each file
 * stores its complete key as well: a hash collision is never a cache hit.
 * Root-relative operations retain the opened directory identity throughout
 * the lifetime, even if a parent directory is renamed concurrently. */
#define RECORDS_MAX (2u * X4_CATALOG_DISK_PRODUCTS)
#define KEY_CAP 640u
#define FILE_CAP 41u
#define HEADER_SIZE 48u
#define IO_CHUNK 65536u
#define SCAN_BOUND 1024u
#if defined(X4_OPENORBIS) || defined(X4_DISK_CACHE_NATIVE_FS_TEST)
#define X4_DISK_PATH_MUTATIONS 1
#endif
enum { KIND_METADATA = 1, KIND_IMAGE = 2 };
typedef struct {
    char name[FILE_CAP], key[KEY_CAP], id[X4_PRODUCT_ID_CAP];
    size_t size;
    uint64_t created, used, order;
    unsigned kind;
    int occupied;
} Record;
struct X4CatalogDiskCache {
    int directory;
    char root[1024];
    size_t budget;
    unsigned max_products, capacity;
    X4CatalogDiskClock clock;
    void *clock_context;
    uint64_t order;
    uint32_t crc_table[256];
    Record record[RECORDS_MAX];
    X4CatalogDiskStats stats;
};
static uint64_t utc_clock(void *unused)
{
    (void)unused; time_t n = time(NULL); return n > 0 ? (uint64_t)n : 0;
}
static int cancelled(X4CatalogDiskCache *c, const _Atomic int *flag)
{
    if (flag && atomic_load(flag)) { if (c) ++c->stats.cancelled; return 1; }
    return 0;
}
static void put32(unsigned char *p, uint32_t x)
{ for (unsigned i=0;i<4;++i) p[i]=(unsigned char)(x>>(8*i)); }
static uint32_t get32(const unsigned char *p)
{ uint32_t n=0; for(unsigned i=0;i<4;++i)n|=(uint32_t)p[i]<<(8*i); return n; }
static void put64(unsigned char *p, uint64_t x)
{ for (unsigned i=0;i<8;++i) p[i]=(unsigned char)(x>>(8*i)); }
static uint64_t get64(const unsigned char *p)
{ uint64_t n=0;for(unsigned i=0;i<8;++i)n|=(uint64_t)p[i]<<(8*i);return n; }
static uint32_t crc(X4CatalogDiskCache *c,uint32_t v,const void *p,size_t n)
{
    const unsigned char *s=p;
    while(n--)v=c->crc_table[(v^*s++)&255]^(v>>8);
    return v;
}
static int exact_io(int fd,void *bytes,size_t n,int writing,
    X4CatalogDiskCache *c,const _Atomic int *cancel)
{
    unsigned char *p=bytes;
    while(n){
        if(cancelled(c,cancel))return 0;
        size_t chunk=n<IO_CHUNK?n:IO_CHUNK;
        ssize_t r=writing?write(fd,p,chunk):read(fd,p,chunk);
        if(r<0&&errno==EINTR)continue;
        if(r<=0)return -1;
        p+=(size_t)r;n-=(size_t)r;
    }
    return 1;
}
static int locale(const char *s,size_t max,int language)
{
    if(!s)return 0;
    size_t n=0;
    while(n<max&&s[n]){
        unsigned char v=(unsigned char)s[n];
        if(!((v>='A'&&v<='Z')||(v>='a'&&v<='z')||
            (language&&((v>='0'&&v<='9')||v=='-'))))return 0;
        ++n;
    }
    return language?(n>=2&&n<max):(n==2);
}
static void canonical(char *dst,const char *src,int upper)
{
    while(*src){unsigned char c=(unsigned char)*src++;
        if(upper&&c>='a'&&c<='z')c-=32;
        if(!upper&&c>='A'&&c<='Z')c+=32;
        *dst++=(char)c;
    }
    *dst=0;
}
static int key_make(unsigned kind,const char *id,const char *market,const char *language,
    const char *url,char key[KEY_CAP],char normalized[X4_PRODUCT_ID_CAP])
{
    char m[3],l[24];
    if(!x4_product_id_valid(id)||!locale(market,3,0)||!locale(language,sizeof(l),1))return 0;
    if(kind==KIND_IMAGE&&(!url||!x4_http_public_image_url(url)))return 0;
    canonical(normalized,id,1);canonical(m,market,1);canonical(l,language,0);
    int n=snprintf(key,KEY_CAP,"%u|%s|%s|%s|%s",kind,normalized,m,l,kind==KIND_IMAGE?url:"");
    return n>0&&(unsigned)n<KEY_CAP;
}
static void file_name(const char *key,char name[FILE_CAP])
{
    uint64_t a=UINT64_C(14695981039346656037),b=UINT64_C(7809847782465536322);
    for(const unsigned char *s=(const unsigned char *)key;*s;++s){
        a=(a^*s)*UINT64_C(1099511628211);
        b=(b^*s)*UINT64_C(14029467366897019727);
    }
    snprintf(name,FILE_CAP,"x4c-%016llx%016llx.bin",(unsigned long long)a,(unsigned long long)b);
}
static int generated_name(const char *name,int *temporary)
{
    if(strlen(name)!=40||strncmp(name,"x4c-",4))return 0;
    for(unsigned i=4;i<36;++i)if(!((name[i]>='0'&&name[i]<='9')||(name[i]>='a'&&name[i]<='f')))return 0;
    if(!strcmp(name+36,".bin")){*temporary=0;return 1;}
    if(!strcmp(name+36,".tmp")){*temporary=1;return 1;}
    return 0;
}
#ifdef X4_DISK_PATH_MUTATIONS
/* The pinned SDK's mkdirat/fstatat/renameat/unlinkat/utimensat objects are
 * explicit ENOSYS stubs. openat delegates to the real libkernel _openat.
 * Keep descriptor-relative reads/writes. Only directory creation and final
 * unlink/rename need guarded, fixed-root paths; never fall back to arbitrary
 * caller roots on PS4 or alter process-global cwd. */
static int root_guard(X4CatalogDiskCache *c)
{
    char prefix[1024];size_t length=strlen(c->root);struct stat path,held;
    if(fstat(c->directory,&held)<0||!S_ISDIR(held.st_mode))return 0;
    memcpy(prefix,c->root,length+1);
    for(size_t i=1;i<=length;++i)if(prefix[i]=='/'||prefix[i]==0){
        char saved=prefix[i];prefix[i]=0;
        int rc=lstat(prefix,&path);prefix[i]=saved;
        if(rc<0||!S_ISDIR(path.st_mode))return 0;
    }
    return path.st_dev==held.st_dev&&path.st_ino==held.st_ino;
}
static int owned_path(X4CatalogDiskCache *c,const char *name,char out[1080])
{
    int temporary;
    if(!generated_name(name,&temporary)||!root_guard(c)){errno=EPERM;return 0;}
    size_t n=strlen(c->root);memcpy(out,c->root,n);out[n]='/';memcpy(out+n+1,name,FILE_CAP);return 1;
}
#endif
static int record_stat(X4CatalogDiskCache *c,const char *name,struct stat *out)
{
#ifdef X4_DISK_PATH_MUTATIONS
    int fd=openat(c->directory,name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    int rc=fstat(fd,out);int saved=errno;close(fd);errno=saved;return rc;
#else
    return fstatat(c->directory,name,out,AT_SYMLINK_NOFOLLOW);
#endif
}
static int record_unlink(X4CatalogDiskCache *c,const char *name)
{
#ifdef X4_DISK_PATH_MUTATIONS
    char path[1080];if(!owned_path(c,name,path))return -1;
    int rc=unlink(path);if(!root_guard(c)){errno=EPERM;return -1;}return rc;
#else
    return unlinkat(c->directory,name,0);
#endif
}
static int record_rename(X4CatalogDiskCache *c,const char *from,const char *to)
{
#ifdef X4_DISK_PATH_MUTATIONS
    char source[1080],dest[1080];
    if(!owned_path(c,from,source)||!owned_path(c,to,dest))return -1;
    int rc=rename(source,dest);if(!root_guard(c)){errno=EPERM;return -1;}return rc;
#else
    return renameat(c->directory,from,c->directory,to);
#endif
}
static int record_touch(int fd,uint64_t now)
{
#ifdef X4_DISK_PATH_MUTATIONS
    struct timeval times[2]={{(time_t)now,0},{(time_t)now,0}};return futimes(fd,times);
#else
    struct timespec times[2]={{(time_t)now,0},{(time_t)now,0}};return futimens(fd,times);
#endif
}
static int utf8_display(const char *s,size_t cap)
{
    size_t end=0;while(end<cap&&s[end])++end;if(end==cap)return 0;
    for(size_t i=0;i<end;){
        unsigned char a=(unsigned char)s[i++];
        if(a<128){if(a<32||a==127)return 0;continue;}
        unsigned count;uint32_t value;
        if(a>=0xc2&&a<=0xdf){count=1;value=a&31;}
        else if(a>=0xe0&&a<=0xef){count=2;value=a&15;}
        else if(a>=0xf0&&a<=0xf4){count=3;value=a&7;}else return 0;
        if(end-i<count)return 0;
        for(unsigned k=0;k<count;++k){unsigned char b=(unsigned char)s[i++];if((b&0xc0)!=0x80)return 0;value=(value<<6)|(b&63);}
        if((count==1&&value<128)||(count==2&&value<2048)||(count==3&&value<65536)||
            value>0x10ffff||(value>=0xd800&&value<=0xdfff))return 0;
    }
    return 1;
}
static int metadata_normalize(const X4ProductMetadata *in,const char *id,X4ProductMetadata *out)
{
    char normalized[X4_PRODUCT_ID_CAP];
    if(!in||!x4_product_id_valid(in->product_id))return 0;
    canonical(normalized,in->product_id,1);if(strcmp(normalized,id))return 0;
    if(!utf8_display(in->name,sizeof(in->name))||!utf8_display(in->publisher,sizeof(in->publisher))||
        !utf8_display(in->developer,sizeof(in->developer))||!utf8_display(in->category,sizeof(in->category))||
        !utf8_display(in->description,sizeof(in->description)))return 0;
    size_t n=0;while(n<sizeof(in->cover_url)&&in->cover_url[n])++n;
    if(n==sizeof(in->cover_url)||(n&&!x4_http_public_image_url(in->cover_url)))return 0;
    memset(out,0,sizeof(*out));
    memcpy(out->product_id,id,strlen(id)+1);
#define COPY_FIELD(f) memcpy(out->f,in->f,strlen(in->f)+1)
    COPY_FIELD(name);COPY_FIELD(publisher);COPY_FIELD(developer);COPY_FIELD(category);
    COPY_FIELD(description);COPY_FIELD(cover_url);
#undef COPY_FIELD
    return 1;
}
static unsigned be16(const unsigned char *p){return ((unsigned)p[0]<<8)|p[1];}
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int image_valid(const unsigned char *p,size_t n)
{
    if(!p||n<10||n>X4_HTTP_IMAGE_MAX)return 0;
    static const unsigned char png[]={137,80,78,71,13,10,26,10};
    if(n>=33&&!memcmp(p,png,8)){
        if(be32(p+8)!=13||memcmp(p+12,"IHDR",4))return 0;
        uint32_t w=be32(p+16),h=be32(p+20);
        return w>0&&h>0&&w<=2048&&h<=2048;
    }
    if(p[0]!=255||p[1]!=216)return 0;
    size_t offset=2;
    while(offset+4<=n){
        if(p[offset++]!=255)return 0;
        while(offset<n&&p[offset]==255)++offset;
        if(offset>=n)return 0;
        unsigned marker=p[offset++];
        if(marker==0xd9||marker==0xda)return 0;
        if(marker==0x01||(marker>=0xd0&&marker<=0xd8))continue;
        if(offset+2>n)return 0;
        unsigned len=be16(p+offset);if(len<2||len>n-offset)return 0;
        if((marker>=0xc0&&marker<=0xc3)||(marker>=0xc5&&marker<=0xc7)||
            (marker>=0xc9&&marker<=0xcb)||(marker>=0xcd&&marker<=0xcf)){
            if(len<8)return 0;
            unsigned h=be16(p+offset+3),w=be16(p+offset+5);
            return w>0&&h>0&&w<=2048&&h<=2048;
        }
        offset+=len;
    }
    return 0;
}
static unsigned product_count(const X4CatalogDiskCache *c)
{
    unsigned count=0;
    for(unsigned i=0;i<c->capacity;++i)if(c->record[i].occupied){
        unsigned j=0;for(;j<i;++j)if(c->record[j].occupied&&!strcmp(c->record[i].id,c->record[j].id))break;
        if(j==i)++count;
    }
    return count;
}
static void recount(X4CatalogDiskCache *c)
{
    c->stats.bytes=0;c->stats.records=0;
    for(unsigned i=0;i<c->capacity;++i)if(c->record[i].occupied){c->stats.bytes+=c->record[i].size;++c->stats.records;}
    c->stats.products=product_count(c);
}
static int erase_file(X4CatalogDiskCache *c,const char *name)
{
    struct stat s;
    if(record_stat(c,name,&s)<0)return errno==ENOENT?1:0;
    if(!S_ISREG(s.st_mode))return 0;
    return record_unlink(c,name)==0||errno==ENOENT;
}
static void remove_record(X4CatalogDiskCache *c,unsigned index,unsigned reason)
{
    Record *r=&c->record[index];if(!r->occupied)return;
    if(!erase_file(c,r->name)){++c->stats.io_errors;return;}
    memset(r,0,sizeof(*r));recount(c);
    if(reason==1)++c->stats.evictions;
    if(reason==2)++c->stats.expired;
    if(reason==3)++c->stats.corrupt;
}
static int oldest(const X4CatalogDiskCache *c,int except)
{
    int best=-1;
    for(unsigned i=0;i<c->capacity;++i)if(c->record[i].occupied&&(int)i!=except){
        if(best<0||c->record[i].used<c->record[best].used||
            (c->record[i].used==c->record[best].used&&c->record[i].order<c->record[best].order))best=(int)i;
    }
    return best;
}
static int find(const X4CatalogDiskCache *c,const char *key)
{
    for(unsigned i=0;i<c->capacity;++i)if(c->record[i].occupied&&!strcmp(c->record[i].key,key))return (int)i;
    return -1;
}
static int header_read(X4CatalogDiskCache *c,int fd,Record *r,uint32_t *payload_size,uint32_t *checksum)
{
    unsigned char h[HEADER_SIZE];struct stat st;
    if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<HEADER_SIZE||
        exact_io(fd,h,sizeof(h),0,c,NULL)!=1||memcmp(h,"X4PUBLIC",8)||get32(h+8)!=1||
        (get32(h+12)!=KIND_METADATA&&get32(h+12)!=KIND_IMAGE)||
        get32(h+40)!=(crc(c,UINT32_MAX,h,40)^UINT32_MAX)||get32(h+44)!=0)return 0;
    uint32_t kn=get32(h+24),pn=get32(h+28);
    if(!kn||kn>=KEY_CAP||!pn||pn>X4_HTTP_IMAGE_MAX||
        (uint64_t)st.st_size!=HEADER_SIZE+(uint64_t)kn+pn||
        (get32(h+12)==KIND_METADATA&&pn!=sizeof(X4ProductMetadata)))return 0;
    memset(r,0,sizeof(*r));
    if(exact_io(fd,r->key,kn,0,c,NULL)!=1||memchr(r->key,0,kn))return 0;
    r->key[kn]=0;r->kind=get32(h+12);r->created=get64(h+16);
    if(get32(h+36)!=(crc(c,UINT32_MAX,r->key,kn)^UINT32_MAX))return 0;
    char key[KEY_CAP],id[X4_PRODUCT_ID_CAP],market[3],lang[24];
    /* Parse the stored key through the same public input policy. */
    const char *a=strchr(r->key,'|'),*b=a?strchr(a+1,'|'):NULL,
        *d=b?strchr(b+1,'|'):NULL,*e=d?strchr(d+1,'|'):NULL;
    if(!a||!b||!d||!e||a-r->key!=1||r->key[0]!=(char)('0'+r->kind)||
        b-a-1<=0||b-a-1>=X4_PRODUCT_ID_CAP||d-b-1!=2||e-d-1<=0||e-d-1>=(ptrdiff_t)sizeof(lang))return 0;
    memcpy(id,a+1,(size_t)(b-a-1));id[b-a-1]=0;
    memcpy(market,b+1,2);market[2]=0;memcpy(lang,d+1,(size_t)(e-d-1));lang[e-d-1]=0;
    char normalized[X4_PRODUCT_ID_CAP];
    if((r->kind==KIND_METADATA&&e[1])||!key_make(r->kind,id,market,lang,e+1,key,normalized)||strcmp(key,r->key))return 0;
    memcpy(r->id,normalized,strlen(normalized)+1);r->size=(size_t)st.st_size;
    r->used=st.st_mtime>0?(uint64_t)st.st_mtime:r->created;r->occupied=1;
    *payload_size=pn;*checksum=get32(h+32);return 1;
}
static int expired(const Record *r,uint64_t now)
{
    uint64_t ttl=r->kind==KIND_METADATA?X4_CATALOG_DISK_METADATA_TTL:X4_CATALOG_DISK_IMAGE_TTL;
    return !now||!r->created||r->created>now||now-r->created>=ttl;
}
static int prune(X4CatalogDiskCache *c,int except)
{
    recount(c);
    while(c->stats.bytes>c->budget||c->stats.products>c->max_products){
        int v=oldest(c,except);if(v<0)return 0;
        unsigned before=c->stats.records;remove_record(c,(unsigned)v,1);
        if(c->stats.records==before)return 0;
    }
    return 1;
}
static int open_directory_tree(const char *root)
{
    if(!root||root[0]!='/'||strlen(root)>1023||root[1]==0)return -1;
#ifdef X4_DISK_PATH_MUTATIONS
    char path[1024];size_t length=strlen(root);memcpy(path,root,length+1);
    struct stat st;
    for(size_t i=1;i<=length;++i)if(path[i]=='/'||path[i]==0){
        size_t start=i;while(start>0&&path[start-1]!='/')--start;size_t n=i-start;
        if(!n||n>255||(n==1&&path[start]=='.')||(n==2&&path[start]=='.'&&path[start+1]=='.'))return -1;
        for(size_t p=start;p<i;++p)if((unsigned char)path[p]<32||(unsigned char)path[p]==127)return -1;
        char saved=path[i];path[i]=0;
        int rc=lstat(path,&st);
        if(rc<0&&errno==ENOENT){rc=mkdir(path,0700);if(!rc||errno==EEXIST)rc=lstat(path,&st);}
        path[i]=saved;if(rc<0||!S_ISDIR(st.st_mode))return -1;
    }
    int fd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    struct stat held,path_stat;
    if(fstat(fd,&held)<0||lstat(root,&path_stat)<0||!S_ISDIR(path_stat.st_mode)||
        held.st_dev!=path_stat.st_dev||held.st_ino!=path_stat.st_ino){close(fd);return -1;}
    return fd;
#else
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    const char *at=root+1;
    while(*at){
        size_t n=strcspn(at,"/");char name[256];
        if(!n||n>=sizeof(name)||(n==1&&at[0]=='.')||(n==2&&at[0]=='.'&&at[1]=='.')){close(fd);return -1;}
        memcpy(name,at,n);name[n]=0;
        for(size_t i=0;i<n;++i)if((unsigned char)name[i]<32||(unsigned char)name[i]==127){close(fd);return -1;}
        if(mkdirat(fd,name,0700)<0&&errno!=EEXIST){close(fd);return -1;}
        int next=openat(fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(fd);if(next<0)return -1;fd=next;
        at+=n;if(*at=='/'){++at;if(!*at){close(fd);return -1;}}
    }
    return fd;
#endif
}
X4CatalogDiskCache *x4_catalog_disk_cache_open(const char *root,size_t budget,unsigned products,
    X4CatalogDiskClock clock,void *context)
{
    if(!root)return NULL;
#ifdef X4_OPENORBIS
    if(strcmp(root,X4_CATALOG_DISK_ROOT))return NULL;
#endif
    if(!budget)budget=X4_CATALOG_DISK_BUDGET;
    if(!products)products=X4_CATALOG_DISK_PRODUCTS;
    if(budget>X4_CATALOG_DISK_BUDGET||products>X4_CATALOG_DISK_PRODUCTS)return NULL;
    int fd=open_directory_tree(root);if(fd<0)return NULL;
    X4CatalogDiskCache *c=calloc(1,sizeof(*c));if(!c){close(fd);return NULL;}
    c->directory=fd;memcpy(c->root,root,strlen(root)+1);
    c->budget=budget;c->max_products=products;c->capacity=products*2;
    c->clock=clock?clock:utc_clock;c->clock_context=context;
    for(unsigned i=0;i<256;++i){uint32_t v=i;for(unsigned k=0;k<8;++k)v=(v>>1)^((v&1)?UINT32_C(0xedb88320):0);c->crc_table[i]=v;}
    int scan_fd=dup(fd);DIR *dir=scan_fd<0?NULL:fdopendir(scan_fd);
    if(!dir){if(scan_fd>=0)close(scan_fd);x4_catalog_disk_cache_close(c);return NULL;}
    uint64_t now=c->clock(context);struct dirent *entry;unsigned scanned=0;int safe=1;
    if(!now){closedir(dir);x4_catalog_disk_cache_close(c);return NULL;}
    while((entry=readdir(dir))!=NULL){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        if(++scanned>SCAN_BOUND){++c->stats.scan_omitted;safe=0;break;}
        int temporary;if(!generated_name(entry->d_name,&temporary))continue;
        if(temporary){if(!erase_file(c,entry->d_name))++c->stats.io_errors;continue;}
        int file=openat(fd,entry->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        Record r;uint32_t size,sum;
        int valid=file>=0&&header_read(c,file,&r,&size,&sum);
        if(file>=0)close(file);
        char name[FILE_CAP];if(valid){file_name(r.key,name);valid=!strcmp(name,entry->d_name);}
        if(!valid||expired(&r,now)){
            if(erase_file(c,entry->d_name)){if(!valid)++c->stats.corrupt;else ++c->stats.expired;}
            else ++c->stats.io_errors;
            continue;
        }
        memcpy(r.name,entry->d_name,FILE_CAP);r.order=++c->order;
        int slot=-1;for(unsigned i=0;i<c->capacity;++i)if(!c->record[i].occupied){slot=(int)i;break;}
        if(slot<0){slot=oldest(c,-1);
            if(slot<0||r.used<c->record[slot].used){if(!erase_file(c,r.name))safe=0;else ++c->stats.evictions;continue;}
            remove_record(c,(unsigned)slot,1);if(c->record[slot].occupied){safe=0;break;}
        }
        c->record[slot]=r;if(!prune(c,-1)){safe=0;break;}
    }
    closedir(dir);recount(c);
    if(!safe){x4_catalog_disk_cache_close(c);return NULL;}
    return c;
}
void x4_catalog_disk_cache_close(X4CatalogDiskCache *c)
{ if(c){close(c->directory);free(c);} }
static int cache_get(X4CatalogDiskCache *c,const char *key,unsigned kind,void *out,size_t capacity,
    size_t *length,const _Atomic int *cancel)
{
    *length=0;
    if(!c)return 0;
    if(cancelled(c,cancel))return 0;
#ifdef X4_DISK_PATH_MUTATIONS
    if(!root_guard(c)){++c->stats.io_errors;return -1;}
#endif
    int i=find(c,key);if(i<0){++c->stats.misses;return 0;}
    Record *known=&c->record[i];uint64_t now=c->clock(c->clock_context);
    if(!now){++c->stats.misses;return 0;}
    if(expired(known,now)){remove_record(c,(unsigned)i,2);++c->stats.misses;return 0;}
    int fd=openat(c->directory,known->name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    Record r;uint32_t n,sum;
    int valid=fd>=0&&header_read(c,fd,&r,&n,&sum)&&r.kind==kind&&!strcmp(r.key,key);
    if(valid&&n>capacity){close(fd);return -1;}
    int result=valid?exact_io(fd,out,n,0,c,cancel):-1;
    if(result==0){if(fd>=0)close(fd);memset(out,0,capacity);return 0;}
    if(result==1){uint32_t actual=crc(c,UINT32_MAX,key,strlen(key));actual=crc(c,actual,out,n)^UINT32_MAX;
        valid=actual==sum;
        if(valid&&kind==KIND_METADATA){X4ProductMetadata normalized;valid=metadata_normalize(out,r.id,&normalized)&&!memcmp(&normalized,out,sizeof(normalized));}
        if(valid&&kind==KIND_IMAGE)valid=image_valid(out,n);
    }else valid=0;
    if(!valid){if(fd>=0)close(fd);memset(out,0,capacity);remove_record(c,(unsigned)i,3);++c->stats.misses;return 0;}
    /* LRU persistence alters only filesystem timestamps, never payload/header.
     * One-second disk granularity; in-process order disambiguates ties. */
    if(record_touch(fd,now)<0)++c->stats.io_errors;
    close(fd);known->used=now;known->order=++c->order;*length=n;
    if(kind==KIND_METADATA)++c->stats.metadata_hits;else ++c->stats.image_hits;
    return 1;
}
static int cache_put(X4CatalogDiskCache *c,const char *key,const char *id,unsigned kind,
    const void *payload,size_t length,const _Atomic int *cancel)
{
    if(!c)return 0;
    if(cancelled(c,cancel))return 0;
#ifdef X4_DISK_PATH_MUTATIONS
    if(!root_guard(c)){++c->stats.io_errors;return -1;}
#endif
    uint64_t now=c->clock(c->clock_context);if(!now)return 0;
    size_t total=HEADER_SIZE+strlen(key)+length;if(total>c->budget)return 0;
    char name[FILE_CAP],temporary[FILE_CAP];file_name(key,name);memcpy(temporary,name,FILE_CAP);memcpy(temporary+36,".tmp",5);
    /* A filename collision with another complete key must remain a miss. */
    for(unsigned i=0;i<c->capacity;++i)if(c->record[i].occupied&&!strcmp(c->record[i].name,name)&&strcmp(c->record[i].key,key))return -1;
    if(!erase_file(c,temporary)){++c->stats.io_errors;return -1;}
    int fd=openat(c->directory,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){++c->stats.io_errors;return -1;}
    unsigned char h[HEADER_SIZE]={0};memcpy(h,"X4PUBLIC",8);put32(h+8,1);put32(h+12,kind);put64(h+16,now);
    put32(h+24,(uint32_t)strlen(key));put32(h+28,(uint32_t)length);
    uint32_t key_crc=crc(c,UINT32_MAX,key,strlen(key));put32(h+32,crc(c,key_crc,payload,length)^UINT32_MAX);
    put32(h+36,key_crc^UINT32_MAX);put32(h+40,crc(c,UINT32_MAX,h,40)^UINT32_MAX);
    int rc=exact_io(fd,h,sizeof(h),1,c,cancel);
    if(rc==1)rc=exact_io(fd,(void *)key,strlen(key),1,c,cancel);
    if(rc==1)rc=exact_io(fd,(void *)payload,length,1,c,cancel);
    if(rc==1&&!cancelled(c,cancel)){if(fsync(fd)<0)rc=-1;}else if(rc==1)rc=0;
    if(close(fd)<0&&rc==1)rc=-1;
    if(rc!=1){erase_file(c,temporary);if(rc<0)++c->stats.io_errors;return rc;}
    /* Make room before the final rename. This permits at most one bounded
     * temporary <=2MiB above the configured committed-file budget. */
    int slot=find(c,key);
    if(slot<0){for(unsigned i=0;i<c->capacity;++i)if(!c->record[i].occupied){slot=(int)i;break;}}
    if(slot<0){slot=oldest(c,-1);if(slot>=0)remove_record(c,(unsigned)slot,1);}
    if(slot<0||(c->record[slot].occupied&&strcmp(c->record[slot].key,key))){erase_file(c,temporary);return -1;}
    size_t previous=c->record[slot].occupied?c->record[slot].size:0;
    int present=0;for(unsigned i=0;i<c->capacity;++i)if(c->record[i].occupied&&!strcmp(c->record[i].id,id)){present=1;break;}
    while(c->stats.bytes-previous+total>c->budget||(!present&&c->stats.products>=c->max_products)){
        int victim=oldest(c,slot);if(victim<0){erase_file(c,temporary);return 0;}
        unsigned before=c->stats.records;remove_record(c,(unsigned)victim,1);
        if(before==c->stats.records){erase_file(c,temporary);return -1;}
        present=0;for(unsigned i=0;i<c->capacity;++i)if(c->record[i].occupied&&!strcmp(c->record[i].id,id)){present=1;break;}
    }
    if(cancelled(c,cancel)){erase_file(c,temporary);return 0;}
    struct stat st;int existing=record_stat(c,name,&st);
    if((existing<0&&errno!=ENOENT)||(existing==0&&!S_ISREG(st.st_mode))){erase_file(c,temporary);++c->stats.io_errors;return -1;}
    if(record_rename(c,temporary,name)<0){erase_file(c,temporary);++c->stats.io_errors;return -1;}
    int saved=openat(c->directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(saved>=0){if(record_touch(saved,now)<0)++c->stats.io_errors;close(saved);}
    /* Directory sync is best effort (some consoles/filesystems reject it).
     * Atomic visibility is guaranteed by rename; power-loss durability is
     * not claimed. Startup discards incomplete/corrupt files regardless. */
    if(fsync(c->directory)<0)++c->stats.io_errors;
    Record *r=&c->record[slot];memset(r,0,sizeof(*r));
    memcpy(r->name,name,FILE_CAP);memcpy(r->key,key,strlen(key)+1);memcpy(r->id,id,strlen(id)+1);
    r->size=total;r->kind=kind;r->created=r->used=now;r->order=++c->order;r->occupied=1;
    recount(c);++c->stats.stores;return 1;
}
int x4_catalog_disk_cache_get_metadata(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,
    X4ProductMetadata *out,const _Atomic int *cancel)
{
    if(!out)return -1;
    memset(out,0,sizeof(*out));char key[KEY_CAP],normalized[X4_PRODUCT_ID_CAP];
    if(!key_make(KIND_METADATA,id,market,language,NULL,key,normalized))return -1;
    size_t length;return cache_get(c,key,KIND_METADATA,out,sizeof(*out),&length,cancel);
}
int x4_catalog_disk_cache_put_metadata(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,
    const X4ProductMetadata *in,const _Atomic int *cancel)
{
    char key[KEY_CAP],id_normal[X4_PRODUCT_ID_CAP];X4ProductMetadata normalized;
    if(!key_make(KIND_METADATA,id,market,language,NULL,key,id_normal)||!metadata_normalize(in,id_normal,&normalized))return -1;
    return cache_put(c,key,id_normal,KIND_METADATA,&normalized,sizeof(normalized),cancel);
}
int x4_catalog_disk_cache_get_image(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,
    const char *url,unsigned char *bytes,size_t capacity,size_t *length,const _Atomic int *cancel)
{
    if(!length)return -1;
    *length=0;if(!bytes||!capacity||capacity>X4_HTTP_IMAGE_MAX+1u)return -1;
    char key[KEY_CAP],normalized[X4_PRODUCT_ID_CAP];
    if(!key_make(KIND_IMAGE,id,market,language,url,key,normalized))return -1;
    return cache_get(c,key,KIND_IMAGE,bytes,capacity,length,cancel);
}
int x4_catalog_disk_cache_put_image(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,
    const char *url,const unsigned char *bytes,size_t length,const _Atomic int *cancel)
{
    char key[KEY_CAP],normalized[X4_PRODUCT_ID_CAP];
    if(!key_make(KIND_IMAGE,id,market,language,url,key,normalized)||!image_valid(bytes,length))return -1;
    return cache_put(c,key,normalized,KIND_IMAGE,bytes,length,cancel);
}
int x4_catalog_disk_cache_remove_image(X4CatalogDiskCache *c,const char *id,const char *market,const char *language,const char *url)
{
    char key[KEY_CAP],normalized[X4_PRODUCT_ID_CAP];
    if(!key_make(KIND_IMAGE,id,market,language,url,key,normalized))return -1;
    if(!c)return 0;
    int i=find(c,key);if(i<0)return 0;
    remove_record(c,(unsigned)i,3);return c->record[i].occupied?-1:1;
}
int x4_catalog_disk_cache_clear(X4CatalogDiskCache *c,const _Atomic int *cancel)
{
    if(!c)return 0;
    for(unsigned i=0;i<c->capacity;++i){if(cancelled(c,cancel))return 0;remove_record(c,i,0);}
    recount(c);return c->stats.records? -1:1;
}
void x4_catalog_disk_cache_stats(const X4CatalogDiskCache *c,X4CatalogDiskStats *out)
{ if(out){memset(out,0,sizeof(*out));if(c)*out=c->stats;} }
