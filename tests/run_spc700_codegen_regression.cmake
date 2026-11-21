# SPC700 code generation regression, included by run_toolchain_regression.cmake.
# Compiled DiscoC programs run in the SPC700 model; where a result is defined,
# the GSU backend must produce the same value.
foreach(tool IN ITEMS SPC700_RUNNER GSU_RUNNER)
    if(NOT DEFINED ${tool})
        message(FATAL_ERROR "${tool} is required for SPC700 code generation regressions")
    endif()
endforeach()

set(fixture "${ROOT_DIR}/tests/fixtures/spc700/codegen_aggregates.dc")

# Same result on both backends; the SPC700 run starts on poisoned RAM twice.
run_command("${DISCC}" --target spc700 "${fixture}" -o "${TEST_DIR}/aggregates.o")
run_command("${DISCLD}" "${TEST_DIR}/aggregates.o" --init-runtime -o "${TEST_DIR}/aggregates.bin"
    --emit-asm "${TEST_DIR}/aggregates-final.s")
run_command("${SPC700_RUNNER}" "${TEST_DIR}/aggregates.bin" 0x0200 --fill 0xEE --runs 2 --register ya 316)
run_command("${DISCC}" "${fixture}" -o "${TEST_DIR}/aggregates-gsu.o")
run_command("${DISCLD}" "${TEST_DIR}/aggregates-gsu.o" --init-runtime --ram-bank 0 --stack-pointer 0x3000
    -o "${TEST_DIR}/aggregates-gsu.bin")
run_command("${GSU_RUNNER}" "${TEST_DIR}/aggregates-gsu.bin" 0x008000 --register 0 316 --register 6 0)

# The object is an SPC700 object (target byte 1), never GSU code.
file(READ "${TEST_DIR}/aggregates.o" object_hex HEX LIMIT 7)
string(SUBSTRING "${object_hex}" 12 2 target_byte)
if(NOT target_byte STREQUAL "01")
    message(FATAL_ERROR "SPC700 compilation produced a non-SPC700 object (target ${target_byte})")
endif()

# --emit-asm and the linked listing reassemble to the same payload.
run_command("${DISCC}" --target spc700 --emit-asm "${fixture}" -o "${TEST_DIR}/aggregates.s")
run_command("${DISCAS}" "${TEST_DIR}/aggregates.s" -o "${TEST_DIR}/aggregates-asm.o")
run_command("${DISCLD}" "${TEST_DIR}/aggregates-asm.o" --init-runtime -o "${TEST_DIR}/aggregates-asm.bin")
run_command("${DISCAS}" "${TEST_DIR}/aggregates-final.s" -o "${TEST_DIR}/aggregates-final.o")
run_command("${DISCLD}" "${TEST_DIR}/aggregates-final.o" -o "${TEST_DIR}/aggregates-final.bin")
file(SHA256 "${TEST_DIR}/aggregates.bin" direct_hash)
foreach(path IN ITEMS aggregates-asm.bin aggregates-final.bin)
    file(SHA256 "${TEST_DIR}/${path}" hash)
    if(NOT hash STREQUAL direct_hash)
        message(FATAL_ERROR "SPC700 ${path} differs from direct compilation")
    endif()
endforeach()

# Every optimization level is accepted and keeps the result.
foreach(level IN ITEMS 1 2 s)
    run_command("${DISCC}" --target spc700 "-O${level}" "${fixture}" -o "${TEST_DIR}/aggregates-O${level}.o")
    run_command("${DISCLD}" "${TEST_DIR}/aggregates-O${level}.o" --init-runtime -o "${TEST_DIR}/aggregates-O${level}.bin")
    run_command("${SPC700_RUNNER}" "${TEST_DIR}/aggregates-O${level}.bin" 0x0200 --register ya 316)
endforeach()

# Run-time checks report through the fault channel with the language's codes.
# The pointer comes from memory so no check can be decided at compile time.
file(WRITE "${TEST_DIR}/pointer.dc"
    "word main() {\n    u16 raw = *(volatile u16*)0xC000;\n    word* p = (word*)raw;\n"
    "    word* q = p + 100;\n    return *p + *q;\n}\n")
run_command("${DISCC}" --target spc700 "${TEST_DIR}/pointer.dc" -o "${TEST_DIR}/pointer.o")
run_command("${DISCLD}" "${TEST_DIR}/pointer.o" --init-runtime -o "${TEST_DIR}/pointer.bin")
run_command("${SPC700_RUNNER}" "${TEST_DIR}/pointer.bin" 0x0200 --ram 0xC000 0x01 --ram 0xC001 0x40 --fault 1)
run_command("${SPC700_RUNNER}" "${TEST_DIR}/pointer.bin" 0x0200 --ram 0xC000 0x00 --ram 0xC001 0x00 --fault 2)
run_command("${SPC700_RUNNER}" "${TEST_DIR}/pointer.bin" 0x0200 --ram 0xC000 0xF0 --ram 0xC001 0xFF --fault 3)
run_command("${SPC700_RUNNER}" "${TEST_DIR}/pointer.bin" 0x0200 --ram 0xC000 0x00 --ram 0xC001 0x40
    --ram 0x4000 0x05 --ram 0x4001 0x00 --ram 0x40C8 0x07 --ram 0x40C9 0x00 --register ya 12)

# Unbounded recursion stops before CALL wraps the 256-byte hardware stack.
file(WRITE "${TEST_DIR}/deep.dc" "word deep(word n) { return deep(n + 1) + 1; }\nword main() { return deep(0); }\n")
run_command("${DISCC}" --target spc700 "${TEST_DIR}/deep.dc" -o "${TEST_DIR}/deep.o")
run_command("${DISCLD}" "${TEST_DIR}/deep.o" --init-runtime -o "${TEST_DIR}/deep.bin")
run_command("${SPC700_RUNNER}" "${TEST_DIR}/deep.bin" 0x0200 --fault 2)

# Large frames: the software-stack guard faults when the frame does not fit
# above the stack floor, and the same program succeeds with room.
file(WRITE "${TEST_DIR}/frames.dc"
    "word big(word n) {\n    byte pad[100];\n    pad[0] = (byte)n;\n    if (n == 0) { return 0; }\n"
    "    return big(n - 1) + (word)pad[0];\n}\nword main() { return big(20); }\n")
run_command("${DISCC}" --target spc700 "${TEST_DIR}/frames.dc" -o "${TEST_DIR}/frames.o")
run_command("${DISCLD}" "${TEST_DIR}/frames.o" --init-runtime -o "${TEST_DIR}/frames.bin")
run_command("${SPC700_RUNNER}" "${TEST_DIR}/frames.bin" 0x0200 --register ya 210)
file(SIZE "${TEST_DIR}/frames.bin" payload_bytes)
math(EXPR small_top "(0x0200 + ${payload_bytes} + 400 + 1) / 2 * 2")
run_command("${DISCLD}" "${TEST_DIR}/frames.o" --init-runtime --stack-pointer "${small_top}" -o "${TEST_DIR}/frames-small.bin")
run_command("${SPC700_RUNNER}" "${TEST_DIR}/frames-small.bin" 0x0200 --fault 2)

# Over-aligned locals: the object base is rounded up inside its reserved
# padding, while a pointer slot to the type stays word aligned. main returns
# a failure mask, so 0 on poisoned RAM at every level and on the GSU.
set(overaligned "${ROOT_DIR}/tests/fixtures/spc700/codegen_overaligned.dc")
foreach(level IN ITEMS 0 1 2 s)
    run_command("${DISCC}" --target spc700 "-O${level}" "${overaligned}" -o "${TEST_DIR}/overaligned-O${level}.o")
    run_command("${DISCLD}" "${TEST_DIR}/overaligned-O${level}.o" --init-runtime -o "${TEST_DIR}/overaligned-O${level}.bin")
    run_command("${SPC700_RUNNER}" "${TEST_DIR}/overaligned-O${level}.bin" 0x0200 --fill 0xEE --runs 2 --register ya 0)
endforeach()
run_command("${DISCC}" "${overaligned}" -o "${TEST_DIR}/overaligned-gsu.o")
run_command("${DISCLD}" "${TEST_DIR}/overaligned-gsu.o" --init-runtime --ram-bank 0 --stack-pointer 0x3000
    -o "${TEST_DIR}/overaligned-gsu.bin")
run_command("${GSU_RUNNER}" "${TEST_DIR}/overaligned-gsu.bin" 0x008000 --register 0 0 --register 6 0)
