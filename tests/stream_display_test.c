/* SPDX-License-Identifier: GPL-3.0-only */
/* Real UI display lifecycle with host native-call doubles. This cannot
 * establish PS4 VideoOut behavior, but verifies ownership on every rollback. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/video/display.c"

static struct {
    int open_error, allocate_error, map_error, register_error, rate_error;
    int close_error, unmap_error, release_error;
    unsigned open_calls, close_calls, allocate_calls, map_calls;
    unsigned register_calls, rate_calls, unmap_calls, release_calls;
    int opened, allocated, mapped;
    size_t bytes;
    void *allocation;
} native;

static void reset_fixture(void)
{
    assert(!native.opened && !native.allocated && !native.mapped);
    memset(&native, 0, sizeof native);
}
int sceVideoOutOpen(int user, int bus, int index, const void *parameter)
{
    (void)user; (void)bus; (void)index; (void)parameter;
    ++native.open_calls; assert(!native.opened);
    if (native.open_error) return native.open_error;
    native.opened=1; return 73;
}
int sceVideoOutClose(int handle)
{
    assert(handle==73 && native.opened); ++native.close_calls;
    if (native.close_error) return native.close_error;
    native.opened=0; return 0;
}
size_t sceKernelGetDirectMemorySize(void) { return 256u*1024u*1024u; }
int sceKernelAllocateDirectMemory(off_t low, off_t high, size_t bytes, size_t alignment, int type, off_t *offset)
{
    (void)low; (void)high; assert(alignment==0x200000 && type==3);
    ++native.allocate_calls; assert(!native.allocated);
    if (native.allocate_error) return native.allocate_error;
    native.allocation=malloc(bytes); assert(native.allocation);
    native.bytes=bytes; native.allocated=1; *offset=0x200000; return 0;
}
int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, off_t offset, size_t alignment)
{
    (void)protection; (void)flags; (void)alignment;
    assert(native.allocated && !native.mapped && bytes==native.bytes && offset==0x200000);
    ++native.map_calls;
    if (native.map_error) return native.map_error;
    *address=native.allocation; native.mapped=1; return 0;
}
int sceKernelMunmap(void *address, size_t bytes)
{
    assert(!native.opened && native.mapped && address==native.allocation && bytes==native.bytes);
    ++native.unmap_calls;
    if (native.unmap_error) return native.unmap_error;
    native.mapped=0; return 0;
}
int sceKernelReleaseDirectMemory(off_t offset, size_t bytes)
{
    assert(!native.opened && !native.mapped && native.allocated && offset==0x200000 && bytes==native.bytes);
    ++native.release_calls;
    if (native.release_error) return native.release_error;
    native.allocated=0; free(native.allocation); native.allocation=NULL; return 0;
}
void sceVideoOutSetBufferAttribute(OrbisVideoOutBufferAttribute *attribute, int format, int tiling,
    int aspect, unsigned width, unsigned height, unsigned pitch)
{
    (void)format; (void)tiling; (void)aspect;
    *attribute=(OrbisVideoOutBufferAttribute){width,height,pitch};
}
int sceVideoOutRegisterBuffers(int handle, int first, void *const *buffers, int count,
    const OrbisVideoOutBufferAttribute *attribute)
{
    assert(handle==73 && first==0 && count==2 && native.opened && native.mapped);
    assert(buffers[0]==native.allocation && buffers[1] && attribute->width==1920 && attribute->height==1080);
    ++native.register_calls; return native.register_error;
}
int sceVideoOutSetFlipRate(int handle, int rate)
{
    (void)rate; assert(handle==73 && native.opened && native.mapped);
    ++native.rate_calls; return native.rate_error;
}
static void assert_clean(const X4Display *display)
{
    assert(display->handle<0 && !display->memory && !display->allocated);
    assert(!native.opened && !native.mapped && !native.allocated);
}
static void assert_cannot_reopen(X4Display *display)
{
    X4Display before=*display;
    unsigned opens=native.open_calls, closes=native.close_calls;
    unsigned unmaps=native.unmap_calls, releases=native.release_calls;
    assert(x4_display_open(display)<0);
    assert(memcmp(&before,display,sizeof before)==0);
    assert(opens==native.open_calls && closes==native.close_calls);
    assert(unmaps==native.unmap_calls && releases==native.release_calls);
}
static void case_success(void)
{
    reset_fixture(); X4Display d={.handle=-1};
    assert(x4_display_open(&d)==0); assert_cannot_reopen(&d);
    assert(x4_display_close_checked(&d)==0); assert_clean(&d);
    assert(x4_display_close_checked(&d)==0); assert(native.close_calls==1);
    assert(x4_display_open(&d)==0); assert(x4_display_close_checked(&d)==0); assert_clean(&d);
}
static void case_failed_open(unsigned stage)
{
    reset_fixture(); X4Display d={.handle=-1};
    int *errors[]={&native.open_error,&native.allocate_error,&native.map_error,&native.register_error,&native.rate_error};
    *errors[stage]=-200-(int)stage;
    assert(x4_display_open(&d)==*errors[stage]); assert_clean(&d);
    assert(native.close_calls==(stage?1u:0u));
}
static void case_retained_close(unsigned stage, int close_error)
{
    reset_fixture(); X4Display d={.handle=-1};
    if(stage==0)native.map_error=-301;
    else if(stage==1)native.register_error=-302;
    else native.rate_error=-303;
    native.close_error=close_error;
    assert(x4_display_open(&d)<0);
    assert(d.handle==73 && d.allocated && native.opened && native.allocated);
    assert((d.memory!=NULL)==(stage!=0));
    assert(native.unmap_calls==0 && native.release_calls==0);
    assert_cannot_reopen(&d);
    /* Legacy void wrapper must also retain the resource in new profiles. */
    x4_display_close(&d); assert(d.handle==73 && native.release_calls==0);
    native.close_error=0; assert(x4_display_close_checked(&d)==0); assert_clean(&d);
}
static void case_retained_unmap(void)
{
    reset_fixture(); X4Display d={.handle=-1};
    native.register_error=-401; native.unmap_error=-402;
    assert(x4_display_open(&d)==-402);
    assert(d.handle<0 && d.memory && d.allocated && !native.opened);
    assert(native.release_calls==0); assert_cannot_reopen(&d);
    native.unmap_error=0; assert(x4_display_close_checked(&d)==0); assert_clean(&d);
    assert(native.close_calls==1);
}
static void case_retained_release(void)
{
    reset_fixture(); X4Display d={.handle=-1};
    native.register_error=-501; native.release_error=-502;
    assert(x4_display_open(&d)==-502);
    assert(d.handle<0 && !d.memory && d.allocated); assert_cannot_reopen(&d);
    native.release_error=0; assert(x4_display_close_checked(&d)==0); assert_clean(&d);
    assert(native.close_calls==1 && native.unmap_calls==1);
}
int main(void)
{
    assert(x4_display_open(NULL)<0);
    case_success();
    for(unsigned stage=0;stage<5;++stage)case_failed_open(stage);
    for(unsigned stage=0;stage<3;++stage){case_retained_close(stage,-601);case_retained_close(stage,74);}
    case_retained_unmap(); case_retained_release();
    puts("PASS actual display.c rollback/open/close ownership; native calls are host doubles, not PS4 evidence");
    return 0;
}
