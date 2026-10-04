if(CASE STREQUAL "language_extensions_execution")
    check_round_trip(extensions "${ROOT_DIR}/tests/fixtures/language_extensions.dc" --init-runtime)
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/extensions${suffix}.bin" 0x8000 --word 0x100 405 --register 6 0)
    endforeach()
elseif(CASE STREQUAL "language_control_execution")
    check_round_trip(control "${ROOT_DIR}/tests/fixtures/language_control.dc" --init-runtime)
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/control${suffix}.bin" 0x8000
            --word 0x100 13 --word 0x102 15 --word 0x104 195 --word 0x106 1
            --word 0x108 1 --word 0x10a 1 --word 0x10c 1 --word 0x10e 149
            --word 0x110 2 --word 0x112 15 --word 0x114 65408 --word 0x116 127
            --word 0x118 1 --word 0x11a 1 --reads 0x700110 2 --writes 0x700110 3 --register 6 0)
    endforeach()
elseif(CASE STREQUAL "language_arrays_execution")
    check_round_trip(arrays "${ROOT_DIR}/tests/fixtures/language_arrays.dc" --init-runtime)
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/arrays${suffix}.bin" 0x8000
            --word 0x100 149 --word 0x102 12 --word 0x104 1 --word 0x106 1
            --word 0x108 149 --word 0x10a 42 --word 0x10c 162 --word 0x10e 0
            --word 0x110 6 --word 0x112 65 --register 6 0)
    endforeach()
elseif(CASE STREQUAL "language_modules_execution")
    foreach(unit IN ITEMS main math)
        run_command("${DISCC}" "${ROOT_DIR}/examples/language_modules/${unit}.dc" -o "${TEST_DIR}/${unit}.o")
        run_command("${DISCC}" --emit-asm "${ROOT_DIR}/examples/language_modules/${unit}.dc" -o "${TEST_DIR}/${unit}.s")
        run_command("${DISCAS}" "${TEST_DIR}/${unit}.s" -o "${TEST_DIR}/${unit}-asm.o")
    endforeach()
    foreach(backends IN ITEMS direct asm mixed)
        if(backends STREQUAL "direct")
            set(objects "${TEST_DIR}/main.o" "${TEST_DIR}/math.o")
        elseif(backends STREQUAL "asm")
            set(objects "${TEST_DIR}/main-asm.o" "${TEST_DIR}/math-asm.o")
        else()
            set(objects "${TEST_DIR}/main.o" "${TEST_DIR}/math-asm.o")
        endif()
        run_command("${DISCLD}" ${objects} --init-runtime --ram-origin 0x402 -o "${TEST_DIR}/${backends}.bin")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/${backends}.bin" 0x8000 --word 0x100 149 --word 0x102 16 --register 6 0)
    endforeach()
    file(SHA256 "${TEST_DIR}/direct.bin" direct_hash)
    file(SHA256 "${TEST_DIR}/asm.bin" asm_hash)
    file(SHA256 "${TEST_DIR}/mixed.bin" mixed_hash)
    if(NOT direct_hash STREQUAL asm_hash OR NOT direct_hash STREQUAL mixed_hash)
        message(FATAL_ERROR "Imported multi-file modules differ between backends.")
    endif()
elseif(CASE STREQUAL "language_warnings")
    file(WRITE "${TEST_DIR}/warnings.dc" [=[
internal word unused() { return 0; }
void main() {
    word outer; word dead = 0;
    { word outer = 1; }
    outer;
    word divisor = 3; word value = 12 / divisor;
    switch (1) { case 1: value = 1; case 2: break; }
    return; value;
}
]=])
    execute_process(COMMAND "${DISCC}" --check -Wall "${TEST_DIR}/warnings.dc" RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Warnings should not be fatal by default: ${error}")
    endif()
    foreach(category IN ITEMS shadowing unused-variable unused-function uninitialized implicit-fallthrough unreachable expensive-helper)
        if(NOT error MATCHES "\\[-W${category}\\]")
            message(FATAL_ERROR "Missing warning ${category}: ${error}")
        endif()
    endforeach()
    run_expected_failure_contains("\\[-Wuninitialized\\]" "${DISCC}" --check -Wall -Werror "${TEST_DIR}/warnings.dc")
    run_command("${DISCC}" --check -Wall -Werror -Wno-shadowing -Wno-unused-variable -Wno-unused-function
        -Wno-uninitialized -Wno-implicit-fallthrough -Wno-unreachable -Wno-expensive-helper "${TEST_DIR}/warnings.dc")
    file(WRITE "${TEST_DIR}/initialized.dc" "void main() { word x; if (1) { x = 1; } else { x = 2; } x; }")
    run_command("${DISCC}" --check -Werror "${TEST_DIR}/initialized.dc")
elseif(CASE STREQUAL "language_cli_configuration")
    set(source "${ROOT_DIR}/tests/language/configuration/valid.dc")
    run_expected_failure_contains("Origin must fit in 24 bits" "${DISCC}" --check --origin 0x1000000 "${source}")
    run_expected_failure_contains("outside supported ROM/RAM" "${DISCC}" --check --origin 0x7e8000 "${source}")
    run_expected_failure_contains("does not match selected execution memory" "${DISCC}" --check --origin 0x8000 --execution-memory ram "${source}")
    run_expected_failure_contains("GSU placement options" "${DISCC}" --check --target spc700 --execution-memory ram "${source}")
    run_expected_failure_contains("Unknown warning category" "${DISCC}" --check -Wunknown "${source}")
    run_command("${DISCC}" --check --target spc700 "${source}")
endif()
