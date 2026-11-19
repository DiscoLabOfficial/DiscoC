# SPC700 runtime/ABI regression, included by run_toolchain_regression.cmake.
#
# Hand-written assembly follows the ABI in docs/spc700-target.md: arguments on
# the software stack (SSP at $00/$01, last allocated byte), frames laid out
# upward from FP ($02/$03) and addressed with [fp]+Y, caller pops its
# arguments, word results in YA, scratch $04-$1F caller-saved. The payload
# links with --init-runtime and runs in the SPC700 instruction model.
if(NOT DEFINED SPC700_RUNNER)
    message(FATAL_ERROR "SPC700_RUNNER is required for SPC700 runtime regressions")
endif()

# Object A (tests/fixtures/spc700/runtime_program.s): main with a local
# accumulator, a nested call chain f -> g, a recursive factorial with a local
# and a word + byte argument call. Object B (runtime_statics.s): static RAM,
# zeroed and initialized, and a helper called across objects.
run_command("${DISCAS}" "${ROOT_DIR}/tests/fixtures/spc700/runtime_program.s" -o "${TEST_DIR}/program.o")
run_command("${DISCAS}" "${ROOT_DIR}/tests/fixtures/spc700/runtime_statics.s" -o "${TEST_DIR}/statics.o")

# Explicit placement: RAM at $4000 (8 bytes), so the software stack floor is
# $4008 and __disco_stack_limit_16 is $4018. RAM starts poisoned ($EE) to
# prove the startup clears and initializes it; two runs prove it is redone.
run_command("${DISCLD}" "${TEST_DIR}/program.o" "${TEST_DIR}/statics.o" --init-runtime --ram-origin 0x4000
    -o "${TEST_DIR}/runtime.bin" --emit-asm "${TEST_DIR}/runtime-final.s")
run_command("${SPC700_RUNNER}" "${TEST_DIR}/runtime.bin" 0x0200 --fill 0xEE --runs 2
    --port 2 0xCF --port 3 0x9D --register ya 0x9DCF --register sp 0xFF
    --word 0x0000 0xFFC0 --word 0x0002 0xFFC0
    --byte 0x4000 0x01 --byte 0x4001 0x00 --word 0x4002 0x2211 --word 0x4004 0x3300 --word 0x4006 0x4018)

# Startup prefix: clrp; mov x,#$FF; mov sp,x; mov a,#$C0; mov y,#$FF.
file(READ "${TEST_DIR}/runtime.bin" runtime_hex HEX)
string(SUBSTRING "${runtime_hex}" 0 18 startup_prefix)
if(NOT startup_prefix STREQUAL "20cdffbde8c08dffda")
    message(FATAL_ERROR "Unexpected SPC700 startup prefix: ${startup_prefix}")
endif()

# The linked listing, startup included, reassembles to the same payload.
run_command("${DISCAS}" "${TEST_DIR}/runtime-final.s" -o "${TEST_DIR}/runtime-final.o")
run_command("${DISCLD}" "${TEST_DIR}/runtime-final.o" -o "${TEST_DIR}/runtime-final.bin")
file(SHA256 "${TEST_DIR}/runtime.bin" direct_hash)
file(SHA256 "${TEST_DIR}/runtime-final.bin" final_hash)
if(NOT direct_hash STREQUAL final_hash)
    message(FATAL_ERROR "SPC700 linked runtime listing did not round-trip")
endif()

# Default placement: static RAM follows the payload; same observable result.
run_command("${DISCLD}" "${TEST_DIR}/program.o" "${TEST_DIR}/statics.o" --init-runtime
    -o "${TEST_DIR}/default.bin")
run_command("${SPC700_RUNNER}" "${TEST_DIR}/default.bin" 0x0200 --fill 0xEE --runs 2 --register ya 0x9DCF)

# Host-initialized globals: no startup, static RAM still placed after the payload.
run_command("${DISCLD}" "${TEST_DIR}/program.o" "${TEST_DIR}/statics.o" --host-initialized-globals
    -o "${TEST_DIR}/host-globals.bin")

# A program with no static RAM gets the shortest startup and still returns.
file(WRITE "${TEST_DIR}/plain.s" ".setcpu \"SPC700\"\n.export main\n.segment \"CODE\"\nmain:\n    mov a,#$34\n    mov y,#$12\n    ret\n")
run_command("${DISCAS}" "${TEST_DIR}/plain.s" -o "${TEST_DIR}/plain.o")
run_command("${DISCLD}" "${TEST_DIR}/plain.o" --init-runtime --stack-pointer 0x8000 -o "${TEST_DIR}/plain.bin")
run_command("${SPC700_RUNNER}" "${TEST_DIR}/plain.bin" 0x0200 --port 2 0x34 --port 3 0x12 --word 0x0000 0x8000)

# --- Rejected layouts; the output file must survive every rejection.
function(spc700_reject name diagnostic)
    set(output "${TEST_DIR}/${name}.bin")
    file(WRITE "${output}" "preserve-existing-output")
    run_expected_failure_contains("${diagnostic}" "${DISCLD}" ${ARGN} -o "${output}")
    file(READ "${output}" unchanged)
    if(NOT unchanged STREQUAL "preserve-existing-output")
        message(FATAL_ERROR "Rejected SPC700 link overwrote its output: ${name}")
    endif()
endfunction()
set(objects "${TEST_DIR}/program.o" "${TEST_DIR}/statics.o")
spc700_reject(hw-stack "SPC700 payload overlaps the hardware stack page" ${objects} --init-runtime --origin 0x0180)
spc700_reject(runtime-dp "SPC700 payload overlaps the runtime's direct page" ${objects} --init-runtime --origin 0x0000)
spc700_reject(mmio "SPC700 payload overlaps the I/O registers" "${TEST_DIR}/plain.o" --origin 0x00EE)
spc700_reject(ipl "SPC700 static RAM overlaps the IPL ROM area" ${objects} --init-runtime --ram-origin 0xFFBC)
spc700_reject(ram-mmio "SPC700 static RAM overlaps the I/O registers" ${objects} --init-runtime --ram-origin 0x00F0)
spc700_reject(ram-hw-stack "SPC700 static RAM overlaps the hardware stack page" ${objects} --init-runtime --ram-origin 0x0100)
spc700_reject(ram-runtime-dp "SPC700 static RAM overlaps the runtime's direct page" ${objects} --init-runtime --ram-origin 0x0010)
spc700_reject(payload-straddles-top "SPC700 payload overlaps the software stack top" ${objects} --init-runtime
    --ram-origin 0x4000 --stack-pointer 0x0204)
spc700_reject(ram-payload "Static RAM overlaps the linked payload" ${objects} --init-runtime --ram-origin 0x0200)
spc700_reject(no-runtime "Static globals require --init-runtime or --host-initialized-globals" ${objects})
spc700_reject(stack-top "SPC700 software stack top must not exceed \\$FFC0" ${objects} --init-runtime --stack-pointer 0xFFC2)
spc700_reject(ram-straddles-top "SPC700 static RAM overlaps the software stack top" ${objects} --init-runtime
    --ram-origin 0x4000 --stack-pointer 0x4004)
spc700_reject(no-stack-room "leaves no room for the software stack" ${objects} --init-runtime
    --ram-origin 0x4000 --stack-pointer 0x4008)
spc700_reject(ram-bank "--ram-bank and --rom-bank are GSU-only options" ${objects} --init-runtime --ram-bank 1)
spc700_reject(entry "Runtime entry must name an instruction in CODE: missing" ${objects} --init-runtime --entry missing)
spc700_reject(entry-ram "Runtime entry must name an instruction in CODE: t3" ${objects} --init-runtime --entry t3)
file(WRITE "${TEST_DIR}/big-frame.s" ".setcpu \"SPC700\"\n.export main\n.segment \"CODE\"\nmain:\n    mov a,#<__disco_stack_limit_60000\n    ret\n")
run_command("${DISCAS}" "${TEST_DIR}/big-frame.s" -o "${TEST_DIR}/big-frame.o")
spc700_reject(big-frame "Initial stack pointer leaves no room for the requested stack frame" "${TEST_DIR}/big-frame.o"
    --init-runtime --stack-pointer 0x8000)
file(WRITE "${TEST_DIR}/limit-dp.s" ".setcpu \"SPC700\"\n.export main\n.segment \"CODE\"\nmain:\n    mov a,__disco_stack_limit_4\n    ret\n")
run_command("${DISCAS}" "${TEST_DIR}/limit-dp.s" -o "${TEST_DIR}/limit-dp.o")
spc700_reject(limit-dp "Stack-limit symbol requires an SPC700 absolute or immediate relocation" "${TEST_DIR}/limit-dp.o" --init-runtime)
