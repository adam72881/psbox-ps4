/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/ui/catalog_disk_cache.h"
#include "../src/auth/http_client.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static unsigned checks;
#define CHECK(v) do {++checks;if(!(v)){fprintf(stderr,"disk cache check %u failed %s:%d: %s\n",checks,__FILE__,__LINE__,#v);exit(1);}}while(0)
static uint64_t now=1000000;
static _Atomic int cancel;
static int clock_cancel;
static uint64_t clock_seconds(void *unused)
{ (void)unused;if(clock_cancel)atomic_store(&cancel,1);return now; }
static void path_join(char out[1024],const char *root,const char *suffix)
{
    size_t a=strlen(root),b=strlen(suffix);CHECK(a+b+2<=1024);
    memcpy(out,root,a);out[a]='/';memcpy(out+a+1,suffix,b+1);
}
static X4ProductMetadata metadata(const char *id)
{
    X4ProductMetadata m={0};snprintf(m.product_id,sizeof(m.product_id),"%s",id);
    snprintf(m.name,sizeof(m.name),"Edición pingüino ¿sí? ¡Sí! %s",id);
    strcpy(m.description,"Descripción pública, sin credenciales ni permisos.");
    strcpy(m.cover_url,"https://store-images.s-microsoft.com/image/apps.cache-test?w=512&q=80&format=jpg");return m;
}
static X4CatalogDiskCache *open_cache(const char *root,size_t budget,unsigned products)
{
    X4CatalogDiskCache *c=x4_catalog_disk_cache_open(root,budget,products,clock_seconds,NULL);CHECK(c);return c;
}
static unsigned char *read_image(const char *path,size_t *length)
{
    FILE *f=fopen(path,"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long n=ftell(f);CHECK(n>0);rewind(f);
    unsigned char *p=malloc((size_t)n);CHECK(p);CHECK(fread(p,1,(size_t)n,f)==(size_t)n);fclose(f);*length=(size_t)n;return p;
}
static void one_file(const char *root,char out[1024])
{
    DIR *d=opendir(root);CHECK(d);struct dirent *e;out[0]=0;
    while((e=readdir(d)))if(strlen(e->d_name)==40&&!strcmp(e->d_name+36,".bin")){
        path_join(out,root,e->d_name);break;
    }
    closedir(d);CHECK(out[0]);
}
static void test_roundtrip(const char *root,const unsigned char *image,size_t n)
{
    X4CatalogDiskCache *c=open_cache(root,0,0);X4ProductMetadata m=metadata("P1"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"P1","MX","es-MX",&m,NULL)==1);
    CHECK(x4_catalog_disk_cache_put_image(c,"P1","MX","es-MX",m.cover_url,image,n,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"p1","mx","ES-mx",&out,NULL)==1);
    CHECK(!memcmp(&m,&out,sizeof(m)));
    CHECK(x4_catalog_disk_cache_get_metadata(c,"P1","US","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"P1","MX","en-US",&out,NULL)==0);
    unsigned char *bytes=malloc(X4_HTTP_IMAGE_MAX+1u);CHECK(bytes);size_t length;
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==1);
    CHECK(length==n&&!memcmp(image,bytes,n));
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,1,&length,NULL)==-1);
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==1);
    const char *other="https://store-images.s-microsoft.com/image/apps.other-variant?w=512&q=80&format=jpg";
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",other,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==0);
    CHECK(x4_catalog_disk_cache_remove_image(c,"P1","MX","es-MX",other)==0);
    X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);CHECK(s.records==2&&s.products==1&&s.stores==2&&s.metadata_hits==1&&s.image_hits==2);
    x4_catalog_disk_cache_close(c);++now;c=open_cache(root,0,0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"P1","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==1);
    now+=X4_CATALOG_DISK_METADATA_TTL;
    CHECK(x4_catalog_disk_cache_get_metadata(c,"P1","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==1);
    now+=X4_CATALOG_DISK_IMAGE_TTL;
    CHECK(x4_catalog_disk_cache_get_image(c,"P1","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==0);
    CHECK(!length);x4_catalog_disk_cache_close(c);free(bytes);
}
static void test_lru(const char *root)
{
    X4CatalogDiskCache *c=open_cache(root,0,2);X4ProductMetadata a=metadata("A"),b=metadata("B"),d=metadata("D"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"A","MX","es-MX",&a,NULL)==1);++now;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"B","MX","es-MX",&b,NULL)==1);++now;
    CHECK(x4_catalog_disk_cache_get_metadata(c,"A","MX","es-MX",&out,NULL)==1);
    x4_catalog_disk_cache_close(c);++now;c=open_cache(root,0,2);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"D","MX","es-MX",&d,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"A","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"B","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"D","MX","es-MX",&out,NULL)==1);
    X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);CHECK(s.products==2&&s.evictions==1);
    x4_catalog_disk_cache_close(c);c=open_cache(root,sizeof(X4ProductMetadata)+100,2);
    x4_catalog_disk_cache_stats(c,&s);CHECK(s.records<=1&&s.bytes<=sizeof(X4ProductMetadata)+100);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
}
static void test_corruption(const char *root,const unsigned char *image,size_t n)
{
    X4CatalogDiskCache *c=open_cache(root,0,0);X4ProductMetadata m=metadata("CORRUPT"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"CORRUPT","MX","es-MX",&m,NULL)==1);
    char file[1024];one_file(root,file);int fd=open(file,O_RDWR);CHECK(fd>=0);
    CHECK(lseek(fd,-1,SEEK_END)>0);unsigned char bad=37;CHECK(write(fd,&bad,1)==1);close(fd);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"CORRUPT","MX","es-MX",&out,NULL)==0);CHECK(!out.name[0]);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"CORRUPT","MX","es-MX",&m,NULL)==1);
    one_file(root,file);CHECK(truncate(file,23)==0);x4_catalog_disk_cache_close(c);
    c=open_cache(root,0,0);X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);CHECK(s.corrupt==1&&!s.records);
    CHECK(x4_catalog_disk_cache_put_image(c,"CORRUPT","MX","es-MX",m.cover_url,image,n,NULL)==1);
    one_file(root,file);fd=open(file,O_RDWR);CHECK(fd>=0);CHECK(lseek(fd,-1,SEEK_END)>0);CHECK(write(fd,&bad,1)==1);close(fd);
    unsigned char *bytes=malloc(X4_HTTP_IMAGE_MAX+1u);CHECK(bytes);size_t length;
    CHECK(x4_catalog_disk_cache_get_image(c,"CORRUPT","MX","es-MX",m.cover_url,bytes,X4_HTTP_IMAGE_MAX+1u,&length,NULL)==0);CHECK(!length);
    x4_catalog_disk_cache_stats(c,&s);CHECK(s.corrupt==2);free(bytes);
    CHECK(x4_catalog_disk_cache_put_image(c,"CORRUPT","MX","es-MX",m.cover_url,image,n,NULL)==1);
    CHECK(x4_catalog_disk_cache_remove_image(c,"CORRUPT","MX","es-MX",m.cover_url)==1);
    CHECK(x4_catalog_disk_cache_remove_image(c,"CORRUPT","MX","es-MX",m.cover_url)==0);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"CORRUPT","MX","es-MX",&m,NULL)==1);one_file(root,file);
    char temporary[1024];memcpy(temporary,file,strlen(file)+1);memcpy(temporary+strlen(temporary)-4,".tmp",5);
    fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);CHECK(write(fd,"incomplete",10)==10);close(fd);
    x4_catalog_disk_cache_close(c);c=open_cache(root,0,0);CHECK(access(temporary,F_OK)<0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"CORRUPT","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
}
static void test_record_bound_and_startup_expiry(const char *root)
{
    X4CatalogDiskCache *c=open_cache(root,0,1);X4ProductMetadata m=metadata("ONE"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"ONE","MX","es-MX",&m,NULL)==1);++now;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"ONE","MX","en-US",&m,NULL)==1);++now;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"ONE","MX","fr-FR",&m,NULL)==1);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"ONE","MX","es-MX",&out,NULL)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"ONE","MX","en-US",&out,NULL)==1);
    X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);CHECK(s.records==2&&s.products==1&&s.evictions==1);
    x4_catalog_disk_cache_close(c);now+=X4_CATALOG_DISK_METADATA_TTL;
    c=open_cache(root,0,1);x4_catalog_disk_cache_stats(c,&s);CHECK(!s.records&&s.expired==2);
    x4_catalog_disk_cache_close(c);
    /* A byte budget smaller than a single typed record stores nothing. */
    c=open_cache(root,100,1);CHECK(x4_catalog_disk_cache_put_metadata(c,"ONE","MX","es-MX",&m,NULL)==0);
    x4_catalog_disk_cache_stats(c,&s);CHECK(!s.records&&!s.bytes);x4_catalog_disk_cache_close(c);
}
static void test_validation_cancel(const char *root,const unsigned char *image,size_t n)
{
    X4CatalogDiskCache *c=open_cache(root,0,0);X4ProductMetadata m=metadata("SAFE"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"../BAD","MX","es-MX",&m,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","../","es-MX",&m,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es/../../",&m,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"OTHER","MX","es-MX",&m,NULL)==-1);
    X4ProductMetadata bad=m;memset(bad.name,'a',sizeof(bad.name));CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&bad,NULL)==-1);
    bad=m;strcpy(bad.name,"\xc0\xaf");CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&bad,NULL)==-1);
    bad=m;strcpy(bad.cover_url,"https://example.org/auth?token=private");CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&bad,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_image(c,"SAFE","MX","es-MX",m.cover_url,(const unsigned char *)"corrupt",7,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_image(c,"SAFE","MX","es-MX","https://example.org/cover",image,n,NULL)==-1);
    atomic_store(&cancel,1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&m,&cancel)==0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"SAFE","MX","es-MX",&out,&cancel)==0);
    atomic_store(&cancel,0);clock_cancel=1;
    CHECK(x4_catalog_disk_cache_put_image(c,"SAFE","MX","es-MX",m.cover_url,image,n,&cancel)==0);
    clock_cancel=0;atomic_store(&cancel,0);X4CatalogDiskStats s;x4_catalog_disk_cache_stats(c,&s);CHECK(!s.records&&s.cancelled>=3);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&m,NULL)==1);
    X4ProductMetadata replacement=m;strcpy(replacement.name,"replacement must not publish");
    clock_cancel=1;CHECK(x4_catalog_disk_cache_put_metadata(c,"SAFE","MX","es-MX",&replacement,&cancel)==0);
    clock_cancel=0;atomic_store(&cancel,0);
    CHECK(x4_catalog_disk_cache_get_metadata(c,"SAFE","MX","es-MX",&out,NULL)==1&&!strcmp(out.name,m.name));
    atomic_store(&cancel,1);CHECK(x4_catalog_disk_cache_clear(c,&cancel)==0);
    atomic_store(&cancel,0);CHECK(x4_catalog_disk_cache_get_metadata(c,"SAFE","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
}
static void test_paths(const char *base,const char *root)
{
    CHECK(!x4_catalog_disk_cache_open(NULL,0,0,NULL,NULL));
    CHECK(!x4_catalog_disk_cache_open("relative/cache",0,0,NULL,NULL));
    CHECK(!x4_catalog_disk_cache_open("/tmp/../escape",0,0,NULL,NULL));
    CHECK(!x4_catalog_disk_cache_open(root,X4_CATALOG_DISK_BUDGET+1u,0,NULL,NULL));
    CHECK(!x4_catalog_disk_cache_open(root,0,129,NULL,NULL));
    char link[1024],linked_root[1024];path_join(link,base,"link");CHECK(!symlink(root,link));
    path_join(linked_root,base,"link/subdir");CHECK(!x4_catalog_disk_cache_open(linked_root,0,0,NULL,NULL));
    char outside[1024],owned[1024],unknown[1024];path_join(outside,base,"outside");
    int fd=open(outside,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);CHECK(write(fd,"keep",4)==4);close(fd);
    path_join(owned,root,"x4c-00000000000000000000000000000000.bin");CHECK(!symlink(outside,owned));
    path_join(unknown,root,"not-our-file");fd=open(unknown,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);close(fd);
    X4CatalogDiskCache *c=open_cache(root,0,0);CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
    struct stat s;CHECK(!lstat(owned,&s)&&S_ISLNK(s.st_mode));CHECK(!stat(outside,&s)&&s.st_size==4);CHECK(!access(unknown,F_OK));
    CHECK(!unlink(owned));CHECK(!unlink(unknown));CHECK(!unlink(link));CHECK(!unlink(outside));
}
static void test_root_identity(const char *base,const char *root)
{
    X4CatalogDiskCache *c=open_cache(root,0,0);X4ProductMetadata m=metadata("IDENTITY"),out;
    CHECK(x4_catalog_disk_cache_put_metadata(c,"IDENTITY","MX","es-MX",&m,NULL)==1);
    char old_root[1024];path_join(old_root,base,"old-cache");CHECK(!rename(root,old_root));CHECK(!mkdir(root,0700));
#ifdef X4_DISK_CACHE_NATIVE_FS_TEST
    CHECK(x4_catalog_disk_cache_get_metadata(c,"IDENTITY","MX","es-MX",&out,NULL)==-1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"IDENTITY","MX","es-MX",&m,NULL)==-1);
#else
    /* Descriptor-relative host mutations stay inside the retained identity. */
    CHECK(x4_catalog_disk_cache_get_metadata(c,"IDENTITY","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_put_metadata(c,"IDENTITY","MX","es-MX",&m,NULL)==1);
#endif
    CHECK(!rmdir(root));CHECK(!rename(old_root,root));
    CHECK(x4_catalog_disk_cache_get_metadata(c,"IDENTITY","MX","es-MX",&out,NULL)==1);
    CHECK(x4_catalog_disk_cache_clear(c,NULL)==1);x4_catalog_disk_cache_close(c);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);char base[]="/tmp/x4-public-cache-test-XXXXXX";CHECK(mkdtemp(base));
    char root[1024];path_join(root,base,"cache");size_t n;unsigned char *image=read_image(argv[1],&n);
    test_roundtrip(root,image,n);test_lru(root);test_corruption(root,image,n);
    test_record_bound_and_startup_expiry(root);test_validation_cancel(root,image,n);
    test_paths(base,root);test_root_identity(base,root);
    CHECK(!rmdir(root));CHECK(!rmdir(base));free(image);
    printf("catalog disk cache: %u checks passed\n",checks);return 0;
}
