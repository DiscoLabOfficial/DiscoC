set(link --origin 0x008000 --init-runtime --ram-bank 0 --stack-pointer 0x2000)
foreach(level IN ITEMS 0 1 2)
    set(OPTIMIZATION ${level})
    check_round_trip("proved-${level}" "${ROOT_DIR}/tests/fixtures/gsu_checked_proofs.dc" ${link})
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/proved-${level}${suffix}.o" ${link}
            --emit-asm "${TEST_DIR}/proved-${level}${suffix}-final.s" -o "${TEST_DIR}/proved-${level}${suffix}.bin")
        run_command("${DISCAS}" "${TEST_DIR}/proved-${level}${suffix}-final.s" -o "${TEST_DIR}/proved-${level}${suffix}-final.o")
        run_command("${DISCLD}" "${TEST_DIR}/proved-${level}${suffix}-final.o" -o "${TEST_DIR}/proved-${level}${suffix}-final.bin")
        file(SHA256 "${TEST_DIR}/proved-${level}${suffix}.bin" direct)
        file(SHA256 "${TEST_DIR}/proved-${level}${suffix}-final.bin" final)
        if(NOT direct STREQUAL final)
            message(FATAL_ERROR "O${level} checked-proofs final assembly failed its byte-exact round trip")
        endif()
        foreach(seed IN ITEMS 0 1 15 16 17 31 127 128 255 32767 32768 65535)
            math(EXPR low "${seed} & 255")
            math(EXPR high "${seed} >> 8")
            math(EXPR index "${seed} & 15")
            math(EXPR reverse "${seed} & 16")
            if(reverse)
                math(EXPR index "16 - ${index}")
            endif()
            math(EXPR selected "${index} + 1")
            math(EXPR backwards "(${seed} & 7) + 2")
            math(EXPR repeated "3 * ((${seed} & 31) + 2)")
            math(EXPR returned "${selected} + ${backwards} + 42 + ${repeated}")
            run_command("${GSU_RUNNER}" "${TEST_DIR}/proved-${level}${suffix}.bin" 0x008000
                --initial-sp 0x7777 --initial-ram-bank 1 --screen-mode 0 --screen-base 16
                --ram-byte 0x700100 ${low} --ram-byte 0x700101 ${high}
                --reads 0x700100 1 --word 0x700120 ${selected} --writes 0x700120 1
                --word 0x700122 ${backwards} --word 0x700124 42 --word 0x700126 ${repeated}
                --word 0x710200 9 --word 0x700128 10 --word 0x70012a 10 --word 0x70012c 2
                --plots 0 1 --rpix 0 2 --rambr 0 0 --register 0 ${returned} --register 6 0 --register 10 0x1ffc)
        endforeach()
    endforeach()
endforeach()

# Unknown runtime pointers and indices still fail at the original operation.
# Earlier observable stores survive, and no later store may execute.
function(pointer_case name base index byte_access fault expected)
    if(byte_access)
        set(type "unsigned byte")
        set(expression "(word)p[offset]")
    else()
        set(type "word")
        set(expression "p[offset]")
    endif()
    file(WRITE "${TEST_DIR}/${name}.dc"
        "word main() { unsigned word address=*(volatile unsigned word*)0x100; word offset=*(volatile word*)0x102; *(volatile word*)0x120=7; ${type}* p=(${type}*)address; word value=${expression}; *(volatile word*)0x122=value; return value; }")
    math(EXPR base_low "${base} & 255")
    math(EXPR base_high "${base} >> 8")
    math(EXPR index_low "${index} & 255")
    math(EXPR index_high "(${index} & 65535) >> 8")
    foreach(level IN ITEMS 0 1 2)
        set(OPTIMIZATION ${level})
        check_round_trip("${name}-${level}" "${TEST_DIR}/${name}.dc" ${link})
        foreach(suffix IN ITEMS "" "-asm")
            set(state --word 0x700120 7 --writes 0x700120 1 --register 6 ${fault})
            if(fault)
                list(APPEND state --word 0x700122 0 --writes 0x700122 0)
            else()
                list(APPEND state --word 0x700122 ${expected} --writes 0x700122 1 --register 0 ${expected} --register 10 0x1ffc)
            endif()
            run_command("${GSU_RUNNER}" "${TEST_DIR}/${name}-${level}${suffix}.bin" 0x008000
                --ram-byte 0x700100 ${base_low} --ram-byte 0x700101 ${base_high}
                --ram-byte 0x700102 ${index_low} --ram-byte 0x700103 ${index_high}
                --ram-byte 0x70fffe 149 --ram-byte 0x70ffff 0 --ram-byte 0x700002 149 ${state})
        endforeach()
    endforeach()
endfunction()
pointer_case(null-base 0 1 FALSE 2 0)
pointer_case(odd-word 65535 0 FALSE 1 0)
pointer_case(high-wrap 65534 1 FALSE 3 0)
pointer_case(low-wrap 2 -2 FALSE 3 0)
pointer_case(large-scale 2 32767 FALSE 3 0)
pointer_case(last-word 65534 0 FALSE 0 149)
pointer_case(negative-index 4 -1 FALSE 0 149)
pointer_case(last-byte 65535 0 TRUE 0 0)
pointer_case(byte-wrap 65535 1 TRUE 3 0)

# Canonical induction and its strength-reduced affine recurrence stay inside
# the RAM bank. Verify the first/last words and adjacent untouched storage,
# including assembly round trips. The stack is deliberately above this range.
file(WRITE "${TEST_DIR}/bounded-clear.dc"
    "word main() { for(word column=9;column<=22;column++) { unsigned word address=(unsigned word)(column*384+80); word* tiles=(word*)address; @cache for(word i=0;i<112;i++) tiles[i]=0; } return 42; }")
foreach(level IN ITEMS 0 1 2)
    set(OPTIMIZATION ${level})
    check_round_trip("bounded-clear-${level}" "${TEST_DIR}/bounded-clear.dc"
        --origin 0x008000 --init-runtime --ram-bank 0 --stack-pointer 0xfffe)
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/bounded-clear-${level}${suffix}.bin" 0x008000
            --ram-byte 0x700dce 149 --ram-byte 0x700dd0 149
            --ram-byte 0x70222e 149 --ram-byte 0x702230 149
            --word 0x700dce 149 --writes 0x700dce 0 --word 0x700dd0 0 --writes 0x700dd0 1
            --word 0x70222e 0 --writes 0x70222e 1 --word 0x702230 149 --writes 0x702230 0
            --register 0 42 --register 6 0 --register 10 0xfffa)
    endforeach()
endforeach()

# Both dominating guards are needed before a widened integer-to-pointer cast
# becomes safe. Inputs taking either false edge must not perform the read.
file(WRITE "${TEST_DIR}/guarded-interval.dc"
    "word main() { word seed=*(volatile word*)0x100; if(seed>=2) { if(seed<=100) return *((word*)(unsigned word)(seed*2)); } return 42; }")
foreach(level IN ITEMS 0 1 2)
    set(OPTIMIZATION ${level})
    check_round_trip("guarded-interval-${level}" "${TEST_DIR}/guarded-interval.dc" ${link})
    foreach(seed IN ITEMS 1 2 100 101 32767 32768 65535)
        math(EXPR low "${seed} & 255")
        math(EXPR high "${seed} >> 8")
        if(seed EQUAL 2 OR seed EQUAL 100)
            set(expected 149)
        else()
            set(expected 42)
        endif()
        foreach(suffix IN ITEMS "" "-asm")
            run_command("${GSU_RUNNER}" "${TEST_DIR}/guarded-interval-${level}${suffix}.bin" 0x008000
                --ram-byte 0x700100 ${low} --ram-byte 0x700101 ${high}
                --ram-byte 0x700004 149 --ram-byte 0x7000c8 149
                --register 0 ${expected} --register 6 0 --register 10 0x1ffc)
        endforeach()
    endforeach()
endforeach()

# Leave one temporary word above the actual entry frame requirement, including
# spills, at a zero/nonzero linker floor. O0 uses it to evaluate the witness.
# The following call cannot fit; its check must not precede that witness.
# Five arguments keep a stack argument area at every level: a four-word call
# to a stack-free leaf needs no stack at O2 and would legitimately fit.
foreach(reserved IN ITEMS 0 16)
    if(reserved)
        set(global "word reserved[8];")
    else()
        set(global "")
    endif()
    file(WRITE "${TEST_DIR}/stack-${reserved}.dc"
        "${global} word callee(word a, word b, word c, word d, word x); word main() { *(volatile word*)0x120=7; return callee(1, 2, 3, 4, 42); } word callee(word a, word b, word c, word d, word x) { *(volatile word*)0x122=9; return x; }")
    foreach(level IN ITEMS 0 1 2)
        set(OPTIMIZATION ${level})
        check_round_trip("stack-${reserved}-${level}" "${TEST_DIR}/stack-${reserved}.dc"
            --origin 0x008000 --ram-origin 0 --host-initialized-globals)
        file(READ "${TEST_DIR}/stack-${reserved}-${level}.s" assembly)
        if(NOT assembly MATCHES "__disco_stack_limit_([0-9]+)")
            message(FATAL_ERROR "Stack test lost the mandatory entry frame guard")
        endif()
        math(EXPR exact "${CMAKE_MATCH_1} + ${reserved} + 2")
        math(EXPR short "${CMAKE_MATCH_1} + ${reserved} - 2")
        math(EXPR odd "${CMAKE_MATCH_1} + ${reserved} + 1")
        foreach(suffix IN ITEMS "" "-asm")
            run_command("${GSU_RUNNER}" "${TEST_DIR}/stack-${reserved}-${level}${suffix}.bin" 0x008000
                --initial-sp ${exact} --word 0x700120 7 --writes 0x700120 1
                --word 0x700122 0 --writes 0x700122 0 --register 6 2)
            run_command("${GSU_RUNNER}" "${TEST_DIR}/stack-${reserved}-${level}${suffix}.bin" 0x008000
                --initial-sp ${short} --writes 0x700120 0 --writes 0x700122 0 --register 6 2)
            run_command("${GSU_RUNNER}" "${TEST_DIR}/stack-${reserved}-${level}${suffix}.bin" 0x008000
                --initial-sp ${odd} --writes 0x700120 0 --writes 0x700122 0 --register 6 1)
        endforeach()
    endforeach()
endforeach()

# A skipped call may not demand its stack space on a zero-trip path.
file(WRITE "${TEST_DIR}/skip-call.dc"
    "word callee(word x); word main() { *(volatile word*)0x120=7; if(*(volatile word*)0x100) return callee(42); return 42; } word callee(word x) { *(volatile word*)0x122=9; return x; }")
foreach(level IN ITEMS 0 1 2)
    set(OPTIMIZATION ${level})
    check_round_trip("skip-call-${level}" "${TEST_DIR}/skip-call.dc" --origin 0x008000)
    file(READ "${TEST_DIR}/skip-call-${level}.s" assembly)
    if(NOT assembly MATCHES "__disco_stack_limit_([0-9]+)")
        message(FATAL_ERROR "Skipped-call test lost its entry frame guard")
    endif()
    math(EXPR available "${CMAKE_MATCH_1} + 2")
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/skip-call-${level}${suffix}.bin" 0x008000
            --initial-sp ${available} --word 0x700120 7 --writes 0x700120 1
            --word 0x700122 0 --writes 0x700122 0 --register 0 42 --register 6 0)
    endforeach()
endforeach()

# The O2 frame check covers both saved-register pushes, but must itself remain.
file(WRITE "${TEST_DIR}/prologue.dc" "word main() { volatile word v=42; return v; }")
foreach(level IN ITEMS 1 2)
    set(OPTIMIZATION ${level})
    check_round_trip("prologue-${level}" "${TEST_DIR}/prologue.dc" ${link})
    file(READ "${TEST_DIR}/prologue-${level}.s" assembly)
    string(REGEX MATCHALL "__disco_stack_limit_2[^0-9]" pushes "${assembly}")
    list(LENGTH pushes push_checks)
    if((level EQUAL 1 AND NOT push_checks EQUAL 2) OR (level EQUAL 2 AND NOT push_checks EQUAL 0))
        message(FATAL_ERROR "Unexpected O${level} prologue push checks: ${push_checks}")
    endif()
    if(NOT assembly MATCHES "__disco_stack_limit_[0-9]+")
        message(FATAL_ERROR "Mandatory entry frame guard was removed")
    endif()
    run_command("${GSU_RUNNER}" "${TEST_DIR}/prologue-${level}.bin" 0x008000
        --register 0 42 --register 6 0 --register 10 0x1ffc)
endforeach()

# An O2 array frame can end at SP=0 without treating its bottom empty word as
# an address. The real elements remain nonnull/aligned, including member sugar.
file(WRITE "${TEST_DIR}/minimum-frame.dc" "word main() { word values[2]={12,30}; return values[0]+values[1]; }")
set(OPTIMIZATION 2)
check_round_trip(minimum-frame "${TEST_DIR}/minimum-frame.dc" --origin 0x008000)
file(READ "${TEST_DIR}/minimum-frame.s" assembly)
if(NOT assembly MATCHES "__disco_stack_limit_([0-9]+)")
    message(FATAL_ERROR "Minimum-frame test lost its entry extent check")
endif()
set(minimum "${CMAKE_MATCH_1}")
math(EXPR below "${minimum} - 2")
foreach(suffix IN ITEMS "" "-asm")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/minimum-frame${suffix}.bin" 0x008000
        --initial-sp ${minimum} --register 0 42 --register 6 0)
    run_command("${GSU_RUNNER}" "${TEST_DIR}/minimum-frame${suffix}.bin" 0x008000
        --initial-sp ${below} --register 6 2)
endforeach()

# A checked O2 caller interoperates with an O0/O1/O2 callee; its pointer
# argument is not considered constant/proven merely because it is on a stack.
file(WRITE "${TEST_DIR}/caller.dc"
    "word step(word* p); word main() { word v=40; step(&v); step(&v); return v; }")
file(WRITE "${TEST_DIR}/callee.dc" "word step(word* p) { *p+=1; return *p; }")
unset(OPTIMIZATION)
run_command("${DISCC}" -O2 "${TEST_DIR}/caller.dc" -o "${TEST_DIR}/caller.o")
foreach(level IN ITEMS 0 1 2)
    run_command("${DISCC}" -O${level} "${TEST_DIR}/callee.dc" -o "${TEST_DIR}/callee-${level}.o")
    run_command("${DISCLD}" "${TEST_DIR}/caller.o" "${TEST_DIR}/callee-${level}.o" ${link} -o "${TEST_DIR}/mixed-${level}.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/mixed-${level}.bin" 0x008000 --register 0 42 --register 6 0 --register 10 0x1ffc)
endforeach()

# Register entries across objects: an O2/Os caller calls `__disco_regcall1$step`
# and `__disco_regcall4$order`. An O2/Os callee defines them after loading its
# stack arguments; for an O0/O1 callee the linker supplies guarded adapters to
# the stack convention. O0/O1 callers keep calling `step` and `order`. The
# four-argument `order` is not symmetric in any pair of arguments, so a
# misordered load, push or register (R4 is the fourth) changes its result.
# `scaled` is @cache: both of its entries must reach the same CACHE.
# Every pairing must return the same result with R9/R10 preserved, and the
# linked export must reassemble byte for byte.
run_command("${DISCC}" -Os "${TEST_DIR}/callee.dc" -o "${TEST_DIR}/callee-s.o")
file(WRITE "${TEST_DIR}/order-caller.dc"
    "word order(word a, word b, word c, word d); word scaled(word a, word b); word main() { word v = *(volatile word*)0x100; return order(v, 2, v - 33, 3) + order(9, v, 7, v + 1) + scaled(v, 5); }")
file(WRITE "${TEST_DIR}/order-callee.dc"
    "word order(word a, word b, word c, word d) { return ((a - b) << 4) + c - d - d; } @cache word scaled(word a, word b) { return (a << 2) - b; }")
foreach(level IN ITEMS 0 1 2 s)
    run_command("${DISCC}" -O${level} "${TEST_DIR}/order-caller.dc" -o "${TEST_DIR}/order-caller-${level}.o")
    run_command("${DISCC}" -O${level} "${TEST_DIR}/order-callee.dc" -o "${TEST_DIR}/order-callee-${level}.o")
endforeach()
foreach(caller IN ITEMS 0 1 2 s)
    run_command("${DISCC}" -O${caller} "${TEST_DIR}/caller.dc" -o "${TEST_DIR}/caller-${caller}.o")
    foreach(callee IN ITEMS 0 1 2 s)
        # v = 42: ((40 << 4) + 9 - 6) + ((-33 << 4) + 7 - 86) + (168 - 5) = 643 - 607 + 163 = 199.
        set(pair "${TEST_DIR}/order-${caller}-${callee}")
        run_command("${DISCLD}" "${TEST_DIR}/order-caller-${caller}.o" "${TEST_DIR}/order-callee-${callee}.o"
            --origin 0x008000 --init-runtime --ram-bank 0 --stack-pointer 0x2000 -o "${pair}.bin")
        run_command("${GSU_RUNNER}" "${pair}.bin" 0x008000 --ram-byte 0x700100 42 --ram-byte 0x700101 0
            --register 0 199 --register 6 0 --register 10 0x1ffc)
        set(pair "${TEST_DIR}/regcall-${caller}-${callee}")
        run_command("${DISCLD}" "${TEST_DIR}/caller-${caller}.o" "${TEST_DIR}/callee-${callee}.o" ${link}
            --emit-asm "${pair}.s" -o "${pair}.bin")
        run_command("${DISCAS}" "${pair}.s" -o "${pair}-asm.o")
        run_command("${DISCLD}" "${pair}-asm.o" -o "${pair}-asm.bin")
        file(SHA256 "${pair}.bin" direct_hash)
        file(SHA256 "${pair}-asm.bin" assembled_hash)
        if(NOT direct_hash STREQUAL assembled_hash)
            message(FATAL_ERROR "Register-entry link O${caller}/O${callee} did not reassemble byte for byte")
        endif()
        file(READ "${pair}.s" assembly)
        string(FIND "${assembly}" "__disco_regcall1$step:" entry)
        if(caller MATCHES "^[2s]$" OR callee MATCHES "^[2s]$")
            if(entry EQUAL -1)
                message(FATAL_ERROR "O${caller}/O${callee} link lost the register entry or its adapter")
            endif()
        elseif(NOT entry EQUAL -1)
            message(FATAL_ERROR "O${caller}/O${callee} link added an unused register entry")
        endif()
        foreach(binary IN ITEMS "${pair}.bin" "${pair}-asm.bin")
            run_command("${GSU_RUNNER}" "${binary}" 0x008000 --register 0 42 --register 6 0 --register 10 0x1ffc)
        endforeach()
    endforeach()
endforeach()
# The adapter keeps the per-push checks of a stack call, while an O2 callee
# that is a stack-free leaf entered through registers needs no stack at all.
# At the smallest initial SP with which the O2/O2 pairing returns, the
# adapter to the O0 callee must stop with R6=2 instead.
run_command("${DISCC}" -O2 --emit-asm "${TEST_DIR}/caller.dc" -o "${TEST_DIR}/caller-2.s")
file(READ "${TEST_DIR}/caller-2.s" caller_assembly)
if(NOT caller_assembly MATCHES "__disco_stack_limit_([0-9]+)")
    message(FATAL_ERROR "O2 register-entry caller lost its entry frame guard")
endif()
set(caller_frame "${CMAKE_MATCH_1}")
foreach(callee IN ITEMS 0 2)
    run_command("${DISCLD}" "${TEST_DIR}/caller-2.o" "${TEST_DIR}/callee-${callee}.o" --origin 0x008000
        -o "${TEST_DIR}/regcall-exact-${callee}.bin")
endforeach()
unset(native_sp)
foreach(offset RANGE 0 32 2)
    math(EXPR sp "${caller_frame} + ${offset}")
    execute_process(COMMAND "${GSU_RUNNER}" "${TEST_DIR}/regcall-exact-2.bin" 0x008000 --initial-sp ${sp}
        --register 0 42 --register 6 0 RESULT_VARIABLE native_result OUTPUT_QUIET ERROR_QUIET)
    if(native_result STREQUAL "0")
        set(native_sp ${sp})
        break()
    endif()
endforeach()
if(NOT DEFINED native_sp)
    message(FATAL_ERROR "The O2 register-entry pairing never returned near its caller's frame size")
endif()
run_command("${GSU_RUNNER}" "${TEST_DIR}/regcall-exact-0.bin" 0x008000 --initial-sp ${native_sp} --register 6 2)
file(READ "${TEST_DIR}/caller-2.o" caller_object HEX LIMIT 6)
if(NOT caller_object STREQUAL "444953434f0c")
    message(FATAL_ERROR "An object referencing a register entry was not written as format version 12")
endif()

# A register entry is accepted only next to its own exported function.
file(WRITE "${TEST_DIR}/orphan-entry.s" ".setcpu \"GSU\"\n.export __disco_regcall1$missing\n.segment \"CODE\"\n__disco_regcall1$missing:\n    jmp r11\n    nop\n")
run_command("${DISCAS}" "${TEST_DIR}/orphan-entry.s" -o "${TEST_DIR}/orphan-entry.o")
run_expected_failure_contains("defined without its function" "${DISCLD}" "${TEST_DIR}/orphan-entry.o" --origin 0x008000
    -o "${TEST_DIR}/orphan-entry.bin")

# Defined static objects: in-object accesses drop their null/alignment/wrap
# checks at O2/Os. Those objects then demand a nonzero static RAM segment.
file(WRITE "${TEST_DIR}/static-objects.dc"
    "internal word table[8]; internal unsigned byte flags[5]; word main() { word seed=*(volatile word*)0x100; *(volatile word*)0x120=7; table[0]=3; table[7]=5; word i=seed&7; table[i]=table[i]+10; flags[4]=9; word total=table[0]+table[7]+table[i]+(word)flags[4]; *(volatile word*)0x122=total; return total; }")
foreach(level IN ITEMS 0 1 2 s)
    set(OPTIMIZATION ${level})
    check_round_trip("static-objects-${level}" "${TEST_DIR}/static-objects.dc" ${link})
    file(READ "${TEST_DIR}/static-objects-${level}.s" assembly)
    if(level MATCHES "^[2s]$")
        if(NOT assembly MATCHES "__DISCO_STATIC_RAM_NONNULL")
            message(FATAL_ERROR "O${level} static-object proofs did not request the linker marker")
        endif()
        run_expected_failure_contains("static RAM starts at address 0" "${DISCLD}" "${TEST_DIR}/static-objects-${level}.o"
            --origin 0x008000 --ram-origin 0 --host-initialized-globals -o "${TEST_DIR}/static-objects-${level}-zero.bin")
    elseif(assembly MATCHES "__DISCO_STATIC_RAM_NONNULL")
        message(FATAL_ERROR "O${level} emitted a static-object proof marker without global proofs")
    endif()
    foreach(suffix IN ITEMS "" "-asm")
        foreach(seed IN ITEMS 0 3 7 8 15 65535)
            math(EXPR low "${seed} & 255")
            math(EXPR high "${seed} >> 8")
            math(EXPR index "${seed} & 7")
            if(index EQUAL 0)
                set(total 40)
            elseif(index EQUAL 7)
                set(total 42)
            else()
                set(total 27)
            endif()
            run_command("${GSU_RUNNER}" "${TEST_DIR}/static-objects-${level}${suffix}.bin" 0x008000
                --ram-byte 0x700100 ${low} --ram-byte 0x700101 ${high}
                --word 0x700120 7 --word 0x700122 ${total} --register 0 ${total} --register 6 0)
        endforeach()
    endforeach()
endforeach()

# Proved near ROM table reads carry no marker of their own: every one still
# selects __disco_near_rom_bank, and discld must keep rejecting a payload whose
# tables would not lie in that bank's ROM window. Check the object and its
# canonical assembly alike; execution must match the checked O1 reads.
file(WRITE "${TEST_DIR}/rom-objects.dc"
    "rom const byte palette[8] = {1, 2, 3, 4, 5, 6, 7, 100}; word main() { word s = *(volatile word*)0x100; word v = palette[s & 7]; *(volatile word*)0x120 = v; return v; }")
foreach(level IN ITEMS 1 2 s)
    set(OPTIMIZATION ${level})
    check_round_trip("rom-objects-${level}" "${TEST_DIR}/rom-objects.dc" ${link})
    file(READ "${TEST_DIR}/rom-objects-${level}.s" assembly)
    if(NOT assembly MATCHES "#\\^__disco_near_rom_bank")
        message(FATAL_ERROR "O${level} ROM table read no longer selects the linker-checked near ROM bank")
    endif()
    foreach(suffix IN ITEMS "" "-asm")
        set(object "${TEST_DIR}/rom-objects-${level}${suffix}.o")
        run_expected_failure_contains("Near ROM data relocation does not match" "${DISCLD}" "${object}"
            --origin 0x706000 --init-runtime --ram-bank 0 --stack-pointer 0x2000 -o "${TEST_DIR}/rom-objects-${level}${suffix}-ram.bin")
        run_expected_failure_contains("Near ROM data relocation does not match" "${DISCLD}" "${object}"
            ${link} --rom-bank 1 -o "${TEST_DIR}/rom-objects-${level}${suffix}-bank.bin")
        foreach(seed IN ITEMS 0 3 7 8 65535)
            math(EXPR low "${seed} & 255")
            math(EXPR high "${seed} >> 8")
            math(EXPR index "${seed} & 7")
            if(index EQUAL 7)
                set(value 100)
            else()
                math(EXPR value "${index} + 1")
            endif()
            run_command("${GSU_RUNNER}" "${TEST_DIR}/rom-objects-${level}${suffix}.bin" 0x008000
                --ram-byte 0x700100 ${low} --ram-byte 0x700101 ${high}
                --word 0x700120 ${value} --register 0 ${value} --register 6 0)
        endforeach()
    endforeach()
endforeach()
