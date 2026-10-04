file(RELATIVE_PATH library_dir "${TEST_DIR}" "${ROOT_DIR}/lib")
string(REPLACE "\\" "/" library_dir "${library_dir}")

function(build_library name source)
    run_command("${DISCC}" "${source}" -o "${TEST_DIR}/${name}.o")
    run_command("${DISCC}" --emit-asm "${source}" -o "${TEST_DIR}/${name}.s")
    run_command("${DISCAS}" "${TEST_DIR}/${name}.s" -o "${TEST_DIR}/${name}-asm.o")
endfunction()

function(library_fixture name source library)
    file(WRITE "${TEST_DIR}/${name}.dc" "${source}")
    build_library("${name}" "${TEST_DIR}/${name}.dc")
    foreach(backend IN ITEMS direct asm mixed)
        if(backend STREQUAL "direct")
            set(objects "${TEST_DIR}/${name}.o")
        elseif(backend STREQUAL "asm")
            set(objects "${TEST_DIR}/${name}-asm.o")
        else()
            set(objects "${TEST_DIR}/${name}.o")
        endif()
        foreach(unit IN LISTS library)
            if(backend STREQUAL "direct")
                list(APPEND objects "${TEST_DIR}/${unit}.o")
            else()
                list(APPEND objects "${TEST_DIR}/${unit}-asm.o")
            endif()
        endforeach()
        run_command("${DISCLD}" ${objects} --init-runtime --emit-asm "${TEST_DIR}/${name}-${backend}-final.s"
            -o "${TEST_DIR}/${name}-${backend}.bin")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/${name}-${backend}.bin" 0x8000 ${ARGN})
        file(SHA256 "${TEST_DIR}/${name}-${backend}.bin" ${backend}_hash)
    endforeach()
    if(NOT direct_hash STREQUAL asm_hash OR NOT direct_hash STREQUAL mixed_hash)
        message(FATAL_ERROR "Library ${name} payloads differ between direct/assembled/mixed objects.")
    endif()
    run_command("${DISCAS}" "${TEST_DIR}/${name}-direct-final.s" -o "${TEST_DIR}/${name}-final.o")
    run_command("${DISCLD}" "${TEST_DIR}/${name}-final.o" -o "${TEST_DIR}/${name}-final.bin")
    file(SHA256 "${TEST_DIR}/${name}-final.bin" final_hash)
    if(NOT direct_hash STREQUAL final_hash)
        message(FATAL_ERROR "Final linked library assembly is not byte-exact for ${name}.")
    endif()
endfunction()

if(CASE STREQUAL "language_fixed_point")
    build_library(fixed "${ROOT_DIR}/lib/core/fixed.dc")
    run_command("${DISCC}" --check --target spc700 "${ROOT_DIR}/lib/core/fixed.dc")
    run_expected_failure_contains("-Wexpensive-helper" "${DISCC}" --check -Wall -Werror "${ROOT_DIR}/lib/core/fixed.dc")
    run_command("${DISCC}" --check -Wall -Werror -Wno-expensive-helper "${ROOT_DIR}/lib/core/fixed.dc")
    set(pairs "0,0" "1,1" "-1,1" "1,-1" "-1,-1" "255,257" "-255,257"
        "256,384" "384,512" "32767,32767" "-32768,-32768" "-32768,1"
        "-32768,-1" "32767,1" "32767,-1" "-12345,23456" "12345,-23456"
        "32512,384" "16,3" "-16,3" "1,32767" "-1,32767" "-32768,32767" "0,256")
    set(body "")
    set(expectations "")
    set(address 0x100)
    foreach(format IN ITEMS 8_8 12_4)
        set(body "")
        set(expectations "")
        set(address 0x100)
        if(format STREQUAL "8_8")
            set(scale 256)
        else()
            set(scale 16)
        endif()
        foreach(pair IN LISTS pairs)
            string(REPLACE "," ";" operands "${pair}")
            list(GET operands 0 left)
            list(GET operands 1 right)
            foreach(operation IN ITEMS add sub mul div)
                if(operation STREQUAL "div" AND right EQUAL 0)
                    continue()
                endif()
                # CMake evaluates signed 64-bit intermediates independently of
                # the word-based DiscoC implementation, then applies its wrap.
                if(operation STREQUAL "add")
                    math(EXPR reference "(${left}) + (${right})")
                elseif(operation STREQUAL "sub")
                    math(EXPR reference "(${left}) - (${right})")
                elseif(operation STREQUAL "mul")
                    math(EXPR reference "((${left}) * (${right})) / ${scale}")
                else()
                    math(EXPR reference "((${left}) * ${scale}) / (${right})")
                endif()
                math(EXPR expected "(${reference}) & 65535")
                string(APPEND body "*(i16*)${address} = dc_q${format}_${operation}(${left}, ${right});\n")
                list(APPEND expectations --word ${address} ${expected})
                math(EXPR address "${address} + 2")
            endforeach()
        endforeach()
        foreach(value IN ITEMS -32768 -32767 -257 -256 -255 -1 0 1 255 256 257 32767)
            math(EXPR expected "((${value}) * ${scale}) & 65535")
            string(APPEND body "*(i16*)${address} = dc_q${format}_from_int(${value});\n")
            list(APPEND expectations --word ${address} ${expected})
            math(EXPR address "${address} + 2")
            math(EXPR expected "((${value}) / ${scale}) & 65535")
            string(APPEND body "*(i16*)${address} = dc_q${format}_to_int(${value});\n")
            list(APPEND expectations --word ${address} ${expected})
            math(EXPR address "${address} + 2")
        endforeach()
        library_fixture(fixed_vectors_${format} "import \"${library_dir}/core/fixed.dci\";\nvoid main() { ${body} }" fixed ${expectations} --register 6 0)
        math(EXPR vectors "(${address} - 256) / 2")
        message(STATUS "Verified ${vectors} Q${format} outputs against independent integer references")
    endforeach()
    library_fixture(fixed_zero "import \"${library_dir}/core/fixed.dci\";\nvoid main() { dc_q8_8_div(256, 0); }" fixed --register 6 6)
    library_fixture(fixed_zero_q12 "import \"${library_dir}/core/fixed.dci\";\nvoid main() { dc_q12_4_div(16, 0); }" fixed --register 6 6)
    build_library(memory "${ROOT_DIR}/lib/core/memory.dc")
    file(READ "${ROOT_DIR}/examples/fixed_point/main.dc" example)
    string(REPLACE "../../lib/" "${library_dir}/" example "${example}")
    library_fixture(fixed_example "${example}" "fixed;memory"
        --word 0x100 768 --word 0x102 65440 --word 0x104 0 --word 0x106 72
        --byte 0x700110 0 --byte 0x700111 128 --byte 0x700112 255 --byte 0x700113 42 --register 6 0)
elseif(CASE STREQUAL "language_memory_library")
    build_library(memory "${ROOT_DIR}/lib/core/memory.dc")
    run_command("${DISCC}" --check --target spc700 "${ROOT_DIR}/lib/core/memory.dc")
    set(body [=[
void main() {
    u8 source[4] = {1, 127, 128, 255};
    u8 destination[6] = {9, 9, 9, 9, 9, 9};
    dc_memcpy(&destination[1], &source[0], 4);
    for (u16 index = 0; index < 6; index++) *(u8*)(0x100 + index) = destination[index];
    dc_memset(&destination[0], 255, 6);
    for (u16 index = 0; index < 6; index++) *(u8*)(0x110 + index) = destination[index];
    u8 overlap[6] = {1, 2, 3, 4, 5, 6};
    dc_memmove(&overlap[1], &overlap[0], 5);
    for (u16 index = 0; index < 6; index++) *(u8*)(0x120 + index) = overlap[index];
    dc_memmove(&overlap[0], &overlap[1], 5);
    for (u16 index = 0; index < 6; index++) *(u8*)(0x130 + index) = overlap[index];
    dc_memmove(&overlap[0], &overlap[0], 6);
    for (u16 index = 0; index < 6; index++) *(u8*)(0x140 + index) = overlap[index];
    dc_memcpy(null, null, 0);
    dc_memmove(null, null, 0);
    dc_memset(null, 255, 0);
}
]=])
    library_fixture(memory_vectors "import \"${library_dir}/core/memory.dci\";\n${body}" memory
        --byte 0x700100 9 --byte 0x700101 1 --byte 0x700102 127 --byte 0x700103 128 --byte 0x700104 255 --byte 0x700105 9
        --byte 0x700110 255 --byte 0x700111 255 --byte 0x700112 255 --byte 0x700113 255 --byte 0x700114 255 --byte 0x700115 255
        --byte 0x700120 1 --byte 0x700121 1 --byte 0x700122 2 --byte 0x700123 3 --byte 0x700124 4 --byte 0x700125 5
        --byte 0x700130 1 --byte 0x700131 2 --byte 0x700132 3 --byte 0x700133 4 --byte 0x700134 5 --byte 0x700135 5
        --byte 0x700140 1 --byte 0x700141 2 --byte 0x700142 3 --byte 0x700143 4 --byte 0x700144 5 --byte 0x700145 5 --register 6 0)
    library_fixture(memory_end "import \"${library_dir}/core/memory.dci\";\nvoid main() { dc_memset((u8*)0xffff, 149, 1); }" memory
        --byte 0x70ffff 149 --register 6 0)
    library_fixture(memory_invalid "import \"${library_dir}/core/memory.dci\";\nvoid main() { dc_memset(null, 149, 1); }" memory --register 6 2)
    library_fixture(memory_crossing "import \"${library_dir}/core/memory.dci\";\nvoid main() { dc_memset((u8*)0xffff, 149, 2); }" memory
        --byte 0x70ffff 149 --register 6 3)
    file(WRITE "${TEST_DIR}/memory_volatile.dc" "import \"${library_dir}/core/memory.dci\";\nvoid main() { volatile u8* port = (volatile u8*)0x100; dc_memset(port, 1, 1); }")
    run_expected_failure_contains("Cannot implicitly convert" "${DISCC}" --check "${TEST_DIR}/memory_volatile.dc")
    file(WRITE "${TEST_DIR}/memory_rom.dc" "import \"${library_dir}/core/memory.dci\";\nrom const u8 source[] = {1}; void main() { dc_memcpy((u8*)0x100, source, 1); }")
    run_expected_failure_contains("Cannot implicitly convert" "${DISCC}" --check "${TEST_DIR}/memory_rom.dc")
elseif(CASE STREQUAL "language_target_libraries")
    build_library(graphics "${ROOT_DIR}/lib/targets/gsu/graphics.dc")
    file(WRITE "${TEST_DIR}/selected.dc" [=[
@cfg(gsu) @target(gsu) i16 selected() { return 149; }
@cfg(spc700) @target(spc700) i16 selected() { return 42; }
void main() { *(i16*)0x100 = selected(); }
]=])
    file(READ "${TEST_DIR}/selected.dc" source)
    library_fixture(selected "${source}" graphics --word 0x100 149 --register 6 0)
    execute_process(COMMAND "${DISCC}" --emit-ir --target spc700 "${TEST_DIR}/selected.dc"
        RESULT_VARIABLE result OUTPUT_VARIABLE ir ERROR_VARIABLE error)
    if(NOT result STREQUAL "0" OR NOT ir MATCHES "42" OR ir MATCHES "149")
        message(FATAL_ERROR "Target selection did not choose the SPC700 implementation: ${ir}\n${error}")
    endif()
    run_expected_failure_contains("@target does not match" "${DISCC}" --check --target spc700 "${ROOT_DIR}/lib/targets/gsu/graphics.dc")
endif()
