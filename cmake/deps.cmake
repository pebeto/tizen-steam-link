# Third-party sources, pinned by hash. Nothing here is installed system-wide.
include(FetchContent)

if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()

# ihslib: Steam Remote Play protocol. We build it with our own target (cmake/ihslib.cmake),
# so SOURCE_SUBDIR points at a directory without a CMakeLists.txt to only download it.
#
# Pinned to the head of ihslib PR #4 (f68de5f): upstream master 1881b9a plus the PIN-masked
# X25519 key exchange that current Steam requires for pairing. Without it Steam answers
# k_ERemoteDeviceAuthorizationFailed once the PIN is typed. GitHub serves PR commits from
# the upstream repository.
# patches/ihslib: two threading fixes, a callback signature fix that only matters on
# Emscripten (the timer thread), fixes for packet loss (cumulative ACKs on the control
# channel, data channels getting past a lost packet, skipping a lost frame at once), stream
# statistics, video FEC, the quality and bitrate settings, the codecs and resolution the
# client asks for, 64-bit packet timestamps (they overflowed on WebAssembly), lost audio
# frames reported to the app, the host's MAC address, 250 Hz HID polling, a timeout for a
# silent host, frame rates above 59.94, a streaming request that can ask for Big Picture, a
# use-after-free when an ACK meets its retransmit timer, data channels that start over after a
# gap longer than their window, HDR, a race in the silence check, size checks for unconnected
# packets that hold on 32 bits, FEC allocation failures and empty audio codec data, and an adaptive
# bitrate; each patch starts with a description.
# (0003, logging for the pairing request, was dropped once pairing worked on the TV.)
set(TSL_IHSLIB_PATCHES ${CMAKE_CURRENT_LIST_DIR}/../patches/ihslib)
FetchContent_Declare(ihslib
        URL https://github.com/mariotaku/ihslib/archive/f68de5f07e0c27ccea9f47f9f5e38fd92c99919e.tar.gz
        URL_HASH SHA256=a4bd258835a9689929fc08aa68146f2a13f079cb50e0d59cf91c73e2690ea767
        PATCH_COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0001-control-send-lock.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0002-client-task-start-lock-order.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0004-timer-thread-worker-signature.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0005-control-cumulative-ack.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0006-data-channel-loss-recovery.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0007-data-channel-skip-lost-frames.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0008-session-stream-stats.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0009-unreliable-fec.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0010-quality-and-bitrate.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0011-client-video-caps.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0012-packet-timestamp-overflow.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0013-audio-lost-frames.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0014-host-mac-address.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0015-hid-poll-250hz.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0016-session-host-silence-timeout.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0017-max-framerate.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0018-streaming-request-interface.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0019-retransmission-cancel-race.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0020-data-window-gap.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0021-hdr.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0022-silence-check-race.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0023-unconnected-size-checks.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0024-fec-allocation-failure.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0025-audio-empty-codec-data.patch
              COMMAND patch -p1 --forward -i ${TSL_IHSLIB_PATCHES}/0026-adaptive-bitrate.patch
        SOURCE_SUBDIR no-cmake)

# protobuf-c runtime only; the generated .pb-c.c files ship with ihslib.
FetchContent_Declare(protobufc
        URL https://github.com/protobuf-c/protobuf-c/releases/download/v1.5.2/protobuf-c-1.5.2.tar.gz
        URL_HASH SHA256=e2c86271873a79c92b58fef7ebf8de1aa0df4738347a8bd5d4e65a80a16d0d24
        SOURCE_SUBDIR no-cmake)

# mbedTLS 3.6 LTS. ihslib uses the 2.x/3.x crypto API, which 4.x removed.
FetchContent_Declare(mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
        URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)

FetchContent_Declare(opus
        URL https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
        URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)

set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_DEEP_PLC OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
if (EMSCRIPTEN)
    set(OPUS_FIXED_POINT ON CACHE BOOL "" FORCE)
    set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
    set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
    set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
endif ()

FetchContent_MakeAvailable(ihslib protobufc mbedtls opus)

# CMake before 3.24 (the toolchain image has 3.22) keeps the archive's timestamps when it
# extracts, so after a version bump the new sources can look older than the objects built
# from the previous ones and never get recompiled. Touch the sources whenever this file
# (and so possibly a pin) changes.
if (CMAKE_VERSION VERSION_LESS 3.24)
    foreach (dep ihslib protobufc mbedtls opus)
        FetchContent_GetProperties(${dep} SOURCE_DIR dep_src)
        file(READ ${CMAKE_CURRENT_LIST_FILE} deps_text)
        string(MD5 dep_pin "${deps_text}")
        set(stamp ${dep_src}/.tsl-extracted)
        set(previous "")
        if (EXISTS ${stamp})
            file(READ ${stamp} previous)
        endif ()
        if (NOT previous STREQUAL dep_pin)
            file(GLOB_RECURSE dep_files ${dep_src}/*)
            file(TOUCH_NOCREATE ${dep_files})
            file(WRITE ${stamp} "${dep_pin}")
        endif ()
    endforeach ()
endif ()

foreach (target mbedcrypto mbedx509 mbedtls)
    target_compile_definitions(${target} PUBLIC
            "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_LIST_DIR}/mbedtls_user_config.h\"")
endforeach ()
if (EMSCRIPTEN)
    set(TSL_ENTROPY_SRC ${CMAKE_CURRENT_LIST_DIR}/../src/platform/mbedtls_entropy_wasm.c)
    target_sources(mbedcrypto PRIVATE ${TSL_ENTROPY_SRC})
    # mbedTLS builds as -std=c99; EM_ASM needs the GNU dialect.
    set_source_files_properties(${TSL_ENTROPY_SRC} TARGET_DIRECTORY mbedcrypto PROPERTIES COMPILE_FLAGS -std=gnu11)
endif ()

add_library(protobuf-c STATIC ${protobufc_SOURCE_DIR}/protobuf-c/protobuf-c.c)
target_include_directories(protobuf-c SYSTEM PUBLIC ${protobufc_SOURCE_DIR})
