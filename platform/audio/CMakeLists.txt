cmake_minimum_required(VERSION 3.25)
project(sunshine_ps5_audio_adapter LANGUAGES NONE)

# Standalone console-only target. Never add_subdirectory() this from the
# portable root; the fragment rejects non-pinned toolchains explicitly.
include("${CMAKE_CURRENT_LIST_DIR}/../ps5-adapters.cmake")
sp5_ps5_adapter(sp5_ps5_audio_adapter
    "${CMAKE_CURRENT_LIST_DIR}/ps5.c"
    "${CMAKE_CURRENT_LIST_DIR}/../ps5_media_global.c"
    "${CMAKE_CURRENT_LIST_DIR}/../firmware/13.60.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../core/runtime.c")
