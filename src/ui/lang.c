/* SPDX-License-Identifier: GPL-3.0-only */
#include "lang.h"
#include "../core/build_identity.h"
#include <stdio.h>
#include <string.h>
#ifdef X4_OPENORBIS
#include <orbis/SystemService.h>
#else
enum {
    ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG = 1,
    ORBIS_SYSTEM_PARAM_LANG_JAPANESE = 0,
    ORBIS_SYSTEM_PARAM_LANG_ENGLISH_US = 1,
    ORBIS_SYSTEM_PARAM_LANG_FRENCH = 2,
    ORBIS_SYSTEM_PARAM_LANG_SPANISH = 3,
    ORBIS_SYSTEM_PARAM_LANG_GERMAN = 4,
    ORBIS_SYSTEM_PARAM_LANG_ITALIAN = 5,
    ORBIS_SYSTEM_PARAM_LANG_DUTCH = 6,
    ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_PT = 7,
    ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_BR = 17,
    ORBIS_SYSTEM_PARAM_LANG_ENGLISH_GB = 18,
    ORBIS_SYSTEM_PARAM_LANG_SPANISH_LA = 20,
    ORBIS_SYSTEM_PARAM_LANG_FRENCH_CA = 22,
};
#endif

static enum X4Lang current_lang = X4_LANG_ES;
static char current_locale[16] = "es-MX";
static char current_market[8] = "MX";
static char current_store_suffix[64] = "&market=MX&languages=es-MX&fieldsTemplate=Details";

void x4_lang_set(enum X4Lang lang, const char *locale, const char *market)
{
    current_lang = (lang == X4_LANG_ES) ? X4_LANG_ES : X4_LANG_EN;
    snprintf(current_locale, sizeof(current_locale), "%s", (locale && locale[0]) ? locale : "en-US");
    snprintf(current_market, sizeof(current_market), "%s", (market && market[0]) ? market : "US");
    snprintf(current_store_suffix, sizeof(current_store_suffix),
        "&market=%s&languages=%s&fieldsTemplate=Details", current_market, current_locale);
}

void x4_lang_apply_system_id(int32_t param_lang)
{
    switch (param_lang) {
    case ORBIS_SYSTEM_PARAM_LANG_SPANISH:
        x4_lang_set(X4_LANG_ES, "es-ES", "ES");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_SPANISH_LA:
        x4_lang_set(X4_LANG_ES, "es-MX", "MX");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_ENGLISH_US:
        x4_lang_set(X4_LANG_EN, "en-US", "US");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_ENGLISH_GB:
        x4_lang_set(X4_LANG_EN, "en-GB", "GB");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_BR:
        x4_lang_set(X4_LANG_EN, "pt-BR", "BR");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_PORTUGUESE_PT:
        x4_lang_set(X4_LANG_EN, "pt-PT", "PT");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_FRENCH:
        x4_lang_set(X4_LANG_EN, "fr-FR", "FR");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_FRENCH_CA:
        x4_lang_set(X4_LANG_EN, "fr-CA", "CA");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_GERMAN:
        x4_lang_set(X4_LANG_EN, "de-DE", "DE");
        break;
    case ORBIS_SYSTEM_PARAM_LANG_ITALIAN:
        x4_lang_set(X4_LANG_EN, "it-IT", "IT");
        break;
    default:
        x4_lang_set(X4_LANG_EN, "en-US", "US");
        break;
    }
}

int x4_lang_init(void)
{
#ifdef X4_OPENORBIS
    int32_t raw_lang = -1;
    int32_t rc = sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG, &raw_lang);
    if (rc < 0) {
        x4_lang_set(X4_LANG_EN, "en-US", "US");
        printf("XCloud4: sceSystemServiceParamGetInt(LANG) fallo rc=0x%08x; usando en-US/US\n", (unsigned)rc);
        return (int)rc;
    }
    x4_lang_apply_system_id(raw_lang);
    printf("XCloud4: idioma consola=%d ui=%s locale=%s market=%s\n",
        (int)raw_lang, current_lang == X4_LANG_ES ? "es" : "en", current_locale, current_market);
    return 0;
#else
    x4_lang_set(X4_LANG_EN, "en-US", "US");
    return 0;
#endif
}

enum X4Lang x4_lang_current(void)
{
    return current_lang;
}

const char *x4_lang_locale(void)
{
    return current_locale;
}

const char *x4_lang_market(void)
{
    return current_market;
}

const char *x4_lang_store_suffix(void)
{
    return current_store_suffix;
}

static const char *const table_es[X4_TR_COUNT] = {
    [X4_TR_DS4_CONNECTED] = "DUALSHOCK 4 CONECTADO",
    [X4_TR_DS4_CONNECT] = "CONECTA TU DUALSHOCK 4",
    [X4_TR_CONTROLLER_ERR_FMT] = "CONTROL: 0X%08X",

    [X4_TR_HOME_TITLE] = "TU PUNTO DE PARTIDA",
    [X4_TR_HOME_SUBTITLE_PUBLIC] = "XBOX CLOUD GAMING EN TU CONSOLA PS4",
    [X4_TR_HOME_SUBTITLE_LAB] = "EL PROYECTO AVANZA EN TU CONSOLA",
    [X4_TR_CARD_ACCOUNT_TITLE] = "CUENTA",
    [X4_TR_CARD_ACCOUNT_DESC] = "INICIA SESION CON\nMICROSOFT Y JUEGA.",
    [X4_TR_CARD_ABOUT_TITLE] = "ACERCA DE",
    [X4_TR_CARD_ABOUT_DESC] = "INFORMACION, CREDITOS\nY CONTROLES.",
    [X4_TR_CARD_SUPPORT_TITLE] = "APOYA EL PROYECTO",
    [X4_TR_CARD_SUPPORT_DESC] = "DONACION VOLUNTARIA\nPARA EL DESARROLLO.",
    [X4_TR_X_OPEN] = "X  ABRIR",
    [X4_TR_HOME_HINT] = "ABRE CUENTA PARA CONECTAR Y CONSULTAR EL CATALOGO.",
    [X4_TR_HOME_FOOTER_PUBLIC] = "CRUCETA  ELEGIR     X  ABRIR     PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_HOME_FOOTER_LAB] = "CRUCETA  ELEGIR     X  ABRIR     OPTIONS  SALIR",

    [X4_TR_ABOUT_TITLE] = "ACERCA DE",
    [X4_TR_ABOUT_LINE1] = "XCLOUD4 " X4_PRODUCT_VERSION " - CLIENTE NO OFICIAL DE XBOX CLOUD GAMING PARA PS4",
    [X4_TR_ABOUT_LINE2] = "REQUIERE XBOX GAME PASS CON JUEGO EN LA NUBE Y BUENA CONEXION A INTERNET.",
    [X4_TR_ABOUT_LINE3] = "NO ESTA AFILIADO CON MICROSOFT, XBOX NI SONY.",
    [X4_TR_ABOUT_LINE4] = "SOFTWARE LIBRE GPL-3.0 - GITHUB.COM/ASP11EDGR/XCLOUD4",
    [X4_TR_ABOUT_LINE5] = "DESARROLLADO POR ASAP_EDGR",
    [X4_TR_ABOUT_LINE6] = "CREDITOS: OPENORBIS, GOLDHEN, LIBDATACHANNEL, OPUS, MBED TLS. REFERENCIAS: GREENVITA Y PSBOX.",
    [X4_TR_ABOUT_LINE7] = "EN EL JUEGO: MANTÉN DOS DEDOS EN EL TOUCHPAD 1 S PARA ABRIR EL MENÚ",
    [X4_TR_ABOUT_LINE8] = "PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_CIRCLE_BACK] = "CIRCULO  VOLVER",

    [X4_TR_SUPPORT_TITLE] = "APOYA EL PROYECTO",
    [X4_TR_SUPPORT_LINE1] = "NUESTRA MISION: QUE MAS PERSONAS PUEDAN JUGAR TITULOS DE NUEVA GENERACION SIN COMPRAR UNA CONSOLA NUEVA.",
    [X4_TR_SUPPORT_LINE2] = "XCLOUD4 ES GRATUITO Y DE CODIGO ABIERTO. SI TE ES UTIL, PUEDES APOYAR SU DESARROLLO CON UNA DONACION VOLUNTARIA.",
    [X4_TR_SUPPORT_LINE3] = "LAS DONACIONES NO DAN ACCESO A JUEGOS NI A SERVICIOS DE XBOX.",
    [X4_TR_SUPPORT_LINE4] = "ESCANEA EL CODIGO CON TU TELEFONO O VISITA: KO-FI.COM/ASAPEDGR",
    [X4_TR_SUPPORT_LINE5] = "GRACIAS POR TU APOYO.",

    [X4_TR_EXIT_ERR_PUBLIC_FMT] = "NO SE PUDO SALIR: 0X%08X. PARA SALIR: BOTON PS > CERRAR APLICACION.",
    [X4_TR_EXIT_ERR_LAB_FMT] = "NO SE PUDO SALIR: 0X%08X. OPTIONS PARA REINTENTAR.",

    [X4_TR_AUTH_TITLE] = "TU CUENTA MICROSOFT",
    [X4_TR_AUTH_PROMPT_PUBLIC] = "INICIA SESION CON TU CUENTA DE MICROSOFT DESDE TU TELEFONO O PC.",
    [X4_TR_AUTH_NOTE_PUBLIC] = "INICIA SESION CON TU CUENTA DE MICROSOFT DESDE TU TELEFONO O PC.",
    [X4_TR_AUTH_CLOSING_TITLE] = "CERRANDO LA CONEXION...",
    [X4_TR_AUTH_CLOSING_SUB] = "ESPERA UNOS SEGUNDOS PARA VOLVER AL INICIO DE PS4.",
    [X4_TR_AUTH_CODE_PROMPT] = "INTRODUCE ESTE CODIGO EN MICROSOFT:",
    [X4_TR_AUTH_EXPIRES_FMT] = "CADUCA EN %u:%02u",
    [X4_TR_AUTH_CANCEL_HINT] = "CIRCULO CANCELA EL ACCESO Y VUELVE AL MENU.",
    [X4_TR_AUTH_READY] = "LISTO PARA INICIAR SESION",
    [X4_TR_AUTH_CONNECTING] = "CONECTANDO CON MICROSOFT...",
    [X4_TR_AUTH_CONNECTED] = "CONEXION A MICROSOFT CORRECTA",
    [X4_TR_AUTH_AUTHORIZED] = "CUENTA MICROSOFT AUTORIZADA",
    [X4_TR_AUTH_CANCELLED] = "ACCESO CANCELADO",
    [X4_TR_AUTH_EXPIRED] = "EL ACCESO HA CADUCADO",
    [X4_TR_AUTH_DENIED] = "ACCESO RECHAZADO",
    [X4_TR_AUTH_ERROR] = "NO SE PUDO COMPLETAR EL ACCESO",
    [X4_TR_AUTH_AUTHORIZED_HINT] = "PULSA R1 PARA CONSULTAR EL CATALOGO DE XBOX.\nLA CUENTA SE CONSERVA HASTA CERRAR ESTA APP.",
    [X4_TR_AUTH_IDLE_HINT_PUBLIC] = "X SOLICITA UN CODIGO PARA INICIAR SESION.\nTU CONTRASENA SE INTRODUCE EN LA PAGINA DE MICROSOFT.",
    [X4_TR_AUTH_IDLE_HINT_LAB] = "X SOLICITA UN CODIGO. CUADRADO REVISA LA CONEXION.\nTU CONTRASENA SE INTRODUCE EN LA PAGINA DE MICROSOFT.",
    [X4_TR_DETAIL_HTTP_FMT] = "DETALLE 0X%08X    HTTP %d",
    [X4_TR_AUTH_FOOTER_PUBLIC] = "X INICIAR SESION   TRIANGULO CERRAR SESION   CIRCULO VOLVER   R1 CATALOGO",
    [X4_TR_AUTH_FOOTER_LAB] = "X INICIAR   CUADRADO CONEXION   TRIANGULO BORRAR SESION   CIRCULO VOLVER   OPTIONS SALIR",
    [X4_TR_AUTH_R1_HINT] = "R1 CATALOGO DE XBOX. ELIGE UN TITULO CON ACCESO PARA JUGAR.",

    [X4_TR_SESS_TITLE] = "SESION DE JUEGO",
    [X4_TR_SESS_SELECTED_TITLE] = "TITULO SELECCIONADO",
    [X4_TR_SESS_PREPARING] = "PREPARANDO LA SOLICITUD...",
    [X4_TR_SESS_WAITING] = "XBOX ESTA PREPARANDO EL JUEGO...",
    [X4_TR_SESS_READY] = "XBOX PREPARO LA SESION",
    [X4_TR_SESS_AUTHORIZING] = "AUTORIZANDO LA CONEXION...",
    [X4_TR_SESS_AUTHORIZED] = "XBOX ACEPTO LA CONEXION",
    [X4_TR_SESS_NEGOTIATING] = "NEGOCIANDO IMAGEN Y SONIDO...",
    [X4_TR_SESS_CONNECTING] = "CONECTANDO EL JUEGO POR WEBRTC...",
    [X4_TR_SESS_STREAMING] = "RECIBIENDO PAQUETES DEL JUEGO",
    [X4_TR_SESS_STOPPING] = "CERRANDO LA SESION EN XBOX...",
    [X4_TR_SESS_CLOSED] = "SESION CERRADA",
    [X4_TR_SESS_CANCELLED] = "SOLICITUD CANCELADA",
    [X4_TR_SESS_ERROR] = "NO SE PUDO COMPLETAR LA SESION",
    [X4_TR_SESS_CLOSING_APP] = "CERRANDO LA SESION Y LA APLICACION...",
    [X4_TR_SESS_PREP_AV] = "PREPARANDO LA CONEXION DE IMAGEN Y SONIDO.",
    [X4_TR_SESS_PREP_AUTH] = "PREPARANDO LA AUTORIZACION DE ESTA SESION.",
    [X4_TR_SESS_PERM_ACCEPTED] = "XBOX ACEPTO EL PERMISO PARA CONECTAR ESTA SESION.",
    [X4_TR_SESS_CONFIRMED_READY] = "XBOX CONFIRMO QUE LA SESION ESTABA LISTA.",
    [X4_TR_SESS_ELAPSED_FMT] = "TIEMPO TRANSCURRIDO: %u SEGUNDOS.",
    [X4_TR_SESS_WEBRTC_FMT] = "SDP HTTP %d   ICE HTTP %d   WEBRTC %s",
    [X4_TR_SESS_RTC_CONNECTED] = "CONECTADO",
    [X4_TR_SESS_RTC_WAITING] = "EN ESPERA",
    [X4_TR_SESS_CLEANUP_FAIL_FMT] = "CIERRE SIN CONFIRMAR: 0X%08X   HTTP %d",
    [X4_TR_SESS_DETAIL_FMT] = "DETALLE 0X%08X   HTTP %d",
    [X4_TR_SESS_FOOTER_PUBLIC] = "CIRCULO  CERRAR Y VOLVER AL CATALOGO     PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_SESS_FOOTER_LAB] = "CIRCULO  CERRAR Y VOLVER AL CATALOGO     OPTIONS  SALIR",
    [X4_TR_GAME_TOUCH_HINT] = "EN EL JUEGO: MANTÉN DOS DEDOS EN EL TOUCHPAD 1 S PARA ABRIR EL MENÚ",

    [X4_TR_LIVE_PACKETS_FMT] = "H264: %llu PAQUETES, %llu IMAGENES   OPUS: %llu PAQUETES, %llu BLOQUES",
    [X4_TR_LIVE_VIDEO_ERR_PUBLIC_FMT] = "Código %d · audio %s · PARA SALIR: BOTON PS > CERRAR APLICACION",
    [X4_TR_LIVE_VIDEO_ERR_LAB_FMT] = "Código %d · audio %s · OPTIONS para salir",
    [X4_TR_LIVE_AUDIO_UNAVAIL] = "no disponible",
    [X4_TR_LIVE_AUDIO_ACTIVE] = "activo",
    [X4_TR_LIVE_AUDIO_WAITING] = "en espera",
    [X4_TR_LIVE_MEDIA_ERR_FMT] = "MEDIOS: 0X%08X  VIDEO: 0X%08X  AUDIO: 0X%08X",
    [X4_TR_TOUCH_MENU_TITLE] = "MENÚ DEL JUEGO",
    [X4_TR_TOUCH_MENU_BACK] = "Volver al catálogo",
    [X4_TR_TOUCH_MENU_MUTE] = "Silenciar sonido",
    [X4_TR_TOUCH_MENU_UNMUTE] = "Activar sonido",
    [X4_TR_TOUCH_MENU_XBOX] = "Botón Xbox",
    [X4_TR_TOUCH_MENU_CANCEL] = "Cancelar",
    [X4_TR_TOUCH_MENU_MARK_TEST] = "Marcar prueba",
    [X4_TR_TOUCH_MENU_EXIT_APP] = "Salir de la app",
    [X4_TR_TOUCH_MENU_FOOTER] = "CRUCETA Elegir   X Aceptar   CÍRCULO Cerrar",
    [X4_TR_OVERLAY_AUDIO_UNAVAIL] = "SONIDO NO DISPONIBLE",
    [X4_TR_OVERLAY_AUDIO_MUTED] = "SONIDO SILENCIADO",
    [X4_TR_OVERLAY_INPUT_UNAVAIL] = "CONTROL NO DISPONIBLE",
    [X4_TR_VIDEO_REJECTED_DIM_PUBLIC_FMT] = "Xbox envió %u×%u; esta PS4 solo admite hasta 1080p. Cierra con botón PS.",
    [X4_TR_VIDEO_REJECTED_DIM_LAB_FMT] = "Xbox envió %u×%u; esta PS4 solo admite hasta 1080p. Sal con OPTIONS.",
    [X4_TR_VIDEO_STOPPED_PUBLIC_FMT] = "Vídeo detenido (código %d). Cierra con botón PS.",
    [X4_TR_VIDEO_STOPPED_LAB_FMT] = "Vídeo detenido (código %d). Sal con OPTIONS.",

    [X4_TR_CAT_DS4_CONNECTED] = "DualShock 4 conectado",
    [X4_TR_CAT_DS4_CONNECT] = "Conecta tu DualShock 4",
    [X4_TR_CAT_TITLE] = "Catálogo",
    [X4_TR_CAT_SEARCH_BTN] = "TRIÁNGULO Buscar juegos",
    [X4_TR_CAT_RECENT_TITLE] = "Jugados recientemente",
    [X4_TR_CAT_RECENT_EMPTY] = "Tus juegos recientes aparecerán aquí",
    [X4_TR_CAT_RECENT_LOADING] = "Cargando tus recientes…",
    [X4_TR_CAT_RECENT_ERROR] = "No se pudieron cargar tus recientes",
    [X4_TR_CAT_RECENT_RETRY] = "CUADRADO Reintentar recientes",
    [X4_TR_CAT_NEXT_GAME] = "Tu próximo juego",
    [X4_TR_CAT_FILTER_ALL] = "Todos",
    [X4_TR_CAT_FILTER_ENTITLED] = "Con acceso",
    [X4_TR_CAT_FILTER_UNCONFIRMED] = "Por confirmar",
    [X4_TR_CAT_LOADING] = "Cargando el catálogo…",
    [X4_TR_CAT_NO_MATCH] = "No hay juegos en esta búsqueda",
    [X4_TR_CAT_OPEN_ACCOUNT] = "Abre tu cuenta para cargar el catálogo",
    [X4_TR_CAT_ERROR] = "No se pudo cargar el catálogo",
    [X4_TR_CAT_PERMISSION_UNKNOWN] = "Permiso todavía desconocido",
    [X4_TR_CAT_NOT_AVAILABLE] = "No disponible ahora",
    [X4_TR_CAT_REFRESH_PROMPT] = "Actualiza el catálogo",
    [X4_TR_CAT_PREPARING] = "Preparando…",
    [X4_TR_CAT_QUERYING] = "Consultando xCloud…",
    [X4_TR_CAT_PLAY_BTN] = "X  Jugar",
    [X4_TR_CAT_CHOOSE_GAME] = "Elige un juego",
    [X4_TR_CAT_FOOTER_PUBLIC] = "Cruceta Elegir   X Jugar   L1/R1 Página   L2/R2 Grupos   CUADRADO Actualizar   CÍRCULO Cuenta",
    [X4_TR_CAT_FOOTER_LAB] = "Cruceta Elegir   X Jugar   L1/R1 Página   L2/R2 Grupos   CUADRADO Actualizar   CÍRCULO Cuenta   OPTIONS Salir",
    [X4_TR_CAT_FINISHING_COVERS] = "Terminando la carga de portadas…",
    [X4_TR_CAT_RESULTS_FMT] = "%u resultados · Página %u/%u · %s",
    [X4_TR_KB_TITLE] = "Buscar juegos",
    [X4_TR_KB_PLACEHOLDER] = "Escribe un título o ID",
    [X4_TR_KB_HELP1] = "X Añadir   CUADRADO Borrar   TRIÁNGULO Limpiar",
    [X4_TR_KB_HELP2_PUBLIC] = "R1 Espacio   R2 Aplicar   CÍRCULO Volver",
    [X4_TR_KB_HELP2_LAB] = "R1 Espacio   R2 Aplicar   CÍRCULO Volver   OPTIONS Salir",
    [X4_TR_ASSET_DOWNLOADING] = "Descargando…",
    [X4_TR_ASSET_READY] = "Lista",
    [X4_TR_ASSET_RETRY_PENDING] = "Reintento pendiente",
    [X4_TR_ASSET_MISSING] = "Sin imagen",
    [X4_TR_ASSET_PENDING] = "Pendiente",
    [X4_TR_ASSET_SMALL_LOADING] = "Cargando",
    [X4_TR_ASSET_SMALL_RETRY] = "Reintento",
    [X4_TR_NOTICE_COVER_CLEANUP] = "No se pudo cerrar la conexión de portadas. Sal y abre XCloud4 otra vez.",
    [X4_TR_NOTICE_QUERY_ERR_FMT] = "No se pudo iniciar la consulta (%d)",
    [X4_TR_HOME_UPDATE_AVAILABLE_FMT] = "NUEVA VERSION DISPONIBLE: %s · GITHUB.COM/ASP11EDGR/XCLOUD4",
    [X4_TR_UPDATE_CHECKING] = "COMPROBANDO ACTUALIZACIONES...",
    [X4_TR_UPDATE_LATEST] = "ESTAS USANDO LA ULTIMA VERSION",
    [X4_TR_UPDATE_AVAILABLE_FMT] = "HAY UNA VERSION NUEVA: %s",
    [X4_TR_UPDATE_FAILED] = "NO SE PUDO COMPROBAR SI HAY ACTUALIZACIONES",
    [X4_TR_AUTH_SCAN_PHONE] = "ESCANEA CON TU TELEFONO",
};

static const char *const table_en[X4_TR_COUNT] = {
    [X4_TR_DS4_CONNECTED] = "DUALSHOCK 4 CONNECTED",
    [X4_TR_DS4_CONNECT] = "CONNECT YOUR DUALSHOCK 4",
    [X4_TR_CONTROLLER_ERR_FMT] = "CONTROLLER: 0X%08X",

    [X4_TR_HOME_TITLE] = "YOUR STARTING POINT",
    [X4_TR_HOME_SUBTITLE_PUBLIC] = "XBOX CLOUD GAMING ON YOUR PS4 CONSOLE",
    [X4_TR_HOME_SUBTITLE_LAB] = "THE PROJECT ADVANCES ON YOUR CONSOLE",
    [X4_TR_CARD_ACCOUNT_TITLE] = "ACCOUNT",
    [X4_TR_CARD_ACCOUNT_DESC] = "SIGN IN WITH\nMICROSOFT AND PLAY.",
    [X4_TR_CARD_ABOUT_TITLE] = "ABOUT",
    [X4_TR_CARD_ABOUT_DESC] = "INFORMATION, CREDITS\nAND CONTROLS.",
    [X4_TR_CARD_SUPPORT_TITLE] = "SUPPORT THE PROJECT",
    [X4_TR_CARD_SUPPORT_DESC] = "VOLUNTARY DONATION\nFOR DEVELOPMENT.",
    [X4_TR_X_OPEN] = "X  OPEN",
    [X4_TR_HOME_HINT] = "OPEN ACCOUNT TO SIGN IN AND BROWSE THE CATALOG.",
    [X4_TR_HOME_FOOTER_PUBLIC] = "D-PAD  SELECT     X  OPEN     TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_HOME_FOOTER_LAB] = "D-PAD  SELECT     X  OPEN     OPTIONS  EXIT",

    [X4_TR_ABOUT_TITLE] = "ABOUT",
    [X4_TR_ABOUT_LINE1] = "XCLOUD4 " X4_PRODUCT_VERSION " - UNOFFICIAL XBOX CLOUD GAMING CLIENT FOR PS4",
    [X4_TR_ABOUT_LINE2] = "REQUIRES XBOX GAME PASS WITH CLOUD GAMING AND A GOOD INTERNET CONNECTION.",
    [X4_TR_ABOUT_LINE3] = "NOT AFFILIATED WITH MICROSOFT, XBOX OR SONY.",
    [X4_TR_ABOUT_LINE4] = "FREE SOFTWARE GPL-3.0 - GITHUB.COM/ASP11EDGR/XCLOUD4",
    [X4_TR_ABOUT_LINE5] = "DEVELOPED BY ASAP_EDGR",
    [X4_TR_ABOUT_LINE6] = "CREDITS: OPENORBIS, GOLDHEN, LIBDATACHANNEL, OPUS, MBED TLS. REFERENCES: GREENVITA AND PSBOX.",
    [X4_TR_ABOUT_LINE7] = "IN GAME: HOLD TWO FINGERS ON THE TOUCHPAD FOR 1 S TO OPEN THE MENU",
    [X4_TR_ABOUT_LINE8] = "TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_CIRCLE_BACK] = "CIRCLE  BACK",

    [X4_TR_SUPPORT_TITLE] = "SUPPORT THE PROJECT",
    [X4_TR_SUPPORT_LINE1] = "OUR MISSION: HELPING MORE PEOPLE PLAY NEXT-GEN GAMES WITHOUT BUYING A NEW CONSOLE.",
    [X4_TR_SUPPORT_LINE2] = "XCLOUD4 IS FREE AND OPEN SOURCE. IF YOU FIND IT USEFUL, YOU CAN SUPPORT ITS DEVELOPMENT WITH A VOLUNTARY DONATION.",
    [X4_TR_SUPPORT_LINE3] = "DONATIONS DO NOT GRANT ACCESS TO GAMES OR XBOX SERVICES.",
    [X4_TR_SUPPORT_LINE4] = "SCAN THE CODE WITH YOUR PHONE OR VISIT: KO-FI.COM/ASAPEDGR",
    [X4_TR_SUPPORT_LINE5] = "THANK YOU FOR YOUR SUPPORT.",

    [X4_TR_EXIT_ERR_PUBLIC_FMT] = "COULD NOT EXIT: 0X%08X. TO EXIT: PS BUTTON > CLOSE APPLICATION.",
    [X4_TR_EXIT_ERR_LAB_FMT] = "COULD NOT EXIT: 0X%08X. PRESS OPTIONS TO RETRY.",

    [X4_TR_AUTH_TITLE] = "YOUR MICROSOFT ACCOUNT",
    [X4_TR_AUTH_PROMPT_PUBLIC] = "SIGN IN WITH YOUR MICROSOFT ACCOUNT FROM YOUR PHONE OR PC.",
    [X4_TR_AUTH_NOTE_PUBLIC] = "SIGN IN WITH YOUR MICROSOFT ACCOUNT FROM YOUR PHONE OR PC.",
    [X4_TR_AUTH_CLOSING_TITLE] = "CLOSING CONNECTION...",
    [X4_TR_AUTH_CLOSING_SUB] = "WAIT A FEW SECONDS TO RETURN TO THE PS4 HOME SCREEN.",
    [X4_TR_AUTH_CODE_PROMPT] = "ENTER THIS CODE AT MICROSOFT:",
    [X4_TR_AUTH_EXPIRES_FMT] = "EXPIRES IN %u:%02u",
    [X4_TR_AUTH_CANCEL_HINT] = "CIRCLE CANCELS SIGN-IN AND RETURNS TO THE MENU.",
    [X4_TR_AUTH_READY] = "READY TO SIGN IN",
    [X4_TR_AUTH_CONNECTING] = "CONNECTING TO MICROSOFT...",
    [X4_TR_AUTH_CONNECTED] = "MICROSOFT CONNECTION VERIFIED",
    [X4_TR_AUTH_AUTHORIZED] = "MICROSOFT ACCOUNT AUTHORIZED",
    [X4_TR_AUTH_CANCELLED] = "SIGN-IN CANCELLED",
    [X4_TR_AUTH_EXPIRED] = "SIGN-IN CODE EXPIRED",
    [X4_TR_AUTH_DENIED] = "SIGN-IN DENIED",
    [X4_TR_AUTH_ERROR] = "COULD NOT COMPLETE SIGN-IN",
    [X4_TR_AUTH_AUTHORIZED_HINT] = "PRESS R1 TO BROWSE THE XBOX CATALOG.\nYOUR ACCOUNT STAYS SIGNED IN UNTIL YOU CLOSE THIS APP.",
    [X4_TR_AUTH_IDLE_HINT_PUBLIC] = "PRESS X TO REQUEST A SIGN-IN CODE.\nYOUR PASSWORD IS ENTERED ON THE MICROSOFT PAGE.",
    [X4_TR_AUTH_IDLE_HINT_LAB] = "PRESS X FOR A CODE. SQUARE CHECKS CONNECTION.\nYOUR PASSWORD IS ENTERED ON THE MICROSOFT PAGE.",
    [X4_TR_DETAIL_HTTP_FMT] = "DETAIL 0X%08X    HTTP %d",
    [X4_TR_AUTH_FOOTER_PUBLIC] = "X SIGN IN   TRIANGLE SIGN OUT   CIRCLE BACK   R1 CATALOG",
    [X4_TR_AUTH_FOOTER_LAB] = "X SIGN IN   SQUARE CONNECTION   TRIANGLE SIGN OUT   CIRCLE BACK   OPTIONS EXIT",
    [X4_TR_AUTH_R1_HINT] = "R1 XBOX CATALOG. SELECT AN AVAILABLE TITLE TO PLAY.",

    [X4_TR_SESS_TITLE] = "GAME SESSION",
    [X4_TR_SESS_SELECTED_TITLE] = "SELECTED TITLE",
    [X4_TR_SESS_PREPARING] = "PREPARING REQUEST...",
    [X4_TR_SESS_WAITING] = "XBOX IS PREPARING THE GAME...",
    [X4_TR_SESS_READY] = "XBOX PREPARED THE SESSION",
    [X4_TR_SESS_AUTHORIZING] = "AUTHORIZING CONNECTION...",
    [X4_TR_SESS_AUTHORIZED] = "XBOX ACCEPTED THE CONNECTION",
    [X4_TR_SESS_NEGOTIATING] = "NEGOTIATING AUDIO AND VIDEO...",
    [X4_TR_SESS_CONNECTING] = "CONNECTING GAME VIA WEBRTC...",
    [X4_TR_SESS_STREAMING] = "RECEIVING GAME STREAM",
    [X4_TR_SESS_STOPPING] = "CLOSING SESSION ON XBOX...",
    [X4_TR_SESS_CLOSED] = "SESSION CLOSED",
    [X4_TR_SESS_CANCELLED] = "REQUEST CANCELLED",
    [X4_TR_SESS_ERROR] = "COULD NOT COMPLETE THE SESSION",
    [X4_TR_SESS_CLOSING_APP] = "CLOSING SESSION AND APPLICATION...",
    [X4_TR_SESS_PREP_AV] = "PREPARING AUDIO AND VIDEO CONNECTION.",
    [X4_TR_SESS_PREP_AUTH] = "PREPARING AUTHORIZATION FOR THIS SESSION.",
    [X4_TR_SESS_PERM_ACCEPTED] = "XBOX ACCEPTED PERMISSION TO CONNECT THIS SESSION.",
    [X4_TR_SESS_CONFIRMED_READY] = "XBOX CONFIRMED THAT THE SESSION WAS READY.",
    [X4_TR_SESS_ELAPSED_FMT] = "ELAPSED TIME: %u SECONDS.",
    [X4_TR_SESS_WEBRTC_FMT] = "SDP HTTP %d   ICE HTTP %d   WEBRTC %s",
    [X4_TR_SESS_RTC_CONNECTED] = "CONNECTED",
    [X4_TR_SESS_RTC_WAITING] = "WAITING",
    [X4_TR_SESS_CLEANUP_FAIL_FMT] = "UNCONFIRMED CLOSE: 0X%08X   HTTP %d",
    [X4_TR_SESS_DETAIL_FMT] = "DETAIL 0X%08X   HTTP %d",
    [X4_TR_SESS_FOOTER_PUBLIC] = "CIRCLE  CLOSE AND RETURN TO CATALOG     TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_SESS_FOOTER_LAB] = "CIRCLE  CLOSE AND RETURN TO CATALOG     OPTIONS  EXIT",
    [X4_TR_GAME_TOUCH_HINT] = "IN GAME: HOLD TWO FINGERS ON THE TOUCHPAD FOR 1 S TO OPEN THE MENU",

    [X4_TR_LIVE_PACKETS_FMT] = "H264: %llu PACKETS, %llu FRAMES   OPUS: %llu PACKETS, %llu BLOCKS",
    [X4_TR_LIVE_VIDEO_ERR_PUBLIC_FMT] = "Code %d · audio %s · TO EXIT: PS BUTTON > CLOSE APPLICATION",
    [X4_TR_LIVE_VIDEO_ERR_LAB_FMT] = "Code %d · audio %s · OPTIONS to exit",
    [X4_TR_LIVE_AUDIO_UNAVAIL] = "unavailable",
    [X4_TR_LIVE_AUDIO_ACTIVE] = "active",
    [X4_TR_LIVE_AUDIO_WAITING] = "waiting",
    [X4_TR_LIVE_MEDIA_ERR_FMT] = "MEDIA: 0X%08X  VIDEO: 0X%08X  AUDIO: 0X%08X",
    [X4_TR_TOUCH_MENU_TITLE] = "GAME MENU",
    [X4_TR_TOUCH_MENU_BACK] = "Return to catalog",
    [X4_TR_TOUCH_MENU_MUTE] = "Mute audio",
    [X4_TR_TOUCH_MENU_UNMUTE] = "Unmute audio",
    [X4_TR_TOUCH_MENU_XBOX] = "Xbox button",
    [X4_TR_TOUCH_MENU_CANCEL] = "Cancel",
    [X4_TR_TOUCH_MENU_MARK_TEST] = "Mark test",
    [X4_TR_TOUCH_MENU_EXIT_APP] = "Exit app",
    [X4_TR_TOUCH_MENU_FOOTER] = "D-PAD Choose   X Confirm   CIRCLE Close",
    [X4_TR_OVERLAY_AUDIO_UNAVAIL] = "AUDIO UNAVAILABLE",
    [X4_TR_OVERLAY_AUDIO_MUTED] = "AUDIO MUTED",
    [X4_TR_OVERLAY_INPUT_UNAVAIL] = "CONTROLLER UNAVAILABLE",
    [X4_TR_VIDEO_REJECTED_DIM_PUBLIC_FMT] = "Xbox sent %u×%u; this PS4 supports up to 1080p. Close via PS button.",
    [X4_TR_VIDEO_REJECTED_DIM_LAB_FMT] = "Xbox sent %u×%u; this PS4 supports up to 1080p. Exit with OPTIONS.",
    [X4_TR_VIDEO_STOPPED_PUBLIC_FMT] = "Video stopped (code %d). Close via PS button.",
    [X4_TR_VIDEO_STOPPED_LAB_FMT] = "Video stopped (code %d). Exit with OPTIONS.",

    [X4_TR_CAT_DS4_CONNECTED] = "DualShock 4 connected",
    [X4_TR_CAT_DS4_CONNECT] = "Connect your DualShock 4",
    [X4_TR_CAT_TITLE] = "Catalog",
    [X4_TR_CAT_SEARCH_BTN] = "TRIANGLE Search games",
    [X4_TR_CAT_RECENT_TITLE] = "Recently played",
    [X4_TR_CAT_RECENT_EMPTY] = "Your recently played games will appear here",
    [X4_TR_CAT_RECENT_LOADING] = "Loading your recent games…",
    [X4_TR_CAT_RECENT_ERROR] = "Could not load your recent games",
    [X4_TR_CAT_RECENT_RETRY] = "SQUARE Retry recent games",
    [X4_TR_CAT_NEXT_GAME] = "Your next game",
    [X4_TR_CAT_FILTER_ALL] = "All",
    [X4_TR_CAT_FILTER_ENTITLED] = "Available",
    [X4_TR_CAT_FILTER_UNCONFIRMED] = "Unconfirmed",
    [X4_TR_CAT_LOADING] = "Loading catalog…",
    [X4_TR_CAT_NO_MATCH] = "No games match this search",
    [X4_TR_CAT_OPEN_ACCOUNT] = "Open your account to load the catalog",
    [X4_TR_CAT_ERROR] = "Could not load the catalog",
    [X4_TR_CAT_PERMISSION_UNKNOWN] = "Access not yet confirmed",
    [X4_TR_CAT_NOT_AVAILABLE] = "Unavailable right now",
    [X4_TR_CAT_REFRESH_PROMPT] = "Refresh catalog",
    [X4_TR_CAT_PREPARING] = "Preparing…",
    [X4_TR_CAT_QUERYING] = "Contacting xCloud…",
    [X4_TR_CAT_PLAY_BTN] = "X  Play",
    [X4_TR_CAT_CHOOSE_GAME] = "Select a game",
    [X4_TR_CAT_FOOTER_PUBLIC] = "D-Pad Select   X Play   L1/R1 Page   L2/R2 Filter   SQUARE Refresh   CIRCLE Account",
    [X4_TR_CAT_FOOTER_LAB] = "D-Pad Select   X Play   L1/R1 Page   L2/R2 Filter   SQUARE Refresh   CIRCLE Account   OPTIONS Exit",
    [X4_TR_CAT_FINISHING_COVERS] = "Finishing cover loading…",
    [X4_TR_CAT_RESULTS_FMT] = "%u results · Page %u/%u · %s",
    [X4_TR_KB_TITLE] = "Search games",
    [X4_TR_KB_PLACEHOLDER] = "Type a title or ID",
    [X4_TR_KB_HELP1] = "X Add   SQUARE Delete   TRIANGLE Clear",
    [X4_TR_KB_HELP2_PUBLIC] = "R1 Space   R2 Apply   CIRCLE Back",
    [X4_TR_KB_HELP2_LAB] = "R1 Space   R2 Apply   CIRCLE Back   OPTIONS Exit",
    [X4_TR_ASSET_DOWNLOADING] = "Downloading…",
    [X4_TR_ASSET_READY] = "Ready",
    [X4_TR_ASSET_RETRY_PENDING] = "Retry pending",
    [X4_TR_ASSET_MISSING] = "No image",
    [X4_TR_ASSET_PENDING] = "Pending",
    [X4_TR_ASSET_SMALL_LOADING] = "Loading",
    [X4_TR_ASSET_SMALL_RETRY] = "Retry",
    [X4_TR_NOTICE_COVER_CLEANUP] = "Could not close cover connection. Close and reopen XCloud4.",
    [X4_TR_NOTICE_QUERY_ERR_FMT] = "Could not start request (%d)",
    [X4_TR_HOME_UPDATE_AVAILABLE_FMT] = "NEW VERSION AVAILABLE: %s · GITHUB.COM/ASP11EDGR/XCLOUD4",
    [X4_TR_UPDATE_CHECKING] = "CHECKING FOR UPDATES...",
    [X4_TR_UPDATE_LATEST] = "YOU ARE USING THE LATEST VERSION",
    [X4_TR_UPDATE_AVAILABLE_FMT] = "NEW VERSION AVAILABLE: %s",
    [X4_TR_UPDATE_FAILED] = "COULD NOT CHECK FOR UPDATES",
    [X4_TR_AUTH_SCAN_PHONE] = "SCAN WITH YOUR PHONE",
};

const char *x4_tr(enum X4TextId id)
{
    if (id < 0 || id >= X4_TR_COUNT) return "";
    const char *const *table = (current_lang == X4_LANG_ES) ? table_es : table_en;
    return table[id] ? table[id] : "";
}

static const struct {
    const char *es;
    const char *en;
} stage_map[] = {
    {"sin cuenta Microsoft", "no Microsoft account signed in"},
    {"solicitando codigo a Microsoft", "requesting code from Microsoft"},
    {"esperando autorizacion", "waiting for authorization"},
    {"cuenta Microsoft autorizada", "Microsoft account authorized"},
    {"acceso cancelado", "sign-in cancelled"},
    {"el codigo caduco", "the code expired"},
    {"token Microsoft caducado", "Microsoft token expired"},
    {"la sesion de Microsoft caduco", "Microsoft session expired"},
    {"verificando token", "verifying token"},
    {"TLS con Microsoft verificado", "Microsoft TLS verified"},
    {"acceso rechazado en la cuenta", "sign-in denied on account"},
    {"Microsoft no reconoce el codigo", "Microsoft did not recognize the code"},
    {"Microsoft no acepta esta aplicacion", "Microsoft rejected this application"},
    {"Microsoft rechazo los permisos pedidos", "Microsoft rejected the requested permissions"},
    {"Microsoft rechazo la solicitud", "Microsoft rejected the request"},
    {"estado HTTP inesperado", "unexpected HTTP status"},
    {"respuesta de error no valida", "invalid error response"},
    {"respuesta de token no valida", "invalid token response"},
    {"respuesta de codigo no valida", "invalid code response"},
    {"pagina de verificacion desconocida", "unknown verification page"},
    {"metadatos de Microsoft no validos", "invalid Microsoft metadata"},
    {"inicio HTTPS (red/TLS)", "HTTPS startup (network/TLS)"},
    {"sin memoria para el acceso", "out of memory for sign-in"},
    {"no se pudo iniciar el acceso", "could not start sign-in"},
    {"sin catalogo", "no catalog loaded"},
    {"iniciando HTTPS", "starting HTTPS"},
    {"token de usuario Xbox", "Xbox user token"},
    {"autorizacion XSTS", "XSTS authorization"},
    {"credenciales de Xbox Cloud Gaming", "Xbox Cloud Gaming credentials"},
    {"credenciales de juegos gratuitos", "free-to-play credentials"},
    {"lista de titulos de la region", "regional title list"},
    {"nombres de Microsoft Store", "Microsoft Store titles"},
    {"catalogo listo", "catalog ready"},
    {"consulta cancelada", "request cancelled"},
    {"solicitando sesion en la nube", "requesting cloud session"},
    {"esperando respuesta de Xbox", "waiting for Xbox response"},
    {"Xbox prepara la sesion", "Xbox is preparing the session"},
    {"esperando recursos de Xbox", "waiting for Xbox resources"},
    {"Xbox lista para negociar la conexion", "Xbox ready to negotiate connection"},
    {"sesion aprovisionada por Xbox", "session provisioned by Xbox"},
    {"Xbox reporto un fallo de la sesion", "Xbox reported a session failure"},
    {"autorizando conexion con Microsoft", "authorizing connection with Microsoft"},
    {"enviando autorizacion a Xbox", "sending authorization to Xbox"},
    {"preparando oferta WebRTC", "preparing WebRTC offer"},
    {"esperando respuesta SDP de Xbox", "waiting for Xbox SDP answer"},
    {"negociando ruta ICE con Xbox", "negotiating ICE route with Xbox"},
    {"recibiendo imagen y sonido de Xbox", "receiving audio and video from Xbox"},
    {"recibiendo video de Xbox", "receiving video from Xbox"},
    {"recibiendo audio de Xbox", "receiving audio from Xbox"},
    {"conectado con Xbox, esperando medios", "connected to Xbox, waiting for media"},
    {"esperando cierre seguro del control", "waiting for safe controller shutdown"},
    {"cerrando sesion en Xbox", "closing session on Xbox"},
    {"sesion cerrada en Xbox", "session closed on Xbox"},
    {"sesion cancelada", "session cancelled"},
    {"Xbox no inicio la sesion", "Xbox did not start the session"},
    {"Xbox no preparo la sesion a tiempo", "Xbox did not prepare the session in time"},
    {"Xbox rechazo la autorizacion de conexion", "Xbox rejected connection authorization"},
    {"WebRTC no negocio a tiempo", "WebRTC did not negotiate in time"},
    {"WebRTC no conecto a tiempo", "WebRTC did not connect in time"},
    {"se perdio la conexion WebRTC", "WebRTC connection lost"},
    {"fallo la conexion WebRTC", "WebRTC connection failed"},
    {"WebRTC conectado sin recibir imagen o sonido", "WebRTC connected without receiving audio or video"},
    {"no se confirmo el cierre de la sesion", "session close was not confirmed"},
    {"Esperando 1080p de Xbox…", "Waiting for 1080p from Xbox…"},
    {"no se pudieron preparar o cerrar los medios nativos", "could not prepare or close native media"},
};

const char *x4_tr_stage(const char *stage)
{
    if (!stage || !stage[0]) return "";
    if (current_lang == X4_LANG_ES) return stage;
    for (size_t i = 0; i < sizeof(stage_map) / sizeof(stage_map[0]); ++i) {
        if (!strcmp(stage, stage_map[i].es)) return stage_map[i].en;
    }
    return stage;
}
