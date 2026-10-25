/* SPDX-License-Identifier: GPL-3.0-only */
/* Execute the formatter used by main and screen, plus the real UTF-8/font
 * measurer. This proves bounded text, not console audio or VideoOut closure. */
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ui/stream_video_status.h"
#include "../src/ui/font.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"stream status check failed line %d: %s\n",__LINE__,#x);abort();} } while(0)
static void check_message(uint32_t width, uint32_t height, int error, const char *expected)
{
    struct Guarded { unsigned char prefix[16]; char text[160]; unsigned char suffix[16]; } b;
    memset(&b,0x5a,sizeof(b));
    CHECK(!x4_stream_video_error_text(b.text,sizeof(b.text),width,height,error));
    CHECK(strlen(b.text)<sizeof(b.text) && !strcmp(b.text,expected));
    for(unsigned i=0;i<16;++i)CHECK(b.prefix[i]==0x5a && b.suffix[i]==0x5a);
    const char *cursor=b.text;
    while(*cursor) CHECK(x4_utf8_next(&cursor)!=0xfffd);
    CHECK(x4_text_width_px(26,0,b.text)<=1752);
    char caption[80];
    CHECK(!x4_stream_video_error_caption(caption,sizeof(caption),error));
    CHECK(strstr(caption,"OPTIONS") && strlen(caption)<sizeof(caption));
}
int main(int argc,char **argv)
{
    CHECK(argc==2 && !x4_font_init(argv[1]));
    check_message(2560,1440,-7413,"Xbox envió 2560×1440; esta PS4 solo admite hasta 1080p. Sal con OPTIONS.");
    check_message(0,0,-7420,"Vídeo detenido (código -7420). Sal con OPTIONS.");
    check_message(0,0,-7,"Vídeo detenido (código -7). Sal con OPTIONS.");
    check_message(UINT32_MAX,UINT32_MAX,INT_MIN,
        "Xbox envió 4294967295×4294967295; esta PS4 solo admite hasta 1080p. Sal con OPTIONS.");
    char b[4]={'a','b','c','d'};
    CHECK(x4_stream_video_error_text(NULL,160,1,1,-1)==-1);
    CHECK(x4_stream_video_error_text(b,0,1,1,-1)==-1 && b[0]=='a');
    CHECK(x4_stream_video_error_text(b,1,1,1,-1)==-1 && b[0]==0 && b[1]=='b');
    CHECK(x4_stream_video_error_caption(NULL,80,-1)==-1);
    CHECK(x4_stream_video_error_caption(b,1,-1)==-1 && b[0]==0 && b[1]=='b');
    char partial[160];
    CHECK(!x4_stream_video_error_text(partial,sizeof(partial),2560,0,-7413));
    CHECK(!strcmp(partial,"Vídeo detenido (código -7413). Sal con OPTIONS."));
    x4_font_close();
    printf("real streaming error formatter/font PASS: %u checks; 4 cases + bounds; profile=%d\n",checks,X4_STREAM_PROFILE);
    return 0;
}
