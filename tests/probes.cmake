# Nonshipping probe tests use simulated services/memory and no PS5 headers/imports.
add_executable(test_media_probe unit/test_media_probe.c
    "${PROJECT_SOURCE_DIR}/tools/ps5-probes/capture_encode.c")
target_compile_definitions(test_media_probe PRIVATE R3_HOST R3_MEDIA_TEST)
add_executable(test_remoteplay_export unit/test_remoteplay_export.c
    "${PROJECT_SOURCE_DIR}/tools/ps5-probes/re/remoteplay_export.c")
target_compile_definitions(test_remoteplay_export PRIVATE R3_RE_TEST
    _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE)
add_executable(test_remoteplay_export_resource unit/test_remoteplay_export.c
    "${PROJECT_SOURCE_DIR}/tools/ps5-probes/re/remoteplay_export.c")
target_compile_definitions(test_remoteplay_export_resource PRIVATE R3_RE_TEST R3_RE_RESOURCE_EXPORT
    _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE)
add_executable(test_remoteplay_export_recording unit/test_remoteplay_export.c
    "${PROJECT_SOURCE_DIR}/tools/ps5-probes/re/remoteplay_export.c")
target_compile_definitions(test_remoteplay_export_recording PRIVATE R3_RE_TEST R3_RE_LOAD_RECORDING
    _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE)
add_executable(test_input_probe unit/test_input_probe.c
    "${PROJECT_SOURCE_DIR}/tools/ps5-probes/input_system.c")
target_compile_definitions(test_input_probe PRIVATE R3_HOST R3_INPUT_TEST)
add_executable(test_input_interactive unit/test_input_interactive.c)
foreach(probe_test IN ITEMS test_media_probe test_remoteplay_export
    test_remoteplay_export_resource test_remoteplay_export_recording test_input_probe
    test_input_interactive)
    target_compile_options(${probe_test} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME ${probe_test} COMMAND ${probe_test})
    set_tests_properties(${probe_test} PROPERTIES LABELS "unit;nonshipping-probe" TIMEOUT 15)
    add_dependencies(check ${probe_test})
endforeach()
