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
run_checked("${CMAKE_COMMAND}" "-DROOT_DIR=${ROOT_DIR}" -P "${ROOT_DIR}/tests/BuildTargetParity.cmake")
run_checked("${CMAKE_COMMAND}" "-DTEST_DIR=${BIN_DIR}/test-output/build_target_parity" -P "${ROOT_DIR}/tests/BuildTargetParityTests.cmake")
foreach(test IN ITEMS disco_object_tests disco_ir_verifier_tests
                      disco_linear_scan_tests disco_target_foundation_tests disco_gsu_mapping_tests
                      disco_assembly_export_tests disco_language_contract_tests disco_project_manifest_tests
                      disco_module_loader_tests disco_linker_hardening_tests disco_ir_local_optimizer_tests disco_ir_global_optimizer_tests disco_ir_value_optimizer_tests disco_gsu_cost_tests disco_ir_conditional_optimizer_tests disco_gsu_scheduler_tests disco_gsu_checked_proof_tests disco_gsu_size_optimization_tests disco_gsu_compaction_tests)
    message(STATUS "Running ${test}")
    run_checked("${BIN_DIR}/${test}${EXE_SUFFIX}")
endforeach()
run_checked("${BIN_DIR}/disco_gsu_execution_tests${EXE_SUFFIX}" --self-test)
run_checked("${CMAKE_COMMAND}" "-DTOOLS_DIR=${BIN_DIR}"
    "-DRUNNER=${BIN_DIR}/disco_gsu_execution_tests${EXE_SUFFIX}"
    "-DOUTPUT_DIR=${BIN_DIR}/benchmark-tests/value-optimizations" -P "${ROOT_DIR}/benchmarks/optimizer/run.cmake")
function(add_toolchain_regression name case_name)
    message(STATUS "Running ${name}")
    run_checked("${CMAKE_COMMAND}"
        "-DCASE=${case_name}" "-DROOT_DIR=${ROOT_DIR}"
        "-DOPTIMIZATION=${ARGV2}" "-DTEST_DIR=${BIN_DIR}/test-output/${name}"
        "-DDISCC=${BIN_DIR}/discc${EXE_SUFFIX}"
        "-DDISCAS=${BIN_DIR}/discas${EXE_SUFFIX}"
        "-DDISCLD=${BIN_DIR}/discld${EXE_SUFFIX}"
        "-DGSU_RUNNER=${BIN_DIR}/disco_gsu_execution_tests${EXE_SUFFIX}"
        "-DGSU_MAPPING_TESTER=${BIN_DIR}/disco_gsu_mapping_tests${EXE_SUFFIX}"
        -P "${ROOT_DIR}/tests/run_toolchain_regression.cmake")
endfunction()
include("${ROOT_DIR}/tests/toolchain-cases.cmake")
run_checked("${BIN_DIR}/disco_gsu_benchmarks${EXE_SUFFIX}" --self-test)
run_checked("${CMAKE_COMMAND}" "-DTOOLS_DIR=${BIN_DIR}"
    "-DOUTPUT_DIR=${BIN_DIR}/benchmark-tests/suite" -P "${ROOT_DIR}/benchmarks/run.cmake")
run_checked("${CMAKE_COMMAND}" "-DTOOLS_DIR=${BIN_DIR}" "-DOPTIMIZATION=1"
    "-DOUTPUT_DIR=${BIN_DIR}/benchmark-tests/O1" -P "${ROOT_DIR}/benchmarks/run.cmake")
run_checked("${CMAKE_COMMAND}" "-DTOOLS_DIR=${BIN_DIR}" "-DOPTIMIZATION=2"
    "-DOUTPUT_DIR=${BIN_DIR}/benchmark-tests/O2" -P "${ROOT_DIR}/benchmarks/run.cmake")
run_checked("${CMAKE_COMMAND}" "-DTOOLS_DIR=${BIN_DIR}" "-DOPTIMIZATION=s"
    "-DOUTPUT_DIR=${BIN_DIR}/benchmark-tests/Os" -P "${ROOT_DIR}/benchmarks/run.cmake")
run_checked("${CMAKE_COMMAND}" "-DOUTPUT_DIR=${BIN_DIR}/benchmark-tests/reports"
    -P "${ROOT_DIR}/benchmarks/test_reports.cmake")
run_checked("${CMAKE_COMMAND}" "-DOUTPUT_DIR=${BIN_DIR}/benchmark-tests/profile-contract"
    -P "${ROOT_DIR}/benchmarks/test_profile.cmake")
message(STATUS "All prebuilt toolchain tests passed")
