/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <orbis/GnmDriver.h>
#include <orbis/libkernel.h>
#include "../video/display.h"
#include "../input/controller.h"
#include "../ui/screen.h"
#include "../ui/lang.h"
#include "../ui/catalog_gui.h"
#include "../ui/catalog_presentation.h"
#include "../ui/font.h"
#include "../auth/auth_profile.h"
#include "../media/live_media.h"
#include "lifecycle.h"
#include "build_identity.h"
#include "psbox_exit.h"
#include "../psbox/psbox_ui.h"
#include "probe_controller.h"
#include "../streaming/stream_profile.h"
#include "../ui/stream_video_status.h"
#include "../ui/stream_settings.h"
#include "../auth/release_check.h"

#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
/* Main is the sole writer and exporter. No pad read or local action is
 * changed by these measurements. Durations use upper 50us histogram bins. */
enum { INTERACTION_HIST_BINS=256, INTERACTION_PHASES=17, INTERACTION_GAPS=128 };
typedef struct { uint64_t bins[INTERACTION_HIST_BINS],count,total,max; } InteractionTimes;
typedef struct { uint64_t at_us,elapsed_us; unsigned phase; } InteractionGap;
static struct {
    bool available;
    unsigned modes;
    uint64_t begin_us,ended_us,calls,ok,errors,rate_second,second_calls,max_second;
    int last_result;
    struct { int result; uint64_t count; } results[16];
    unsigned result_count;
    uint64_t result_omitted;
    InteractionTimes call_times;
    uint64_t loop_previous,phase_at,phase_us[INTERACTION_PHASES],phase_gaps[INTERACTION_PHASES],gaps,gap_omitted;
    unsigned phase;
    InteractionGap gap[INTERACTION_GAPS];
} interaction_main;
typedef struct {
    OrbisPadVibeParam applied;
    int handle;
    uint64_t last_call_us;
    bool called,stop_pending;
} InteractionRumble;
static void interaction_time(InteractionTimes *h,uint64_t value)
{
    unsigned b=value/50u<INTERACTION_HIST_BINS?(unsigned)(value/50u):INTERACTION_HIST_BINS-1;
    ++h->bins[b];++h->count;h->total+=value;if(value>h->max)h->max=value;
}
static uint64_t interaction_percentile(const InteractionTimes *h,unsigned percent)
{
    uint64_t target=(h->count*percent+99u)/100u,n=0;
    if(!target)return 0;
    for(unsigned i=0;i<INTERACTION_HIST_BINS;++i)if((n+=h->bins[i])>=target)
        return i==INTERACTION_HIST_BINS-1?h->max:(uint64_t)(i+1)*50u;
    return h->max;
}
static void interaction_main_begin(unsigned modes,uint64_t now)
{
    memset(&interaction_main,0,sizeof(interaction_main));
    interaction_main.available=true;interaction_main.modes=modes;interaction_main.begin_us=now;
}
static void interaction_phase(unsigned phase,uint64_t now)
{
    if(!interaction_main.available)return;
    if(interaction_main.phase_at&&now>=interaction_main.phase_at&&interaction_main.phase<INTERACTION_PHASES)
        interaction_main.phase_us[interaction_main.phase]+=now-interaction_main.phase_at;
    interaction_main.phase=phase;interaction_main.phase_at=now;
}
static void interaction_main_read(uint64_t now)
{
    if(!interaction_main.available)return;
    interaction_phase(X4_MON_PHASE_MAIN_POLL,now);
    if(interaction_main.loop_previous&&now>=interaction_main.loop_previous&&now-interaction_main.loop_previous>20000u){
        unsigned longest=0;
        for(unsigned i=1;i<INTERACTION_PHASES;++i)if(interaction_main.phase_us[i]>interaction_main.phase_us[longest])longest=i;
        ++interaction_main.phase_gaps[longest];
        uint64_t index=interaction_main.gaps++;
        if(index<INTERACTION_GAPS)interaction_main.gap[index]=(InteractionGap){now,now-interaction_main.loop_previous,longest};
        else ++interaction_main.gap_omitted;
    }
    memset(interaction_main.phase_us,0,sizeof(interaction_main.phase_us));interaction_main.loop_previous=now;
}
static void interaction_rumble_result(int result,uint64_t began,uint64_t ended)
{
    if(!interaction_main.available)return;
    uint64_t second=began>=interaction_main.begin_us?(began-interaction_main.begin_us)/1000000u:0;
    if(second!=interaction_main.rate_second){interaction_main.rate_second=second;interaction_main.second_calls=0;}
    ++interaction_main.second_calls;if(interaction_main.second_calls>interaction_main.max_second)interaction_main.max_second=interaction_main.second_calls;
    ++interaction_main.calls;if(result>=0)++interaction_main.ok;else ++interaction_main.errors;
    interaction_main.last_result=result;
    unsigned i=0;while(i<interaction_main.result_count&&interaction_main.results[i].result!=result)++i;
    if(i<16){if(i==interaction_main.result_count){interaction_main.results[i].result=result;++interaction_main.result_count;}++interaction_main.results[i].count;}
    else ++interaction_main.result_omitted;
    interaction_time(&interaction_main.call_times,ended>=began?ended-began:0);
}
/* A stop observed while the 16ms cap is active remains latched, even if a
 * newer positive intensity arrives before the next permitted native call. */
static void interaction_rumble_step(InteractionRumble *s,int handle,OrbisPadVibeParam latest,uint64_t now)
{
    if(handle<0){s->handle=-1;return;}
    if(!latest.lgMotor&&!latest.smMotor&&(s->applied.lgMotor||s->applied.smMotor))s->stop_pending=true;
    OrbisPadVibeParam wanted=s->stop_pending?(OrbisPadVibeParam){0}:latest;
    if(s->handle==handle&&s->applied.lgMotor==wanted.lgMotor&&s->applied.smMotor==wanted.smMotor){
        if(!wanted.lgMotor&&!wanted.smMotor)s->stop_pending=false;
        return;
    }
    if(s->called&&(now<s->last_call_us||now-s->last_call_us<16000u))return;
    uint64_t began=sceKernelGetProcessTime();
    int result=scePadSetVibration(handle,&wanted);
    uint64_t ended=sceKernelGetProcessTime();
    s->last_call_us=began;s->called=true;
    interaction_rumble_result(result,began,ended);
    if(result>=0){s->applied=wanted;s->handle=handle;if(!wanted.lgMotor&&!wanted.smMotor)s->stop_pending=false;}
}
static void interaction_rumble_stop(InteractionRumble *s,int handle)
{
    if(handle<0)return;
    if(s->handle==handle&&!s->applied.lgMotor&&!s->applied.smMotor&&!s->stop_pending)return;
    for(unsigned attempt=0;attempt<3;++attempt){
        uint64_t now=sceKernelGetProcessTime();
        if(s->called&&now>=s->last_call_us&&now-s->last_call_us<16000u){
            sceKernelUsleep((unsigned)(16000u-(now-s->last_call_us)));now=sceKernelGetProcessTime();
        }
        interaction_rumble_step(s,handle,(OrbisPadVibeParam){0},now);
        if(s->handle==handle&&!s->applied.lgMotor&&!s->applied.smMotor&&!s->stop_pending)return;
    }
    printf("XCloud4: parada de vibracion no confirmada tras 3 intentos; retorno=%d\n",interaction_main.last_result);
}
void x4_main_interaction_metrics_write(FILE *f)
{
    if(!interaction_main.ended_us)interaction_main.ended_us=sceKernelGetProcessTime();
    uint64_t ended=interaction_main.ended_us;
    uint64_t elapsed=ended>=interaction_main.begin_us?ended-interaction_main.begin_us:0;
    fprintf(f,"{\"available\":%s,\"rumble_enabled\":%s,\"native_calls\":%llu,\"successful\":%llu,\"errors\":%llu,\"elapsed_us\":%llu,\"calls_per_second\":%.6f,\"maximum_calls_in_one_second\":%llu,\"last_result\":%d,\"call_duration_us\":{\"n\":%llu,\"p50_upper\":%llu,\"p95_upper\":%llu,\"max\":%llu,\"bin_us\":50},\"native_results\":[",
        interaction_main.available?"true":"false",(interaction_main.modes&X4_INTERACTION_RUMBLE_OFF)?"false":"true",
        (unsigned long long)interaction_main.calls,(unsigned long long)interaction_main.ok,(unsigned long long)interaction_main.errors,
        (unsigned long long)elapsed,elapsed?(double)interaction_main.calls*1000000.0/(double)elapsed:0.0,(unsigned long long)interaction_main.max_second,interaction_main.last_result,
        (unsigned long long)interaction_main.call_times.count,(unsigned long long)interaction_percentile(&interaction_main.call_times,50),
        (unsigned long long)interaction_percentile(&interaction_main.call_times,95),(unsigned long long)interaction_main.call_times.max);
    for(unsigned i=0;i<interaction_main.result_count;++i){if(i)fputc(',',f);fprintf(f,"{\"result\":%d,\"count\":%llu}",interaction_main.results[i].result,(unsigned long long)interaction_main.results[i].count);}
    fprintf(f,"],\"native_results_omitted\":%llu,\"main_loop_gaps_over_20ms\":{\"count\":%llu,\"omitted\":%llu,\"phase_scope\":\"phase with most measured time in the pad-read interval; numeric phases use existing monitor enum; session startup and close included\",\"records\":[",
        (unsigned long long)interaction_main.result_omitted,(unsigned long long)interaction_main.gaps,(unsigned long long)interaction_main.gap_omitted);
    uint64_t n=interaction_main.gaps<INTERACTION_GAPS?interaction_main.gaps:INTERACTION_GAPS;
    for(uint64_t i=0;i<n;++i){if(i)fputc(',',f);fprintf(f,"{\"at_us\":%llu,\"elapsed_us\":%llu,\"phase\":%u}",(unsigned long long)interaction_main.gap[i].at_us,(unsigned long long)interaction_main.gap[i].elapsed_us,interaction_main.gap[i].phase);}
    fputs("],\"counts_by_phase\":[",f);
    for(unsigned i=0;i<INTERACTION_PHASES;++i){if(i)fputc(',',f);fprintf(f,"%llu",(unsigned long long)interaction_main.phase_gaps[i]);}
    fputs("]}}",f);
}
#endif

#if !X4_VIDEO_PROBE
static void trace_inactive(X4LiveMedia *live)
{
    if (live) {
        X4Trace *trace = x4_live_media_trace(live);
        x4_trace_monitor_epoch(trace, X4_MON_ACTOR_SESSION, X4_MON_EPOCH_TRANSITION);
        x4_trace_set_active(trace, false);
    }
}
static bool trace_session_active(enum X4SessionState state)
{
    return state != X4_SESSION_STOPPING && state != X4_SESSION_CLOSED &&
        state != X4_SESSION_CANCELLED && state != X4_SESSION_ERROR;
}
#if X4_STREAM_PROFILE
typedef struct {
    const X4LiveMediaSnapshot *media;
    const X4SessionSnapshot *session;
    int muted;
    const X4TouchMenu *touch_menu;
    bool show_banner;
    bool received_banner;
} StreamOverlayContext;
static void stream_overlay(uint32_t *pixels,void *context)
{
    const StreamOverlayContext *c=context;
    x4_live_overlay(c->media,c->session,c->muted,c->touch_menu,c->show_banner,pixels);
    if(c->received_banner) x4_stream_received_draw(pixels,c->media->width,c->media->height,
        c->media->video_matrix_received,c->media->video_matrix_selected,c->media->video_full_range);
}
/* Main is the only display owner. Stop only video once, preserve the RTC
 * and audio owners, and let the explicit detached observation authorize UI
 * reopening. A native cleanup error may retain the media context. */
static void stream_halt_video_error(X4LiveMedia *live, X4LiveMediaSnapshot *status,
    int error, bool *handled)
{
    if (*handled) return;
    *handled = true;
    int halt_rc = x4_live_media_stream_halt_video(live);
    x4_live_media_snapshot(live, status);
    char message[160];
    (void)x4_stream_video_error_text(message, sizeof(message),
        status->video_rejected_width, status->video_rejected_height, error);
    printf("XCloud4: %s video_error=%d halt_rc=%d display_detached=%d; audio/RTC no cancelados\n",
        message, error, halt_rc, status->video_display_detached);
    if (!status->video_display_detached) {
#if X4_PUBLIC
        printf("XCloud4: VideoOut sigue retenido; no se puede dibujar el aviso ni reabrir UI con seguridad. PARA SALIR: BOTON PS > CERRAR APLICACION.\n");
#else
        printf("XCloud4: VideoOut sigue retenido; no se puede dibujar el aviso ni reabrir UI con seguridad. OPTIONS sigue disponible para salir.\n");
#endif
    }
}
#endif
static void main_submit_done(void)
{
    static bool gnm_idle_error_logged;
    int rc = sceGnmSubmitDone();
    if (rc < 0 && !gnm_idle_error_logged) {
        printf("XCloud4: Gnm submitDone idle result=0x%08x\n", (unsigned)rc);
        gnm_idle_error_logged = true;
    }
}
#endif

int main(void)
{
#if X4_VIDEO_PROBE
    /* Local source only: enter before any auth/catalog/network allocation. */
    return x4_probe_controller_run();
#else
    /* PSBox: blocca SIGTERM prima di qualsiasi thread (Chiudi dal menu PS). */
    (void)psbox_exit_install();
    int psbox_ui = 0;
    X4Display display={.handle=-1};
    X4Controller controller;
    X4Screen screen = {0};
    X4DemoVideo video = {0};
    X4DemoAudio audio = {.handle = -1};
    X4Auth *auth = x4_auth_create();
    X4AuthSnapshot account = {0};
    static X4CatalogSnapshot catalog;
    static X4CatalogBrowser catalog_browser;
    X4AuthCatalogCursor catalog_cursor = {0};
    X4AuthRecentCursor recent_cursor = {0};
    X4RecentSnapshot recent_snapshot = {0};
    X4RecentGames recent;
    static X4CatalogPresentation presentation;
    X4CatalogAssets *catalog_assets = NULL;
    uint64_t ui_generation = 0, metadata_revision = 0;
    int pending_auth = -1, pending_launch = 0;
    char pending_title_id[96] = {0};
    uint64_t pending_generation = 0;
    char ui_notice[160] = {0};
    X4SessionSnapshot session = {0};
    unsigned catalog_selected = 0;
    int session_back = 0;
    int game_controls = 0;
    int media_active = 0;
    int closing = 0;
    X4LiveMedia *live = NULL;
    X4LiveMediaSnapshot live_status = {0};
    int live_error = 0, live_muted = 0;
#if X4_STREAM_PROFILE
    bool video_start_error = false;
#endif
    int live_retained = 0;
    bool live_presented = false;
#if X4_STREAM_PROFILE
    bool stream_display_active=false;
    X4StreamSettingsUi stream_settings_ui={0};
    uint64_t stream_received_until=0;
#endif
    bool diagnostic_stable = false;
    uint64_t diagnostic_mark = 0;
    X4TouchMenu touch_menu;
    x4_touch_menu_init(&touch_menu, X4_PUBLIC != 0);
    uint64_t video_banner_until_us = 0;
    int overlay_key[11] = {0};
    uint64_t overlay_at = 0;
    uint64_t input_previous = 0, input_report_at = 0;
    uint64_t input_intervals = 0, input_interval_us = 0, input_interval_max_us = 0;
    uint64_t idle_present_skips = 0;
#if X4_RUMBLE
#if X4_STREAM_PROFILE
    InteractionRumble rumble_state={.handle=-1};
#else
    OrbisPadVibeParam rumble_applied={0};
    int rumble_handle=-1;
    uint64_t rumble_pad_calls=0,rumble_pad_ok=0,rumble_pad_errors=0;
#endif
    bool rumble_was_connected=false;
#endif
    x4_catalog_browser_init(&catalog_browser);
    x4_recent_init(&recent);
    x4_catalog_presentation_init(&presentation);
    setvbuf(stdout, NULL, _IONBF, 0);
    (void)x4_lang_init();
    x4_release_check_start();
reopen_interface:;
    if (x4_font_init("/app0/assets/fonts/noto_sans.x4font") < 0)
        printf("XCloud4: fuente externa no disponible; respaldo suavizado activo\n");
    catalog_assets = x4_catalog_assets_create();
    int rc = x4_display_open(&display);
    if (rc < 0) {
        printf("XCloud4: no se pudo iniciar VideoOut: 0x%08x\n", (unsigned)rc);
        sceKernelUsleep(3000000);
        /* Never enter the known failing libc _exit path on this firmware. */
        if (x4_exit_prepare() >= 0) x4_exit_request();
        for (;;) sceKernelUsleep(100000);
    }
    x4_controller_init(&controller);
    psbox_ui = (psbox_ui_init() == 0);
    if (psbox_ui) {
        printf("PSBox: UI PSCloud attiva (base XCloud4)\n");
        psbox_ui_poll_local_update("01.57");
    } else {
        printf("PSBox: UI PSCloud non disponibile, resto su UI XCloud4 nativa\n");
    }
#if X4_STREAM_PROFILE
    stream_display_active=false;
    if(!live){live_retained=0;live_error=0;
        video_start_error=false;
    }
#endif
#if X4_RUMBLE
#if X4_STREAM_PROFILE
    rumble_state=(InteractionRumble){.handle=-1};rumble_was_connected=false;
#else
    rumble_applied=(OrbisPadVibeParam){0}; rumble_handle=-1; rumble_was_connected=false;
#endif
#endif
#ifdef X4_INGRESS_BASELINE
    printf("XCloud4 %s BASE: solo para referencia, no es la prueba autorizada\n",X4_PRODUCT_VERSION);
#else
    printf("XCloud4 %s: progreso independiente, base MPSC\n", X4_PRODUCT_VERSION);
#endif
    printf("XCloud4: %s\n", X4_AUTH_PROFILE_NOTE);
    printf("XCloud4: diagnostic build=%s\n", X4_BUILD_ID);
    for (unsigned frame = 0;; ++frame) {
#if X4_STREAM_PROFILE
        bool stream_settings_consumed=false;
#endif
        uint64_t input_at = sceKernelGetProcessTime();
        x4_trace_monitor_state(live ? x4_live_media_trace(live) : NULL,
            X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAIN_POLL, input_at);
#if X4_STREAM_PROFILE
        uint64_t controller_read_at=sceKernelGetProcessTime();
        if(live&&screen.page==6&&x4_auth_busy(auth))interaction_main_read(controller_read_at);
#endif
        x4_controller_read(&controller, frame);
#if X4_STREAM_PROFILE
        if(live&&screen.page==6&&x4_auth_busy(auth))
            x4_live_media_stream_note_controller_read(live,controller_read_at);
#endif
        /* Local pad sampling cadence, not Xbox RTT or end-to-end latency. */
        if (screen.page == 6 && x4_auth_busy(auth)) {
            if (input_previous) {
                uint64_t elapsed = input_at - input_previous;
                ++input_intervals;
                input_interval_us += elapsed;
                if (elapsed > input_interval_max_us) input_interval_max_us = elapsed;
            }
            input_previous = input_at;
            if (!input_report_at) input_report_at = input_at;
            if (input_at - input_report_at >= 5000000) {
                printf("XCloud4 input: samples=%llu mean_us=%llu max_us=%llu idle_flips_skipped=%llu\n",
                    (unsigned long long)input_intervals,
                    (unsigned long long)(input_intervals ? input_interval_us / input_intervals : 0),
                    (unsigned long long)input_interval_max_us,
                    (unsigned long long)idle_present_skips);
                input_report_at = input_at;
                input_intervals = input_interval_us = input_interval_max_us = idle_present_skips = 0;
            }
        } else {
            input_previous = input_report_at = 0;
            input_intervals = input_interval_us = input_interval_max_us = idle_present_skips = 0;
        }
        int previous_page = screen.page;
        /* Keep Xbox button ownership across transient RTC disconnects. A
         * momentary status change must never turn B/Menu into local exit. */
        if (!x4_auth_busy(auth) || screen.page != 6) game_controls = 0;
        else if (session.rtc_connected) game_controls = 1;
        bool game_input = screen.page == 6 && game_controls && x4_auth_busy(auth) &&
            !closing && !session_back && !live_error;
#if X4_PUBLIC
        (void)diagnostic_stable;
        (void)diagnostic_mark;
#endif
        if (game_input) {
            x4_touch_menu_feed_touch(&touch_menu, game_input, controller.data.touch.fingers, controller_read_at);
        } else {
            x4_touch_menu_close(&touch_menu);
        }
        X4GamepadFrame gamepad = {0};
        if (game_input) {
            if (x4_touch_menu_is_open(&touch_menu)) {
                gamepad.connected = controller.data.connected;
            } else {
                X4Controller filtered = controller;
                filtered.data.buttons = x4_touch_menu_filter_buttons(&touch_menu, controller.data.buttons);
                x4_controller_gamepad(&filtered, &gamepad);
                if (x4_touch_menu_is_nexus_active(&touch_menu, controller_read_at)) {
                    gamepad.buttons |= X4_GAMEPAD_NEXUS;
                }
            }
        }
#if X4_STREAM_PROFILE
        gamepad.sampled_us=controller_read_at;
#endif
        x4_auth_set_gamepad(auth, &gamepad);
#if X4_RUMBLE
        X4RumbleOutput desired={0};
        /* A report can arrive after the frame-start input sample. Poll with
         * a fresh time so that earlier sample is not treated as clock reversal. */
        x4_auth_poll_rumble(auth,sceKernelGetProcessTime(),&desired);
#if X4_STREAM_PROFILE
        if((!controller.data.connected&&rumble_was_connected)||controller.handle<0)rumble_state.handle=-1;
#else
        if((!controller.data.connected&&rumble_was_connected)||controller.handle<0)rumble_handle=-1;
#endif
        rumble_was_connected=controller.data.connected;
        if(!game_input||!session.rtc_connected||!controller.data.connected)
            desired.lgMotor=desired.smMotor=0;
#if X4_STREAM_PROFILE
        if(interaction_main.modes&X4_INTERACTION_RUMBLE_OFF)desired.lgMotor=desired.smMotor=0;
        interaction_rumble_step(&rumble_state,controller.handle,
            (OrbisPadVibeParam){desired.lgMotor,desired.smMotor},sceKernelGetProcessTime());
#else
        if(controller.handle>=0&&(rumble_handle!=controller.handle||
            rumble_applied.lgMotor!=desired.lgMotor||rumble_applied.smMotor!=desired.smMotor)) {
            OrbisPadVibeParam next={desired.lgMotor,desired.smMotor};
            int vibration_rc=scePadSetVibration(controller.handle,&next);
            ++rumble_pad_calls;
            if(vibration_rc>=0) ++rumble_pad_ok; else ++rumble_pad_errors;
            if(vibration_rc>=0) { rumble_applied=next; rumble_handle=controller.handle; }
        }
#endif
#endif
        if (!closing) {
            uint32_t ui_pressed = controller.pressed;
            if (game_input) ui_pressed &= ~ORBIS_PAD_BUTTON_OPTIONS;
            /* CIRCLE finishes keyboard editing before account navigation.
             * OPTIONS still follows the global cleanup/exit path. */
            if (screen.page == 5 && catalog_browser.editing)
                ui_pressed &= ~ORBIS_PAD_BUTTON_CIRCLE;
#if X4_STREAM_PROFILE && !X4_PUBLIC
            /* R3 belongs to local video settings only while the catalog is
             * idle. Consume the opening/closing frame, including any X edge. */
            if (screen.page == 5) {
                unsigned actions=0;
                if(controller.pressed&ORBIS_PAD_BUTTON_R3) actions|=X4_SETTINGS_OPEN;
                if(controller.pressed&ORBIS_PAD_BUTTON_UP) actions|=X4_SETTINGS_UP;
                if(controller.pressed&ORBIS_PAD_BUTTON_DOWN) actions|=X4_SETTINGS_DOWN;
                if(controller.pressed&ORBIS_PAD_BUTTON_LEFT) actions|=X4_SETTINGS_LEFT;
                if(controller.pressed&ORBIS_PAD_BUTTON_RIGHT) actions|=X4_SETTINGS_RIGHT;
                if(controller.pressed&ORBIS_PAD_BUTTON_CROSS) actions|=X4_SETTINGS_CONFIRM;
                if(controller.pressed&ORBIS_PAD_BUTTON_CIRCLE) actions|=X4_SETTINGS_CANCEL;
                bool can_edit=!catalog_browser.editing&&!x4_auth_busy(auth)&&pending_auth<0&&
                    !pending_launch&&!session_back&&!live_retained;
                stream_settings_consumed=x4_stream_settings_ui_handle(&stream_settings_ui,actions,can_edit);
                if(stream_settings_consumed) ui_pressed&=ORBIS_PAD_BUTTON_OPTIONS;
            } else stream_settings_ui.open=false;
#elif X4_STREAM_PROFILE
            (void)stream_settings_ui;
#endif
            if (game_input && x4_touch_menu_is_open(&touch_menu)) {
                X4TouchMenuAction menu_action = x4_touch_menu_update(&touch_menu, controller.pressed, controller.data.buttons, controller_read_at);
                switch (menu_action) {
                case X4_TOUCH_MENU_ACTION_BACK_TO_CATALOG:
                    trace_inactive(live);
                    x4_auth_cancel(auth);
                    session_back = 1;
                    break;
                case X4_TOUCH_MENU_ACTION_TOGGLE_MUTE:
                    live_muted = !live_muted;
                    if (live) x4_live_media_set_muted(live, live_muted != 0);
                    break;
                case X4_TOUCH_MENU_ACTION_XBOX_BUTTON:
                    x4_touch_menu_trigger_nexus(&touch_menu, controller_read_at);
                    break;
#if !X4_PUBLIC
                case X4_TOUCH_MENU_ACTION_MARK_TEST: {
                    X4Trace *live_trace = live ? x4_live_media_trace(live) : NULL;
                    diagnostic_stable = !diagnostic_stable;
                    unsigned epoch = diagnostic_stable ? X4_MON_EPOCH_STABLE : X4_MON_EPOCH_TRANSITION;
                    x4_trace_monitor_epoch(live_trace, X4_MON_ACTOR_SESSION, epoch);
                    x4_trace_record(live_trace, X4_TRACE_SESSION_PHASE, 0, epoch, ++diagnostic_mark);
                    printf("XCloud4: diagnostic phase=%s mark=%llu\n",
                        diagnostic_stable ? "stable" : "transition", (unsigned long long)diagnostic_mark);
                    break;
                }
                case X4_TOUCH_MENU_ACTION_EXIT_APP:
                    screen.exit_requested = 1;
                    break;
#endif
                default:
                    break;
                }
                ui_pressed = 0;
            }
            x4_screen_update(&screen, ui_pressed, controller.data.buttons);
        }
        if ((previous_page == 4 || previous_page == 5 || previous_page == 6) && screen.page != previous_page) {
            x4_catalog_assets_suspend(catalog_assets);
            pending_launch = 0;
            pending_auth = -1;
            trace_inactive(live);
            x4_auth_cancel(auth);
        }
        if (screen.exit_requested) {
            x4_catalog_assets_suspend(catalog_assets);
            pending_auth = -1;
            pending_launch = 0;
            trace_inactive(live);
            if (x4_auth_busy(auth) || (!x4_catalog_assets_idle(catalog_assets) &&
                x4_catalog_assets_fault(catalog_assets) != X4_ASSET_FAULT_CLEANUP)) {
                x4_auth_cancel(auth);
                closing = 1;
            } else {
                closing = 0;
                screen.exit_error = x4_exit_prepare();
                if (screen.exit_error < 0) {
                    printf("XCloud4: preparar salida fallo 0x%08x\n", (unsigned)screen.exit_error);
                    screen.exit_requested = 0;
                } else break;
            }
        }
        if (!closing && screen.page == 4 && previous_page == 4 && !x4_auth_busy(auth) && pending_auth < 0) {
            if (controller.pressed & ORBIS_PAD_BUTTON_CROSS) pending_auth = X4_AUTH_SIGN_IN;
#if !X4_PUBLIC
            else if (controller.pressed & ORBIS_PAD_BUTTON_SQUARE) pending_auth = X4_AUTH_CHECK_CONNECTION;
#endif
            else if (controller.pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
                x4_catalog_assets_suspend(catalog_assets);
                pending_auth = -1;
                if (!x4_auth_busy(auth)) x4_auth_forget(auth);
                x4_recent_init(&recent);
            }
            else if ((controller.pressed & ORBIS_PAD_BUTTON_R1) &&
                     account.state == X4_AUTH_AUTHORIZED && !x4_auth_busy(auth)) {
                screen.page = 5;
                if (catalog.state != X4_CATALOG_READY) {
                    catalog_selected = 0;
                    pending_auth = X4_AUTH_XBOX_CATALOG;
                } else pending_auth = X4_AUTH_XBOX_RECENTS;
            }
        }
        /* Public Store work must release native HTTP/Net before auth uses
         * it. Cancellation is nonblocking; keep drawing/navigation alive.
         * No credential or arbitrary URL crosses this coordinator. */
        if (pending_auth >= 0) {
            x4_catalog_assets_suspend(catalog_assets);
            if (x4_catalog_assets_fault(catalog_assets) == X4_ASSET_FAULT_CLEANUP) {
                pending_auth = -1;
                pending_launch = 0;
                snprintf(ui_notice, sizeof(ui_notice), "%s", x4_tr(X4_TR_NOTICE_COVER_CLEANUP));
            } else if (!x4_auth_busy(auth) && x4_catalog_assets_idle(catalog_assets)) {
                enum X4AuthAction action = (enum X4AuthAction)pending_auth;
                pending_auth = -1;
                rc = x4_auth_start(auth, action);
                if (rc) snprintf(ui_notice, sizeof(ui_notice), x4_tr(X4_TR_NOTICE_QUERY_ERR_FMT), rc);
                else ui_notice[0] = 0;
            }
        }
        x4_trace_monitor_state(live ? x4_live_media_trace(live) : NULL,
            X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAIN_STATUS, 0);
#if X4_STREAM_PROFILE
        interaction_phase(X4_MON_PHASE_MAIN_STATUS,sceKernelGetProcessTime());
#endif
        x4_auth_snapshot(auth, &account);
        if (psbox_ui) psbox_ui_set_auth(&account);
        bool catalog_copied = x4_auth_catalog_snapshot_cached(auth, &catalog, &catalog_cursor) != 0;
        if (catalog_copied) {
            ++ui_generation;
            x4_catalog_presentation_set_generation(&presentation, ui_generation);
            x4_catalog_assets_set_generation(catalog_assets, ui_generation);
            x4_catalog_presentation_apply_names(&presentation, &catalog);
            x4_catalog_browser_rebuild(&catalog_browser, &catalog);
            x4_recent_refresh_catalog(&recent, &catalog);
            if (catalog.state != X4_CATALOG_READY) catalog_browser.editing = false;
            if (psbox_ui) psbox_ui_set_catalog(&catalog);
        }
        bool recent_copied = x4_auth_recent_snapshot_cached(auth, &recent_snapshot, &recent_cursor) != 0;
        if (recent_copied) {
            x4_recent_begin(&recent, recent_snapshot.account_epoch, recent_snapshot.request_generation);
            if (recent_snapshot.state == X4_CATALOG_READY || recent_snapshot.state == X4_CATALOG_ERROR ||
                recent_snapshot.state == X4_CATALOG_CANCELLED)
                x4_recent_publish(&recent, recent_snapshot.account_epoch, recent_snapshot.request_generation,
                    recent_snapshot.title_ids, recent_snapshot.count,
                    recent_snapshot.state == X4_CATALOG_READY ? 0 :
                        recent_snapshot.error ? recent_snapshot.error : X4_AUTH_E_RESPONSE,
                    recent_snapshot.http_status, &catalog);
            else if (recent_snapshot.state == X4_CATALOG_IDLE) x4_recent_init(&recent);
            x4_catalog_browser_sync_recents(&catalog_browser, &recent);
            if (psbox_ui) psbox_ui_set_recent(&recent_snapshot);
        }
        x4_auth_session_snapshot(auth, &session);
        if (psbox_ui) psbox_ui_set_session(&session);
        if (psbox_ui && !closing && screen.page != 6) {
            char want_title[96] = {0};
            float lx = (controller.data.leftStick.x - 128) / 128.f;
            float ly = (controller.data.leftStick.y - 128) / 128.f;
            float rx = (controller.data.rightStick.x - 128) / 128.f;
            float ry = (controller.data.rightStick.y - 128) / 128.f;
            float l2 = controller.data.analogButtons.l2 / 255.f;
            float r2 = controller.data.analogButtons.r2 / 255.f;
            int ui_act = psbox_ui_handle(controller.pressed, controller.data.buttons, lx, ly, rx, ry, l2, r2,
                                         sceKernelGetProcessTime() / 1000ull, want_title, sizeof want_title);
            if (ui_act < 0 && pending_launch) {
                pending_launch = 0;
            } else if (ui_act > 0 && want_title[0] && !x4_auth_busy(auth) && pending_auth < 0 &&
                       !pending_launch) {
                for (unsigned i = 0; i < catalog.count; ++i) {
                    if (strcmp(catalog.titles[i].id, want_title) == 0) {
                        catalog_selected = i;
                        pending_launch = 1;
                        screen.page = 5;
                        break;
                    }
                }
            }
        }
        bool metadata_changed = false;
        if (screen.page == 5 && !x4_auth_busy(auth)) {
            X4ProductMetadata completed[X4_CATALOG_ASSET_SLOTS];
            uint64_t revision = 0;
            size_t n = x4_catalog_assets_copy_metadata(catalog_assets, completed, X4_CATALOG_ASSET_SLOTS, &revision);
            if (revision != metadata_revision) {
                metadata_revision = revision;
                x4_catalog_presentation_ingest(&presentation, ui_generation, revision, completed, n);
                metadata_changed = x4_catalog_presentation_apply_names(&presentation, &catalog);
                if (psbox_ui && n) psbox_ui_ingest_metadata(completed, n);
                if (metadata_changed) {
                    x4_catalog_browser_rebuild(&catalog_browser, &catalog);
                    x4_recent_refresh_catalog(&recent, &catalog);
                }
            }
        }
        x4_auth_session_snapshot(auth, &session);
        if (!closing && screen.page == 6 && previous_page == 6) {
            if ((controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) && !game_input) {
                trace_inactive(live);
                x4_auth_cancel(auth);
                session_back = 1;
            }
            /* Leave from PSBox in-stream menu is handled after draw (return 2). */
            /* Keep the session page visible until remote cleanup finishes. */
            if (session_back && !x4_auth_busy(auth)) {
                session_back = 0;
                screen.page = 5;
                pending_auth = X4_AUTH_XBOX_RECENTS;
            }
        }
        if (!closing && screen.page == 5 && previous_page == 5) {
            uint32_t pressed = controller.pressed;
            bool queued_launch_ready = false;
            if (pending_launch) {
                const X4CatalogTitle *chosen = x4_catalog_browser_selected_title(&catalog_browser, &catalog, &recent);
                if (x4_catalog_assets_fault(catalog_assets) == X4_ASSET_FAULT_CLEANUP) {
                    pending_launch = 0;
                    snprintf(ui_notice, sizeof(ui_notice), "%s", x4_tr(X4_TR_NOTICE_COVER_CLEANUP));
                } else if (pressed || catalog_copied || recent_copied || pending_generation != ui_generation ||
                    !chosen || strcmp(chosen->id, pending_title_id)) pending_launch = 0;
                else if (!metadata_changed && !x4_auth_busy(auth) && x4_catalog_assets_idle(catalog_assets)) {
                    queued_launch_ready = true;
                    pending_launch = 0;
                }
            }
#if X4_STREAM_PROFILE
            if(stream_settings_consumed) {
                pending_launch=0;
            } else
#endif
            if (catalog_browser.editing) {
                if (pressed & (ORBIS_PAD_BUTTON_R2 | ORBIS_PAD_BUTTON_CIRCLE)) {
                    catalog_browser.editing = false;
                } else {
                    if (pressed & ORBIS_PAD_BUTTON_UP)
                        x4_catalog_browser_keyboard_move(&catalog_browser, 0, -1);
                    if (pressed & ORBIS_PAD_BUTTON_DOWN)
                        x4_catalog_browser_keyboard_move(&catalog_browser, 0, 1);
                    if (pressed & ORBIS_PAD_BUTTON_LEFT)
                        x4_catalog_browser_keyboard_move(&catalog_browser, -1, 0);
                    if (pressed & ORBIS_PAD_BUTTON_RIGHT)
                        x4_catalog_browser_keyboard_move(&catalog_browser, 1, 0);
                    if (pressed & ORBIS_PAD_BUTTON_TRIANGLE)
                        x4_catalog_browser_query_clear(&catalog_browser);
                    else if (pressed & ORBIS_PAD_BUTTON_SQUARE)
                        x4_catalog_browser_query_delete(&catalog_browser);
                    else if (pressed & ORBIS_PAD_BUTTON_R1)
                        x4_catalog_browser_query_append(&catalog_browser, ' ');
                    else if (pressed & ORBIS_PAD_BUTTON_CROSS)
                        x4_catalog_browser_query_append(&catalog_browser,
                            x4_catalog_browser_keyboard_char(&catalog_browser));
                }
                if (catalog_browser.criteria_dirty)
                    x4_catalog_browser_rebuild(&catalog_browser, &catalog);
            } else if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) &&
                       catalog.state == X4_CATALOG_READY && !x4_auth_busy(auth)) {
                catalog_browser.editing = true;
            } else if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && !x4_auth_busy(auth)) {
                x4_catalog_assets_retry(catalog_assets);
                pending_auth = catalog_browser.section == X4_CATALOG_SECTION_RECENT && recent.state == X4_RECENT_ERROR ?
                    X4_AUTH_XBOX_RECENTS : X4_AUTH_XBOX_CATALOG;
            } else {
                if (pressed & ORBIS_PAD_BUTTON_L2)
                    x4_catalog_browser_cycle_filter(&catalog_browser, -1);
                if (pressed & ORBIS_PAD_BUTTON_R2)
                    x4_catalog_browser_cycle_filter(&catalog_browser, 1);
                if (catalog_browser.criteria_dirty)
                    x4_catalog_browser_rebuild(&catalog_browser, &catalog);
                if (pressed & ORBIS_PAD_BUTTON_UP)
                    x4_catalog_browser_nav(&catalog_browser, &catalog, &recent, 0, -1);
                if (pressed & ORBIS_PAD_BUTTON_DOWN)
                    x4_catalog_browser_nav(&catalog_browser, &catalog, &recent, 0, 1);
                if (pressed & ORBIS_PAD_BUTTON_LEFT)
                    x4_catalog_browser_nav(&catalog_browser, &catalog, &recent, -1, 0);
                if (pressed & ORBIS_PAD_BUTTON_RIGHT)
                    x4_catalog_browser_nav(&catalog_browser, &catalog, &recent, 1, 0);
                if (pressed & ORBIS_PAD_BUTTON_L1)
                    x4_catalog_browser_page_move(&catalog_browser, &catalog, &recent, -1);
                if (pressed & ORBIS_PAD_BUTTON_R1)
                    x4_catalog_browser_page_move(&catalog_browser, &catalog, &recent, 1);
                unsigned source_index;
                /* A fresh response or a navigation/filter press can select a
                 * row that has not been shown yet. Require a later X edge. */
                bool selection_changing = (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN |
                    ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT |
                    ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_R2)) != 0;
                bool launch_edge = queued_launch_ready || (pressed & ORBIS_PAD_BUTTON_CROSS);
                if (launch_edge && !catalog_copied && !recent_copied && !metadata_changed && !selection_changing &&
                    pending_auth < 0 && !x4_auth_busy(auth) &&
                    x4_catalog_browser_resolved_index(&catalog_browser, &catalog, &recent, &source_index)) {
                    x4_catalog_assets_suspend(catalog_assets);
                    if (x4_catalog_assets_fault(catalog_assets) == X4_ASSET_FAULT_CLEANUP) {
                        snprintf(ui_notice, sizeof(ui_notice), "%s", x4_tr(X4_TR_NOTICE_COVER_CLEANUP));
                    } else if (!x4_catalog_assets_idle(catalog_assets)) {
                        pending_launch = 1;
                        pending_generation = ui_generation;
                        snprintf(pending_title_id, sizeof(pending_title_id), "%s", catalog.titles[source_index].id);
                    } else {
                    /* The visible filtered ordinal is never a session index. */
                    catalog_selected = source_index;
                    live_presented = false;
                    live_error = 0;
#if X4_STREAM_PROFILE
                    video_start_error = false;
                    stream_received_until=0;
                    /* One immutable main-thread copy binds media and /play
                     * to the same confirmed menu selection for this session. */
                    X4StreamRequestConfig request_cfg=x4_stream_settings_get();
#endif
                    if (live) {
                        trace_inactive(live);
                        live_error = x4_auth_set_media_callback(auth, NULL, NULL, NULL);
#if X4_STREAM_PROFILE && X4_RUMBLE
                        if(!live_error)interaction_rumble_stop(&rumble_state,controller.handle);
#endif
                        if (!live_error) live_error = x4_live_media_close(live);
                        if (!live_error) live = NULL;
                    }
                    if (!live_error) {
                        live_retained = 0;
#if X4_STREAM_PROFILE
                        interaction_main_begin(request_cfg.interaction_modes,sceKernelGetProcessTime());
                        x4_rtc_input_metrics_reset();
                        live = x4_live_media_create_request(&live_error,&request_cfg);
#else
                        live = x4_live_media_create(&live_error);
#endif
                        diagnostic_stable = false;
                        diagnostic_mark = 0;
                        if (live) {
                            live_error = x4_live_media_set_payload_type(live, X4_LIVE_KIND_VIDEO, 102);
                            if (!live_error) live_error = x4_live_media_set_payload_type(live, X4_LIVE_KIND_AUDIO, 111);
                            if (!live_error) live_error = x4_live_media_start(live);
                        }
                    }
                    memset(&live_status, 0, sizeof(live_status));
                    live_muted = 0;
                    rc = live_error;
                    if (!rc) rc = x4_auth_set_media_callback(auth, x4_live_media_receive, live, x4_live_media_trace(live));
                    if (!rc) rc = x4_auth_set_grace_inbox(auth,x4_live_media_grace_inbox(live));
#if X4_STREAM_PROFILE
                    if (!rc) rc = x4_auth_set_stream_startup(auth,x4_live_media_stream_startup(live));
                    if (!rc) rc = x4_auth_set_keyframe_limiter(auth,x4_live_media_keyframe_limiter(live));
#endif
                    if (!rc) { game_controls = 0;
#if X4_STREAM_PROFILE
                        rc = x4_auth_start_session_request(auth,catalog_selected,&request_cfg);
#else
                        rc = x4_auth_start_session(auth, catalog_selected);
#endif
                    }
                    if (rc && !live_error) live_error = rc;
                    x4_auth_session_snapshot(auth, &session);
                    if (rc == 0 || session.state == X4_SESSION_ERROR || live_error) {
                        session_back = 0;
                        screen.page = 6;
                    }
                    if (rc == X4_AUTH_E_SIGNED_OUT) screen.page = 4;
                    if (rc) printf("XCloud4: solicitar sesion fallo 0x%08x\n", (unsigned)rc);
                    }
                }
            }
        }
        if (!closing && screen.page == 5 && catalog.state == X4_CATALOG_READY && !x4_auth_busy(auth) &&
            pending_auth < 0 && !pending_launch) {
            char wanted[X4_CATALOG_ASSET_SLOTS][X4_PRODUCT_ID_CAP];
            const char *ids[X4_CATALOG_ASSET_SLOTS];
            size_t wanted_count = x4_catalog_presentation_demand(&presentation, &catalog_browser, &catalog, &recent, wanted);
            for (size_t i = 0; i < wanted_count; ++i) ids[i] = wanted[i];
            x4_catalog_assets_request(catalog_assets, ids, wanted_count);
            x4_catalog_assets_resume(catalog_assets);
        } else x4_catalog_assets_suspend(catalog_assets);
        if (live && !live_retained) {
            x4_live_media_tick(live);
            x4_live_media_snapshot(live, &live_status);
            if (x4_auth_busy(auth)) {
                if (x4_live_media_take_keyframe_request(live)) x4_auth_request_keyframe(auth);
                if (live_status.video_error) {
#if X4_STREAM_PROFILE
                    if(!live_error)live_error=live_status.video_error;
                    stream_halt_video_error(live, &live_status, live_error, &video_start_error);
#else
                    trace_inactive(live); x4_auth_cancel(auth);
#endif
                }
                if (screen.page == 6 && (controller.pressed & ORBIS_PAD_BUTTON_SQUARE) && !game_input) {
                    live_muted = !live_muted;
                    x4_live_media_set_muted(live, live_muted != 0);
                }
            } else {
                /* The worker closes RTC before publishing completion. */
                trace_inactive(live);
                int stop_rc = x4_auth_set_media_callback(auth, NULL, NULL, NULL);
#if X4_STREAM_PROFILE && X4_RUMBLE
                if(!stop_rc)interaction_rumble_stop(&rumble_state,controller.handle);
#endif
                if (!stop_rc) stop_rc = x4_live_media_close(live);
                if (!stop_rc) live = NULL;
                else { live_error = stop_rc; live_retained = 1; printf("XCloud4: medios retienen recursos, cierre 0x%08x\n", (unsigned)stop_rc); }
#if X4_STREAM_PROFILE
                /* Successful full close is also proof that stream VideoOut
                 * released its ownership before the context was freed. */
                if (!stop_rc) live_status.video_display_detached = true;
#endif
            }
        }
#if X4_STREAM_PROFILE
        /* The stream close joins CPU readers and closes its VideoOut before
         * UI ownership can be restored. Never reopen over retained resources. */
        if(stream_display_active && live_status.video_display_detached){
            rc=x4_display_open(&display);
            if(rc<0){printf("XCloud4: reabrir pantalla UI fallo 0x%08x; no se puede dibujar el aviso, cierre seguro\n",(unsigned)rc);break;}
            stream_display_active=false;
        }
#endif
        if (screen.page != 3 && media_active) {
            x4_audio_stop(&audio);
            x4_video_stop(&video);
            media_active = 0;
        }
        if (screen.page == 3) {
            if (!media_active || (controller.pressed & ORBIS_PAD_BUTTON_CROSS)) {
                /* Present the menu before entering potentially slow system APIs. */
                x4_screen_draw(&screen, &controller, x4_display_pixels(&display));
                x4_media_draw(&video, &audio, x4_display_pixels(&display));
                rc = x4_display_present(&display);
                if (rc < 0) break;
                x4_audio_stop(&audio);
                x4_video_start(&video);
                x4_audio_start(&audio);
                media_active = 1;
            }
            if (controller.pressed & ORBIS_PAD_BUTTON_SQUARE)
                atomic_store(&audio.muted, !atomic_load(&audio.muted));
            x4_video_tick(&video);
        }
        /* Keep scanout unchanged between completed pictures. A real UI
         * transition or periodic status refresh still redraws the entire
         * inactive framebuffer, so two-buffer reuse never mixes old pixels. */
        bool live_visible = screen.page == 6 && live && x4_auth_busy(auth) && !closing && !session_back &&
            !live_error && !live_status.video_error && session.state != X4_SESSION_STOPPING &&
            live_status.video_ready;
        X4Trace *live_trace = live ? x4_live_media_trace(live) : NULL;
        if (!closing && screen.page == 6 && live && x4_auth_busy(auth)) {
            if (live_status.video_ready && !video_banner_until_us) {
                video_banner_until_us = controller_read_at + 6000000ULL;
            }
        } else {
            video_banner_until_us = 0;
        }
        bool show_banner = (video_banner_until_us != 0 && controller_read_at < video_banner_until_us);
        bool trace_active = live_visible && !live_retained && trace_session_active(session.state);
        x4_trace_set_active(live_trace, trace_active);
        /* Runs before the idle continue too: a repeated/status-only refresh
         * must not conceal a gap in actual NEW live completions. */
        x4_trace_poll(live_trace);
#if X4_STREAM_PROFILE
        if(live_visible&&!stream_display_active){
            rc=x4_display_close_checked(&display);
            if(rc){
                live_error=rc;trace_inactive(live);x4_auth_cancel(auth);
                printf("XCloud4: cerrar pantalla UI fallo 0x%08x; recursos conservados, no se puede dibujar el aviso, cierre seguro\n",(unsigned)rc);
                break;
            }
            rc=x4_live_media_stream_display_open(live);
            if(rc){
                live_error=rc;
                printf("XCloud4: transicion VideoOut fallo 0x%08x\n",(unsigned)rc);
                /* UI may have relinquished ownership even if the new open
                 * failed. Only confirmed video detach permits UI reopen. */
                stream_display_active=true;
                stream_halt_video_error(live, &live_status, live_error, &video_start_error);
            } else stream_display_active=true;
        }
        if(stream_display_active){
            if(live&&!live_retained&&!live_error){
                if(!stream_received_until&&live_status.video_color_ready)
                    stream_received_until=input_at+6000000;
                StreamOverlayContext overlay={&live_status,&session,live_muted,&touch_menu,show_banner,
                    live_status.video_color_ready&&input_at<stream_received_until};
#if X4_STREAM_PROFILE
                interaction_phase(X4_MON_PHASE_DRAW,sceKernelGetProcessTime());
#endif
                int stream_rc=x4_live_media_stream_present(live,stream_overlay,&overlay);
                if(stream_rc<0&&!live_error){
                    live_error=stream_rc;
                    printf("XCloud4: presentacion stream fallo 0x%08x\n",(unsigned)stream_rc);
                    stream_halt_video_error(live, &live_status, live_error, &video_start_error);
                }
            }
            /* Same-owner state needs no new lock. Flip completion has no wake
             * event here: observe it and read the pad at 1 ms while anything
             * is in flight; only fully idle presentation uses 5 ms. */
            unsigned main_in_flight=x4_live_media_stream_main_state(live);
            main_submit_done();
            interaction_phase(X4_MON_PHASE_FLIP_WAIT,sceKernelGetProcessTime());
            int wait_rc=x4_live_media_stream_wait(live,main_in_flight?1000u:5000u);
            if(wait_rc<0){
                if(!live_error){
                    live_error=wait_rc;
                    printf("XCloud4: espera principal stream fallo 0x%08x\n",(unsigned)wait_rc);
                    stream_halt_video_error(live,&live_status,live_error,&video_start_error);
                }
                /* A broken wait endpoint cannot service an error screen.
                 * Enter the existing safe close; never poll a failed wait. */
                trace_inactive(live);x4_auth_cancel(auth);break;
            }
            continue;
        }
#endif
        int next_overlay_key[11] = {screen.page, live_muted, live_status.audio_error,
            live_status.audio_playing, session.input_error, session.input_ready,
            session.rtc_connected, session.state, screen.exit_error, controller.data.connected,
            (touch_menu.is_open ? (10 + touch_menu.selected_index) : 0) + (show_banner ? 100 : 0)};
        uint64_t draw_at = sceKernelGetProcessTime();
        bool overlay_changed = memcmp(overlay_key, next_overlay_key, sizeof(overlay_key)) != 0;
        if (live_visible && live_presented && !overlay_changed && draw_at - overlay_at < 500000 &&
            !x4_live_media_has_new_picture(live)) {
            ++idle_present_skips;
            x4_trace_monitor_state(live_trace, X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAIN_IDLE, 0);
#if X4_STREAM_PROFILE
            interaction_phase(X4_MON_PHASE_MAIN_IDLE,sceKernelGetProcessTime());
#endif
            main_submit_done();
            sceKernelUsleep(2000);
            continue;
        }
        x4_trace_monitor_state(live_trace, X4_MON_ACTOR_MAIN, X4_MON_PHASE_DRAW, 0);
#if X4_STREAM_PROFILE
        interaction_phase(X4_MON_PHASE_DRAW,sceKernelGetProcessTime());
#endif
        bool drew_live = live_visible && x4_live_media_draw(live, x4_display_pixels(&display));
        uint64_t drawn_generation = 0, drawn_output = 0;
        bool drawn_fresh = false;
        if (drew_live && !live_retained)
            x4_live_media_drawn(live, &drawn_generation, &drawn_output, &drawn_fresh);
        /* Media draw records the independent native-output identity. Display
         * uses the actual drawn publication tag, never a later snapshot. */
        (void)drawn_output;
        if (psbox_ui && !drew_live && screen.page != 6 && !closing) {
            psbox_ui_draw(x4_display_pixels(&display), sceKernelGetProcessTime() / 1000ull);
        } else {
        if (!drew_live && screen.page != 5) x4_screen_draw(&screen, &controller, x4_display_pixels(&display));
        if (screen.page == 3) x4_media_draw(&video, &audio, x4_display_pixels(&display));
        if (screen.page == 6) {
            if (drew_live) {
                x4_live_overlay(&live_status, &session, live_muted, &touch_menu, show_banner, x4_display_pixels(&display));
                if (psbox_ui) {
                    int menu_rc = psbox_ui_stream_menu(x4_display_pixels(&display), controller.pressed,
                                                       controller.data.buttons,
                                                       sceKernelGetProcessTime() / 1000ull,
                                                       (int)live_status.width, (int)live_status.height, 0, 0);
                    if (menu_rc == 2 && !session_back) {
                        trace_inactive(live);
                        x4_auth_cancel(auth);
                        session_back = 1;
                    }
                }
            } else {
                X4SessionSnapshot shown = session;
#if X4_STREAM_PROFILE
                if(!live_error && live && !closing && !session_back &&
                    x4_auth_busy(auth) && x4_live_media_waiting_initial_resolution(live))
                    snprintf(shown.stage,sizeof(shown.stage),"Esperando 1080p de Xbox…");
#endif
#if X4_STREAM_PROFILE
                if (live_error && (!x4_auth_busy(auth) || video_start_error)) {
#else
                if (live_error && !x4_auth_busy(auth)) {
#endif
                    shown.state = X4_SESSION_ERROR;
                    shown.error = live_error;
                    shown.http_status = 0;
                    shown.ready_seen = shown.connection_authorized = 0;
                    shown.rtc_connected = 0;
                    snprintf(shown.stage, sizeof(shown.stage), "no se pudieron preparar o cerrar los medios nativos");
#if X4_STREAM_PROFILE
                    if(video_start_error)(void)x4_stream_video_error_caption(shown.stage,sizeof(shown.stage),live_error);
#endif
                    if (catalog_selected < catalog.count)
                        snprintf(shown.title_name, sizeof(shown.title_name), "%s", catalog.titles[catalog_selected].name);
                }
                x4_session_draw(&shown, x4_auth_busy(auth), closing, x4_display_pixels(&display));
                x4_live_status_draw(&live_status, live_error, x4_display_pixels(&display));
            }
        }
        else if (screen.page == 4 || closing)
            x4_auth_draw(&account, x4_auth_busy(auth), closing, x4_display_pixels(&display));
        else if (screen.page == 5) {
            X4CatalogGuiStatus gui = {controller.data.connected, x4_auth_busy(auth),
                pending_launch || pending_auth >= 0, ui_notice};
            x4_catalog_gui_draw(&catalog, &catalog_browser, &recent, catalog_assets, &gui, x4_display_pixels(&display));
#if X4_STREAM_PROFILE && !X4_PUBLIC
            x4_stream_settings_ui_draw(&stream_settings_ui,x4_display_pixels(&display));
#endif
        }
        }
        x4_exit_error_draw(screen.exit_error, x4_display_pixels(&display));
        uint64_t present_begin = sceKernelGetProcessTime();
        rc = drew_live && trace_active ?
            x4_display_present_trace(&display, live_trace, drawn_generation, drawn_fresh) :
            x4_display_present(&display);
        if (rc == 0 && drew_live && live && !live_retained) {
            x4_live_media_note_present(live, sceKernelGetProcessTime() - present_begin);
            memcpy(overlay_key, next_overlay_key, sizeof(overlay_key));
            overlay_at = draw_at;
            live_presented = true;
        } else live_presented = false;
        if (rc < 0) {
            printf("XCloud4: error de presentacion: 0x%08x\n", (unsigned)rc);
            break;
        }
    }
    /* A display failure can break the loop early: cancel then join before
     * cleaning the UI. Ordinary OPTIONS keeps rendering while it cancels. */
    trace_inactive(live);
    x4_catalog_assets_suspend(catalog_assets);
    x4_catalog_assets_destroy(catalog_assets);
    catalog_assets = NULL;
    x4_auth_cancel(auth);
#if X4_RUMBLE
#if X4_STREAM_PROFILE
    interaction_rumble_stop(&rumble_state,controller.handle);
    printf("XCloud4: vibracion Pad calls=%llu success=%llu errors=%llu (resultado local)\n",
        (unsigned long long)interaction_main.calls,(unsigned long long)interaction_main.ok,
        (unsigned long long)interaction_main.errors);
#else
    if(controller.handle>=0) {
        OrbisPadVibeParam zero={0};
        (void)scePadSetVibration(controller.handle,&zero);
        rumble_applied=zero; rumble_handle=controller.handle;
    }
    printf("XCloud4: vibracion Pad calls=%llu success=%llu errors=%llu (resultado local)\n",
        (unsigned long long)rumble_pad_calls,(unsigned long long)rumble_pad_ok,
        (unsigned long long)rumble_pad_errors);
#endif
#endif
    while (x4_auth_busy(auth)) sceKernelUsleep(100000);
    if (live) {
        int live_rc = x4_auth_set_media_callback(auth, NULL, NULL, NULL);
        if (!live_rc) live_rc = x4_live_media_close(live);
        if (!live_rc) live = NULL;
        else live_error = live_rc;
    }
    printf("XCloud4: cerrar acceso Microsoft\n");
    int auth_rc = x4_auth_close(auth);
    if (auth_rc >= 0) {
        auth = NULL;
        catalog_cursor = (X4AuthCatalogCursor){0};
        recent_cursor = (X4AuthRecentCursor){0};
        x4_recent_init(&recent);
        x4_catalog_browser_init(&catalog_browser);
    }
    printf("XCloud4: cerrar audio\n");
    x4_audio_stop(&audio);
    printf("XCloud4: cerrar decoder\n");
    x4_video_stop(&video);
    printf("XCloud4: cerrar control\n");
    x4_controller_close(&controller);
    printf("XCloud4: cerrar pantalla\n");
#if X4_STREAM_PROFILE
    int display_close_rc=x4_display_close_checked(&display);
    if(display_close_rc<0)live_error=display_close_rc;
#else
    x4_display_close(&display);
#endif
    x4_font_close();
    printf("XCloud4: recursos cerrados\n");
    x4_release_check_stop();
    int exit_rc = x4_exit_prepare();
    if (exit_rc >= 0) exit_rc = x4_exit_request();
    if (exit_rc >= 0) {
        /* LoadExec is asynchronous; permit recovery if the shell never exits. */
        for (unsigned wait = 0; wait < 100; ++wait) sceKernelUsleep(100000);
        exit_rc = -10;
        printf("XCloud4: tiempo de salida agotado\n");
    }
#if X4_STREAM_PROFILE
    if(live||display.handle>=0||display.memory||display.allocated){
        /* An unsuccessful shell exit cannot authorize a second owner over
         * unresolved resources. Keep all failed-close contexts alive. */
        printf("XCloud4: salida no completada con recursos retenidos; no se reabre VideoOut\n");
        for(;;)sceKernelUsleep(100000);
    }
#endif
    /* A rejected/timed-out request reopens the interface and permits a retry. */
    screen.exit_error = exit_rc;
    screen.exit_requested = 0;
    screen.page = 0;
    media_active = 0;
    live_presented = false;
    overlay_at = input_previous = input_report_at = 0;
    input_intervals = input_interval_us = input_interval_max_us = idle_present_skips = 0;
    closing = 0;
    session_back = 0;
    game_controls = 0;
    if (!auth) auth = x4_auth_create();
    sceKernelUsleep(250000);
    goto reopen_interface;
#endif
}
