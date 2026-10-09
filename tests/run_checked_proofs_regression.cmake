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
foreach(reserved IN ITEMS 0 16)
    if(reserved)
        set(global "word reserved[8];")
    else()
        set(global "")
    endif()
    file(WRITE "${TEST_DIR}/stack-${reserved}.dc"
        "${global} word callee(word x); word main() { *(volatile word*)0x120=7; return callee(42); } word callee(word x) { *(volatile word*)0x122=9; return x; }")
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
