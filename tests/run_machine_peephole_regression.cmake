# O2/Os machine peepholes: dead static stores and direct materialization.
# Store counts come from the exported assembly; execution checks the result,
# the volatile/numeric write counts, and that reassembly reproduces the bytes.
function(count_stores text function_name out)
    string(REGEX MATCH "\n${function_name}:\n(([^\n]*\n)*)" body "${text}")
    set(body "${CMAKE_MATCH_1}")
    # The body ends at the next public label (local labels start with __disco).
    string(REGEX REPLACE "\n[a-z][a-z_0-9]*:\n.*" "\n" body "${body}")
    # Stack pushes (STW (R10)) are frame traffic, not the stores under test.
    string(REGEX MATCHALL "\n    (stw \\(r[0-9]\\)|stw \\(r11\\)|stb \\(r[0-9]+\\)|msbk)" stores "\n${body}")
    list(LENGTH stores count)
    set(${out} ${count} PARENT_SCOPE)
endfunction()

set(source "${ROOT_DIR}/tests/fixtures/gsu_machine_peephole.dc")
foreach(level IN ITEMS 2 s)
    set(stem "${TEST_DIR}/peephole-${level}")
    run_command("${DISCC}" "-O${level}" "${source}" -o "${stem}.o")
    run_command("${DISCC}" "-O${level}" --emit-asm "${source}" -o "${stem}.s")
    run_command("${DISCAS}" "${stem}.s" -o "${stem}-asm.o")
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${stem}${suffix}.o" --init-runtime
            --origin 0x706000 --stack-pointer 0x2000 -o "${stem}${suffix}.bin")
        run_command("${GSU_RUNNER}" "${stem}${suffix}.bin" 0x706000 --register 0 19
            --word 0x700100 6 --writes 0x700100 2 --word 0x700104 8 --writes 0x700104 2)
    endforeach()
    file(SHA256 "${stem}.bin" direct)
    file(SHA256 "${stem}-asm.bin" assembly)
    if(NOT direct STREQUAL assembly)
        message(FATAL_ERROR "-O${level}: peephole assembly round trip differs from direct codegen")
    endif()

    file(READ "${stem}.s" text)
    string(REGEX REPLACE " *;[^\n]*" "" text "${text}")
    foreach(expectation IN ITEMS twice:2 volatile_twice:4 absolute_twice:2 read_between:2 call_between:2 through:2)
        string(REPLACE ":" ";" pair "${expectation}")
        list(GET pair 0 name)
        list(GET pair 1 expected)
        count_stores("${text}" "${name}" actual)
        if(NOT actual EQUAL expected)
            message(FATAL_ERROR "-O${level}: ${name} has ${actual} stores, expected ${expected}")
        endif()
    endforeach()
    # The numeric address is materialized straight into its register: the
    # following literal makes R0 dead, so no WITH R0 / TO Rn copy remains.
    string(REGEX MATCH "\nabsolute_twice:\n(([^\n]*\n)*)" body "${text}")
    string(REGEX REPLACE "\n[a-z][a-z_0-9]*:\n.*" "\n" body "${CMAKE_MATCH_1}")
    if(body MATCHES "with r0\n    to r" OR NOT body MATCHES "iwt r[1-9][0-2]?, #\\$0104")
        message(FATAL_ERROR "-O${level}: absolute_twice still copies its address through R0")
    endif()
endforeach()
