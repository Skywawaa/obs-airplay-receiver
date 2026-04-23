cmake_minimum_required(VERSION 3.16)
project(airplay-stream VERSION 1.0.0 LANGUAGES C)

# ---------------------------------------------------------------
# Inherit path variables from the parent scope (set in the root
# CMakeLists.txt before add_subdirectory is called).
# Fallback definitions are provided for standalone builds.
# ---------------------------------------------------------------

if(NOT DEFINED DEPS_DIR)
    set(DEPS_DIR "${CMAKE_SOURCE_DIR}/../deps")
endif()

if(NOT DEFINED UXPLAY_BUILD_DIR)
    set(UXPLAY_BUILD_DIR "${DEPS_DIR}/uxplay-build/build")
endif()

if(NOT DEFINED UXPLAY_LIB_DIR)
    set(UXPLAY_LIB_DIR "${DEPS_DIR}/mika-obs-airplay/UxPlay/lib")
endif()

if(NOT DEFINED FFMPEG_INCLUDE_DIR)
    set(FFMPEG_INCLUDE_DIR "${DEPS_DIR}/ffmpeg7-include")
endif()

if(NOT DEFINED FFMPEG_LIB_DIR)
    set(FFMPEG_LIB_DIR "${DEPS_DIR}/obs-ffmpeg-libs")
endif()

if(NOT DEFINED LIBPLIST_DIR)
    set(LIBPLIST_DIR "${DEPS_DIR}/libplist-2.7.0")
endif()

# w32-pthreads headers: required by UxPlay headers on Windows.
# Defaults to the path inside the OBS SDK; override with -DW32_PTHREADS_DIR=...
# when building without the OBS SDK.
if(NOT DEFINED W32_PTHREADS_DIR)
    set(W32_PTHREADS_DIR "${DEPS_DIR}/obs-sdk/obs-studio-32.1.1/deps/w32-pthreads")
endif()

if(NOT DEFINED OPENSSL_DIR)
    # Default matches the root CMakeLists.txt; override via -DOPENSSL_DIR=...
    set(OPENSSL_DIR "C:/Users/kitti/scoop/apps/openssl/current")
endif()

# OpenSSL libcrypto
if(NOT DEFINED OPENSSL_CRYPTO_LIB OR NOT OPENSSL_CRYPTO_LIB)
    # Search for libcrypto by version-specific name first (newest to oldest)
    # so that whatever OpenSSL 3.x variant is installed is found automatically.
    #   OpenSSL 3.4+ ships libcrypto-4-x64.dll / libcrypto.lib
    #   OpenSSL 3.0-3.3 ships libcrypto-3-x64.dll / libcrypto.lib
    find_library(OPENSSL_CRYPTO_LIB
        NAMES libcrypto-4-x64 libcrypto-3-x64 libcrypto crypto
        PATHS
            "${OPENSSL_DIR}/lib"
            "${OPENSSL_DIR}/lib/VC"
            "${OPENSSL_DIR}/lib/VC/x64"
            "${OPENSSL_DIR}/lib/VC/x64/MD"
            "${OPENSSL_DIR}/lib/VC/x64/MT"
            "${OPENSSL_DIR}/lib/VC/x86_64"
        NO_DEFAULT_PATH)
    if(NOT OPENSSL_CRYPTO_LIB)
        find_library(OPENSSL_CRYPTO_LIB NAMES libcrypto-4-x64 libcrypto-3-x64 libcrypto crypto)
    endif()
endif()
message(STATUS "[standalone] OpenSSL crypto: ${OPENSSL_CRYPTO_LIB}")

# ---------------------------------------------------------------
# Executable target
# ---------------------------------------------------------------

add_executable(airplay-stream
    main.c
    airplay-stream.c
    ts-output.c
)

# ---------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------

target_include_directories(airplay-stream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    # UxPlay headers
    ${UXPLAY_LIB_DIR}
    ${UXPLAY_LIB_DIR}/playfair
    ${UXPLAY_LIB_DIR}/llhttp
    # libplist headers
    ${LIBPLIST_DIR}/include
    # FFmpeg headers
    ${FFMPEG_INCLUDE_DIR}
    # OpenSSL headers
    ${OPENSSL_DIR}/include
    # w32-pthreads headers (needed by UxPlay headers on Windows)
    ${W32_PTHREADS_DIR}
)

# ---------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------

# UxPlay static libs
target_link_libraries(airplay-stream PRIVATE
    ${UXPLAY_BUILD_DIR}/airplay.lib
    ${UXPLAY_BUILD_DIR}/playfair.lib
    ${UXPLAY_BUILD_DIR}/llhttp.lib
)

# libplist
target_link_libraries(airplay-stream PRIVATE
    ${LIBPLIST_DIR}/build/plist.lib
)

# OpenSSL
target_link_libraries(airplay-stream PRIVATE
    ${OPENSSL_CRYPTO_LIB}
)

# FFmpeg  (avformat added for MPEG-TS mux)
target_link_directories(airplay-stream PRIVATE ${FFMPEG_LIB_DIR})
target_link_libraries(airplay-stream PRIVATE
    avformat avcodec avutil swresample
)

# Windows system libraries
target_link_libraries(airplay-stream PRIVATE
    ws2_32 iphlpapi
    ${DEPS_DIR}/w32-pthreads.lib
    ${DEPS_DIR}/dnssd.lib
)

# ---------------------------------------------------------------
# Compile definitions / properties
# ---------------------------------------------------------------

target_compile_definitions(airplay-stream PRIVATE
    WIN32
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    LIBPLIST_STATIC
)

set_target_properties(airplay-stream PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# ---------------------------------------------------------------
# Install
# ---------------------------------------------------------------

install(TARGETS airplay-stream
    DESTINATION "$ENV{PROGRAMDATA}/obs-studio/plugins/obs-airplay-receiver/bin/64bit")
