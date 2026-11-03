cmake_minimum_required(VERSION 3.25)

project(SunshinePS5 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
    message(FATAL_ERROR "Portable builds require Clang, AppleClang, or GCC with warnings as errors")
endif()

# Console adapters and SDK import libraries must be attached to separate targets.
add_library(sunshine_core STATIC core/runtime.c)
target_sources(sunshine_core PRIVATE core/settings/host.c)
target_sources(sunshine_core PRIVATE core/protocol/http.c core/protocol/http_parser.c)
target_sources(sunshine_core PRIVATE core/protocol/pairing.c)
target_sources(sunshine_core PRIVATE core/protocol/discovery.c)
target_sources(sunshine_core PRIVATE core/protocol/rtsp.c core/protocol/negotiation.c)
target_sources(sunshine_core PRIVATE core/protocol/address.c core/session/session.c
    core/protocol/pairing_state.c)
target_sources(sunshine_core PRIVATE core/settings/store.c core/settings/devices.c)
set_source_files_properties(platform/system/storage.c PROPERTIES
    COMPILE_DEFINITIONS "_POSIX_C_SOURCE=200809L;_DARWIN_C_SOURCE")
add_library(SunshinePS5::core ALIAS sunshine_core)
target_compile_features(sunshine_core PUBLIC c_std_11)
target_compile_options(sunshine_core PUBLIC -Wall -Wextra -Wpedantic -Werror)
target_include_directories(sunshine_core PUBLIC "${PROJECT_SOURCE_DIR}/core")
add_library(sunshine_posix_storage STATIC platform/system/storage.c)
target_link_libraries(sunshine_posix_storage PUBLIC SunshinePS5::core)

# Crypto is built locally from the manifest's verified archive; never substitute
# a system OpenSSL whose version/provenance can drift independently.
include(ExternalProject)
file(READ "${PROJECT_SOURCE_DIR}/cmake/dependencies.lock.json" dependency_lock)
string(JSON openssl_url GET "${dependency_lock}" dependencies openssl source_archive url)
string(JSON openssl_sha GET "${dependency_lock}" dependencies openssl source_archive sha256)
string(JSON openssl_patch GET "${dependency_lock}" dependencies openssl ps5_probe_build patches 0 unified_diff)
string(JSON openssl_patch_sha GET "${dependency_lock}" dependencies openssl ps5_probe_build patches 0 sha256)
string(SHA256 actual_patch_sha "${openssl_patch}")
if(NOT actual_patch_sha STREQUAL openssl_patch_sha)
    message(FATAL_ERROR "OpenSSL metadata patch checksum mismatch")
endif()
file(WRITE "${PROJECT_BINARY_DIR}/openssl-metadata.patch" "${openssl_patch}")
find_package(Perl REQUIRED)
find_program(SP5_MAKE_EXECUTABLE make REQUIRED)
find_program(SP5_PATCH_EXECUTABLE patch REQUIRED)
set(openssl_directory "${PROJECT_BINARY_DIR}/dependencies/openssl")
file(MAKE_DIRECTORY "${openssl_directory}/include")
ExternalProject_Add(sp5_openssl_build
    URL "${openssl_url}" URL_HASH "SHA256=${openssl_sha}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${openssl_directory}"
    BUILD_IN_SOURCE TRUE
    PATCH_COMMAND "${SP5_PATCH_EXECUTABLE}" -p1 -i "${PROJECT_BINARY_DIR}/openssl-metadata.patch"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}" "TERM=xterm-256color"
        "${PERL_EXECUTABLE}" <SOURCE_DIR>/Configure no-shared no-dso no-module no-tests
        no-asm no-secure-memory no-devcryptoeng --with-rand-seed=os --libdir=lib -Werror
    BUILD_COMMAND "${SP5_MAKE_EXECUTABLE}" -j4 build_libs
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${openssl_directory}/libcrypto.a" "${openssl_directory}/libssl.a"
)
add_library(sp5_crypto STATIC IMPORTED GLOBAL)
set_target_properties(sp5_crypto PROPERTIES
    IMPORTED_LOCATION "${openssl_directory}/libcrypto.a"
    INTERFACE_INCLUDE_DIRECTORIES "${openssl_directory}/include")
add_dependencies(sp5_crypto sp5_openssl_build)
add_library(sp5_tls STATIC IMPORTED GLOBAL)
set_target_properties(sp5_tls PROPERTIES
    IMPORTED_LOCATION "${openssl_directory}/libssl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${openssl_directory}/include"
    INTERFACE_LINK_LIBRARIES sp5_crypto)
add_dependencies(sp5_tls sp5_openssl_build)
# The selected Moonlight ENet fork is built separately, from the exact lock
# archive and audited unused-parameter patch. Upstream's -Wno-error is never used.
string(JSON enet_url GET "${dependency_lock}" dependencies enet source_archive url)
string(JSON enet_sha GET "${dependency_lock}" dependencies enet source_archive sha256)
string(JSON enet_patch_count LENGTH "${dependency_lock}" dependencies enet patches)
math(EXPR enet_patch_last "${enet_patch_count} - 1")
set(enet_patches "")
foreach(enet_patch_index RANGE 0 ${enet_patch_last})
    string(JSON enet_patch GET "${dependency_lock}" dependencies enet patches ${enet_patch_index} unified_diff)
    string(JSON enet_patch_sha GET "${dependency_lock}" dependencies enet patches ${enet_patch_index} sha256)
    string(SHA256 actual_enet_patch_sha "${enet_patch}")
    if(NOT actual_enet_patch_sha STREQUAL enet_patch_sha)
        message(FATAL_ERROR "ENet patch ${enet_patch_index} checksum mismatch")
    endif()
    string(APPEND enet_patches "${enet_patch}")
endforeach()
file(WRITE "${PROJECT_BINARY_DIR}/enet-portable.patch" "${enet_patches}")
set(enet_directory "${PROJECT_BINARY_DIR}/dependencies/enet")
file(MAKE_DIRECTORY "${enet_directory}/include" "${PROJECT_BINARY_DIR}/enet-portable")
file(WRITE "${PROJECT_BINARY_DIR}/enet-portable/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.25)
project(sp5_enet_dependency LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_library(enet STATIC)
foreach(unit callbacks compress host list packet peer protocol unix)
    target_sources(enet PRIVATE "${SP5_ENET_SOURCE}/${unit}.c")
endforeach()
target_include_directories(enet PRIVATE "${SP5_ENET_SOURCE}/include")
# Upstream protocol.c computes a header offset as &((ENetProtocolHeader *)0)->sentTime,
# which UBSan's null check reports as member access through a null pointer. It is a
# constant offset computation, so exempt only that check for this dependency.
target_compile_options(enet PRIVATE -Wall -Wextra -Wpedantic -Werror -fno-sanitize=null)
target_compile_definitions(enet PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE=1
    _DARWIN_C_SOURCE=1 HAS_FCNTL=1 HAS_IOCTL=1 HAS_POLL=1 HAS_GETADDRINFO=1
    HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
]=])
ExternalProject_Add(sp5_enet_build
    URL "${enet_url}" URL_HASH "SHA256=${enet_sha}"
    DOWNLOAD_DIR "${PROJECT_BINARY_DIR}/downloads/enet"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${enet_directory}"
    PATCH_COMMAND "${SP5_PATCH_EXECUTABLE}" -p1 -i "${PROJECT_BINARY_DIR}/enet-portable.patch"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -S "${PROJECT_BINARY_DIR}/enet-portable"
        -B "${PROJECT_BINARY_DIR}/enet-build" "-DSP5_ENET_SOURCE=<SOURCE_DIR>"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}/enet-build" --parallel 4
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${PROJECT_BINARY_DIR}/enet-build/libenet.a")
add_library(sp5_enet STATIC IMPORTED GLOBAL)
set_target_properties(sp5_enet PROPERTIES
    IMPORTED_LOCATION "${PROJECT_BINARY_DIR}/enet-build/libenet.a"
    INTERFACE_INCLUDE_DIRECTORIES "${enet_directory}/include")
add_dependencies(sp5_enet sp5_enet_build)
target_link_libraries(sunshine_core PUBLIC sp5_enet)
target_sources(sunshine_core PRIVATE core/protocol/control.c)
target_sources(sunshine_core PRIVATE core/protocol/video.c core/protocol/fec.c)
target_sources(sunshine_core PRIVATE core/protocol/audio.c)
target_sources(sunshine_core PRIVATE core/input/controller.c)
find_package(Threads REQUIRED)
target_link_libraries(sunshine_core PUBLIC sp5_tls Threads::Threads ${CMAKE_DL_LIBS})

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "Portable runtime only: no streaming host or PS5 artifact is implemented")

target_sources(sunshine_core PRIVATE core/metrics/records.c)
target_sources(sunshine_core PRIVATE core/metrics/coverage.c)
target_sources(sunshine_core PRIVATE core/metrics/aggregate.c)
target_sources(sunshine_core PRIVATE core/metrics/engine.c)
target_sources(sunshine_core PRIVATE core/metrics/evidence_export.c)
target_sources(sunshine_core PRIVATE core/session/lifecycle.c)

target_sources(sunshine_core PRIVATE core/web/json.c core/web/server.c core/web/status.c
    core/web/devices.c core/session/revoke.c core/web/settings.c core/web/stop.c)

option(SP5_BUILD_FUZZERS "Build bounded local parser fuzz/replay targets" OFF)
if(SP5_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

target_sources(sunshine_core PRIVATE core/web/metrics.c core/web/diagnostics.c)

# FR-013: the console serves the web app from immutable embedded bytes. This
# allowlist must equal webapp/ exactly; the generator also rejects unembedded
# imports, absolute http(s) URLs and assets above SP5_WEB_RESPONSE_LIMIT.
set(SP5_WEBAPP_ASSETS index.html app.js status.js devices.js settings.js stop.js style.css
    metrics.js diagnostics.js)
list(TRANSFORM SP5_WEBAPP_ASSETS PREPEND "${PROJECT_SOURCE_DIR}/webapp/"
    OUTPUT_VARIABLE sp5_webapp_sources)
list(JOIN SP5_WEBAPP_ASSETS "|" sp5_webapp_argument)
set(sp5_web_assets_source "${PROJECT_BINARY_DIR}/generated/web_assets.c")
add_custom_command(OUTPUT "${sp5_web_assets_source}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/generated"
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}/webapp"
        "-DASSETS=${sp5_webapp_argument}" "-DOUTPUT=${sp5_web_assets_source}" -DLIMIT=32768
        -P "${PROJECT_SOURCE_DIR}/cmake/embed-webapp.cmake"
    DEPENDS ${sp5_webapp_sources} "${PROJECT_SOURCE_DIR}/cmake/embed-webapp.cmake"
    COMMENT "Embedding the allowlisted local web app"
    VERBATIM)
target_sources(sunshine_core PRIVATE core/web/assets.c "${sp5_web_assets_source}")
