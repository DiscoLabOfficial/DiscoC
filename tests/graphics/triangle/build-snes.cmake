cmake_minimum_required(VERSION 3.15)
set(DEMO_OUTPUT_SUBDIR "build/plot-triangle")
include("${CMAKE_CURRENT_LIST_DIR}/../snes_demo_helpers.cmake")

message(STATUS "Compiling the RAM-executed triangle and comparing both assembly paths")
build_gsu_payload("${CMAKE_CURRENT_LIST_DIR}/triangle.dc")
message(STATUS "Assembling the SNES host and its deliberate wrong-result variant")
build_snes_host("${CMAKE_CURRENT_LIST_DIR}" triangle-test)
build_snes_host("${CMAKE_CURRENT_LIST_DIR}" triangle-test-negative -D EXPECTED_RESULT=9408)

if(VERIFY_MESEN)
    # Only this test process gets Lua I/O and deterministic rendering. Neither
    # setting is saved to the user's Mesen configuration.
    set(mesen_options --testrunner --timeout=15 --doNotSaveSettings
        --snes.disableFrameSkipping=true --debug.scriptWindow.allowIoOsAccess=true)
    foreach(suffix IN ITEMS "" "-negative")
        # Reject stale evidence if the emulator never executes the script.
        file(REMOVE "${OUTPUT_DIR}/triangle${suffix}-verification.log"
            "${OUTPUT_DIR}/triangle${suffix}-preview.png")
        run_command("${MESEN}" ${mesen_options}
            "${CMAKE_CURRENT_LIST_DIR}/verify-triangle${suffix}.lua"
            "${OUTPUT_DIR}/triangle-test${suffix}.sfc")
        if(NOT EXISTS "${OUTPUT_DIR}/triangle${suffix}-verification.log" OR
           NOT EXISTS "${OUTPUT_DIR}/triangle${suffix}-preview.png")
            message(FATAL_ERROR "Mesen produced no verification evidence for triangle${suffix}")
        endif()
        file(READ "${OUTPUT_DIR}/triangle${suffix}-verification.log" evidence)
        if(NOT evidence MATCHES "^PASS triangle")
            message(FATAL_ERROR "Mesen verification failed: ${evidence}")
        endif()
        string(STRIP "${evidence}" evidence)
        message(STATUS "${evidence}")
    endforeach()
endif()

message(STATUS "Open ${OUTPUT_DIR}/triangle-test.sfc in a SuperFX-capable SNES emulator")
message(STATUS "GSU: $70:6000; framebuffer: $70:0000-$70:5FFF; initial stack: $70:FFFE")
