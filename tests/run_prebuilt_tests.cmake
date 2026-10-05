cmake_minimum_required(VERSION 3.15)
if(NOT DEFINED BIN_DIR OR NOT DEFINED ROOT_DIR)
    message(FATAL_ERROR "BIN_DIR and ROOT_DIR are required")
endif()
if(NOT DEFINED EXE_SUFFIX)
    set(EXE_SUFFIX "")
endif()
function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Test command failed (${result}): ${ARGV}")
    endif()
endfunction()
foreach(test IN ITEMS disco_object_tests disco_ir_verifier_tests
                      disco_linear_scan_tests disco_target_foundation_tests disco_gsu_mapping_tests
                      disco_assembly_export_tests disco_language_contract_tests disco_project_manifest_tests
                      disco_module_loader_tests disco_linker_hardening_tests)
    message(STATUS "Running ${test}")
    run_checked("${BIN_DIR}/${test}${EXE_SUFFIX}")
endforeach()
run_checked("${BIN_DIR}/disco_gsu_execution_tests${EXE_SUFFIX}" --self-test)
function(add_toolchain_regression name case_name)
    message(STATUS "Running ${name}")
    run_checked("${CMAKE_COMMAND}"
        "-DCASE=${case_name}" "-DROOT_DIR=${ROOT_DIR}"
        "-DTEST_DIR=${BIN_DIR}/test-output/${case_name}"
        "-DDISCC=${BIN_DIR}/discc${EXE_SUFFIX}"
        "-DDISCAS=${BIN_DIR}/discas${EXE_SUFFIX}"
        "-DDISCLD=${BIN_DIR}/discld${EXE_SUFFIX}"
        "-DGSU_RUNNER=${BIN_DIR}/disco_gsu_execution_tests${EXE_SUFFIX}"
        "-DGSU_MAPPING_TESTER=${BIN_DIR}/disco_gsu_mapping_tests${EXE_SUFFIX}"
        -P "${ROOT_DIR}/tests/run_toolchain_regression.cmake")
endfunction()
include("${ROOT_DIR}/tests/toolchain-cases.cmake")
message(STATUS "All prebuilt toolchain tests passed")
