/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/stream_sps.h"
#include <stdio.h>
#include <string.h>
static int hex(char x)
{return x>='0'&&x<='9'?x-'0':x>='a'&&x<='f'?x-'a'+10:x>='A'&&x<='F'?x-'A'+10:-1;}
int main(int argc,char **argv)
{
    static uint8_t bytes[4096];
    if(argc!=2||strlen(argv[1])%2||strlen(argv[1])>sizeof(bytes)*2)return 2;
    size_t n=strlen(argv[1])/2;
    for(size_t i=0;i<n;++i){int a=hex(argv[1][2*i]),b=hex(argv[1][2*i+1]);if(a<0||b<0)return 2;bytes[i]=(uint8_t)(16*a+b);}
    X4StreamSps s={0};X4StreamSpsDiagnostics d={0};int rc=x4_stream_sps_inspect_diagnostic(bytes,n,&s,&d);
    printf("{\"rc\":%d,\"syntax_complete\":%s,\"stage\":%u,\"reason\":%u,\"valid_mask\":%llu,\"warning_mask\":%u,\"reader_error\":%u,\"rbsp_bit_offset\":%zu,\"ebsp_byte_offset\":%zu",
        rc,d.syntax_complete?"true":"false",d.stage,d.reason,(unsigned long long)d.valid_mask,d.warning_mask,d.reader_error,d.rbsp_bit_offset,d.ebsp_byte_offset);
#define FIELD(f) printf(",\"" #f "\":%u",s.f)
    FIELD(profile_idc);FIELD(constraints);FIELD(level_idc);FIELD(seq_parameter_set_id);
    FIELD(chroma_format_idc);FIELD(bit_depth_luma);FIELD(bit_depth_chroma);
    FIELD(log2_max_frame_num_minus4);FIELD(pic_order_cnt_type);FIELD(log2_max_pic_order_cnt_lsb_minus4);
    FIELD(max_num_ref_frames);FIELD(max_dec_frame_buffering);FIELD(num_reorder_frames);FIELD(dpb_source);
    FIELD(coded_width);FIELD(coded_height);FIELD(visible_width);FIELD(visible_height);
    FIELD(crop_left);FIELD(crop_right);FIELD(crop_top);FIELD(crop_bottom);FIELD(frame_mbs_only_flag);
    FIELD(direct_8x8_inference_flag);FIELD(vui_present);FIELD(video_signal_type_present);FIELD(colour_description_present);
    FIELD(full_range);FIELD(matrix_coefficients);FIELD(colour_primaries);FIELD(transfer_characteristics);FIELD(bitstream_restriction_present);
#undef FIELD
    puts("}");return 0;
}
