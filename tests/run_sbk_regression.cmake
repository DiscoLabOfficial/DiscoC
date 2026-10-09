# SBK must preserve every observable memory access. Both backends share only
# code generation: reassembly and the independent opcode model check the bytes.
function(check_sbk name source selected)
    foreach(level IN ITEMS 0 1)
        run_command("${DISCC}" "-O${level}" "${source}" -o "${TEST_DIR}/${name}-${level}.o")
        run_command("${DISCC}" "-O${level}" --emit-asm "${source}" -o "${TEST_DIR}/${name}-${level}.s")
        run_command("${DISCAS}" "${TEST_DIR}/${name}-${level}.s" -o "${TEST_DIR}/${name}-${level}-asm.o")
        set(expected 0)
        if(level EQUAL 1)
            set(expected ${selected})
        endif()
        foreach(suffix IN ITEMS "" "-asm")
            run_command("${DISCLD}" "${TEST_DIR}/${name}-${level}${suffix}.o" --init-runtime
                --origin 0x706000 --stack-pointer 0x2000 -o "${TEST_DIR}/${name}-${level}${suffix}.bin")
            run_command("${GSU_RUNNER}" "${TEST_DIR}/${name}-${level}${suffix}.bin" 0x706000
                --sbk 0 ${expected} --register 0 42 --register 6 0 --register 10 0x1ffc ${ARGN})
        endforeach()
        file(SHA256 "${TEST_DIR}/${name}-${level}.bin" direct)
        file(SHA256 "${TEST_DIR}/${name}-${level}-asm.bin" assembly)
        if(NOT direct STREQUAL assembly)
            message(FATAL_ERROR "${name}: SBK assembly round trip differs from direct codegen")
        endif()
        file(READ "${TEST_DIR}/${name}-${level}.s" text)
        if(expected EQUAL 0 AND text MATCHES "msbk")
            message(FATAL_ERROR "${name}: SBK was selected across an address-latch hazard")
        endif()
    endforeach()
endfunction()

check_sbk(rmw "${ROOT_DIR}/tests/fixtures/gsu_sbk.dc" 9
    --word 0x700100 4698 --word 0x700102 24 --byte 0x700104 9
    --ram-byte 0x700105 0xaa --byte 0x700105 0xaa --word 0x700108 30 --word 0x70010c 3
    --reads 0x700100 7 --writes 0x700100 8 --reads 0x700104 1 --writes 0x700104 2)

# Distinct RAM accesses, callee writes, far-bank selection and a CFG join must
# each prevent reuse. Same-named near/far pointer targets are not equivalent.
set(hazards [=[
word mutate() { *(volatile word*)0x100 = 50; return 3; }
word main() {
    volatile word* a = (volatile word*)0x100;
    volatile word* b = (volatile word*)0x102;
    *a = 10;
    *b = 20;
    *a += *b;
    *a += mutate();
    far volatile word* other_bank = (far volatile word*)0x710100;
    *other_bank = 40;
    *a += *other_bank;
    word saved = *a;
    if (*(volatile byte*)0x110 != 0) { *b = 25; } else { *b = 26; }
    *a = saved + 1;
    return 42;
}
]=])
file(WRITE "${TEST_DIR}/hazards.dc" "${hazards}")
foreach(flag IN ITEMS 0 1)
    math(EXPR expected_b "26 - ${flag}")
    check_sbk(hazards-${flag} "${TEST_DIR}/hazards.dc" 0
        --ram-byte 0x700110 ${flag} --word 0x700100 74
        --word 0x700102 ${expected_b} --reads 0x700102 1 --writes 0x700102 2
        --word 0x710100 40 --rambr 0 0 --reads 0x700100 4 --writes 0x700100 6)
endforeach()

# More live volatile reads than R5/R7/R8 force real spills between the read
# and write. A stale latch would overwrite a spill rather than the requested word.
set(pressure [=[
word main() {
    volatile word* p = (volatile word*)0x100;
    *p = *p + (*(volatile word*)0x102 + (*(volatile word*)0x104 +
         (*(volatile word*)0x106 + (*(volatile word*)0x108 + *(volatile word*)0x10a))));
    return 42;
}
]=])
file(WRITE "${TEST_DIR}/pressure.dc" "${pressure}")
check_sbk(pressure "${TEST_DIR}/pressure.dc" 0
    --ram-byte 0x700100 1 --ram-byte 0x700102 2 --ram-byte 0x700104 3
    --ram-byte 0x700106 4 --ram-byte 0x700108 5 --ram-byte 0x70010a 6
    --word 0x700100 21 --reads 0x700100 1 --writes 0x700100 1
    --word 0x700102 2 --word 0x700104 3 --word 0x700106 4 --word 0x700108 5 --word 0x70010a 6)
