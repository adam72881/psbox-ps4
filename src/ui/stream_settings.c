/* SPDX-License-Identifier: GPL-3.0-only */
#include "stream_settings.h"
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
#include "canvas.h"
#include "font.h"
#include <stdio.h>

/* Display order is independent from stable captured profile IDs. */
static const X4StreamRequestProfile profile_order[]={X4_REQUEST_720P60,X4_REQUEST_720P_HQ,
    X4_REQUEST_1080P_G,X4_REQUEST_1080P_G8,X4_REQUEST_1080P_H,X4_REQUEST_1080P_HQ};
static unsigned profile_ordinal(X4StreamRequestProfile profile)
{
    for(unsigned i=0;i<X4_REQUEST_PROFILE_COUNT;++i)if(profile_order[i]==profile)return i;
    return 0;
}

bool x4_stream_settings_ui_handle(X4StreamSettingsUi *s, unsigned a, bool can_edit)
{
    if (!s) return false;
    if (!can_edit) { bool consumed = s->open || (a & X4_SETTINGS_OPEN); s->open = false; return consumed; }
    if (!s->open) {
        if (!(a & X4_SETTINGS_OPEN)) return false;
        X4StreamRequestConfig current = x4_stream_settings_get();
        s->profile = current.profile; s->color = current.color_preference; s->cpu=(X4StreamCpuVariant)current.cpu_variant;
        s->depth=(X4StreamDecoderDepth)current.decoder_depth;
        s->interaction_modes=current.interaction_modes;
        s->row = 0; s->open = true; s->applied = false;
        return true; /* R3+X may open, but cannot confirm or launch. */
    }
    if (a & X4_SETTINGS_CANCEL) { s->open = false; return true; }
    unsigned navigation = a & (X4_SETTINGS_UP | X4_SETTINGS_DOWN | X4_SETTINGS_LEFT | X4_SETTINGS_RIGHT);
    if (navigation) {
        if (a & X4_SETTINGS_DOWN) s->row=(s->row+1u)%8u;
        else if(a & X4_SETTINGS_UP) s->row=(s->row+7u)%8u;
        if (a & (X4_SETTINGS_LEFT | X4_SETTINGS_RIGHT)) {
            if (!s->row) s->profile = profile_order[(profile_ordinal(s->profile) +
                ((a & X4_SETTINGS_RIGHT) ? 1u : X4_REQUEST_PROFILE_COUNT-1u)) % X4_REQUEST_PROFILE_COUNT];
            else if(s->row==1) s->color = (X4StreamColorPreference)((unsigned)s->color ^ 1u);
            else if(s->row==2) s->cpu=(X4StreamCpuVariant)(((unsigned)s->cpu+((a&X4_SETTINGS_RIGHT)?1u:3u))%4u);
            else if(s->row==3) s->depth=(X4StreamDecoderDepth)(((unsigned)s->depth+((a&X4_SETTINGS_RIGHT)?1u:2u))%3u);
            else if(s->row==4) s->interaction_modes^=X4_INTERACTION_INPUT_60HZ;
            else if(s->row==5) s->interaction_modes^=X4_INTERACTION_RUMBLE_OFF;
            else if(s->row==6) s->interaction_modes^=X4_INTERACTION_AUDIO_PREVIOUS;
            else if(s->row==7) s->interaction_modes^=X4_INTERACTION_VSYNC_OFF;
        }
    } else if ((a & X4_SETTINGS_CONFIRM) && x4_stream_settings_set_extended(s->profile,s->color,s->cpu,s->depth,s->interaction_modes)) {
        s->applied = true; s->open = false;
    }
    return true;
}
void x4_stream_settings_summary(char *out, unsigned capacity)
{
    X4StreamRequestConfig current = x4_stream_settings_get();
    if (out && capacity) (void)snprintf(out, capacity, "Calidad: %s · CPU-%u · R3", current.label,current.cpu_variant);
}
int x4_stream_settings_applied_text(const X4StreamSettingsUi *s, char *out, unsigned capacity)
{
    if (!out || !capacity) return -1;
    out[0] = 0;
    if (!s || !s->applied || s->open) return -1;
    X4StreamRequestConfig applied = x4_stream_request_config(s->profile,s->color);
    int n=snprintf(out,capacity,"Aplicado: %s · CPU-%u",applied.label,(unsigned)s->cpu);
    return n>=0&&(unsigned)n<capacity?0:-1;
}
static void text(uint32_t *p, int x, int y, int size, int bold, const char *value, int width, uint32_t color)
{ x4_text_ellipsis_px(p, x, y, size, bold, value, width, color); }
static void border(uint32_t *p, int x, int y, int w, int h, uint32_t c)
{
    x4_rect(p,x,y,w,3,c); x4_rect(p,x,y+h-3,w,3,c);
    x4_rect(p,x,y,3,h,c); x4_rect(p,x+w-3,y,3,h,c);
}
void x4_stream_settings_ui_draw(const X4StreamSettingsUi *s, uint32_t *p)
{
    if (!s || !p) return;
    const uint32_t bg=0xff211910, white=0xfff5f1eb, muted=0xffc4baae, green=0xff64e78c;
    if (!s->open) {
        char applied[96];
        if (!x4_stream_settings_applied_text(s,applied,sizeof(applied))) {
            x4_rect(p,650,109,580,34,bg);
            text(p,664,111,26,1,applied,552,green);
        }
        return;
    }
    x4_rect(p,270,130,1380,845,bg); border(p,270,130,1380,845,green);
    text(p,310,182,40,1,"Opciones de la próxima sesión",1290,white);
    text(p,310,232,26,0,"Se recuerda hasta cerrar XCloud4. Xbox decide qué señal entrega.",1290,muted);
    text(p,325,276,29,1,"Perfil de solicitud",1250,white);
    const char *profiles[] = {"720p60 · android", "720p-HQ · web windows · alias 720HQ", "1080p-G · web windows · alias 1080",
        "1080p-G8 · igual a G · solicitud 8 Mbps",
        "1080p-H · nativa windows · alias 1080", "1080p-HQ · web windows · alias 1080HQ"};
    text(p,345,309,27,1,profiles[profile_ordinal(s->profile)],1220,green);
    if (!s->row) x4_rect(p,300,265,4,74,green);
    text(p,325,350,29,1,"Color si el SPS no especifica la matriz",1240,white);
    text(p,345,383,26,0,s->color==X4_COLOR_AUTO_709?
        "Automático (BT.709 en 720p o más)":"BT.601",1220,green);
    if (s->row==1) x4_rect(p,300,339,4,74,green);
    text(p,325,424,29,1,"CPU de la próxima sesión",1240,white);
    const char *cpus[]={"CPU-0 · actual (700)", "CPU-1 · decoder a prioridad 480",
        "CPU-2 · decoder a 480 y núcleos separados", "CPU-3 · decoder a 256 y núcleos separados"};
    text(p,345,457,26,0,cpus[s->cpu<=X4_CPU_3?s->cpu:X4_CPU_0],1220,green);
    if(s->row==2)x4_rect(p,300,413,4,74,green);
    text(p,325,498,29,1,"Profundidad del decoder",1240,white);
    const char *depths[]={"Profundidad: Auto (720p: 1 · 1080p: 2)","Profundidad: 1","Profundidad: 2"};
    text(p,345,531,26,0,depths[s->depth<=X4_DEPTH_2?s->depth:X4_DEPTH_AUTO],1220,green);
    if(s->row==3)x4_rect(p,300,487,4,74,green);
    text(p,325,572,29,1,"Envío del mando",1240,white);
    text(p,345,605,26,0,s->interaction_modes&X4_INTERACTION_INPUT_60HZ?
        "60 Hz (anterior)":"Inmediato",1220,green);
    if(s->row==4)x4_rect(p,300,561,4,74,green);
    text(p,325,646,29,1,"Vibración",1240,white);
    text(p,345,679,26,0,s->interaction_modes&X4_INTERACTION_RUMBLE_OFF?"No":"Sí",1220,green);
    if(s->row==5)x4_rect(p,300,635,4,74,green);
    text(p,325,720,29,1,"Audio",1240,white);
    text(p,345,753,26,0,s->interaction_modes&X4_INTERACTION_AUDIO_PREVIOUS?"Anterior":"Nuevo",1220,green);
    if(s->row==6)x4_rect(p,300,709,4,74,green);
    text(p,325,794,29,1,"Sincronía vertical",1240,white);
    text(p,345,827,26,0,s->interaction_modes&X4_INTERACTION_VSYNC_OFF?
        "No (menos retraso, puede verse un corte)":"Sí",1220,green);
    if(s->row==7)x4_rect(p,300,783,4,74,green);
    text(p,310,890,26,0,"ARRIBA/ABAJO Campo   IZQUIERDA/DERECHA Cambiar",1290,white);
    text(p,310,932,26,0,"X Aplicar   CÍRCULO Cancelar   OPTIONS Salir",1290,white);
}
int x4_stream_received_text(char *out, unsigned capacity, unsigned width, unsigned height,
    unsigned received, unsigned selected, bool full_range)
{
    if (!out || !capacity) return -1;
    int n=snprintf(out,capacity,"Recibido %u×%u · matriz SPS %u%s · conversión %s · rango %s",
        width,height,received,received==2?" (sin especificar)":"",
        selected==1?"BT.709":"BT.601",full_range?"completo":"limitado");
    return n>=0&&(unsigned)n<capacity?0:-1;
}
void x4_stream_received_draw(uint32_t *p, unsigned width, unsigned height,
    unsigned received, unsigned selected, bool full_range)
{
    char line[224];
    if (x4_stream_received_text(line,sizeof(line),width,height,received,selected,full_range)) return;
    x4_rect(p,84,84,1752,64,0xff211910);
    text(p,104,97,30,1,line,1712,0xfff5f1eb);
}
#endif
