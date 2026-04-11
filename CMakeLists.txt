cmake_minimum_required(VERSION 3.16)
project(obs-airplay-receiver VERSION 1.0.0 LANGUAGES C)

# --- Paths (auto-detected for this machine) ---
set(DEPS_DIR "${CMAKE_SOURCE_DIR}/deps")

# OBS headers from source
set(OBS_SRC_DIR "${DEPS_DIR}/obs-sdk/obs-studio-32.1.1")
set(OBS_LIB_FILE "${DEPS_DIR}/obs.lib")

# FFmpeg dev (matching OBS's FFmpeg version - 7.x)
set(FFMPEG_INCLUDE_DIR "${DEPS_DIR}/ffmpeg7-include")
set(FFMPEG_LIB_DIR "${DEPS_DIR}/obs-ffmpeg-libs")

# Allow overrides
if(DEFINED OBS_DIR)
    set(OBS_SRC_DIR "${OBS_DIR}")
endif()
if(DEFINED FFMPEG_DIR)
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_DIR}/lib")
endif()

# --- Plugin shared library ---
add_library(obs-airplay-receiver MODULE
    src/plugin-main.c
    src/airplay-source.c
    src/airplay/airplay-server.c
    src/airplay/airplay-mirror.c
    src/airplay/airplay-plist.c
    src/airplay/fairplay.c
    src/network/http-server.c
    src/network/mdns-publish.c
    src/network/net-utils.c
    src/video-decoder.c
    src/audio-decoder.c
)

# --- Include directories ---
target_include_directories(obs-airplay-receiver PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    # OBS headers
    ${OBS_SRC_DIR}/libobs
    ${OBS_SRC_DIR}/UI/obs-frontend-api
    # w32-pthreads (OBS dependency on Windows)
    ${OBS_SRC_DIR}/deps/w32-pthreads
    # FFmpeg headers (matching OBS's FFmpeg 7.x)
    ${FFMPEG_INCLUDE_DIR}
)

# --- Link libraries ---
# OBS
target_link_libraries(obs-airplay-receiver PRIVATE ${OBS_LIB_FILE})

# FFmpeg (using OBS's FFmpeg 7.x import libs)
target_link_directories(obs-airplay-receiver PRIVATE ${FFMPEG_LIB_DIR})
target_link_libraries(obs-airplay-receiver PRIVATE
    avcodec
    avutil
    swscale
    swresample
)

# Platform-specific
if(WIN32)
    target_link_libraries(obs-airplay-receiver PRIVATE
        ws2_32 iphlpapi
        ${DEPS_DIR}/w32-pthreads.lib
        ${DEPS_DIR}/dnssd.lib
    )
    target_compile_definitions(obs-airplay-receiver PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
elseif(APPLE)
    target_link_libraries(obs-airplay-receiver PRIVATE
        "-framework CoreFoundation"
        "-framework SystemConfiguration"
    )
elseif(UNIX)
    target_link_libraries(obs-airplay-receiver PRIVATE pthread)
endif()

target_compile_definitions(obs-airplay-receiver PRIVATE
    PLUGIN_VERSION="${PROJECT_VERSION}"
)

# C11
set_target_properties(obs-airplay-receiver PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    PREFIX ""
)

# Install to OBS plugin directory
if(WIN32)
    # OBS on Windows scans %PROGRAMDATA%/obs-studio/plugins/, not %APPDATA%
    set(OBS_PLUGIN_DEST "$ENV{PROGRAMDATA}/obs-studio/plugins/obs-airplay-receiver/bin/64bit")
    set(OBS_DATA_DEST "$ENV{PROGRAMDATA}/obs-studio/plugins/obs-airplay-receiver/data")
else()
    set(OBS_PLUGIN_DEST "$ENV{HOME}/.config/obs-studio/plugins/obs-airplay-receiver/bin/64bit")
    set(OBS_DATA_DEST "$ENV{HOME}/.config/obs-studio/plugins/obs-airplay-receiver/data")
endif()

install(TARGETS obs-airplay-receiver DESTINATION ${OBS_PLUGIN_DEST})
install(DIRECTORY data/ DESTINATION ${OBS_DATA_DEST} OPTIONAL)
