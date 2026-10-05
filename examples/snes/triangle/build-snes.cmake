cmake_minimum_required(VERSION 3.15)
set(DEMO_OUTPUT_SUBDIR "build/snes-triangle")
set(SNES_HOST_SOURCE host.asm)
include("${CMAKE_CURRENT_LIST_DIR}/../snes_demo_helpers.cmake")

message(STATUS "Building discoc.toml and comparing the standalone and assembly paths")
build_gsu_payload("${CMAKE_CURRENT_LIST_DIR}/main.dc" "${CMAKE_CURRENT_LIST_DIR}/discoc.toml")

# Feed the linked metadata into the host rather than duplicating SCBR/SCMR.
# This host's PPU transfer/tilemap are specifically 256x192 4bpp at base zero.
file(READ "${OUTPUT_DIR}/triangle.s" assembly)
foreach(field IN ITEMS CODE_START_ADDRESS BITMAP_SCBR BITMAP_SCMR)
    if(NOT assembly MATCHES "\\.define __DISCO_${field}[ \t]+\\$([0-9A-Fa-f]+)")
        message(FATAL_ERROR "Linked assembly is missing host metadata: ${field}")
    endif()
    set(${field} "${CMAKE_MATCH_1}")
    math(EXPR ${field}_value "0x${${field}}")
endforeach()
math(EXPR framebuffer_end "0x706000")
math(EXPR mailbox_begin "0x70F000")
if(NOT BITMAP_SCBR_value EQUAL 0 OR NOT BITMAP_SCMR_value EQUAL 33 OR
   CODE_START_ADDRESS_value LESS framebuffer_end OR CODE_START_ADDRESS_value GREATER_EQUAL mailbox_begin)
    message(FATAL_ERROR "This SNES host requires a 256x192 4bpp framebuffer at $70:0000 and code in $70:6000-$70:EFFF")
endif()
math(EXPR start_pc "${CODE_START_ADDRESS_value} & 0xFFFF")
file(WRITE "${OUTPUT_DIR}/triangle-config.inc"
    "; Generated from the final linked payload metadata; do not edit.\n"
    ".DEFINE GSU_LOAD_ADDRESS $${CODE_START_ADDRESS}\n"
    ".DEFINE GSU_START_PC ${start_pc}\n"
    ".DEFINE BITMAP_SCBR $${BITMAP_SCBR}\n"
    ".DEFINE BITMAP_SCMR $${BITMAP_SCMR}\n")
message(STATUS "Linked host contract: origin=$${CODE_START_ADDRESS}, SCBR=$${BITMAP_SCBR}, SCMR=$${BITMAP_SCMR}")
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
message(STATUS "GSU: $${CODE_START_ADDRESS}; framebuffer: $70:0000-$70:5FFF; initial stack: $70:FFFE")
