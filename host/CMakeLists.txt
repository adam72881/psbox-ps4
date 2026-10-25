# SPDX-License-Identifier: GPL-3.0-or-later
# Linux headless harness — same auth/GSSV/WebRTC/input/ack code as the PS4 app.
#   cmake -S host -B build-host -G Ninja && cmake --build build-host -j$(nproc)
cmake_minimum_required(VERSION 3.20)
project(psbox_host VERSION 1.0.51 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(PSBOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

set(XC_BUILTIN_CA "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_ca.cpp")
add_custom_command(
    OUTPUT "${XC_BUILTIN_CA}"
    COMMAND ${CMAKE_COMMAND} -DIN=${PSBOX_ROOT}/assets/cacert.pem -DOUT=${XC_BUILTIN_CA}
            -DNAME=xc_builtin_ca_bundle -P ${PSBOX_ROOT}/cmake/embed_file.cmake
    DEPENDS ${PSBOX_ROOT}/assets/cacert.pem ${PSBOX_ROOT}/cmake/embed_file.cmake
    COMMENT "Embed CA bundle")

# --- mbedTLS (vendored) -------------------------------------------------------
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${PSBOX_ROOT}/cmake/deps/mbedtls_user_config.h" CACHE FILEPATH "" FORCE)
add_subdirectory(${PSBOX_ROOT}/deps/mbedtls ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)
add_library(xc_mbedtls INTERFACE)
target_link_libraries(xc_mbedtls INTERFACE mbedtls mbedx509 mbedcrypto)
target_include_directories(xc_mbedtls INTERFACE ${PSBOX_ROOT}/deps/mbedtls/include)

# --- libdatachannel (vendored) ------------------------------------------------
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_WARNINGS OFF CACHE BOOL "" FORCE)
list(PREPEND CMAKE_MODULE_PATH "${PSBOX_ROOT}/cmake/modules")
# FindMbedTLS.cmake uses ${CMAKE_SOURCE_DIR}/deps/mbedtls/include. Provide a
# shim that redirects to PSBOX_ROOT without mutating CMAKE_SOURCE_DIR.
file(WRITE "${CMAKE_BINARY_DIR}/FindMbedTLS.cmake" "
if(NOT TARGET xc_mbedtls)
  message(FATAL_ERROR \"xc_mbedtls missing\")
endif()
set(MbedTLS_FOUND TRUE)
set(MBEDTLS_FOUND TRUE)
set(MbedTLS_VERSION 3.6.2)
set(MBEDTLS_INCLUDE_DIRS \"${PSBOX_ROOT}/deps/mbedtls/include\")
set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
foreach(c MbedTLS MbedX509 MbedCrypto)
  if(NOT TARGET MbedTLS::\${c})
    add_library(MbedTLS::\${c} INTERFACE IMPORTED GLOBAL)
  endif()
endforeach()
set_target_properties(MbedTLS::MbedTLS PROPERTIES INTERFACE_LINK_LIBRARIES \"mbedtls;mbedx509;mbedcrypto\")
set_target_properties(MbedTLS::MbedX509 PROPERTIES INTERFACE_LINK_LIBRARIES \"mbedx509;mbedcrypto\")
set_target_properties(MbedTLS::MbedCrypto PROPERTIES INTERFACE_LINK_LIBRARIES \"mbedcrypto\")
")
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_BINARY_DIR}")
find_package(MbedTLS REQUIRED)
# Host must NOT use the PS4-patched deps/libdatachannel (sconn_len layout).
# Use a clean v0.23.1 tree (same version) prepared at configure time.
set(LDC_HOST "/tmp/libdatachannel-host")
if(NOT EXISTS "${LDC_HOST}/src/impl/sctptransport.cpp")
    message(FATAL_ERROR "Missing ${LDC_HOST}. Clone libdatachannel v0.23.1 with submodules.")
endif()
add_subdirectory(${LDC_HOST} ${CMAKE_BINARY_DIR}/libdatachannel EXCLUDE_FROM_ALL)

add_library(xcloud_core STATIC
    ${XC_BUILTIN_CA}
    ${PSBOX_ROOT}/src/util/json.cpp
    ${PSBOX_ROOT}/src/util/log.cpp
    ${PSBOX_ROOT}/src/net/http.cpp
    ${PSBOX_ROOT}/src/auth/msa.cpp
    ${PSBOX_ROOT}/src/auth/xbox_live.cpp
    ${PSBOX_ROOT}/src/auth/auth_manager.cpp
    ${PSBOX_ROOT}/src/xcloud/gssv.cpp
    ${PSBOX_ROOT}/src/xcloud/catalog.cpp
    ${PSBOX_ROOT}/src/xcloud/titlehub.cpp
    ${PSBOX_ROOT}/src/xcloud/regions.cpp
    ${PSBOX_ROOT}/src/xcloud/prices.cpp
    ${PSBOX_ROOT}/src/xcloud/social.cpp
    ${PSBOX_ROOT}/src/platform/posix_common.cpp
    ${PSBOX_ROOT}/src/platform/host/platform_host.cpp
)
target_include_directories(xcloud_core PUBLIC ${PSBOX_ROOT}/src)
target_compile_options(xcloud_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(xcloud_core PUBLIC Threads::Threads xc_mbedtls)

add_library(xcloud_stream STATIC
    ${PSBOX_ROOT}/src/stream/h264_receiver.cpp
    ${PSBOX_ROOT}/src/stream/input_packet.cpp
    ${PSBOX_ROOT}/src/stream/rtcp_reporter.cpp
    ${PSBOX_ROOT}/src/stream/stream_session.cpp
)
target_link_libraries(xcloud_stream PUBLIC xcloud_core PRIVATE datachannel-static)
target_compile_options(xcloud_stream PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_library(xcloud_media STATIC ${PSBOX_ROOT}/src/media/decoder.cpp)
target_link_libraries(xcloud_media PUBLIC xcloud_core
    PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWRESAMPLE)

add_executable(psbox-harness ${PSBOX_ROOT}/src/app/harness_main.cpp crash_stub.cpp)
target_link_libraries(psbox-harness PRIVATE xcloud_stream xcloud_media)
target_compile_definitions(psbox-harness PRIVATE XC_APP_VERSION="${PROJECT_VERSION}-host")
target_compile_options(psbox-harness PRIVATE -Wall -Wextra -Wno-unused-parameter)
