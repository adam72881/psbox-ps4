/* SPDX-License-Identifier: GPL-3.0-only
 * C facade: PSBox/PSCloud UI over XCloud4 auth, catalog and stream hooks.
 * No PSBox WebRTC / setsockopt wrappers — transport stays XCloud4. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct X4AuthSnapshot X4AuthSnapshot;
typedef struct X4CatalogSnapshot X4CatalogSnapshot;
typedef struct X4RecentSnapshot X4RecentSnapshot;
typedef struct X4SessionSnapshot X4SessionSnapshot;
typedef struct X4ProductMetadata X4ProductMetadata;

/* 0 ok, <0 fatal init failure (fonts/assets). */
int psbox_ui_init(void);
void psbox_ui_shutdown(void);

/* Feed XCloud4 snapshots (no tokens). Call each frame before handle/draw. */
void psbox_ui_set_auth(const X4AuthSnapshot *auth);
void psbox_ui_set_catalog(const X4CatalogSnapshot *catalog);
void psbox_ui_set_recent(const X4RecentSnapshot *recent);
void psbox_ui_set_session(const X4SessionSnapshot *session);
/* Public Store metadata (cover URLs / names) from X4 catalog_assets — no tokens. */
void psbox_ui_ingest_metadata(const X4ProductMetadata *items, size_t count);

/* Pad → UI. Returns nonzero if the caller should start a cloud session for
 * the title id written into title_id_out (nul-terminated, capacity bytes).
 * Returns -1 if the caller should cancel a pending launch. */
int psbox_ui_handle(uint32_t pressed, uint32_t held, float lx, float ly, float rx, float ry,
                    float l2, float r2, uint64_t now_ms, char *title_id_out, size_t title_cap);

/* Draw into the XCloud4 display RGBA buffer (1920x1080). */
void psbox_ui_draw(uint32_t *pixels, uint64_t now_ms);

/* In-stream overlay (pause menu / stats).
 * Returns 0 = closed / pass-through, 1 = overlay consumed input,
 * 2 = Leave requested (caller should x4_auth_cancel + session_back). */
int psbox_ui_stream_menu(uint32_t *pixels, uint32_t pressed, uint32_t held, uint64_t now_ms,
                         int video_w, int video_h, int fps, int bitrate_kbps);

/* Local GoldHEN update scan/install prompts. */
void psbox_ui_poll_local_update(const char *current_app_ver);

/* Crash / SIGTERM guards (complements src/core/psbox_exit.c). */
int psbox_ui_install_crash_guard(void);

#ifdef __cplusplus
}
#endif
