set(source "${ROOT_DIR}/tests/fixtures/gsu_optimization.dc")
foreach(level IN ITEMS default 0 1 alias)
    set(flag "")
    if(level STREQUAL "alias")
        set(flag -O)
    elseif(NOT level STREQUAL "default")
        set(flag "-O${level}")
    endif()
    run_command("${DISCC}" ${flag} "${source}" -o "${TEST_DIR}/${level}.o")
    run_command("${DISCC}" ${flag} --emit-asm "${source}" -o "${TEST_DIR}/${level}.s")
    run_command("${DISCAS}" "${TEST_DIR}/${level}.s" -o "${TEST_DIR}/${level}-asm.o")
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/${level}${suffix}.o" --init-runtime
            --origin 0x706000 --ram-bank 0 --stack-pointer 0x2000
            -o "${TEST_DIR}/${level}${suffix}.bin")
        set(colors 2)
        if(level STREQUAL "1" OR level STREQUAL "alias")
            set(colors 1)
        endif()
        run_command("${GSU_RUNNER}" "${TEST_DIR}/${level}${suffix}.bin" 0x706000
            --initial-sp 0x7770 --initial-ram-bank 1 --ram-byte 0x700100 5
            --reads 0x700100 2 --writes 0x700100 1 --word 0x700104 12
            --word 0x700108 24 --word 0x70010a 20 --word 0x70010c 15 --word 0x70010e 5
            --word 0x700200 127 --word 0x700202 128 --word 0x700204 255 --word 0x700206 32768
            --word 0x700208 65408 --word 0x70020a 65407 --word 0x70020c 1 --word 0x70020e 65534
            --word 0x700110 12 --byte 0x700112 5 --plots 0 2 --rpix 0 3 --color 0 ${colors}
            --register 0 42 --register 6 0 --register 10 0x1ffc --rambr 0 0)
    endforeach()
    file(SHA256 "${TEST_DIR}/${level}.bin" direct)
    file(SHA256 "${TEST_DIR}/${level}-asm.bin" assembly)
    if(NOT direct STREQUAL assembly)
        message(FATAL_ERROR "O${level} assembly export differs from direct codegen")
    endif()
endforeach()
file(SHA256 "${TEST_DIR}/default.o" default)
file(SHA256 "${TEST_DIR}/0.o" explicit)
if(NOT default STREQUAL explicit)
    message(FATAL_ERROR "O0 changed the default baseline")
endif()
file(SHA256 "${TEST_DIR}/1.o" explicit)
file(SHA256 "${TEST_DIR}/alias.o" alias)
if(NOT explicit STREQUAL alias)
    message(FATAL_ERROR "-O must be equivalent to -O1")
endif()
file(SIZE "${TEST_DIR}/0.bin" baseline_bytes)
file(SIZE "${TEST_DIR}/1.bin" optimized_bytes)
if(NOT optimized_bytes LESS baseline_bytes)
    message(FATAL_ERROR "Optimization did not reduce the regression payload")
endif()
run_expected_failure_contains("Unsupported optimization level" "${DISCC}" -O3 "${source}")
run_expected_failure_contains("Unsupported optimization level" "${DISCC}" -Ofast "${source}")
run_expected_failure_contains("Unknown project-build option" "${DISCC}" build -O3)

foreach(count IN ITEMS 0 1 2 127 128 65535)
    set(loop_source [=[
word main() {
    plot {
        options;
        color 5;
        at (32, 40);
        for (unsigned word i = @count@; i > 0; i = i - 1) { pixel; }
        *(unsigned word*)0x100 = (unsigned word)cursor.x;

        at (8, 48);
        color 5;
        for (word j = 4; j > 0; j = j - 1) { color 5; pixel; color 3; }
        *(word*)0x102 = cursor.x;
        flush;
        *(byte*)0x104 = read_pixel at (32, 40);
        *(byte*)0x105 = read_pixel at (8, 48);

        options;
        color 5;
        at (8, 56);
        for (word k = 4; k > 0; k = k - 1) { options; pixel; options dither; }
        *(byte*)0x106 = read_pixel at (9, 56);
    }
    return 42;
}
]=])
    string(CONFIGURE "${loop_source}" loop_source @ONLY)
    file(WRITE "${TEST_DIR}/loop-${count}.dc" "${loop_source}")
    math(EXPR expected_x "(32 + ${count}) & 65535")
    math(EXPR expected_plots "${count} + 8")
    set(first_color 0)
    if(count GREATER 0)
        set(first_color 5)
    endif()
    foreach(level IN ITEMS 0 1)
        run_command("${DISCC}" "-O${level}" "${TEST_DIR}/loop-${count}.dc" -o "${TEST_DIR}/loop-${count}-${level}.o")
        run_command("${DISCC}" "-O${level}" --emit-asm "${TEST_DIR}/loop-${count}.dc" -o "${TEST_DIR}/loop-${count}-${level}.s")
        run_command("${DISCAS}" "${TEST_DIR}/loop-${count}-${level}.s" -o "${TEST_DIR}/loop-${count}-${level}-asm.o")
        foreach(suffix IN ITEMS "" "-asm")
            run_command("${DISCLD}" "${TEST_DIR}/loop-${count}-${level}${suffix}.o" --init-runtime
                -o "${TEST_DIR}/loop-${count}-${level}${suffix}.bin")
            set(plot_check "")
            if(expected_plots LESS_EQUAL 65535)
                set(plot_check --plots 0 ${expected_plots})
            endif()
            run_command("${GSU_RUNNER}" "${TEST_DIR}/loop-${count}-${level}${suffix}.bin" 0x8000
                --word 0x700100 ${expected_x} --word 0x700102 12
                --byte 0x700104 ${first_color} --byte 0x700105 5 --byte 0x700106 5
                --register 0 42 --register 6 0 --register 12 0 --color 0 11 --colr 0 5 ${plot_check})
        endforeach()
    endforeach()
endforeach()

foreach(target IN ITEMS gsu spc700)
    foreach(level IN ITEMS 0 1)
        execute_process(COMMAND "${DISCC}" "-O${level}" --target "${target}" --emit-ir
            "${ROOT_DIR}/tests/fixtures/gsu_optimization_compare.dc"
            RESULT_VARIABLE result OUTPUT_VARIABLE ir_${level} ERROR_VARIABLE error)
        if(NOT result STREQUAL "0" OR ir_${level} STREQUAL "")
            message(FATAL_ERROR "Failed to inspect ${target} IR at O${level}: ${error}")
        endif()
    endforeach()
    if(NOT ir_0 STREQUAL ir_1)
        message(FATAL_ERROR "Optimization policy changed frontend/IR semantics for ${target}")
    endif()
endforeach()

# Independent 8x8 truth tables cover both boolean materialization and fused
# CMP/branch lowering. Seeds are runtime volatile values, not foldable literals.
foreach(level IN ITEMS 0 1)
    run_command("${DISCC}" "-O${level}" "${ROOT_DIR}/tests/fixtures/gsu_optimization_compare.dc"
        -o "${TEST_DIR}/compare-${level}.o")
    run_command("${DISCC}" "-O${level}" --emit-asm
        "${ROOT_DIR}/tests/fixtures/gsu_optimization_compare.dc" -o "${TEST_DIR}/compare-${level}.s")
    run_command("${DISCAS}" "${TEST_DIR}/compare-${level}.s" -o "${TEST_DIR}/compare-${level}-asm.o")
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/compare-${level}${suffix}.o" --init-runtime
            --origin 0x706000 -o "${TEST_DIR}/compare-${level}${suffix}.bin")
    endforeach()
endforeach()
foreach(a IN ITEMS 0 1 127 128 32767 32768 65408 65535)
    foreach(b IN ITEMS 0 1 127 128 32767 32768 65408 65535)
        math(EXPR a_lo "${a} & 255")
        math(EXPR a_hi "${a} >> 8")
        math(EXPR b_lo "${b} & 255")
        math(EXPR b_hi "${b} >> 8")
        set(checks --ram-byte 0x700100 ${a_lo} --ram-byte 0x700101 ${a_hi}
            --ram-byte 0x700102 ${b_lo} --ram-byte 0x700103 ${b_hi}
            --reads 0x700100 1 --reads 0x700102 1 --register 0 42 --register 6 0)
        math(EXPR sa "${a} - (${a} / 32768) * 65536")
        math(EXPR sb "${b} - (${b} / 32768) * 65536")
        set(address 0x700120)
        set(bits_address 0x700138)
        foreach(sign IN ITEMS unsigned signed)
            set(left ${a})
            set(right ${b})
            if(sign STREQUAL "signed")
                set(left ${sa})
                set(right ${sb})
            endif()
            set(bits 0)
            set(bit 1)
            foreach(comparison IN ITEMS LESS LESS_EQUAL GREATER GREATER_EQUAL EQUAL NOT_EQUAL)
                set(expected 0)
                if(comparison STREQUAL "NOT_EQUAL")
                    if(NOT left EQUAL right)
                        set(expected 1)
                    endif()
                elseif(left ${comparison} right)
                    set(expected 1)
                endif()
                list(APPEND checks --word ${address} ${expected})
                math(EXPR bits "${bits} | (${expected} * ${bit})")
                math(EXPR bit "${bit} * 2")
                math(EXPR address "${address} + 2")
            endforeach()
            list(APPEND checks --word ${bits_address} ${bits})
            math(EXPR bits_address "${bits_address} + 2")
        endforeach()
        foreach(level IN ITEMS 0 1)
            foreach(suffix IN ITEMS "" "-asm")
                run_command("${GSU_RUNNER}" "${TEST_DIR}/compare-${level}${suffix}.bin" 0x706000 ${checks})
            endforeach()
        endforeach()
    endforeach()
endforeach()

# Fast near-pointer scaling must preserve the baseline's success addresses
# and fail-stop categories, including -32768 and unsigned counts above 32767.
foreach(level IN ITEMS 0 1)
    run_command("${DISCC}" "-O${level}" "${ROOT_DIR}/tests/fixtures/gsu_optimization_offsets.dc"
        -o "${TEST_DIR}/offsets-${level}.o")
    run_command("${DISCC}" "-O${level}" --emit-asm
        "${ROOT_DIR}/tests/fixtures/gsu_optimization_offsets.dc" -o "${TEST_DIR}/offsets-${level}.s")
    run_command("${DISCAS}" "${TEST_DIR}/offsets-${level}.s" -o "${TEST_DIR}/offsets-${level}-asm.o")
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/offsets-${level}${suffix}.o" --init-runtime
            --origin 0x706000 -o "${TEST_DIR}/offsets-${level}${suffix}.bin")
    endforeach()
endforeach()
# mode,start,count,expected address,expected fault
set(offset_cases
    "0,32768,32767,65535,0" "0,32769,32767,0,3"
    "0,32769,-32768,1,0" "0,32768,-32768,0,2" "0,32767,-32768,0,3"
    "0,65535,1,0,3" "0,1,-1,0,2"
    "1,65535,32767,32768,0" "1,32768,-32768,0,3"
    "2,32768,16383,65534,0" "2,32770,16383,0,3"
    "2,32770,-16384,2,0" "2,32768,-16384,0,2"
    "2,32770,-32768,0,3" "2,2,32767,0,3"
    "3,65534,32766,2,0" "3,2,1,0,2" "3,2,2,0,3"
    "4,32769,-1,32768,0" "4,32768,-1,0,3" "4,32768,32767,65535,0"
    "5,32769,1,32768,0" "5,32768,1,0,3"
    "6,1,65534,65535,0" "6,2,65534,0,3" "6,1,65535,0,3"
    "7,2,32768,0,3")
foreach(test IN LISTS offset_cases)
    string(REPLACE "," ";" fields "${test}")
    list(GET fields 0 mode)
    list(GET fields 1 start)
    list(GET fields 2 count)
    list(GET fields 3 expected)
    list(GET fields 4 fault)
    math(EXPR start_lo "${start} & 255")
    math(EXPR start_hi "${start} >> 8")
    math(EXPR count_lo "${count} & 255")
    math(EXPR count_hi "(${count} >> 8) & 255")
    set(checks --ram-byte 0x700100 ${start_lo} --ram-byte 0x700101 ${start_hi}
        --ram-byte 0x700102 ${count_lo} --ram-byte 0x700103 ${count_hi}
        --ram-byte 0x700106 ${mode} --word 0x700104 ${expected} --register 6 ${fault}
        --rambr 0 0 --rombr 0 0)
    if(fault EQUAL 0)
        list(APPEND checks --register 0 42)
    endif()
    foreach(level IN ITEMS 0 1)
        foreach(suffix IN ITEMS "" "-asm")
            run_command("${GSU_RUNNER}" "${TEST_DIR}/offsets-${level}${suffix}.bin" 0x706000 ${checks})
        endforeach()
    endforeach()
endforeach()

# Single-source and project/import compilation have identical CLI precedence.
file(WRITE "${TEST_DIR}/discoc.toml" "[project]\nsources=['main.dc']\n[compiler]\noptimize=true\n[runtime]\ninitialize=true\n[output]\ndirectory='project'\nbinary='payload.bin'\n")
file(WRITE "${TEST_DIR}/main.dc" "word helper(); word main() { return helper() + 1; }")
file(WRITE "${TEST_DIR}/helper.dc" "word helper() { return 41; }")
# An import adds a dependency without manually listing it in project.sources.
file(WRITE "${TEST_DIR}/main.dc" "import \"helper.dc\"; word main() { return helper() + 1; }")
foreach(level IN ITEMS 0 1)
    run_command("${DISCC}" build --config "${TEST_DIR}/discoc.toml" "-O${level}"
        --output-dir "${TEST_DIR}/project-${level}")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/project-${level}/payload.bin" 0x8000 --register 0 42 --register 6 0)
    run_command("${DISCC}" "-O${level}" "${TEST_DIR}/main.dc" -o "${TEST_DIR}/main-${level}.o")
    run_command("${DISCC}" "-O${level}" "${TEST_DIR}/helper.dc" -o "${TEST_DIR}/helper-${level}.o")
    run_command("${DISCLD}" "${TEST_DIR}/main-${level}.o" "${TEST_DIR}/helper-${level}.o"
        --init-runtime -o "${TEST_DIR}/manual-${level}.bin")
    file(SHA256 "${TEST_DIR}/manual-${level}.bin" manual)
    file(SHA256 "${TEST_DIR}/project-${level}/payload.bin" project)
    if(NOT manual STREQUAL project)
        message(FATAL_ERROR "Project optimization does not reach imported implementations")
    endif()
    foreach(order IN ITEMS before after)
        if(order STREQUAL "before")
            set(arguments "-O${level}" --config "${TEST_DIR}/discoc.toml")
        else()
            set(arguments --config "${TEST_DIR}/discoc.toml" "-O${level}")
        endif()
        run_command("${DISCC}" ${arguments} "${TEST_DIR}/main.dc" -o "${TEST_DIR}/${order}-${level}.o")
        file(SHA256 "${TEST_DIR}/${order}-${level}.o" configured)
        file(SHA256 "${TEST_DIR}/main-${level}.o" cli)
        if(NOT configured STREQUAL cli)
            message(FATAL_ERROR "CLI must win regardless of --config position")
        endif()
    endforeach()
endforeach()
run_command("${DISCC}" build --config "${TEST_DIR}/discoc.toml" --output-dir "${TEST_DIR}/manifest")
file(SHA256 "${TEST_DIR}/manifest/payload.bin" configured)
file(SHA256 "${TEST_DIR}/project-1/payload.bin" cli)
if(NOT configured STREQUAL cli)
    message(FATAL_ERROR "compiler.optimize=true does not select O1")
endif()
# The calling convention and object format remain mixable across levels.
foreach(level IN ITEMS 0 1)
    math(EXPR other "1 - ${level}")
    run_command("${DISCLD}" "${TEST_DIR}/main-${level}.o" "${TEST_DIR}/helper-${other}.o"
        --init-runtime --emit-asm "${TEST_DIR}/mixed-${level}.s" -o "${TEST_DIR}/mixed-${level}.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/mixed-${level}.bin" 0x8000 --register 0 42 --register 6 0)
    run_command("${DISCAS}" "${TEST_DIR}/mixed-${level}.s" -o "${TEST_DIR}/final-${level}.o")
    run_command("${DISCLD}" "${TEST_DIR}/final-${level}.o" -o "${TEST_DIR}/final-${level}.bin")
    file(SHA256 "${TEST_DIR}/mixed-${level}.bin" original)
    file(SHA256 "${TEST_DIR}/final-${level}.bin" reassembled)
    if(NOT original STREQUAL reassembled)
        message(FATAL_ERROR "O1 final linked assembly is not byte exact")
    endif()
endforeach()
