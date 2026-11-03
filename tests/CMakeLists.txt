# A missing future behavior suite must never be hidden by the setup test in CI.
add_custom_target(check
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure --no-tests=error -L unit
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
    COMMENT "Run registered core unit tests; fail if none exist"
    USES_TERMINAL
)

# Later unit tasks register real core behavior through this function.
function(sunshine_add_unit_test target)
    if(NOT ARGN)
        message(FATAL_ERROR "${target} requires actual test sources")
    endif()
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE SunshinePS5::core)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "unit" TIMEOUT 30)
    add_dependencies(check ${target})
endfunction()

# This generated translation unit verifies the C11 build contract, not host behavior.
# The checked-in generator is the only source of this build-directory artifact.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/c11_build_contract.c" CONTENT [=[
#include "platform.h"
#include "evidence.h"
#include <stdint.h>

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ != 201112L
#error "Portable targets must compile as ISO C11"
#endif

_Static_assert(sizeof(uint8_t) == 1, "Protocol bytes require uint8_t");
_Static_assert(sizeof(uint32_t) == 4, "Protocol words require uint32_t");
_Static_assert(_Generic((uint32_t)0, uint32_t: 1, default: 0), "C11 generic selection is required");

int main(void)
{
    return 0;
}
]=])

add_executable(c11_build_contract "${CMAKE_CURRENT_BINARY_DIR}/c11_build_contract.c")
target_link_libraries(c11_build_contract PRIVATE SunshinePS5::core)
add_test(NAME c11_build_contract COMMAND c11_build_contract)
set_tests_properties(c11_build_contract PROPERTIES LABELS "setup" TIMEOUT 10)

sunshine_add_unit_test(test_runtime unit/test_runtime.c)
sunshine_add_unit_test(test_fakes unit/test_fakes.c fakes/platform.c)

# T046-T048 are a deliberate red TDD gate, independent of console readiness.
# Required: ctest --test-dir <build> --no-tests=error -LE pending-core
# Red proof: ctest --test-dir <build> --no-tests=error -L pending-core
# No WILL_FAIL/DISABLED property: missing implementation is a real test failure.
# Weak imports are confined to these test executables, never the core library.
# WHOLE_ARCHIVE ensures newly implemented functions resolve even while pending.
function(sunshine_add_pending_test target source)
    cmake_parse_arguments(PENDING "" "" "SYMBOLS;CASES" ${ARGN})
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,sunshine_core>")
    target_compile_definitions(${target} PRIVATE SP5_PENDING_CORE=1)
    if(APPLE)
        foreach(symbol IN LISTS PENDING_SYMBOLS)
            target_link_options(${target} PRIVATE "LINKER:-U,_${symbol}")
        endforeach()
    endif()
    foreach(case IN LISTS PENDING_CASES)
        add_test(NAME ${target}.${case} COMMAND ${target} ${case})
        set_tests_properties(${target}.${case} PROPERTIES LABELS "pending-core" TIMEOUT 10)
    endforeach()
endfunction()

# Promotion: after each named task, run the associated cases without -LE and
# move passing case labels from pending-core to unit (add_dependencies(check,
# target)). Do not mark T049-T067 done with their relevant cases still pending.
# Once all cases in an executable pass, remove its SP5_PENDING_CORE definition
# and APPLE -U options, making missing API definitions link errors again.
# T052: addresses; T055: http; T058: rtsp, bitrate;
# T051/T053/T059: authorization (registry/admission/control revocation together).
sunshine_add_pending_test(test_protocol unit/test_protocol.c
    CASES addresses http rtsp bitrate authorization
    SYMBOLS sp5_peer_permitted sp5_http_parse sp5_rtsp_parse sp5_negotiate_bitrate
        sp5_authority_create sp5_authority_destroy sp5_authority_pair sp5_authority_revoke
        sp5_session_create sp5_session_destroy sp5_session_admit sp5_session_activate
        sp5_session_authorized sp5_session_end sp5_session_read)
# T054: expiry, cooldown; T056: pin_key, proof. Successful proof is never authorization.
add_executable(test_pairing unit/test_pairing.c)
target_link_libraries(test_pairing PRIVATE SunshinePS5::core)
foreach(case IN ITEMS pin_key proof expiry cooldown)
    add_test(NAME test_pairing.${case} COMMAND test_pairing ${case})
    set_tests_properties(test_pairing.${case} PROPERTIES LABELS "unit" TIMEOUT 10)
endforeach()
# T049-T051: durability, recovery (portable storage adapter contract only).
# T067: unwind, teardown, cleanup_error, conflicts_loss; real adapter evidence remains blocked.
sunshine_add_pending_test(test_lifecycle unit/test_lifecycle.c
    CASES durability recovery unwind teardown cleanup_error conflicts_loss
    SYMBOLS sp5_store_create sp5_store_destroy sp5_store_load sp5_store_read
        sp5_store_identity sp5_store_settings sp5_store_save_settings sp5_store_pair
        sp5_store_authorized sp5_lifecycle_create sp5_lifecycle_destroy sp5_lifecycle_start
        sp5_lifecycle_end sp5_lifecycle_read sp5_lifecycle_contact sp5_lifecycle_tick)
target_sources(test_lifecycle PRIVATE fakes/platform.c)
# T062: all controller cases; no test claims actual console delivery.
sunshine_add_pending_test(test_input unit/test_input.c
    CASES translation controllers independent motion_touch
    SYMBOLS sp5_controller_create sp5_controller_destroy sp5_controller_receive sp5_controller_close)
target_sources(test_input PRIVATE fakes/platform.c)
# T063: off_state/record layouts; T064: retention, toggles, queue_loss;
# T065: percentiles, overflow, counters_thresholds, rolling;
# T066: nonblocking, limits_delivery (and integration of all engine cases).
find_package(Threads REQUIRED)
sunshine_add_pending_test(test_metrics unit/test_metrics.c
    CASES percentiles overflow counters_thresholds rolling retention toggles queue_loss
        off_state nonblocking limits_delivery
    SYMBOLS sp5_histogram_init sp5_histogram_add sp5_histogram_percentile
        sp5_latency_below_frame_interval sp5_metrics_create sp5_metrics_destroy
        sp5_metrics_enable sp5_metrics_push_frame sp5_metrics_push_input
        sp5_metrics_worker_step sp5_metrics_snapshot sp5_metrics_stamp
        sp5_metrics_protocol_header sp5_metrics_export)
target_sources(test_metrics PRIVATE fakes/platform.c)
target_link_libraries(test_metrics PRIVATE Threads::Threads)

sunshine_add_unit_test(test_settings unit/test_settings.c ../platform/system/storage.c)
target_compile_definitions(test_settings PRIVATE
    _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE SP5_STORAGE_TEST)
# TMPDIR is captured at configure time; CI jobs that do not export it fall
# back to the build tree instead of the filesystem root.
if(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
  set(SP5_TEST_TMPDIR "$ENV{TMPDIR}")
else()
  set(SP5_TEST_TMPDIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()
set_tests_properties(test_settings PROPERTIES ENVIRONMENT "TMPDIR=${SP5_TEST_TMPDIR}")
set_tests_properties(test_lifecycle.durability test_lifecycle.recovery PROPERTIES LABELS "unit")
add_dependencies(check test_lifecycle)

set_tests_properties(test_protocol.addresses PROPERTIES LABELS "unit")
add_dependencies(check test_protocol)

sunshine_add_unit_test(test_session unit/test_session.c)

sunshine_add_unit_test(test_pairing_state unit/test_pairing_state.c)

set_tests_properties(test_pairing.expiry test_pairing.cooldown PROPERTIES LABELS "unit")
add_dependencies(check test_pairing)

sunshine_add_unit_test(test_http unit/test_http.c)
set_tests_properties(test_protocol.http PROPERTIES LABELS "unit")
sunshine_add_unit_test(test_pairing_flow unit/test_pairing_flow.c)

sunshine_add_unit_test(test_discovery unit/test_discovery.c)

sunshine_add_unit_test(test_rtsp unit/test_rtsp.c)
set_tests_properties(test_protocol.rtsp test_protocol.bitrate PROPERTIES LABELS "unit")

sunshine_add_unit_test(test_control unit/test_control.c)
set_tests_properties(test_protocol.authorization PROPERTIES LABELS "unit")
# Every test_protocol API now exists: missing symbols must fail the link.
set_property(TARGET test_protocol PROPERTY COMPILE_DEFINITIONS "")
set_property(TARGET test_protocol PROPERTY LINK_OPTIONS "")

sunshine_add_unit_test(test_media_protocol unit/test_media_protocol.c)
sunshine_add_unit_test(test_ps5_parameter_sets unit/test_ps5_parameter_sets.c)

sunshine_add_unit_test(test_audio_protocol unit/test_audio_protocol.c)

sunshine_add_unit_test(test_controller unit/test_controller.c fakes/platform.c)

# T062: every input API now has a real definition and every original case passes.
set_tests_properties(test_input.translation test_input.controllers test_input.independent
    test_input.motion_touch PROPERTIES LABELS "unit")
set_property(TARGET test_input PROPERTY COMPILE_DEFINITIONS "")
set_property(TARGET test_input PROPERTY LINK_OPTIONS "")
add_dependencies(check test_input)

sunshine_add_unit_test(test_metrics_records unit/test_metrics_records.c fakes/platform.c)
sunshine_add_unit_test(test_metrics_engine unit/test_metrics_engine.c fakes/platform.c)

# T063-T066: every original metrics contract now resolves a real core symbol.
set_tests_properties(test_metrics.percentiles test_metrics.overflow test_metrics.counters_thresholds
    test_metrics.rolling test_metrics.retention test_metrics.toggles test_metrics.queue_loss
    test_metrics.off_state test_metrics.nonblocking test_metrics.limits_delivery
    PROPERTIES LABELS "unit")
set_property(TARGET test_metrics PROPERTY COMPILE_DEFINITIONS "")
set_property(TARGET test_metrics PROPERTY LINK_OPTIONS "")
add_dependencies(check test_metrics)

sunshine_add_unit_test(test_lifecycle_policy unit/test_lifecycle_policy.c fakes/platform.c)

# T067: all original lifecycle contracts have real core definitions.
set_tests_properties(test_lifecycle.unwind test_lifecycle.teardown test_lifecycle.cleanup_error
    test_lifecycle.conflicts_loss PROPERTIES LABELS "unit")
set_property(TARGET test_lifecycle PROPERTY COMPILE_DEFINITIONS "")
set_property(TARGET test_lifecycle PROPERTY LINK_OPTIONS "")

# Portable fixture executable, deliberately separate from production adapters.
add_executable(sp5_fake_host fakes/host_main.c fakes/platform.c fakes/web_service.c)
target_link_libraries(sp5_fake_host PRIVATE sunshine_posix_storage)
target_compile_definitions(sp5_fake_host PRIVATE _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE)

sunshine_add_unit_test(test_enet_timeout unit/test_enet_timeout.c)

include(probes.cmake)

add_executable(test_budget_probe "${PROJECT_SOURCE_DIR}/tools/ps5-probes/test_budget.c"
    "${PROJECT_SOURCE_DIR}/tools/ps5-probes/budget.c")
target_compile_definitions(test_budget_probe PRIVATE _POSIX_C_SOURCE=200809L BUDGET_IDLE BUDGET_TEST)
target_compile_options(test_budget_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME test_budget_probe COMMAND test_budget_probe)
set_tests_properties(test_budget_probe PROPERTIES LABELS "unit;nonshipping-probe" TIMEOUT 15)
add_dependencies(check test_budget_probe)

# Nonshipping T050: use the exact store/registry/POSIX implementation, in fresh
# processes, including bounded cleanup and refusal of unexpected tree entries.
# Checks are explicit and remain active in Release/NDEBUG builds.
add_executable(test_storage_probe_binary "${PROJECT_SOURCE_DIR}/tools/ps5-probes/storage.c"
    "${PROJECT_SOURCE_DIR}/core/runtime.c" "${PROJECT_SOURCE_DIR}/core/settings/host.c"
    "${PROJECT_SOURCE_DIR}/core/settings/store.c" "${PROJECT_SOURCE_DIR}/core/settings/devices.c"
    "${PROJECT_SOURCE_DIR}/platform/system/storage.c")
target_link_libraries(test_storage_probe_binary PRIVATE sp5_crypto)
target_include_directories(test_storage_probe_binary PRIVATE "${PROJECT_SOURCE_DIR}/core")
target_compile_definitions(test_storage_probe_binary PRIVATE
    _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE T050_HOST NDEBUG
    SP5_STORAGE_DIAGNOSTICS SP5_STORAGE_DIAGNOSTIC_TEST)
target_compile_options(test_storage_probe_binary PRIVATE -Wall -Wextra -Wpedantic -Werror)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_storage_probe COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/probes/storage_probe.py" "$<TARGET_FILE:test_storage_probe_binary>")
set_tests_properties(test_storage_probe PROPERTIES LABELS "unit;nonshipping-probe" TIMEOUT 60)
add_dependencies(check test_storage_probe_binary)

add_executable(test_storage_syscalls probes/storage_syscalls.c)
target_compile_definitions(test_storage_syscalls PRIVATE _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE NDEBUG)
target_compile_options(test_storage_syscalls PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(test_storage_syscalls PRIVATE Threads::Threads)
add_test(NAME test_storage_syscalls COMMAND test_storage_syscalls)
set_tests_properties(test_storage_syscalls PROPERTIES LABELS "unit;nonshipping-probe" TIMEOUT 10)
add_dependencies(check test_storage_syscalls)

# T050 launcher boundary only: no CommonCrypto inventory fixture or target I/O.
add_executable(test_recovery_inspector_launcher
    "${PROJECT_SOURCE_DIR}/tools/ps5-probes/recovery_inspector_launcher.c"
    "${PROJECT_SOURCE_DIR}/tools/ps5-probes/test_recovery_inspector_launcher.c")
target_compile_definitions(test_recovery_inspector_launcher PRIVATE
    RECOVERY_INSPECTOR_LAUNCHER_TEST NDEBUG)
target_compile_options(test_recovery_inspector_launcher PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
add_test(NAME test_recovery_inspector_launcher COMMAND test_recovery_inspector_launcher)
set_tests_properties(test_recovery_inspector_launcher PROPERTIES
    LABELS "unit;nonshipping-probe" TIMEOUT 10)
add_dependencies(check test_recovery_inspector_launcher)

# T094/T096: revoke/stop composed with a bound lifecycle on the synthetic fake platform.
sunshine_add_unit_test(test_management unit/test_management.c fakes/platform.c fakes/web_service.c)
target_compile_definitions(test_management PRIVATE _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE)

add_executable(sp5_management_host fakes/management_main.c fakes/platform.c
    fakes/web_service.c fakes/pairing_client.c)
target_link_libraries(sp5_management_host PRIVATE sunshine_posix_storage)
target_compile_definitions(sp5_management_host PRIVATE _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE)

sunshine_add_unit_test(test_metrics_web unit/test_metrics_web.c fakes/platform.c)

sunshine_add_unit_test(test_management_json unit/test_management_json.c)

# T077: web pairing contract, embedded assets and response-head policy. The
# synthetic Gen7 client half is a fixture, never official-client evidence.
sunshine_add_unit_test(test_web_pairing unit/test_web_pairing.c fakes/pairing_client.c
    fakes/platform.c)
target_compile_definitions(test_web_pairing PRIVATE
    "SP5_WEBAPP_DIR=\"${PROJECT_SOURCE_DIR}/webapp\"")

add_executable(sp5_metrics_retention fakes/metrics_retention.c fakes/platform.c)
target_link_libraries(sp5_metrics_retention PRIVATE SunshinePS5::core)

# T114 portable guard only: device timing and platform/dependency internals need separate evidence.
sunshine_add_unit_test(test_hotpath_guards unit/test_hotpath_guards.c fakes/platform.c)
target_compile_definitions(test_hotpath_guards PRIVATE
    "SP5_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")

# T114 portable dependency characterization: OpenSSL allocation hooks and the
# production ENet fixed-pool callbacks. Console timing remains a separate gate.
sunshine_add_unit_test(test_dependency_allocations unit/test_dependency_allocations.c
    fakes/platform.c)
