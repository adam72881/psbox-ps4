/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_hash.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static void vector(const char *input, const uint8_t expected[32])
{
    X4ProbeHashContext context={0}; CHECK(!x4_probe_hash_context_init(&context));
    CHECK(context.capacity==X4_PROBE_READ_CHUNK); uint8_t *reserved=context.buffer;
    FILE *f=tmpfile(); CHECK(f);
    size_t n=strlen(input); CHECK(fwrite(input,1,n,f)==n); rewind(f);
    uint8_t actual[32]; atomic_bool stop=ATOMIC_VAR_INIT(false);
    CHECK(!x4_probe_hash_file(f,n,actual,&stop,&context)); CHECK(!memcmp(actual,expected,32));
    CHECK(context.buffer==reserved);
    rewind(f); atomic_store(&stop,true);
    if(n) CHECK(x4_probe_hash_file(f,n,actual,&stop,&context)==X4_PROBE_CANCELLED);
    atomic_store(&stop,false); rewind(f);
    CHECK(x4_probe_hash_file(f,n+1,actual,&stop,&context)==X4_PROBE_IO);
    if(n){rewind(f);CHECK(x4_probe_hash_file(f,n-1,actual,&stop,&context)==X4_PROBE_SIZE);}
    CHECK(context.buffer==reserved);
    CHECK(x4_probe_hash_context_init(&context)==X4_PROBE_STATE);
    x4_probe_hash_context_close(&context); CHECK(!context.buffer&&!context.capacity);
    rewind(f);CHECK(x4_probe_hash_file(f,n,actual,&stop,&context)==X4_PROBE_STATE);
    x4_probe_hash_context_close(&context);
    CHECK(!fclose(f));
}
int main(void)
{
    static const uint8_t empty[32]={0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
    static const uint8_t abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    vector("",empty);vector("abc",abc);
    uint8_t out[32]; CHECK(x4_probe_hash_file(NULL,0,out,NULL,NULL)==X4_PROBE_ARGUMENT);
    FILE *f=tmpfile();CHECK(f);CHECK(x4_probe_hash_file(f,X4_PROBE_FILE_MAX+1,out,NULL,NULL)==X4_PROBE_ARGUMENT);
    CHECK(x4_probe_hash_file(f,0,out,NULL,NULL)==X4_PROBE_STATE);
    CHECK(x4_probe_hash_context_init(NULL)==X4_PROBE_ARGUMENT);
    X4ProbeHashContext empty_context={0};CHECK(x4_probe_hash_file(f,0,out,NULL,&empty_context)==X4_PROBE_STATE);
    CHECK(!fclose(f));
    printf("PASS real pinned MbedTLS SHA adapter checks=%u; host execution only\n",checks);
    return 0;
}
