# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 RafaelNGP
# libdatachannel (WebRTC: ICE via libjuice, DTLS-SRTP, SCTP data channels)
# built static on top of the vendored mbedTLS. Include after mbedtls.cmake.

set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_WARNINGS OFF CACHE BOOL "" FORCE)

# cmake/modules/FindMbedTLS.cmake maps find_package(MbedTLS) to deps/mbedtls.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/modules")
find_package(MbedTLS REQUIRED)

# Log dei primi passi di Init/ThreadPool. deps/ non è nel git: la patch
# si riapplica dopo un clone pulito. Se è già applicata, patch -N esce 1.
set(_xc_ldc_init "${CMAKE_SOURCE_DIR}/deps/libdatachannel/src/impl/init.cpp")
if(EXISTS "${_xc_ldc_init}")
    file(READ "${_xc_ldc_init}" _xc_ldc_init_src)
    if(NOT _xc_ldc_init_src MATCHES "xc_log_flush_line")
        execute_process(
            COMMAND patch -p1 --forward --batch
            INPUT_FILE "${CMAKE_SOURCE_DIR}/patches/libdatachannel-ps4-initlog.patch"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/deps/libdatachannel"
            RESULT_VARIABLE _xc_ldc_patch
        )
        if(NOT _xc_ldc_patch EQUAL 0)
            message(FATAL_ERROR "patch libdatachannel-ps4-initlog fallita (${_xc_ldc_patch})")
        endif()
    endif()
endif()

# SRTP prima di changeState(Connected), sconn_len e sin_len. deps/ non è nel git.
set(_xc_ldc_dtls "${CMAKE_SOURCE_DIR}/deps/libdatachannel/src/impl/dtlstransport.cpp")
if(EXISTS "${_xc_ldc_dtls}")
    file(READ "${_xc_ldc_dtls}" _xc_ldc_dtls_src)
    if(NOT _xc_ldc_dtls_src MATCHES "SRTP prima dello stato")
        execute_process(
            COMMAND patch -p1 --forward --batch
            INPUT_FILE "${CMAKE_SOURCE_DIR}/patches/libdatachannel-ps4-srtp.patch"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/deps/libdatachannel"
            RESULT_VARIABLE _xc_ldc_srtp
        )
        if(NOT _xc_ldc_srtp EQUAL 0)
            message(FATAL_ERROR "patch libdatachannel-ps4-srtp fallita (${_xc_ldc_srtp})")
        endif()
        file(READ "${_xc_ldc_dtls}" _xc_ldc_dtls_src)
    endif()
    if(NOT _xc_ldc_dtls_src MATCHES "dtls: recv in coda")
        execute_process(
            COMMAND patch -p1 --forward --batch
            INPUT_FILE "${CMAKE_SOURCE_DIR}/patches/libdatachannel-ps4-dtls-log.patch"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/deps/libdatachannel"
            RESULT_VARIABLE _xc_ldc_dtls_log
        )
        if(NOT _xc_ldc_dtls_log EQUAL 0)
            message(FATAL_ERROR "patch libdatachannel-ps4-dtls-log fallita (${_xc_ldc_dtls_log})")
        endif()
    endif()
endif()

# sockaddr_conn BSD (len al byte 0, famiglia al byte 1) in libdatachannel e usrsctp.
set(_xc_ldc_sctp "${CMAKE_SOURCE_DIR}/deps/libdatachannel/src/impl/sctptransport.cpp")
if(EXISTS "${_xc_ldc_sctp}")
    file(READ "${_xc_ldc_sctp}" _xc_ldc_sctp_src)
    if(NOT _xc_ldc_sctp_src MATCHES "PSBOX_SCONN")
        execute_process(
            COMMAND patch -p1 --forward --batch
            INPUT_FILE "${CMAKE_SOURCE_DIR}/patches/libdatachannel-ps4-sconn.patch"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/deps/libdatachannel"
            RESULT_VARIABLE _xc_ldc_sconn
        )
        if(NOT _xc_ldc_sconn EQUAL 0)
            message(FATAL_ERROR "patch libdatachannel-ps4-sconn fallita (${_xc_ldc_sconn})")
        endif()
    endif()
endif()

# Solo AF_INET: Orbis rifiuta IPV6_V6ONLY e sendto con famiglia 28.
set(_xc_ldc_udp "${CMAKE_SOURCE_DIR}/deps/libdatachannel/deps/libjuice/src/udp.c")
if(EXISTS "${_xc_ldc_udp}")
    file(READ "${_xc_ldc_udp}" _xc_ldc_udp_src)
    set(_xc_ldc_udp_new "${_xc_ldc_udp_src}")
    string(REPLACE
        "const int families[2] = {AF_INET6, AF_INET}; // Prefer IPv6\n\tconst char *names[2] = {\"IPv6\", \"IPv4\"};\n\tfor (int i = 0; i < 2; ++i) {"
        "/* PSBOX_IPV4_ONLY: Orbis non ha il dual-stack (IPV6_V6ONLY e42). */\n\tconst int families[1] = {AF_INET};\n\tconst char *names[1] = {\"IPv4\"};\n\tfor (int i = 0; i < 1; ++i) {"
        _xc_ldc_udp_new "${_xc_ldc_udp_new}")
    string(REPLACE
        "hints.ai_family = AF_UNSPEC;"
        "hints.ai_family = AF_INET; /* PSBOX_IPV4_ONLY */"
        _xc_ldc_udp_new "${_xc_ldc_udp_new}")
    string(REPLACE
        "int udp_sendto(socket_t sock, const char *data, size_t size, const addr_record_t *dst) {\n#ifndef __linux__\n\taddr_record_t tmp = *dst;\n\taddr_record_t name;\n\tname.len = sizeof(name.addr);\n\tif (getsockname(sock, (struct sockaddr *)&name.addr, &name.len) == 0) {\n\t\tif (name.addr.ss_family == AF_INET6)\n\t\t\taddr_map_inet6_v4mapped(&tmp.addr, &tmp.len);\n\t} else {\n\t\tJLOG_WARN(\"getsockname failed, errno=%d\", sockerrno);\n\t}\n\treturn sendto(sock, data, (socklen_t)size, 0, (const struct sockaddr *)&tmp.addr, tmp.len);\n#else\n\treturn sendto(sock, data, size, 0, (const struct sockaddr *)&dst->addr, dst->len);\n#endif\n}"
        "int udp_sendto(socket_t sock, const char *data, size_t size, const addr_record_t *dst) {\n\t/* PSBOX_IPV4_ONLY: non riscrivere la destinazione come AF_INET6. */\n\treturn sendto(sock, data, (socklen_t)size, 0, (const struct sockaddr *)&dst->addr, dst->len);\n}"
        _xc_ldc_udp_new "${_xc_ldc_udp_new}")
    if(NOT _xc_ldc_udp_new STREQUAL _xc_ldc_udp_src)
        file(WRITE "${_xc_ldc_udp}" "${_xc_ldc_udp_new}")
    endif()
endif()
set(_xc_ldc_addr "${CMAKE_SOURCE_DIR}/deps/libdatachannel/deps/libjuice/src/addr.c")
if(EXISTS "${_xc_ldc_addr}")
    file(READ "${_xc_ldc_addr}" _xc_ldc_addr_src)
    if(NOT _xc_ldc_addr_src MATCHES "PSBOX_IPV4_ONLY")
        string(REPLACE "hints.ai_family = AF_UNSPEC;\n\thints.ai_socktype = SOCK_DGRAM;\n\thints.ai_protocol = IPPROTO_UDP;\n\thints.ai_flags = AI_ADDRCONFIG;"
            "hints.ai_family = AF_INET; /* PSBOX_IPV4_ONLY */\n\thints.ai_socktype = SOCK_DGRAM;\n\thints.ai_protocol = IPPROTO_UDP;\n\thints.ai_flags = AI_ADDRCONFIG;"
            _xc_ldc_addr_src "${_xc_ldc_addr_src}")
        string(REPLACE "if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {"
            "if (ai->ai_family == AF_INET) { /* PSBOX_IPV4_ONLY */"
            _xc_ldc_addr_src "${_xc_ldc_addr_src}")
        file(WRITE "${_xc_ldc_addr}" "${_xc_ldc_addr_src}")
    endif()
endif()

# Clock Orbis: OpenOrbis CLOCK_MONOTONIC=1 è VIRTUAL; il monotonic è 4.
set(_xc_ldc_ts "${CMAKE_SOURCE_DIR}/deps/libdatachannel/deps/libjuice/src/timestamp.c")
if(EXISTS "${_xc_ldc_ts}")
    file(READ "${_xc_ldc_ts}" _xc_ldc_ts_src)
    if(NOT _xc_ldc_ts_src MATCHES "PSBOX_CLOCK4")
        string(REPLACE
            "\tif (clock_gettime(CLOCK_MONOTONIC, &ts))\n\t\treturn 0;"
            "\t/* PSBOX_CLOCK4: OpenOrbis definisce CLOCK_MONOTONIC=1 (Linux), ma il\n\t * kernel Orbis ha il monotonic a 4 (1 è CLOCK_VIRTUAL). */\n\tif (clock_gettime((clockid_t)4, &ts) && clock_gettime(CLOCK_MONOTONIC, &ts))\n\t\treturn 0;"
            _xc_ldc_ts_src "${_xc_ldc_ts_src}")
        file(WRITE "${_xc_ldc_ts}" "${_xc_ldc_ts_src}")
    endif()
endif()

# Consent STUN: poll juice max 50 ms (altrimenti i binding scadono sotto RTP).
set(_xc_ldc_conn "${CMAKE_SOURCE_DIR}/deps/libdatachannel/deps/libjuice/src/conn_thread.c")
if(EXISTS "${_xc_ldc_conn}")
    file(READ "${_xc_ldc_conn}" _xc_ldc_conn_src)
    if(NOT _xc_ldc_conn_src MATCHES "PSBOX_POLL_CAP")
        string(REPLACE
            "\t\tif (timediff < 0)\n\t\t\ttimediff = 0;\n\n\t\tJLOG_VERBOSE(\"Entering poll for %d ms\", (int)timediff);"
            "\t\tif (timediff < 0)\n\t\t\ttimediff = 0;\n\t\tif (timediff > 50)\n\t\t\ttimediff = 50; /* PSBOX_POLL_CAP: consent STUN ogni 50 ms */\n\n\t\tJLOG_VERBOSE(\"Entering poll for %d ms\", (int)timediff);"
            _xc_ldc_conn_src "${_xc_ldc_conn_src}")
        file(WRITE "${_xc_ldc_conn}" "${_xc_ldc_conn_src}")
    endif()
endif()

# SO_RCVBUF Orbis 0x1002 (Linux 8): buffer UDP ICE, scende se 1 MiB fallisce.
if(EXISTS "${_xc_ldc_udp}")
    file(READ "${_xc_ldc_udp}" _xc_ldc_udp_src)
    if(NOT _xc_ldc_udp_src MATCHES "PSBOX_RCVBUF")
        string(REPLACE
            "\tctl_t nbio = 1;\n\tif (ioctlsocket(sock, FIONBIO, &nbio)) {"
            "\t/* PSBOX_RCVBUF: Orbis SO_RCVBUF=0x1002. 1 MiB spesso fallisce. */\n\t{\n\t\tconst int sizes[] = {256 * 1024, 128 * 1024, 64 * 1024};\n\t\tfor (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {\n\t\t\tint sz = sizes[i];\n\t\t\tif (setsockopt(sock, SOL_SOCKET, 0x1002, (const char *)&sz, sizeof(sz)) == 0) {\n\t\t\t\tsetsockopt(sock, SOL_SOCKET, 0x1001, (const char *)&sz, sizeof(sz));\n\t\t\t\tbreak;\n\t\t\t}\n\t\t}\n\t}\n\n\tctl_t nbio = 1;\n\tif (ioctlsocket(sock, FIONBIO, &nbio)) {"
            _xc_ldc_udp_src "${_xc_ldc_udp_src}")
        file(WRITE "${_xc_ldc_udp}" "${_xc_ldc_udp_src}")
    endif()
endif()

add_subdirectory(deps/libdatachannel ${CMAKE_BINARY_DIR}/libdatachannel EXCLUDE_FROM_ALL)
