cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/profile.cmake")
foreach(tool IN ITEMS MESEN WLA_65816 WLALINK GSU_BENCHMARK_RUNNER)
    if(NOT EXISTS "${${tool}}" OR IS_DIRECTORY "${${tool}}")
        message(FATAL_ERROR "Calibration requires ${tool}")
    endif()
endforeach()
get_filename_component(OUTPUT_DIR "${OUTPUT_DIR}" ABSOLUTE)

function(calibrate name bytes size instructions observed fetch result)
    set(directory "${OUTPUT_DIR}/${name}")
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${directory}/payload.inc" ".DB ${bytes}\n")
    mesen_command("${directory}" "${GSU_BENCHMARK_RUNNER}" --mesen-fixture horizontal_span seed.bin)
    mesen_build_host("${directory}" -D CALIBRATION=1 ${ARGN})
    mesen_measure("${directory}" ${size} ${instructions})
    string(JSON actual GET "${MESEN_EVIDENCE}" master_clocks_to_stop_entry)
    string(JSON tail GET "${MESEN_EVIDENCE}" stop_fetch_master_clocks)
    if(NOT actual EQUAL observed OR NOT tail EQUAL fetch)
        message(FATAL_ERROR "${name}: timing hook/fetch calibration changed (${actual}+${tail}, expected ${observed}+${fetch})")
    endif()
    file(READ "${directory}/registers.bin" registers HEX)
    string(SUBSTRING "${registers}" 0 2 low)
    string(SUBSTRING "${registers}" 2 2 high)
    math(EXPR r0 "0x${high}${low}")
    if(NOT r0 EQUAL result)
        message(FATAL_ERROR "${name}: incorrect microprogram result")
    endif()
    message(STATUS "Mesen calibration ${name}: observed ${actual} + STOP fetch ${tail} master clocks")
endfunction()

# Independent hand-encoded programs, not compiler output. The initial synthetic
# pipeline prefetch is outside the timing window; the final STOP fetch is inside.
calibrate(uncached "$01,$00,$01" 3 2 5 5 0)
calibrate(immediate "$F0,$95,$00,$00,$01" 5 2 15 5 149)
calibrate(ibt_positive "$A0,$7F,$00,$01" 4 2 10 5 127)
calibrate(ibt_negative_boundary "$A0,$80,$00,$01" 4 2 10 5 65408)
calibrate(ibt_negative_stack "$A0,$FE,$00,$01" 4 2 10 5 65534)
# Bcc tests C before the selected-register AND, which writes N/Z only. The
# one-byte slot keeps ALT2/WITH until AND executes on either successor.
calibrate(logical_before "$F0,$00,$80,$A1,$01,$B0,$3F,$61,$20,$3E,$71,$0D,$05,$01,$A6,$02,$00,$01,$A7,$03,$00,$01"
    22 12 80 5 0)
calibrate(logical_slot "$F0,$00,$80,$A1,$01,$B0,$3F,$61,$20,$3E,$0D,$05,$71,$A6,$02,$00,$01,$A7,$03,$00,$01"
    21 11 75 5 0)
# Speculative WITH changes selectors on both paths. Fault IBT resets them;
# the successful TO completes the copy using the moved prefix.
calibrate(target_prefix_before "$FA,$00,$20,$F3,$00,$20,$BA,$3F,$63,$0D,$05,$01,$A6,$02,$00,$01,$2A,$10,$00,$01"
    20 10 70 5 8192)
calibrate(target_prefix_slot "$FA,$00,$20,$F3,$00,$20,$BA,$3F,$63,$0D,$05,$2A,$A6,$02,$00,$01,$10,$00,$01"
    19 9 65 5 8192)
calibrate(target_prefix_fault "$FA,$FE,$1F,$F3,$00,$20,$BA,$3F,$63,$0D,$05,$2A,$A6,$02,$00,$01,$10,$00,$01"
    19 9 70 5 0)
# Taken BRA retains only the old-flow IWT/IBT opcode. Operands are fetched
# from the target. The old $EE operand bytes are deliberately unreachable.
calibrate(split_delay_iwt "$05,$03,$F0,$EE,$EE,$95,$00,$00,$01" 9 3 25 5 149)
calibrate(split_delay_ibt "$05,$03,$A0,$EE,$EE,$95,$00,$01" 8 3 20 5 65429)
# Byte extraction/packing and the unusual HIB/LOB S=bit7 flag. A wrongly
# word-based sign flag takes the D0 path and changes the checked result to 129.
calibrate(high_byte "$F0,$CD,$AB,$C0,$00,$01" 6 3 20 5 171)
calibrate(low_byte_swap "$F0,$CD,$AB,$9E,$4D,$00,$01" 7 4 25 5 52480)
calibrate(high_byte_sign "$F0,$00,$80,$C0,$0B,$03,$01,$D0,$01,$00,$01" 11 5 35 5 128)
calibrate(low_byte_sign "$F0,$80,$12,$9E,$0B,$03,$01,$D0,$01,$00,$01" 11 5 35 5 128)
calibrate(cached "$02,$01,$00,$01" 4 3 86 1 0)
# Same ROM byte, same register result and same eight executed opcodes. Move
# independent IWT R0 before TO R2/GETB while R14's ROM-buffer read is pending.
calibrate(rom_wait "$02,$FE,$0F,$80,$12,$EF,$F0,$34,$12,$22,$10,$00,$01,$01,$01,$95" 16 8 98 1 149)
calibrate(rom_scheduled "$02,$FE,$0F,$80,$F0,$34,$12,$12,$EF,$22,$10,$00,$01,$01,$01,$95" 16 8 95 1 149)
calibrate(cache_boundary "$02,$01,$01,$01,$01,$01,$01,$01,$01,$01,$01,$01,$01,$01,$01,$00,$01" 17 16 99 81 0)
# Host delays before start and after STOP must not contaminate measured clocks.
calibrate(host_delay "$01,$00,$01" 3 2 5 5 0 -D HOST_DELAY=1024)
# PC is the prefetched first body address. LOOP runs its delay slot on the
# taken and final paths; R0 and the physical instruction count are independent.
calibrate(hardware_loop "$FC,$03,$00,$2F,$1D,$D0,$3C,$D1,$00,$01" 10 13 70 5 3)
# SBK has no address operand. Reload from R1 verifies its full word write,
# including the high byte, independently of compiler instruction selection.
calibrate(store_back "$F1,$00,$01,$41,$F0,$95,$12,$90,$41,$00,$01" 11 6 50 5 4757)
# LDB latches an odd address too; ALT1 does not turn SBK into STB. FROM R2
# selects the full $ABCD value, stored at $0101/$0100 in XOR-1 byte order.
calibrate(store_back_byte "$F1,$01,$01,$3D,$41,$F2,$CD,$AB,$B2,$3D,$90,$F1,$00,$01,$41,$00,$01"
    17 10 80 5 52651)

set(directory "${OUTPUT_DIR}/no_stop")
file(MAKE_DIRECTORY "${directory}")
file(WRITE "${directory}/payload.inc" ".DB $05,$FE,$01\n")
mesen_command("${directory}" "${GSU_BENCHMARK_RUNNER}" --mesen-fixture horizontal_span seed.bin)
mesen_build_host("${directory}" -D CALIBRATION=1)
# A stale PASS must never survive a failed/no-STOP run.
file(WRITE "${directory}/timing.json" "{\"master_clocks\":10}\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DMESEN=${MESEN}"
    "-DOUTPUT_DIR=${directory}" -P "${CMAKE_CURRENT_LIST_DIR}/test-no-stop.cmake"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 60)
if(status STREQUAL "0" OR EXISTS "${directory}/timing.json" OR
   NOT EXISTS "${directory}/timing-error.log")
    message(FATAL_ERROR "No-STOP run/stale evidence was not rejected: ${stdout}\n${stderr}")
endif()
file(READ "${directory}/timing-error.log" error)
if(NOT error MATCHES "Instruction limit exceeded")
    message(FATAL_ERROR "No-STOP run failed for the wrong reason: ${error}")
endif()
message(STATUS "Mesen no-STOP/stale-evidence check passed")
