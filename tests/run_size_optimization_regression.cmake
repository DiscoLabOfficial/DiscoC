set(link --origin 0x008000 --init-runtime --ram-bank 0 --stack-pointer 0x2000)
set(pairs "0:1" "0:65535" "1:1" "1:3" "1:65535" "65535:1" "65535:3" "65535:65535"
    "32768:1" "32768:65535" "32768:32768" "32768:3" "32768:65533" "32767:32768"
    "32767:3" "32767:65533" "65534:32767" "65533:3" "65533:65533" "12345:7"
    "53191:7" "12345:65529" "53191:65529" "32768:32767" "32767:32767")
set(random 42)
foreach(n RANGE 1 16)
    math(EXPR random "(${random} * 25173 + 13849) & 65535")
    set(a ${random})
    math(EXPR random "(${random} * 25173 + 13849) & 65535")
    math(EXPR b "${random} | 1")
    list(APPEND pairs "${a}:${b}")
endforeach()

foreach(level IN ITEMS 0 1 2 s)
    set(OPTIMIZATION ${level})
    check_round_trip("size-${level}" "${ROOT_DIR}/tests/fixtures/gsu_size_optimization.dc" ${link})
    # Final export includes relocations/startup; it must reproduce the same bytes.
    run_command("${DISCLD}" "${TEST_DIR}/size-${level}.o" ${link} --emit-asm "${TEST_DIR}/size-${level}-final.s" -o "${TEST_DIR}/size-${level}-export.bin")
    run_command("${DISCAS}" "${TEST_DIR}/size-${level}-final.s" -o "${TEST_DIR}/size-${level}-final.o")
    run_command("${DISCLD}" "${TEST_DIR}/size-${level}-final.o" -o "${TEST_DIR}/size-${level}-final.bin")
    file(SHA256 "${TEST_DIR}/size-${level}.bin" expected)
    file(SHA256 "${TEST_DIR}/size-${level}-final.bin" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "O${level} size optimization linked export changed bytes")
    endif()
    foreach(pair IN LISTS pairs)
        string(REPLACE ":" ";" values "${pair}")
        list(GET values 0 a)
        list(GET values 1 b)
        if(a GREATER_EQUAL 32768)
            math(EXPR sa "${a} - 65536")
        else()
            set(sa ${a})
        endif()
        if(b GREATER_EQUAL 32768)
            math(EXPR sb "${b} - 65536")
        else()
            set(sb ${b})
        endif()
        math(EXPR quotient "${sa} / ${sb}")
        math(EXPR remainder "${sa} % ${sb}")
        math(EXPR uq "${a} / ${b}")
        math(EXPR ur "${a} % ${b}")
        set(floor ${quotient})
        if(NOT remainder EQUAL 0 AND ((sa LESS 0 AND sb GREATER 0) OR (sa GREATER 0 AND sb LESS 0)))
            math(EXPR floor "${floor} - 1")
        endif()
        math(EXPR checksum "(${quotient} + ${remainder}) & 65535")
        if(sa LESS 0)
            set(child ${checksum})
        else()
            math(EXPR child "(${quotient} - ${remainder}) & 65535")
        endif()
        math(EXPR repeated "${checksum} * 3 & 65535")
        math(EXPR q "${quotient} & 65535")
        math(EXPR r "${remainder} & 65535")
        math(EXPR floor "${floor} & 65535")
        math(EXPR phi_sum "(${a} * 10 + 14) & 65535")
        set(expect --word 0x700120 ${q} --word 0x700122 ${r} --word 0x700124 ${uq} --word 0x700126 ${ur}
            --word 0x700128 ${q} --word 0x70012a ${r} --word 0x70012c ${floor}
            --word 0x70012e ${checksum} --word 0x700130 ${child} --word 0x700132 ${repeated}
            --word 0x700134 ${checksum} --word 0x700136 7 --word 0x700138 6 --word 0x70013a 2
            --word 0x70013c ${phi_sum} --word 0x710200 9)
        foreach(offset_delta IN ITEMS "0x140:1" "0x142:2" "0x144:0" "0x146:1" "0x148:0"
                                      "0x14a:2" "0x14c:3" "0x14e:4" "0x150:0" "0x152:1")
            string(REPLACE ":" ";" location "${offset_delta}")
            list(GET location 0 offset)
            list(GET location 1 delta)
            math(EXPR address "0x700000 + ${offset}")
            math(EXPR value "(${a} + ${delta}) & 65535")
            list(APPEND expect --word ${address} ${value})
        endforeach()
        math(EXPR al "${a} & 255")
        math(EXPR ah "${a} >> 8")
        math(EXPR bl "${b} & 255")
        math(EXPR bh "${b} >> 8")
        foreach(suffix IN ITEMS "" "-asm")
            run_command("${GSU_RUNNER}" "${TEST_DIR}/size-${level}${suffix}.bin" 0x008000
                --ram-byte 0x700100 ${al} --ram-byte 0x700101 ${ah} --ram-byte 0x700102 ${bl} --ram-byte 0x700103 ${bh}
                --reads 0x700100 1 --reads 0x700102 1 --screen-mode 0 --screen-base 16
                ${expect} --plots 0 2 --color 0 1 --rpix 0 2
                --register 0 42 --register 6 0 --register 10 0x1ffc --rambr 0 0)
        endforeach()
    endforeach()
endforeach()

# Fault ordering at the primary computation, including remainder-first pairs.
foreach(primary IN ITEMS "/" "%")
    if(primary STREQUAL "/")
        set(secondary "%")
        set(name quotient)
    else()
        set(secondary "/")
        set(name remainder)
    endif()
    file(WRITE "${TEST_DIR}/zero-${name}.dc"
        "word main(){word a=*(volatile word*)0x100;word b=*(volatile word*)0x102; *(volatile word*)0x120=7;word first=a${primary}b;*(volatile word*)0x122=9;return first+a${secondary}b;}")
    foreach(level IN ITEMS 0 1 2 s)
        set(OPTIMIZATION ${level})
        check_round_trip("zero-${name}-${level}" "${TEST_DIR}/zero-${name}.dc" ${link})
        foreach(suffix IN ITEMS "" "-asm")
            run_command("${GSU_RUNNER}" "${TEST_DIR}/zero-${name}-${level}${suffix}.bin" 0x008000
                --ram-byte 0x700100 42 --word 0x700120 7 --word 0x700122 0
                --writes 0x700120 1 --writes 0x700122 0 --register 6 6)
        endforeach()
    endforeach()
endforeach()

# The first epilogue is far outside byte-branch range at the later returns.
# Every path must preserve R0 (and R4 for far returns) and restore its own frame.
file(WRITE "${TEST_DIR}/returns.dc"
    "word paths(word seed){if(seed==0)return 11; if(seed==1){")
foreach(n RANGE 1 40)
    file(APPEND "${TEST_DIR}/returns.dc" "*(volatile word*)0x180+=1;")
endforeach()
file(APPEND "${TEST_DIR}/returns.dc"
    "return 22;}return 33;} far word* far_path(word seed){if(seed)return(far word*)0x710200;return(far word*)0x700200;}word main(){word seed=*(volatile word*)0x100;word result=paths(seed);*far_path(seed)=42;return result;}")
foreach(level IN ITEMS 0 1 2 s)
    set(OPTIMIZATION ${level})
    check_round_trip("returns-${level}" "${TEST_DIR}/returns.dc" ${link})
    foreach(seed IN ITEMS 0 1 2)
        if(seed EQUAL 0)
            set(result 11)
            set(bank 0x700200)
            set(witness 0)
        elseif(seed EQUAL 1)
            set(result 22)
            set(bank 0x710200)
            set(witness 40)
        else()
            set(result 33)
            set(bank 0x710200)
            set(witness 0)
        endif()
        foreach(suffix IN ITEMS "" "-asm")
            run_command("${GSU_RUNNER}" "${TEST_DIR}/returns-${level}${suffix}.bin" 0x008000
                --ram-byte 0x700100 ${seed} --word ${bank} 42 --word 0x700180 ${witness}
                --writes 0x700180 ${witness} --register 0 ${result} --register 6 0 --register 10 0x1ffc --rambr 0 0)
        endforeach()
    endforeach()
endforeach()

# Short shared failure branches and out-of-range fallback islands both retain
# the original observable access/fault ordering. Null is storable, not loadable.
file(WRITE "${TEST_DIR}/guards.dc"
    "word main(){word* p=(word*)(u16)*(volatile word*)0x100;word* q=(word*)(u16)*(volatile word*)0x102;*p=11;*(volatile word*)0x120=1;")
foreach(n RANGE 1 40)
    file(APPEND "${TEST_DIR}/guards.dc" "*(volatile word*)0x122+=1;")
endforeach()
file(APPEND "${TEST_DIR}/guards.dc" "*q=22;*(volatile word*)0x120=2;return 42;}")
foreach(level IN ITEMS 0 1 2 s)
    set(OPTIMIZATION ${level})
    check_round_trip("guards-${level}" "${TEST_DIR}/guards.dc" ${link})
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/guards-${level}${suffix}.bin" 0x008000
            --ram-byte 0x700101 4 --ram-byte 0x700103 5 --word 0x700400 11 --word 0x700500 22
            --word 0x700120 2 --writes 0x700120 2 --word 0x700122 40 --writes 0x700122 40
            --register 0 42 --register 6 0 --register 10 0x1ffc)
        run_command("${GSU_RUNNER}" "${TEST_DIR}/guards-${level}${suffix}.bin" 0x008000
            --ram-byte 0x700103 5 --word 0x700120 0 --writes 0x700120 0 --writes 0x700122 0
            --word 0x700400 0 --word 0x700500 0 --register 6 2)
        run_command("${GSU_RUNNER}" "${TEST_DIR}/guards-${level}${suffix}.bin" 0x008000
            --ram-byte 0x700101 4 --word 0x700400 11 --word 0x700120 1 --writes 0x700120 1
            --word 0x700122 40 --word 0x700500 0 --register 6 2)
        run_command("${GSU_RUNNER}" "${TEST_DIR}/guards-${level}${suffix}.bin" 0x008000
            --ram-byte 0x700101 4 --ram-byte 0x700102 1 --ram-byte 0x700103 5
            --word 0x700400 0 --writes 0x700120 0 --writes 0x700122 0 --word 0x700500 0 --register 6 1)
    endforeach()
endforeach()
unset(OPTIMIZATION)
