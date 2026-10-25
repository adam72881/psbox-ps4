/* SPDX-License-Identifier: GPL-3.0-only */
#include "decoder_depth.h"
#if X4_STREAM_PROFILE
#include <string.h>
int x4_decoder_depth_init(X4DecoderDepthLedger *l,unsigned depth)
{
    if(!l||(depth!=1&&depth!=2))return -7;
    memset(l,0,sizeof(*l));l->depth=depth;
    l->input_count=depth==2?4:2;l->output_count=depth==2?5:4;
    return 0;
}
int x4_decoder_depth_input_acquire(X4DecoderDepthLedger *l,unsigned *slot)
{
    if(!l||!slot||l->deleted||l->drained)return -7;
    unsigned i=l->next_input;
    unsigned subsequent=l->depth==2?l->depth+1:1;
    if(l->input_live[i]&&l->decode_completed-l->submitted_at[i]<subsequent)return -7;
    l->input_live[i]=true;l->submitted_at[i]=l->decode_completed+1;
    l->next_input=(i+1)%l->input_count;*slot=i;return 0;
}
void x4_decoder_depth_decode_returned(X4DecoderDepthLedger *l)
{
    if(!l||l->deleted)return;
    ++l->decode_completed;
    unsigned subsequent=l->depth==2?l->depth+1:1;
    for(unsigned i=0;i<l->input_count;++i)
        if(l->input_live[i]&&l->decode_completed>=l->submitted_at[i]&&
            l->decode_completed-l->submitted_at[i]>=subsequent)l->input_live[i]=false;
}
int x4_decoder_depth_output_offer(X4DecoderDepthLedger *l,unsigned slot)
{
    if(!l||l->deleted||slot>=l->output_count||l->output_held[slot])return -7;
    l->output_held[slot]=true;return 0;
}
int x4_decoder_depth_output_return(X4DecoderDepthLedger *l,unsigned slot)
{
    if(!l||l->deleted||slot>=l->output_count||!l->output_held[slot])return -7;
    l->output_held[slot]=false;return 0;
}
void x4_decoder_depth_flush_complete(X4DecoderDepthLedger *l)
{
    if(!l||l->deleted)return;
    l->drained=true;
    /* Keep remaining input reservations until successful Delete at EOF.
     * A no-picture Flush alone does not prove hidden references retired. */
}
void x4_decoder_depth_delete_complete(X4DecoderDepthLedger *l)
{
    if(!l)return;
    l->deleted=true;
    memset(l->input_live,0,sizeof(l->input_live));
    memset(l->output_held,0,sizeof(l->output_held));
}
unsigned x4_decoder_depth_inputs_live(const X4DecoderDepthLedger *l)
{unsigned n=0;if(l)for(unsigned i=0;i<l->input_count;++i)n+=l->input_live[i];return n;}
unsigned x4_decoder_depth_outputs_held(const X4DecoderDepthLedger *l)
{unsigned n=0;if(l)for(unsigned i=0;i<l->output_count;++i)n+=l->output_held[i];return n;}
#endif
