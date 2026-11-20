# Optional end-to-end check of the SPC700 runtime on the Luna emulator: the
# runtime fixture is linked with --init-runtime, uploaded through the APU IPL
# by tests/spc700_host/upload.asm and started twice without re-uploading.
# Both runs must report main's result on ports 2/3 and leave the static RAM
# initialized. Registered only when Luna, wla-65816 and wlalink are available;
# WLA needs a writable system temporary directory.
cmake_minimum_required(VERSION 3.15)
foreach(variable IN ITEMS DISCAS DISCLD WLA_65816 WLALINK LUNA ROOT_DIR TEST_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")
file(COPY "${ROOT_DIR}/tests/spc700_host/upload.asm" DESTINATION "${TEST_DIR}")
file(WRITE "${TEST_DIR}/link.txt" "[objects]\nhost.o\n")

function(spc700_step)
    execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${TEST_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result)
        message(FATAL_ERROR "Command failed (${result}): ${ARGV}\n${output}${error}")
    endif()
endfunction()

spc700_step("${DISCAS}" "${ROOT_DIR}/tests/fixtures/spc700/runtime_program.s" -o program.o)
spc700_step("${DISCAS}" "${ROOT_DIR}/tests/fixtures/spc700/runtime_statics.s" -o statics.o)
spc700_step("${DISCLD}" program.o statics.o --init-runtime --ram-origin 0x4000 -o payload.bin)
spc700_step("${WLA_65816}" -D SPC_ORIGIN=$0200 -D SPC_ENTRY=$0200 -o host.o upload.asm)
spc700_step("${WLALINK}" -r link.txt host.sfc)
# Mailbox: status $01, then each run's YA in memory order. ARAM: counter = 1,
# initialized bytes 11 22 00 33 and the stack limit $4018 (see the fixture).
# Page 0 is not checked: the real IPL ROM clears $0001-$00EF when it restarts.
spc700_step("${LUNA}" state host.sfc --until-frame 120 --no-render
    --assert 7E:0000=01 --assert 7E:0002=CF9D --assert 7E:0004=CF9D
    --assert-aram 4000=0100112200331840
    --out luna.json)
