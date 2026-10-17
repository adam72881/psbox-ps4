# Toolchain OpenOrbis PS4 v0.5.4 (clang/lld di sistema, strumenti in bin/linux).
# Derivata da Moonlight-ps4 (cmake/openorbis.cmake) e dagli esempi OpenOrbis.
# Uso: cmake -B build-ps4 -DCMAKE_TOOLCHAIN_FILE=cmake/openorbis.cmake

cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{OO_PS4_TOOLCHAIN})
    set(OPENORBIS "$ENV{OO_PS4_TOOLCHAIN}")
elseif(EXISTS "$ENV{HOME}/ps4dev/OpenOrbis/OpenOrbis/PS4Toolchain")
    set(OPENORBIS "$ENV{HOME}/ps4dev/OpenOrbis/OpenOrbis/PS4Toolchain")
elseif(EXISTS "$ENV{HOME}/ps4dev/OpenOrbis/PS4Toolchain")
    set(OPENORBIS "$ENV{HOME}/ps4dev/OpenOrbis/PS4Toolchain")
else()
    message(FATAL_ERROR "Imposta OO_PS4_TOOLCHAIN sulla SDK OpenOrbis")
endif()
set(OO_PS4_TOOLCHAIN "${OPENORBIS}" CACHE PATH "OpenOrbis PS4 Toolchain")

set(PS4 TRUE)
set(XCLOUD_PS4 TRUE)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSTEM_VERSION 12)
set(CMAKE_CROSSCOMPILING TRUE)

# Su Ubuntu i binari versionati sono ld.lld-18 / llvm-ar-18.
find_program(OO_LLD NAMES ld.lld ld.lld-18 ld.lld-19)
find_program(OO_AR NAMES llvm-ar llvm-ar-18 llvm-ar-19 ar)
find_program(OO_RANLIB NAMES llvm-ranlib llvm-ranlib-18 llvm-ranlib-19 ranlib)
if(NOT OO_LLD)
    message(FATAL_ERROR "Serve ld.lld (pacchetto lld o lld-18)")
endif()
set(CMAKE_ASM_COMPILER clang CACHE PATH "")
set(CMAKE_C_COMPILER clang CACHE PATH "")
set(CMAKE_CXX_COMPILER clang++ CACHE PATH "")
set(CMAKE_LINKER "${OO_LLD}" CACHE PATH "")
set(CMAKE_AR "${OO_AR}" CACHE PATH "")
set(CMAKE_RANLIB "${OO_RANLIB}" CACHE PATH "")
set(OO_HOST_TOOLS "${OPENORBIS}/bin/linux")

set(CMAKE_FIND_ROOT_PATH "${OPENORBIS}" "${OPENORBIS}/usr")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "Niente librerie dinamiche sulla PS4")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(PS4_COMMON_INCLUDES "-isysroot ${OPENORBIS} -isystem ${OPENORBIS}/include")
# -std=c++20 (senza estensioni GNU) definisce __STRICT_ANSI__ e musl nasconde
# locale_t, nanosleep e isascii. libc++ di OpenOrbis li richiede: _GNU_SOURCE
# li riattiva anche in modalità ANSI.
set(PS4_COMMON_FLAGS "--target=x86_64-pc-freebsd12-elf -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -D_GNU_SOURCE -DXCLOUD_PS4=1 -fPIC -funwind-tables -fno-stack-protector ${PS4_COMMON_INCLUDES}")

set(CMAKE_C_FLAGS_INIT "${PS4_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PS4_COMMON_FLAGS} -nostdinc++ -isystem ${OPENORBIS}/include/c++/v1 -fexceptions -frtti")
set(CMAKE_ASM_FLAGS_INIT "${PS4_COMMON_FLAGS}")

set(PS4_LINKER_FLAGS "-m elf_x86_64 -pie --eh-frame-hdr --script ${OPENORBIS}/link.x ${OPENORBIS}/lib/crt1.o -L${OPENORBIS}/lib --wrap=getaddrinfo --wrap=freeaddrinfo --wrap=getnameinfo --wrap=syscall --wrap=fcntl --wrap=ioctl --wrap=pthread_create --wrap=socket --wrap=usrsctp_init --wrap=juice_create --wrap=mbedtls_entropy_init --wrap=mbedtls_ctr_drbg_seed --wrap=pthread_rwlockattr_init --wrap=pthread_rwlockattr_destroy --wrap=pthread_rwlock_init --wrap=pthread_rwlock_destroy --wrap=pthread_rwlock_rdlock --wrap=pthread_rwlock_wrlock --wrap=pthread_rwlock_unlock --wrap=pthread_rwlock_tryrdlock --wrap=pthread_mutexattr_init --wrap=pthread_mutexattr_destroy --wrap=pthread_mutexattr_settype --wrap=pthread_mutex_init --wrap=pthread_mutex_destroy --wrap=pthread_mutex_lock --wrap=pthread_mutex_unlock --wrap=pthread_mutex_trylock --wrap=pthread_cond_init --wrap=pthread_cond_destroy --wrap=pthread_cond_wait --wrap=pthread_cond_timedwait --wrap=pthread_cond_signal --wrap=pthread_cond_broadcast --wrap=getifaddrs --wrap=freeifaddrs --wrap=getentropy --wrap=getrandom --wrap=getauxval --wrap=prctl --wrap=sched_getaffinity --wrap=sched_setaffinity --wrap=pthread_getaffinity_np --wrap=pthread_setaffinity_np --wrap=if_nametoindex --wrap=if_indextoname --wrap=open --wrap=fopen --wrap=setsockopt --wrap=clock_gettime --wrap=random --wrap=srandom")
# libc++ prima di libc: lld risolve da sinistra a destra.
set(PS4_BASE_LIBS "-lc++ -lc++abi -lclang_rt.builtins-x86_64 -lm -lpthread -lc -lkernel")

set(CMAKE_EXE_LINKER_FLAGS_INIT "${PS4_LINKER_FLAGS}")
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_LINKER> -o <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES> ${PS4_BASE_LIBS}")
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_LINKER> -o <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES> ${PS4_BASE_LIBS}")
