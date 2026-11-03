# Opt-in only: instrumentation never alters an ordinary portable build.
set(SP5_FUZZ_SANITIZERS "address,undefined" CACHE STRING "Fuzz sanitizer list")
option(SP5_FUZZ_REPLAY_ONLY "Build corpus replay without libFuzzer (AppleClang)" OFF)
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzz targets require Clang sanitizer support")
endif()
if(NOT SP5_FUZZ_SANITIZERS MATCHES "^(address,undefined|undefined)$")
    message(FATAL_ERROR "Use address,undefined or undefined; sanitizer checks cannot be disabled")
endif()
# The core is sanitized in both modes: replay-only builds must still catch
# memory and undefined-behavior faults inside the parsers they replay.
if(SP5_FUZZ_REPLAY_ONLY)
    target_compile_options(sunshine_core PRIVATE
        "-fsanitize=${SP5_FUZZ_SANITIZERS}" -fno-omit-frame-pointer)
    target_link_options(sunshine_core INTERFACE "-fsanitize=${SP5_FUZZ_SANITIZERS}")
else()
    target_compile_options(sunshine_core PRIVATE
        "-fsanitize=fuzzer-no-link,${SP5_FUZZ_SANITIZERS}" -fno-omit-frame-pointer)
    target_link_options(sunshine_core INTERFACE
        "-fsanitize=fuzzer-no-link,${SP5_FUZZ_SANITIZERS}")
endif()
# This list must equal run.py PARSERS and the fuzz.yml matrix, and name every
# harness source and corpus here (run.py --check-targets fails otherwise).
foreach(parser IN ITEMS http rtsp control input)
    set(parser_sources "${parser}.c")
    if(parser STREQUAL "input")
        list(APPEND parser_sources "${PROJECT_SOURCE_DIR}/tests/fakes/platform.c")
    endif()
    add_executable("${parser}_replay" ${parser_sources} replay.c)
    target_link_libraries("${parser}_replay" PRIVATE SunshinePS5::core)
    target_compile_options("${parser}_replay" PRIVATE "-fsanitize=${SP5_FUZZ_SANITIZERS}")
    target_link_options("${parser}_replay" PRIVATE "-fsanitize=${SP5_FUZZ_SANITIZERS}")
    if(NOT SP5_FUZZ_REPLAY_ONLY)
        add_executable("${parser}_fuzz" ${parser_sources})
        target_link_libraries("${parser}_fuzz" PRIVATE SunshinePS5::core)
        target_compile_options("${parser}_fuzz" PRIVATE
            "-fsanitize=fuzzer,${SP5_FUZZ_SANITIZERS}" -fno-omit-frame-pointer)
        target_link_options("${parser}_fuzz" PRIVATE "-fsanitize=fuzzer,${SP5_FUZZ_SANITIZERS}")
    endif()
endforeach()
