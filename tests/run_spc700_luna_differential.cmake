# Optional differential check of the SPC700 instruction model against the
# Luna emulator. Generated torture programs run in the model and, uploaded
# through the APU IPL by a small SNES host, in Luna; the per-case result
# tables must match byte for byte. Registered only when Luna, wla-65816 and
# wlalink are available. WLA writes temporary files with tmpfile(), so it
# needs a writable system temporary directory.
cmake_minimum_required(VERSION 3.15)
foreach(variable IN ITEMS RUNNER DISCAS DISCLD WLA_65816 WLALINK LUNA ROOT_DIR TEST_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")
file(COPY "${ROOT_DIR}/tests/spc700_host/upload.asm" DESTINATION "${TEST_DIR}")
file(WRITE "${TEST_DIR}/link.txt" "[objects]\nhost.o\n")

# Must match TortureOrigin, TortureTable and TortureRecord in
# tests/SPC700ExecutionTests.cpp.
set(origin 0x2000)
set(table_offset 49152) # $C000
set(record_bytes 17)

function(spc700_step)
    execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${TEST_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result)
        message(FATAL_ERROR "Command failed (${result}): ${ARGV}\n${output}${error}")
    endif()
endfunction()

# seed:first:count chunks; the first two walk every opcode the generator allows.
foreach(chunk IN ITEMS 12345:0:120 12345:120:120 7:240:120 99:240:120 4242:240:120)
    string(REPLACE ":" ";" parts "${chunk}")
    list(GET parts 0 seed)
    list(GET parts 1 first)
    list(GET parts 2 count)
    execute_process(COMMAND "${RUNNER}" --emit-torture ${seed} ${first} ${count}
        OUTPUT_FILE "${TEST_DIR}/torture.s" RESULT_VARIABLE result)
    if(result)
        message(FATAL_ERROR "Cannot generate torture chunk ${chunk}")
    endif()
    spc700_step("${DISCAS}" torture.s -o torture.o)
    spc700_step("${DISCLD}" torture.o -o payload.bin)
    spc700_step("${RUNNER}" payload.bin ${origin} --dump-aram model.bin --runs 2)
    spc700_step("${WLA_65816}" -D SPC_ORIGIN=${origin} -D SPC_ENTRY=${origin} -o host.o upload.asm)
    spc700_step("${WLALINK}" -r link.txt host.sfc)
    spc700_step("${LUNA}" state host.sfc --until-frame 900 --no-render --assert 7E:0000=01
        --dump-aram luna.bin --out luna.json)

    math(EXPR table_bytes "${count} * ${record_bytes}")
    file(READ "${TEST_DIR}/model.bin" model_table OFFSET ${table_offset} LIMIT ${table_bytes} HEX)
    file(READ "${TEST_DIR}/luna.bin" luna_table OFFSET ${table_offset} LIMIT ${table_bytes} HEX)
    if(NOT model_table STREQUAL luna_table)
        math(EXPR last "${count} - 1")
        foreach(index RANGE ${last})
            math(EXPR at "${index} * ${record_bytes} * 2")
            math(EXPR digits "${record_bytes} * 2")
            string(SUBSTRING "${model_table}" ${at} ${digits} model_record)
            string(SUBSTRING "${luna_table}" ${at} ${digits} luna_record)
            if(NOT model_record STREQUAL luna_record)
                math(EXPR case_number "${first} + ${index}")
                message(FATAL_ERROR "SPC700 model differs from Luna in torture case ${case_number} (seed ${seed}):\n"
                    "  model ${model_record}\n  luna  ${luna_record}\n"
                    "Record: A X Y PSW, dp $20-$27, abs $0400-$0404. See ${TEST_DIR}/torture.s")
            endif()
        endforeach()
    endif()
    message(STATUS "SPC700 torture chunk ${chunk}: ${count} cases match Luna")
endforeach()
