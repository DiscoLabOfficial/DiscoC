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

# Platform & Compiler-Specific Flags
# Set the standard compiler flags for each build type.
# CMake will automatically use these for all targets. This is the most compatible method.
set(CMAKE_CXX_FLAGS_DEBUG          "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE        "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

# For MSVC, the flags are different. CMake knows to use these when the compiler is MSVC.
if(MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG          "/Od /Zi /RTC1")
    set(CMAKE_CXX_FLAGS_RELEASE        "/O2 /DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /Zi /DNDEBUG")
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
        -fPIC                         # Generate Position-Independent Code (good practice)
    )
endif()


# Source File Definitions
set(COMPILER_SOURCES
    src/discc.cpp
    src/Lexer.cpp
    src/Token.cpp
    src/Parser.cpp
    src/Analyzer.cpp
    src/Optimizer.cpp
    src/IR.cpp
    src/IRCodeGenerator.cpp
    src/AssemblyGenerator.cpp
    src/DataSegment.cpp
    src/ObjectFile.cpp
    src/ASTPrinter.cpp
)

set(LINKER_SOURCES
    src/discld.cpp
    src/ObjectFile.cpp
    src/Parser.cpp
    src/Token.cpp
)

set(ASSEMBLER_SOURCES
    src/discas.cpp
    src/Assembler.cpp
    src/ObjectFile.cpp
)

# Build Targets
# Define the compiler, linker, and standalone assembler executables
add_executable(discc ${COMPILER_SOURCES})
add_executable(discld ${LINKER_SOURCES})
add_executable(discas ${ASSEMBLER_SOURCES})

if(DISCO_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "DISCO_BUILD_FUZZERS requires a Clang compiler with libFuzzer support.")
    endif()
    add_executable(disco_frontend_fuzzer
        tests/FrontendFuzzer.cpp
        src/Lexer.cpp
        src/Token.cpp
        src/Parser.cpp
        src/Assembler.cpp
        src/ObjectFile.cpp
    )
    target_include_directories(disco_frontend_fuzzer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(disco_frontend_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(disco_frontend_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

include(CTest)
if(BUILD_TESTING AND NOT DISCO_DOS_BUILD)
    enable_testing()
    add_executable(disco_object_tests
        tests/ObjectFileTests.cpp
        src/ObjectFile.cpp
    )
    target_include_directories(disco_object_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(disco_object_tests PRIVATE disco_common_warnings)

    add_executable(disco_ir_verifier_tests
        tests/IRVerifierTests.cpp
        src/IR.cpp
        src/Token.cpp
    )
    target_include_directories(disco_ir_verifier_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(disco_ir_verifier_tests PRIVATE disco_common_warnings)

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
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_toolchain_regression.cmake
        )
    endfunction()

    add_test(NAME object_file_hardening COMMAND disco_object_tests)
    add_test(NAME ir_verifier COMMAND disco_ir_verifier_tests)
    add_toolchain_regression(ir_and_cfg ir_and_cfg)
    add_toolchain_regression(shadowing shadowing)
    add_toolchain_regression(diagnostics diagnostics)
    add_toolchain_regression(language_diagnostics language_diagnostics)
    add_toolchain_regression(multifile multifile)
    add_toolchain_regression(backend_equivalence backend_equivalence)
    add_toolchain_regression(data_relocation data_relocation)
    add_toolchain_regression(feature_examples feature_examples)
endif()

# Link Dependencies & Apply Flags
# Apply the common flags and include directories to both targets
target_include_directories(discc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(discld PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(discas PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link our warning flags library to the targets
target_link_libraries(discc PRIVATE disco_common_warnings)
target_link_libraries(discld PRIVATE disco_common_warnings)
target_link_libraries(discas PRIVATE disco_common_warnings)
# Final Status Output
cmake_print_variables(CMAKE_CXX_COMPILER_ID CMAKE_SYSTEM_NAME CMAKE_BUILD_TYPE)
message(STATUS "Project configured successfully. Build with 'cmake --build .'")
if(DISCO_DOS_BUILD)
    message(WARNING "DOS build is experimental. Ensure your toolchain file is correctly configured.")
endif()
