add_executable(disco_gsu_benchmarks GSUBenchmarkRunner.cpp)
target_link_libraries(disco_gsu_benchmarks PRIVATE disco_object_core)

set(benchmark_arguments
    -DDISCC=$<TARGET_FILE:discc>
    -DDISCAS=$<TARGET_FILE:discas>
    -DDISCLD=$<TARGET_FILE:discld>
    -DGSU_BENCHMARK_RUNNER=$<TARGET_FILE:disco_gsu_benchmarks>)

add_test(NAME benchmark_counters COMMAND disco_gsu_benchmarks --self-test)
set_tests_properties(benchmark_counters PROPERTIES LABELS "benchmark;gsu;unit")
add_test(NAME benchmark_value_optimizations COMMAND "${CMAKE_COMMAND}"
    -DTOOLS_DIR=$<TARGET_FILE_DIR:discc> -DRUNNER=$<TARGET_FILE:disco_gsu_execution_tests>
    -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-tests/value-optimizations
    -P "${CMAKE_CURRENT_LIST_DIR}/optimizer/run.cmake")
set_tests_properties(benchmark_value_optimizations PROPERTIES LABELS "benchmark;gsu;optimization;correctness" TIMEOUT 120)
include("${CMAKE_CURRENT_LIST_DIR}/profile.cmake")
foreach(name IN LISTS BENCHMARK_NAMES)
    add_test(NAME benchmark_Os_${name} COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments}
        -DOPTIMIZATION=s -DCASE=${name} -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-Os-tests/${name}
        -P "${CMAKE_CURRENT_LIST_DIR}/run.cmake")
    set_tests_properties(benchmark_Os_${name} PROPERTIES LABELS "benchmark;gsu;optimization;correctness" TIMEOUT 120)
    add_test(NAME benchmark_${name} COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments}
        -DCASE=${name} -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-tests/${name}
        -P "${CMAKE_CURRENT_LIST_DIR}/run.cmake")
    set_tests_properties(benchmark_${name} PROPERTIES LABELS "benchmark;gsu;correctness" TIMEOUT 120)
    add_test(NAME benchmark_O1_${name} COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments}
        -DOPTIMIZATION=1 -DCASE=${name} -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-O1-tests/${name}
        -P "${CMAKE_CURRENT_LIST_DIR}/run.cmake")
    set_tests_properties(benchmark_O1_${name} PROPERTIES LABELS "benchmark;gsu;optimization;correctness" TIMEOUT 120)
    add_test(NAME benchmark_O2_${name} COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments}
        -DOPTIMIZATION=2 -DCASE=${name} -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-O2-tests/${name}
        -P "${CMAKE_CURRENT_LIST_DIR}/run.cmake")
    set_tests_properties(benchmark_O2_${name} PROPERTIES LABELS "benchmark;gsu;optimization;correctness" TIMEOUT 120)
endforeach()
add_test(NAME benchmark_report_contract COMMAND "${CMAKE_COMMAND}"
    -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-tests/report-contract
    -P "${CMAKE_CURRENT_LIST_DIR}/test_reports.cmake")
set_tests_properties(benchmark_report_contract PROPERTIES LABELS "benchmark;unit")
add_test(NAME benchmark_profile_contract COMMAND "${CMAKE_COMMAND}"
    -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-tests/profile-contract
    -P "${CMAKE_CURRENT_LIST_DIR}/test_profile.cmake")
set_tests_properties(benchmark_profile_contract PROPERTIES LABELS "benchmark;gsu;unit" TIMEOUT 120)

add_custom_target(benchmarks
    COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments}
        -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-results
        -DBASELINE=${CMAKE_CURRENT_LIST_DIR}/baselines/v0.1.0.json
        -P "${CMAKE_CURRENT_LIST_DIR}/run.cmake"
    DEPENDS discc discas discld disco_gsu_benchmarks
    COMMENT "Verify all GSU workloads and compare opcode/code-size measurements with v0.1.0"
    VERBATIM USES_TERMINAL)

find_program(DISCO_MESEN NAMES Mesen)
add_custom_target(benchmarks-O1
    COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments} -DOPTIMIZATION=1
        -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-O1-results
        -DBASELINE=${CMAKE_CURRENT_LIST_DIR}/baselines/v0.1.0.json
        -P "${CMAKE_CURRENT_LIST_DIR}/run.cmake"
    DEPENDS discc discas discld disco_gsu_benchmarks
    COMMENT "Verify O1 workloads and compare code/opcode counts with the release baseline"
    VERBATIM USES_TERMINAL)
find_program(DISCO_WLA_65816 NAMES wla-65816)
add_custom_target(benchmarks-O2
    COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments} -DOPTIMIZATION=2
        -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-O2-results
        -DBASELINE=${CMAKE_CURRENT_LIST_DIR}/results/gsu-O1-selection.json
        -P "${CMAKE_CURRENT_LIST_DIR}/run.cmake"
    DEPENDS discc discas discld disco_gsu_benchmarks
    COMMENT "Verify O2 workloads and compare with the measured O1 baseline"
    VERBATIM USES_TERMINAL)
add_custom_target(benchmarks-Os
    COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments} -DOPTIMIZATION=s
        -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-Os-results
        -P "${CMAKE_CURRENT_LIST_DIR}/run.cmake"
    DEPENDS discc discas discld disco_gsu_benchmarks
    COMMENT "Verify size-oriented GSU code and measure payload bytes and executed opcodes"
    VERBATIM USES_TERMINAL)
find_program(DISCO_WLALINK NAMES wlalink)
set(mesen_arguments ${benchmark_arguments}
    "-DMESEN=${DISCO_MESEN}" "-DWLA_65816=${DISCO_WLA_65816}" "-DWLALINK=${DISCO_WLALINK}")
add_custom_target(benchmarks-mesen
    COMMAND "${CMAKE_COMMAND}" ${mesen_arguments}
        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-results"
        -P "${CMAKE_CURRENT_LIST_DIR}/run-mesen.cmake"
    DEPENDS discc discas discld disco_gsu_benchmarks
    COMMENT "Verify GSU workloads in Mesen and report emulated master clocks/fast GSU cycles"
    VERBATIM USES_TERMINAL)
add_custom_target(benchmarks-mesen-O1
    COMMAND "${CMAKE_COMMAND}" ${mesen_arguments} -DOPTIMIZATION=1
        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-O1-results"
        -P "${CMAKE_CURRENT_LIST_DIR}/run-mesen.cmake"
    DEPENDS discc discas discld disco_gsu_benchmarks
    COMMENT "Verify optimized GSU workloads and measure O1 emulated timing"
    VERBATIM USES_TERMINAL)
add_custom_target(benchmarks-mesen-O2
    COMMAND "${CMAKE_COMMAND}" ${mesen_arguments} -DOPTIMIZATION=2
        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-O2-results"
        "-DBASELINE=${CMAKE_CURRENT_LIST_DIR}/results/gsu-O1-selection-mesen.json"
        -P "${CMAKE_CURRENT_LIST_DIR}/run-mesen.cmake"
    DEPENDS discc discas discld disco_gsu_benchmarks
    COMMENT "Verify O2 GSU workloads and compare emulated timing with O1"
    VERBATIM USES_TERMINAL)
add_custom_target(benchmarks-mesen-Os
    COMMAND "${CMAKE_COMMAND}" ${mesen_arguments} -DOPTIMIZATION=s
        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-Os-results"
        -P "${CMAKE_CURRENT_LIST_DIR}/run-mesen.cmake"
    DEPENDS discc discas discld disco_gsu_benchmarks
    COMMENT "Verify Os GSU workloads and measure their size/cycle tradeoff in Mesen"
    VERBATIM USES_TERMINAL)
add_custom_target(benchmarks-profile-O2
    COMMAND "${CMAKE_COMMAND}" ${mesen_arguments} -DOPTIMIZATION=2 -DPROFILE_GSU=ON
        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-profile-O2-results"
        -P "${CMAKE_CURRENT_LIST_DIR}/run-mesen.cmake"
    DEPENDS discc discas discld disco_gsu_benchmarks
    COMMENT "Profile byte-exact linked GSU code by opcode, function and machine-label region"
    VERBATIM USES_TERMINAL)
if(DISCO_TEST_MESEN_BENCHMARKS)
    foreach(tool IN ITEMS DISCO_MESEN DISCO_WLA_65816 DISCO_WLALINK)
        if(NOT ${tool} OR NOT EXISTS "${${tool}}" OR IS_DIRECTORY "${${tool}}")
            message(FATAL_ERROR "DISCO_TEST_MESEN_BENCHMARKS requires ${tool}; supply its executable path")
        endif()
    endforeach()
    add_test(NAME benchmark_mesen_profile_contract COMMAND "${CMAKE_COMMAND}" ${mesen_arguments}
        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-tests/profile-contract"
        -P "${CMAKE_CURRENT_LIST_DIR}/mesen/test-profile.cmake")
    set_tests_properties(benchmark_mesen_profile_contract PROPERTIES LABELS "benchmark;gsu;mesen;unit" TIMEOUT 60 RUN_SERIAL TRUE)
    add_test(NAME benchmark_mesen_calibration COMMAND "${CMAKE_COMMAND}" ${mesen_arguments}
        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-tests/calibration"
        -P "${CMAKE_CURRENT_LIST_DIR}/mesen/test-calibration.cmake")
    add_test(NAME benchmark_mesen_workloads COMMAND "${CMAKE_COMMAND}" ${mesen_arguments}
        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-tests/workloads"
        -P "${CMAKE_CURRENT_LIST_DIR}/run-mesen.cmake")
    add_test(NAME benchmark_mesen_O1_workloads COMMAND "${CMAKE_COMMAND}" ${mesen_arguments}
        -DOPTIMIZATION=1 "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-tests/O1"
        -P "${CMAKE_CURRENT_LIST_DIR}/run-mesen.cmake")
    add_test(NAME benchmark_mesen_O2_workloads COMMAND "${CMAKE_COMMAND}" ${mesen_arguments}
        -DOPTIMIZATION=2 "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-tests/O2"
        -P "${CMAKE_CURRENT_LIST_DIR}/run-mesen.cmake")
    add_test(NAME benchmark_mesen_Os_workloads COMMAND "${CMAKE_COMMAND}" ${mesen_arguments}
        -DOPTIMIZATION=s "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-tests/Os"
        -P "${CMAKE_CURRENT_LIST_DIR}/run-mesen.cmake")
    add_test(NAME benchmark_mesen_size_pool COMMAND "${CMAKE_COMMAND}" ${mesen_arguments}
        "-DOUTPUT_DIR=${CMAKE_BINARY_DIR}/benchmark-mesen-tests/size-pool"
        -P "${CMAKE_CURRENT_LIST_DIR}/size/run-mesen.cmake")
    set_tests_properties(benchmark_mesen_calibration benchmark_mesen_workloads benchmark_mesen_O1_workloads benchmark_mesen_O2_workloads benchmark_mesen_Os_workloads
        PROPERTIES LABELS "benchmark;gsu;mesen;integration" TIMEOUT 240 RUN_SERIAL TRUE)
    set_tests_properties(benchmark_mesen_size_pool PROPERTIES
        LABELS "benchmark;gsu;mesen;optimization;correctness" TIMEOUT 120 RUN_SERIAL TRUE)
endif()
