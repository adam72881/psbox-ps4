# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RafaelNGP
# Port PS4: stesso nucleo di PSBox (auth, xCloud, WebRTC, UI). Il target
# predefinito è OpenOrbis. cmake/openorbis.cmake imposta XCLOUD_PS4.
cmake_minimum_required(VERSION 3.20)
project(xcloud_ps4 VERSION 1.0.55 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT XCLOUD_PS4)
    message(FATAL_ERROR "Configura con -DCMAKE_TOOLCHAIN_FILE=cmake/openorbis.cmake")
endif()

# sockaddr_storage della SDK è layout Linux (ss_family al byte 0). Su Orbis
# sockaddr ha sa_len al byte 0 e sa_family al byte 1: juice legge ss_family e
# scarta i server STUN, sendto torna EINVAL. La dimensione resta 128.
if(DEFINED OO_PS4_TOOLCHAIN)
    set(_xc_sock "${OO_PS4_TOOLCHAIN}/include/sys/socket.h")
    if(EXISTS "${_xc_sock}")
        file(READ "${_xc_sock}" _xc_sock_src)
        if(NOT _xc_sock_src MATCHES "unsigned char ss_len")
            string(REPLACE
                "struct sockaddr_storage {\n\tsa_family_t ss_family;\n\tchar __ss_padding[128-sizeof(long)-sizeof(sa_family_t)];\n\tunsigned long __ss_align;\n};"
                "struct sockaddr_storage {\n\tunsigned char ss_len; /* PSBox: layout BSD, allineato a sockaddr */\n\tsa_family_t ss_family;\n\tchar __ss_padding[128-sizeof(long)-2];\n\tunsigned long __ss_align;\n};"
                _xc_sock_src "${_xc_sock_src}")
            file(WRITE "${_xc_sock}" "${_xc_sock_src}")
        endif()
    endif()
endif()

# CMAKE_EXE_LINKER_FLAGS è in cache: i --wrap nuovi non arrivano da FLAGS_INIT.
foreach(_xc_wrap usrsctp_init juice_create mbedtls_entropy_init mbedtls_ctr_drbg_seed pthread_rwlockattr_init pthread_rwlockattr_destroy pthread_rwlock_init pthread_rwlock_destroy pthread_rwlock_rdlock pthread_rwlock_wrlock pthread_rwlock_unlock pthread_rwlock_tryrdlock pthread_mutexattr_init pthread_mutexattr_destroy pthread_mutexattr_settype pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock pthread_mutex_unlock pthread_mutex_trylock pthread_cond_init pthread_cond_destroy pthread_cond_wait pthread_cond_timedwait pthread_cond_signal pthread_cond_broadcast getifaddrs freeifaddrs getentropy getrandom getauxval prctl sched_getaffinity sched_setaffinity pthread_getaffinity_np pthread_setaffinity_np if_nametoindex if_indextoname open fopen close setsockopt clock_gettime random srandom abort pthread_mutex_timedlock pthread_rwlock_timedrdlock pthread_rwlock_timedwrlock sem_timedwait sem_wait scePthreadCondTimedwait scePthreadCondWait pthread_setschedparam pthread_getschedparam pthread_attr_setschedparam pthread_attr_setschedpolicy sched_setscheduler nanosleep read write recvfrom sendto sendmsg recvmsg connect poll bind getdents usrsctp_socket usrsctp_bind usrsctp_connect usrsctp_setsockopt usrsctp_getsockopt exit)
    if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "wrap=${_xc_wrap}([^A-Za-z0-9_]|$)")
        string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=${_xc_wrap}")
    endif()
endforeach()
# --wrap=sysconf tiene nel binario il sysconf di musl (e quindi cpuset_getaffinity)
# anche se nessuno chiama __real_sysconf. Il simbolo sysconf è il nostro.
string(REPLACE " --wrap=sysconf" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

# FIONBIO per libjuice e per la sonda di rete (src/platform/ps5/net_probe.cpp).
# Gli header in include/ sono solo quelli del kernel FreeBSD che usrsctp include
# e che la libc musl di OpenOrbis non ha (sys/mutex.h, net/if_types.h).
add_compile_options(
    -include "${CMAKE_SOURCE_DIR}/src/platform/ps4/ps4_compat.h"
    -isystem "${CMAKE_SOURCE_DIR}/src/platform/ps4/include")

set(XC_BUILTIN_CA "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_ca.cpp")
add_custom_command(
    OUTPUT "${XC_BUILTIN_CA}"
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/assets/cacert.pem -DOUT=${XC_BUILTIN_CA}
            -DNAME=xc_builtin_ca_bundle -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
    DEPENDS assets/cacert.pem cmake/embed_file.cmake
    COMMENT "Incorporo assets/cacert.pem")

add_library(xcloud_core STATIC
    ${XC_BUILTIN_CA}
    src/util/json.cpp
    src/util/log.cpp
    src/net/http.cpp
    src/auth/msa.cpp
    src/auth/xbox_live.cpp
    src/auth/auth_manager.cpp
    src/xcloud/gssv.cpp
    src/xcloud/catalog.cpp
    src/xcloud/titlehub.cpp
    src/xcloud/regions.cpp
    src/xcloud/prices.cpp
    src/xcloud/social.cpp
    src/platform/posix_common.cpp
    src/platform/ps4/platform_ps4.cpp
    src/platform/ps4/libc_compat.c
    src/platform/ps4/entropy_ps4.c
    src/platform/ps5/net_probe.cpp
)
target_include_directories(xcloud_core PUBLIC src)
target_compile_definitions(xcloud_core PUBLIC XCLOUD_PS4=1)
target_compile_options(xcloud_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function)

include(cmake/deps/mbedtls.cmake)
target_link_libraries(xcloud_core PUBLIC xc_mbedtls)
include(cmake/deps/libdatachannel.cmake)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus ${CMAKE_BINARY_DIR}/opus EXCLUDE_FROM_ALL)

add_library(xcloud_stream STATIC
    src/stream/h264_receiver.cpp
    src/stream/input_packet.cpp
    src/stream/rtcp_reporter.cpp
    src/stream/recv_diag.cpp
    src/stream/stream_session.cpp
    src/app/stream_trace.cpp
)
target_link_libraries(xcloud_stream PUBLIC xcloud_core PRIVATE datachannel-static)
target_compile_options(xcloud_stream PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(xcloud_stream PUBLIC XCLOUD_PS4=1)

add_library(xcloud_ui STATIC
    src/ui/strings.cpp
    src/ui/canvas.cpp
    src/ui/font.cpp
    src/ui/image_cache.cpp
    src/ui/app_ui.cpp
    src/ui/stream_menu.cpp
    src/ui/pad_icons.cpp
    src/ui/accent_color.cpp
    src/ui/brand.cpp
    src/app/library.cpp
    src/app/settings.cpp
    src/app/update_check.cpp
    src/app/report.cpp
    src/app/crash_guard.cpp
    src/app/session_stats.cpp
    src/app/updater.cpp
    src/app/auto_deband.cpp
    extern/qrcodegen/qrcodegen.c
)
target_compile_definitions(xcloud_ui PUBLIC XC_APP_VERSION="${PROJECT_VERSION}-ps4" XCLOUD_PS4=1)
target_include_directories(xcloud_ui PUBLIC extern/stb extern/qrcodegen)
target_link_libraries(xcloud_ui PUBLIC xcloud_core)
target_compile_options(xcloud_ui PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# libGoldHEN_Hook: sys_sdk_jailbreak (syscall 500 del SDK ufficiale).
add_library(GoldHEN_Hook STATIC third_party/goldhen_sdk/sdk_jailbreak.c)
target_include_directories(GoldHEN_Hook PUBLIC third_party/goldhen_sdk)
set_target_properties(GoldHEN_Hook PROPERTIES OUTPUT_NAME GoldHEN_Hook)

set(XC_PAYLOAD_H "${CMAKE_BINARY_DIR}/generated/psbox_install_bin.h")
add_custom_command(
    OUTPUT ${XC_PAYLOAD_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/embed_payload.sh ${XC_PAYLOAD_H}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/payload/psbox_install.c
            ${CMAKE_SOURCE_DIR}/src/payload/install_plan.h
            ${CMAKE_SOURCE_DIR}/src/payload/payload.ld
            ${CMAKE_SOURCE_DIR}/scripts/embed_payload.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Incorporo lo shellcode BinLoader"
    VERBATIM
)
set_source_files_properties(${XC_PAYLOAD_H} PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)

add_executable(psbox.elf
    ${XC_PAYLOAD_H}
    src/app/ps5_main.cpp
    src/app/lifecycle.cpp
    src/app/local_update.cpp
    src/app/screenshot.cpp
    src/app/crash_log.cpp
    src/app/worker.cpp
    src/app/stream_screen.cpp
    src/app/price_loop.cpp
    src/app/autoplay.cpp
    src/app/autostart.cpp
    src/app/stream_player.cpp
    src/display/display_ps4.cpp
    src/input/controller_ps4.cpp
    src/media/audio_out.cpp
    src/media/audio_in.cpp
    src/media/hw_decoder_ps4.cpp
    src/media/media_stub_ps4.cpp
    src/platform/ps4/audio_log.cpp
    third_party/moonlight-ps4/src/audio/audio_orbis.c
    third_party/moonlight-ps4/src/orbis/videodec2_loader.c
    third_party/moonlight-ps4/src/video/nv12_blit.c
)
target_include_directories(psbox.elf PRIVATE
    ${CMAKE_BINARY_DIR}/generated
    third_party/moonlight-ps4/include
    third_party/moonlight-ps4/src
    third_party/moonlight-ps4/src/audio
    extern/stb)
target_compile_definitions(psbox.elf PRIVATE XCLOUD_PS4=1)
target_compile_options(psbox.elf PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# L'eboot deve restare nello stesso blocco da 64 KiB del pkg 01.21 (9764864).
set_source_files_properties(
    src/app/local_update.cpp
    src/app/stream_player.cpp
    src/app/worker.cpp
    src/media/hw_decoder_ps4.cpp
    PROPERTIES COMPILE_OPTIONS "-Os")
# Stesso insieme di tools/ps5/link.sh. I percorsi dei .a (non i target) evitano
# il ciclo che CMake vede se mbedTLS sta sia nel gruppo sia fra le dipendenze.
# --start-group perché libjuice e libsrtp citano mbedTLS, che il linker
# altrimenti ha già superato.
add_dependencies(psbox.elf datachannel-static juice-static srtp2 usrsctp mbedtls mbedx509 mbedcrypto)
target_link_libraries(psbox.elf PRIVATE xcloud_stream xcloud_ui opus GoldHEN_Hook
    --start-group
    $<TARGET_FILE:datachannel-static>
    $<TARGET_FILE:juice-static>
    $<TARGET_FILE:srtp2>
    $<TARGET_FILE:usrsctp>
    $<TARGET_FILE:mbedtls>
    $<TARGET_FILE:mbedx509>
    $<TARGET_FILE:mbedcrypto>
    --end-group
    -lSceNet -lSceNetCtl -lScePad -lSceUserService -lSceAudioOut -lSceAudioIn
    -lSceVideoOut -lSceSystemService -lSceSysmodule -lSceCommonDialog
    -lSceAppInstUtil -lSceBgft -lSceLncUtil)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/eboot.bin
    COMMAND ${OO_HOST_TOOLS}/create-fself
            -in=$<TARGET_FILE:psbox.elf>
            -out=${CMAKE_BINARY_DIR}/psbox.oelf
            --eboot ${CMAKE_BINARY_DIR}/eboot.bin
            --paid 0x3800000000000011
    DEPENDS psbox.elf
    COMMENT "Genero eboot.bin"
    VERBATIM
)
add_custom_target(eboot ALL DEPENDS ${CMAKE_BINARY_DIR}/eboot.bin)

add_custom_target(pkg
    COMMAND ${CMAKE_COMMAND} -E env OO_PS4_TOOLCHAIN=${OO_PS4_TOOLCHAIN}
            ${CMAKE_SOURCE_DIR}/scripts/make_pkg.sh
            ${CMAKE_BINARY_DIR}/eboot.bin ${CMAKE_SOURCE_DIR}/dist
    DEPENDS ${CMAKE_BINARY_DIR}/eboot.bin
    COMMENT "Impacchetto il .pkg"
    VERBATIM
)
