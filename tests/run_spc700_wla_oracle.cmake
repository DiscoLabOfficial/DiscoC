# Optional oracle: assemble every SPC700 opcode form with discas and with the
# independent WLA-DX assembler, then compare the bytes. Registered only when
# wla-spc700 and wlalink are installed. WLA writes temporary files through
# tmpfile(), so sandboxes that hide the system temp directory make it fail.
cmake_minimum_required(VERSION 3.15)
foreach(variable IN ITEMS DISCAS CORPUS_TOOL WLA_SPC700 WLALINK TEST_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")

execute_process(COMMAND "${CORPUS_TOOL}" --print-corpus RESULT_VARIABLE result OUTPUT_VARIABLE corpus)
if(result)
    message(FATAL_ERROR "Cannot print the SPC700 corpus")
endif()
string(REPLACE "\n" ";" lines "${corpus}")

set(header ".MEMORYMAP\nSLOTSIZE $10000\nDEFAULTSLOT 0\nSLOT 0 $0000\n.ENDME\n.ROMBANKSIZE $10000\n.ROMBANKS 1\n.BANK 0 SLOT 0\n.ORG $0200\n")
file(WRITE "${TEST_DIR}/link.txt" "[objects]\nwla.o\n")
set(count 0)
foreach(line IN LISTS lines)
    if(line STREQUAL "")
        continue()
    endif()
    file(WRITE "${TEST_DIR}/wla.s" "${header}t: ${line}\n")
    execute_process(COMMAND "${WLA_SPC700}" -o wla.o wla.s WORKING_DIRECTORY "${TEST_DIR}"
        RESULT_VARIABLE wla_result OUTPUT_VARIABLE wla_output ERROR_VARIABLE wla_error)
    if(NOT wla_result)
        execute_process(COMMAND "${WLALINK}" -b link.txt wla.bin WORKING_DIRECTORY "${TEST_DIR}"
            RESULT_VARIABLE wla_result OUTPUT_VARIABLE wla_output ERROR_VARIABLE wla_error)
    endif()
    if(wla_result)
        message(FATAL_ERROR "WLA-DX rejected '${line}':\n${wla_output}${wla_error}")
    endif()
    file(WRITE "${TEST_DIR}/discas.s" ".setcpu \"SPC700\"\n.segment \"CODE\"\nt: ${line}\n")
    execute_process(COMMAND "${DISCAS}" discas.s -o discas.o WORKING_DIRECTORY "${TEST_DIR}"
        RESULT_VARIABLE disco_result OUTPUT_VARIABLE disco_output ERROR_VARIABLE disco_error)
    if(disco_result)
        message(FATAL_ERROR "discas rejected '${line}':\n${disco_output}${disco_error}")
    endif()
    # A relocation-free single-instruction object stores its CODE bytes
    # directly after the fixed v7 header (15 bytes) and the CODE length.
    file(READ "${TEST_DIR}/discas.o" object_hex HEX)
    string(SUBSTRING "${object_hex}" 30 2 length_hex)
    math(EXPR code_bytes "0x${length_hex}")
    math(EXPR code_digits "${code_bytes} * 2")
    string(SUBSTRING "${object_hex}" 38 ${code_digits} disco_hex)
    file(READ "${TEST_DIR}/wla.bin" wla_hex HEX)
    if(NOT disco_hex STREQUAL wla_hex)
        message(FATAL_ERROR "SPC700 encoding differs from WLA-DX for '${line}': discas=${disco_hex} wla=${wla_hex}")
    endif()
    math(EXPR count "${count} + 1")
endforeach()
if(NOT count EQUAL 256)
    message(FATAL_ERROR "Expected 256 SPC700 forms, compared ${count}")
endif()
