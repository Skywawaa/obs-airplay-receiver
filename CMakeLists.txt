cmake_minimum_required(VERSION 3.16)
project(obs-airplay-receiver VERSION 1.0.0 LANGUAGES C)

# --- OBS Studio SDK ---
# Set OBS_DIR to your OBS Studio install or build directory
# e.g. cmake -DOBS_DIR="C:/Program Files/obs-studio" ..
if(NOT DEFINED OBS_DIR)
    if(EXISTS "C:/Program Files/obs-studio")
        set(OBS_DIR "C:/Program Files/obs-studio")
    elseif(EXISTS "$ENV{HOME}/obs-studio")
        set(OBS_DIR "$ENV{HOME}/obs-studio")
    endif()
endif()

# Try to find libobs via cmake package first
find_package(libobs QUIET)
if(NOT libobs_FOUND AND DEFINED OBS_DIR)
    # Manual fallback
    set(OBS_INCLUDE_DIR "${OBS_DIR}/include")
    set(OBS_LIB_DIR "${OBS_DIR}/lib")
    if(NOT EXISTS "${OBS_INCLUDE_DIR}/obs-module.h")
        # Try bin layout
        set(OBS_INCLUDE_DIR "${OBS_DIR}/include/obs")
    endif()
endif()

# --- FFmpeg ---
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET libavcodec libavutil libswscale)
endif()

if(NOT FFMPEG_FOUND)
    # Try manual FFmpeg paths
    if(DEFINED FFMPEG_DIR)
        set(FFMPEG_INCLUDE_DIRS "${FFMPEG_DIR}/include")
        set(FFMPEG_LIBRARY_DIRS "${FFMPEG_DIR}/lib")
        set(FFMPEG_LIBRARIES avcodec avutil swscale)
    endif()
endif()

# --- OpenSSL (for crypto operations) ---
find_package(OpenSSL QUIET)

# --- Plugin shared library ---
add_library(obs-airplay-receiver MODULE
    src/plugin-main.c
    src/airplay-source.c
    src/airplay/airplay-server.c
    src/airplay/airplay-mirror.c
    src/airplay/airplay-plist.c
    src/network/http-server.c
    src/network/mdns-publish.c
    src/network/net-utils.c
    src/video-decoder.c
)

target_include_directories(obs-airplay-receiver PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# OBS headers
if(libobs_FOUND)
    target_link_libraries(obs-airplay-receiver PRIVATE OBS::libobs)
else()
    if(DEFINED OBS_INCLUDE_DIR)
        target_include_directories(obs-airplay-receiver PRIVATE ${OBS_INCLUDE_DIR})
    endif()
    if(DEFINED OBS_LIB_DIR)
        target_link_directories(obs-airplay-receiver PRIVATE ${OBS_LIB_DIR})
    endif()
    target_link_libraries(obs-airplay-receiver PRIVATE obs)
endif()

# FFmpeg
if(FFMPEG_INCLUDE_DIRS)
    target_include_directories(obs-airplay-receiver PRIVATE ${FFMPEG_INCLUDE_DIRS})
endif()
if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(obs-airplay-receiver PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()
target_link_libraries(obs-airplay-receiver PRIVATE
    ${FFMPEG_LIBRARIES}
)

# OpenSSL (optional - for encrypted streams)
if(OpenSSL_FOUND)
    target_link_libraries(obs-airplay-receiver PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(obs-airplay-receiver PRIVATE HAS_OPENSSL=1)
endif()

# Platform-specific
if(WIN32)
    target_link_libraries(obs-airplay-receiver PRIVATE ws2_32 iphlpapi)
    target_compile_definitions(obs-airplay-receiver PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
elseif(APPLE)
    target_link_libraries(obs-airplay-receiver PRIVATE "-framework CoreFoundation" "-framework SystemConfiguration")
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
    PREFIX ""  # no "lib" prefix on Linux
)

# Install to OBS plugin directory
if(WIN32)
    set(OBS_PLUGIN_DEST "$ENV{APPDATA}/obs-studio/plugins/obs-airplay-receiver/bin/64bit")
    set(OBS_DATA_DEST "$ENV{APPDATA}/obs-studio/plugins/obs-airplay-receiver/data")
else()
    set(OBS_PLUGIN_DEST "$ENV{HOME}/.config/obs-studio/plugins/obs-airplay-receiver/bin/64bit")
    set(OBS_DATA_DEST "$ENV{HOME}/.config/obs-studio/plugins/obs-airplay-receiver/data")
endif()

install(TARGETS obs-airplay-receiver DESTINATION ${OBS_PLUGIN_DEST})
install(DIRECTORY data/ DESTINATION ${OBS_DATA_DEST} OPTIONAL)
