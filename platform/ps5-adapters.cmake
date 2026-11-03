# Console-only PS5 adapter build fragment. Not included by the portable root.
# Every object is compiled with the pinned prospero-clang and the exact warning
# policy required by T073/T074; source/object hashes are printed for retention.
include_guard(GLOBAL)

get_filename_component(SP5_PS5_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(SP5_PS5_SDK_DEFAULT "")
if(DEFINED ENV{PS5_PAYLOAD_SDK} AND NOT "$ENV{PS5_PAYLOAD_SDK}" STREQUAL "")
    set(SP5_PS5_SDK_DEFAULT "$ENV{PS5_PAYLOAD_SDK}")
elseif(DEFINED ENV{HOME})
    set(SP5_PS5_SDK_DEFAULT
        "$ENV{HOME}/.agents/skills/ps5-jailbreak/tools/ps5jb/upgrade-13.60-20261024/sources/ps5-payload-sdk")
endif()
set(SP5_PS5_SDK "${SP5_PS5_SDK_DEFAULT}"
    CACHE PATH "Pinned ps5-payload-sdk a7685ce8 tree")
set(SP5_PS5_LLVM_DEFAULT "/opt/homebrew/opt/llvm@18/bin/llvm-config")
if(DEFINED ENV{LLVM_CONFIG} AND NOT "$ENV{LLVM_CONFIG}" STREQUAL "")
    set(SP5_PS5_LLVM_DEFAULT "$ENV{LLVM_CONFIG}")
endif()
set(SP5_PS5_LLVM_CONFIG "${SP5_PS5_LLVM_DEFAULT}"
    CACHE FILEPATH "Pinned LLVM 18.1.8 llvm-config")
set(SP5_PS5_PROSPERO_CLANG "${SP5_PS5_SDK}/bin/prospero-clang"
    CACHE FILEPATH "Pinned prospero-clang")
set(SP5_PS5_LLVM_AR "${SP5_PS5_SDK}/bin/llvm-ar" CACHE FILEPATH "Pinned llvm-ar")

foreach(tool "${SP5_PS5_LLVM_CONFIG}" "${SP5_PS5_PROSPERO_CLANG}" "${SP5_PS5_LLVM_AR}")
    if(NOT EXISTS "${tool}")
        message(FATAL_ERROR "Console adapter tool is missing: ${tool}")
    endif()
endforeach()

execute_process(COMMAND "${SP5_PS5_LLVM_CONFIG}" --version
    OUTPUT_VARIABLE SP5_PS5_LLVM_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE SP5_PS5_LLVM_RESULT)
if(NOT SP5_PS5_LLVM_RESULT EQUAL 0 OR NOT SP5_PS5_LLVM_VERSION STREQUAL "18.1.8")
    message(FATAL_ERROR "Console adapters require LLVM 18.1.8 (found ${SP5_PS5_LLVM_VERSION})")
endif()

set(SP5_PS5_ADAPTER_FLAGS -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror)
set(SP5_PS5_ADAPTER_INCLUDE_DIRS
    "${SP5_PS5_ROOT}/core"
    "${SP5_PS5_ROOT}/platform"
    "${SP5_PS5_ROOT}/platform/capture"
    "${SP5_PS5_ROOT}/platform/encode")

function(sp5_ps5_adapter target)
    set(objects)
    foreach(source IN LISTS ARGN)
        get_filename_component(source_absolute "${source}" ABSOLUTE)
        get_filename_component(source_directory "${source_absolute}" DIRECTORY)
        get_filename_component(source_name "${source_absolute}" NAME_WE)
        set(object "${CMAKE_CURRENT_BINARY_DIR}/${target}-${source_name}.o")
        set(dependencies
            "${source_absolute}"
            "${SP5_PS5_ROOT}/core/platform.h"
            "${SP5_PS5_ROOT}/core/evidence.h"
            "${SP5_PS5_ROOT}/platform/ps5_media_global.h"
            "${SP5_PS5_ROOT}/platform/firmware/13.60.h"
            "${SP5_PS5_ROOT}/platform/ps5_media.h"
            "${SP5_PS5_ROOT}/platform/ps5_clock.h"
            "${SP5_PS5_ROOT}/platform/ps5_annexb.h"
            "${SP5_PS5_ROOT}/platform/capture/ps5.h"
            "${SP5_PS5_ROOT}/platform/encode/ps5.h"
            "${SP5_PS5_ROOT}/platform/system/ps5.h")
        if(EXISTS "${source_directory}/${source_name}.h")
            list(APPEND dependencies "${source_directory}/${source_name}.h")
        endif()
        set(includes)
        foreach(directory IN LISTS SP5_PS5_ADAPTER_INCLUDE_DIRS)
            list(APPEND includes "-I${directory}")
        endforeach()
        list(APPEND includes "-I${source_directory}")
        add_custom_command(
            OUTPUT "${object}"
            COMMAND "${CMAKE_COMMAND}" -E env "LLVM_CONFIG=${SP5_PS5_LLVM_CONFIG}"
                    "${SP5_PS5_PROSPERO_CLANG}" ${SP5_PS5_ADAPTER_FLAGS} ${includes}
                    -c "${source_absolute}" -o "${object}"
            COMMAND "${CMAKE_COMMAND}" -E sha256sum "${source_absolute}" "${object}"
            DEPENDS ${dependencies}
            COMMENT "Compiling ${source_name} with pinned prospero-clang"
            VERBATIM)
        list(APPEND objects "${object}")
    endforeach()
    add_custom_target(${target} ALL DEPENDS ${objects})

    set(archive "${CMAKE_CURRENT_BINARY_DIR}/lib${target}.a")
    add_custom_command(
        OUTPUT "${archive}"
        COMMAND "${CMAKE_COMMAND}" -E env "LLVM_CONFIG=${SP5_PS5_LLVM_CONFIG}"
                "${SP5_PS5_LLVM_AR}" rcs "${archive}" ${objects}
        COMMAND "${CMAKE_COMMAND}" -E sha256sum "${archive}"
        DEPENDS ${objects}
        COMMENT "Archiving console adapter ${target}"
        VERBATIM)
    add_custom_target(${target}_archive ALL DEPENDS "${archive}")
endfunction()
