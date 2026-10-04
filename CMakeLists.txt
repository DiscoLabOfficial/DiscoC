# Project Setup
cmake_minimum_required(VERSION 3.15)

project(DiscoC-Toolchain VERSION 0.1.0 LANGUAGES CXX)

# Include helper module for printing status messages
include(CMakePrintHelpers)

# Build Options
# Add an option to specifically enable settings for a DOS build
option(DISCO_DOS_BUILD "Enable settings for a 16/32-bit DOS cross-compilation" OFF)
option(DISCO_BUILD_FUZZERS "Build optional Clang libFuzzer targets" OFF)

# Compiler & System Configuration
# Set the default C++ standard. This can be overridden for specific platforms.
set(CXX_STANDARD 23)
if(DISCO_DOS_BUILD)
    set(CXX_STANDARD 14)
    message(STATUS "DOS build enabled. Downgrading C++ standard to ${CXX_STANDARD}.")
endif()

set(CMAKE_CXX_STANDARD ${CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Use -std=c++23 instead of -std=gnu++23

if(APPLE)
  # Usually default on macOS, but keeping explicit avoids surprises.
  add_compile_options(-stdlib=libc++)
  add_link_options(-stdlib=libc++)
endif()

# We create an INTERFACE library to hold our common WARNING flags.
# This is a clean, modern way to apply settings to multiple targets.
add_library(disco_common_warnings INTERFACE)

# MSVC (Microsoft Visual C++) specific warning flags
if(MSVC)
    target_compile_options(disco_common_warnings INTERFACE
        /W4                           # High warning level
        /EHsc                         # Standard exception handling
        /permissive-                  # Enforce standard conformance
        # Suppress noisy but often safe warnings from standard library headers
        /D_CRT_SECURE_NO_WARNINGS
    )
# GNU GCC and Clang specific warning flags
else()
    target_compile_options(disco_common_warnings INTERFACE
        -Wall                         # All warnings
        -Wextra                       # Extra warnings
        -Wpedantic                    # Strict ISO C++ compliance warnings
    )
endif()


add_library(disco_platform_options INTERFACE)
if(MSVC)
    target_compile_options(disco_platform_options INTERFACE
        "$<$<CONFIG:Debug>:/Od>"
        "$<$<CONFIG:Debug>:/Zi>"
        "$<$<CONFIG:Debug>:/RTC1>"
        "$<$<CONFIG:Release>:/O2>"
        "$<$<CONFIG:Release>:/DNDEBUG>"
        "$<$<CONFIG:RelWithDebInfo>:/O2>"
        "$<$<CONFIG:RelWithDebInfo>:/Zi>"
        "$<$<CONFIG:RelWithDebInfo>:/DNDEBUG>"
    )
else()
    target_compile_options(disco_platform_options INTERFACE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Release>:-DNDEBUG>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-DNDEBUG>"
    )
    if(NOT DISCO_DOS_BUILD)
        target_compile_options(disco_platform_options INTERFACE -fPIC)
    endif()
endif()

# Source File Definitions
include(cmake/BuildSources.cmake)

add_library(disco_object_core STATIC ${DISCO_SOURCES_object})
add_library(disco_frontend_core STATIC ${DISCO_SOURCES_frontend})
add_library(disco_compiler_core STATIC ${DISCO_SOURCES_compiler})
add_library(disco_project_core STATIC ${DISCO_SOURCES_project})
add_library(disco_linker_core STATIC ${DISCO_SOURCES_linker})
add_library(disco_assembler_core STATIC ${DISCO_SOURCES_assembler})

foreach(core_target IN ITEMS disco_object_core disco_frontend_core disco_compiler_core
                            disco_assembler_core disco_project_core disco_linker_core)
    target_include_directories(${core_target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${core_target} PUBLIC disco_common_warnings disco_platform_options)
endforeach()

target_link_libraries(disco_linker_core PUBLIC disco_object_core disco_project_core)
target_link_libraries(disco_frontend_core PUBLIC disco_project_core)

target_link_libraries(disco_compiler_core PUBLIC disco_frontend_core disco_object_core)
target_link_libraries(disco_assembler_core PUBLIC disco_object_core)

add_executable(discc src/discc.cpp src/CompilerDriver.cpp src/ProjectBuild.cpp)
add_executable(discld src/discld.cpp)
add_executable(discas src/discas.cpp)
target_link_libraries(discc PRIVATE disco_compiler_core disco_linker_core)
target_link_libraries(discld PRIVATE disco_linker_core)
target_link_libraries(discas PRIVATE disco_assembler_core)

if(DISCO_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "DISCO_BUILD_FUZZERS requires a Clang compiler with libFuzzer support.")
    endif()
    add_executable(disco_frontend_fuzzer tests/FrontendFuzzer.cpp)
    target_link_libraries(disco_frontend_fuzzer PRIVATE disco_frontend_core disco_assembler_core disco_project_core)
    target_compile_options(disco_frontend_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(disco_frontend_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

include(CTest)
if(BUILD_TESTING AND NOT DISCO_DOS_BUILD)
    enable_testing()
    add_executable(disco_object_tests tests/ObjectFileTests.cpp)
    target_include_directories(disco_object_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(disco_object_tests PRIVATE disco_object_core)

    add_executable(disco_ir_verifier_tests ${DISCO_SOURCES_disco_ir_verifier_tests})
    target_include_directories(disco_ir_verifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(disco_ir_verifier_tests PRIVATE disco_common_warnings disco_platform_options)

    add_executable(disco_linear_scan_tests ${DISCO_SOURCES_disco_linear_scan_tests})
    target_include_directories(disco_linear_scan_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(disco_linear_scan_tests PRIVATE disco_common_warnings disco_platform_options)

    add_executable(disco_target_foundation_tests tests/TargetFoundationTests.cpp)
    target_include_directories(disco_target_foundation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(disco_target_foundation_tests PRIVATE disco_frontend_core)

    add_executable(disco_language_contract_tests tests/LanguageContractTests.cpp)
    target_link_libraries(disco_language_contract_tests PRIVATE disco_compiler_core)

    add_executable(disco_project_manifest_tests tests/ProjectManifestTests.cpp)
    target_link_libraries(disco_project_manifest_tests PRIVATE disco_project_core)

    add_executable(disco_module_loader_tests tests/ModuleLoaderTests.cpp)
    target_link_libraries(disco_module_loader_tests PRIVATE disco_compiler_core)

    add_executable(disco_gsu_execution_tests ${DISCO_SOURCES_disco_gsu_execution_tests})
    target_link_libraries(disco_gsu_execution_tests PRIVATE disco_common_warnings disco_platform_options)

    add_executable(disco_gsu_mapping_tests tests/GsuMemoryMapTests.cpp)
    target_link_libraries(disco_gsu_mapping_tests PRIVATE disco_object_core)

    add_executable(disco_assembly_export_tests tests/AssemblyExportTests.cpp)
    target_link_libraries(disco_assembly_export_tests PRIVATE disco_assembler_core)

    function(add_toolchain_regression name case_name)
        add_test(
            NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                -DCASE=${case_name}
                -DROOT_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-output/${case_name}
                -DDISCC=$<TARGET_FILE:discc>
                -DDISCAS=$<TARGET_FILE:discas>
                -DDISCLD=$<TARGET_FILE:discld>
                -DGSU_RUNNER=$<TARGET_FILE:disco_gsu_execution_tests>
                -DGSU_MAPPING_TESTER=$<TARGET_FILE:disco_gsu_mapping_tests>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_toolchain_regression.cmake
        )
    endfunction()

    add_test(NAME object_file_hardening COMMAND disco_object_tests)
    add_test(NAME ir_verifier COMMAND disco_ir_verifier_tests)
    add_test(NAME linear_scan_allocator COMMAND disco_linear_scan_tests)
    add_test(NAME target_foundation COMMAND disco_target_foundation_tests)
    add_test(NAME language_contract COMMAND disco_language_contract_tests)
    add_test(NAME project_manifest COMMAND disco_project_manifest_tests)
    add_test(NAME module_loader COMMAND disco_module_loader_tests)
    add_test(NAME gsu_execution_model COMMAND disco_gsu_execution_tests --self-test)
    add_test(NAME gsu_memory_map COMMAND disco_gsu_mapping_tests)
    add_test(NAME assembly_export COMMAND disco_assembly_export_tests)
    include(tests/toolchain-cases.cmake)
endif()

# Core libraries carry their include paths and warning policy to the tools and
# tests that consume them.
# Final Status Output
cmake_print_variables(CMAKE_CXX_COMPILER_ID CMAKE_SYSTEM_NAME CMAKE_BUILD_TYPE)
message(STATUS "Project configured successfully. Build with 'cmake --build .'")
if(DISCO_DOS_BUILD)
    message(WARNING "DOS build is experimental. Ensure your toolchain file is correctly configured.")
endif()
