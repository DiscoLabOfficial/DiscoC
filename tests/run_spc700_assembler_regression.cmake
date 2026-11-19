# SPC700 assembler/linker regression, included by run_toolchain_regression.cmake.
# Two objects exercise every SPC700 relocation type across object boundaries;
# the expected payload is derived by hand from the SPC-700 opcode map.

function(spc700_hex path out_var)
    file(READ "${path}" contents HEX)
    set(${out_var} "${contents}" PARENT_SCOPE)
endfunction()

file(WRITE "${TEST_DIR}/symbols.s" [=[
.setcpu "SPC700"
.define __DISCO_CODE_START_ADDRESS $0000
.export ext_dp, ext_bit, ext_abs
.segment "CODE"
    nop             ; $0000
ext_dp:
    nop             ; $0001
ext_bit:
    nop             ; $0002
.segment "DATA"
ext_abs:
    .byte $55
]=])

file(WRITE "${TEST_DIR}/user.s" [=[
.setcpu "SPC700"
.define __DISCO_CODE_START_ADDRESS $0000
.export entry
.segment "CODE"
entry:
    mov a,ext_dp        ; E4 01
    mov a,!ext_abs      ; E5 18 00
    mov1 c,ext_bit.5    ; AA 02 A0
    mov a,#<ext_abs     ; E8 18
    mov y,#>ext_abs     ; 8D 00
    mov ext_dp,#$12     ; 8F 12 01
    call !helper        ; 3F 17 00
    bra entry           ; 2F EC
helper:
    ret                 ; 6F
]=])

# .setcpu selects the target without --target; an explicit target must agree.
run_command("${DISCAS}" "${TEST_DIR}/symbols.s" -o "${TEST_DIR}/symbols.o")
run_command("${DISCAS}" --target spc700 "${TEST_DIR}/user.s" -o "${TEST_DIR}/user.o")
run_command("${DISCLD}" "${TEST_DIR}/symbols.o" "${TEST_DIR}/user.o" -o "${TEST_DIR}/linked.bin"
    --emit-asm "${TEST_DIR}/linked.s")
spc700_hex("${TEST_DIR}/linked.bin" linked_hex)
set(expected_hex "000000e401e51800aa02a0e8188d008f12013f17002fec6f55")
if(NOT linked_hex STREQUAL expected_hex)
    message(FATAL_ERROR "SPC700 relocations linked incorrectly:\n  got      ${linked_hex}\n  expected ${expected_hex}")
endif()

# The linked listing must reassemble and relink to the same payload.
run_command("${DISCAS}" "${TEST_DIR}/linked.s" -o "${TEST_DIR}/relinked.o")
run_command("${DISCLD}" "${TEST_DIR}/relinked.o" -o "${TEST_DIR}/relinked.bin")
spc700_hex("${TEST_DIR}/relinked.bin" relinked_hex)
if(NOT relinked_hex STREQUAL expected_hex)
    message(FATAL_ERROR "SPC700 linked assembly export did not round-trip: ${relinked_hex}")
endif()

run_expected_failure_contains("does not match the selected GSU target"
    "${DISCAS}" --target gsu "${TEST_DIR}/user.s" -o "${TEST_DIR}/wrong-target.o")
run_expected_failure_contains("unknown target"
    "${DISCAS}" --target z80 "${TEST_DIR}/user.s" -o "${TEST_DIR}/unknown-target.o")

# Range checks happen at link time, where symbol addresses are known.
function(spc700_reject_link name origin diagnostic)
    file(WRITE "${TEST_DIR}/${name}-def.s"
        ".setcpu \"SPC700\"\n.define __DISCO_CODE_START_ADDRESS ${origin}\n.export target\n"
        ".segment \"CODE\"\ntarget:\n    nop\n")
    file(WRITE "${TEST_DIR}/${name}-use.s"
        ".setcpu \"SPC700\"\n.define __DISCO_CODE_START_ADDRESS ${origin}\n.segment \"CODE\"\n${ARGN}\n")
    run_command("${DISCAS}" "${TEST_DIR}/${name}-def.s" -o "${TEST_DIR}/${name}-def.o")
    run_command("${DISCAS}" "${TEST_DIR}/${name}-use.s" -o "${TEST_DIR}/${name}-use.o")
    set(output "${TEST_DIR}/${name}.bin")
    file(WRITE "${output}" "preserve-existing-output")
    run_expected_failure_contains("${diagnostic}" "${DISCLD}"
        "${TEST_DIR}/${name}-def.o" "${TEST_DIR}/${name}-use.o" -o "${output}")
    file(READ "${output}" unchanged)
    if(NOT unchanged STREQUAL "preserve-existing-output")
        message(FATAL_ERROR "Rejected SPC700 link overwrote its output: ${name}")
    endif()
endfunction()
spc700_reject_link(dp-range "$0100" "direct-page relocation must target \\$0000-\\$00FF" "    mov a,target")
spc700_reject_link(membit-range "$2000" "mem.bit relocation must target \\$0000-\\$1FFF" "    not1 target.1")
spc700_reject_link(payload-range "$FFFF" "SPC700 payload exceeds the 64 KiB address space" "    nop")

# GSU and SPC700 objects never link together.
file(WRITE "${TEST_DIR}/gsu.s" ".setcpu \"GSU\"\n.segment \"CODE\"\n    stop\n")
run_command("${DISCAS}" "${TEST_DIR}/gsu.s" -o "${TEST_DIR}/gsu.o")
run_expected_failure_contains("incompatible target configurations"
    "${DISCLD}" "${TEST_DIR}/symbols.o" "${TEST_DIR}/gsu.o" -o "${TEST_DIR}/mixed.bin")
