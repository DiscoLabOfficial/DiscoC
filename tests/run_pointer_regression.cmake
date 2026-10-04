# Included by the common regression harness. Fixtures are generated in each
# CTest case's private output directory, never in the source tree.
function(pointer_fixture name source)
    file(WRITE "${TEST_DIR}/${name}.dc" "${source}")
    check_round_trip(${name} "${TEST_DIR}/${name}.dc")
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/${name}${suffix}.o" --init-runtime
            -o "${TEST_DIR}/${name}${suffix}.bin")
    endforeach()
endfunction()

function(pointer_run name)
    # Run both independently built paths, not just compare their hashes.
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/${name}${suffix}.bin" 0x008000 ${ARGN})
    endforeach()
endfunction()

if(CASE STREQUAL "pointer_far_abi")
    pointer_fixture(abi [=[
far word* echo(word a, far word* pointer, byte b) {
    *pointer = a + b;
    return pointer;
}
far word* nested(far word* pointer) { return echo(46, pointer, 103); }
void main() {
    word local = 42;
    *(far word*)0x701000 = 46;
    far word* pointer = (far word*)0x711000;
    far word* returned = nested(pointer);
    far word* widened = (far word*)&local;
    *widened = 7;
    *(word*)0x0100 = *returned + local;
}
]=])
    pointer_run(abi --word 0x710100 0 --word 0x701000 46 --word 0x711000 149 --word 0x700100 156 --rambr 0 0 --register 6 0 --register 10 0x1ffc)
    # Same code with the other near data/stack bank; poisoning initial RAMBR
    # verifies bootstrap initialization and far access restoration independently.
    run_command("${DISCLD}" "${TEST_DIR}/abi.o" --origin 0x706000 --init-runtime
        --ram-bank 1 --emit-asm "${TEST_DIR}/abi-final.s" -o "${TEST_DIR}/abi-bank1.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/abi-bank1.bin" 0x706000 --initial-ram-bank 0
        --word 0x701000 46 --word 0x711000 149 --word 0x710100 156 --rambr 0 1 --register 6 0 --register 10 0x1ffc)
    run_command("${DISCAS}" "${TEST_DIR}/abi-final.s" -o "${TEST_DIR}/abi-final.o")
    run_command("${DISCLD}" "${TEST_DIR}/abi-final.o" -o "${TEST_DIR}/abi-final.bin")
    file(SHA256 "${TEST_DIR}/abi-bank1.bin" expected)
    file(SHA256 "${TEST_DIR}/abi-final.bin" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Linked far-ABI assembly is not byte-exact")
    endif()

elseif(CASE STREQUAL "pointer_nested")
    pointer_fixture(nested [=[
struct Holder { byte tag; far word* pointer; word tail; };
word read(word* far* pointer) { return **pointer; }
void main() {
    struct Holder holder;
    far word* pointer = (far word*)0x711000;
    word* far* address = &pointer;
    far word* far* remote = (far word* far*)0x710300;
    *pointer = 149;
    *remote = pointer;
    remote[1] = pointer + 1;
    *remote[1] = 103;
    holder.pointer = *remote;
    holder.tag = 3;
    holder.tail = 46;
    *(word*)0x0100 = read(address);
    *(word*)0x0102 = *holder.pointer + holder.tail;
    *(word*)0x0104 = *remote[1];
    far word* identity = &*pointer;
    word* narrowed = (word*)((far word*)&holder.tail);
    *narrowed = 12;
    *(word*)0x0106 = *identity + *narrowed;
}
]=])
    pointer_run(nested --word 0x700100 149 --word 0x700102 195 --word 0x710300 0x71
        --word 0x710302 0x1000 --word 0x710304 0x71 --word 0x710306 0x1002
        --word 0x711000 149 --word 0x711002 103 --word 0x700104 103 --word 0x700106 161
        --rambr 0 0 --register 6 0)
    pointer_fixture(opaque [=[
far void* identity(far void* pointer) { return pointer; }
void main() {
    far void* opaque = identity((far void*)0x711001);
    far byte* byte_address = (far byte*)opaque;
    *byte_address = 42;
}
]=])
    pointer_run(opaque --byte 0x711001 42 --rambr 0 0 --register 6 0)
    pointer_fixture(plot_registers [=[
word read(far word* pointer) { return *pointer + 1; }
void main() {
    far word* pointer = (far word*)0x711000;
    *pointer = 148;
    plot_begin;
    plot.x = 7;
    plot.y = 8;
    word result = read(pointer);
    *(word*)0x0100 = result;
    plot_end;
}
]=])
    pointer_run(plot_registers --word 0x700100 149 --register 1 7 --register 2 8 --rambr 0 0 --register 6 0)

elseif(CASE STREQUAL "pointer_arithmetic")
    pointer_fixture(arithmetic [=[
far byte* step(far byte* pointer, word count) { return pointer + count; }
void main() {
    far byte* edge = (far byte*)0x70FFFF;
    far byte* next = step(edge, 1);
    far byte* previous = next - 1;
    *next = 23;
    *previous = 42;
    word values[3];
    values[0] = 4;
    values[1] = 12;
    values[2] = 30;
    word* start = &values[0];
    *(word*)0x0100 = start[1] + *(start + 2);
    far word* remote = (far word*)0x711000;
    remote[0] = 46;
    remote[1] = 103;
    *(word*)0x0102 = remote[0] + remote[1];
}
]=])
    pointer_run(arithmetic --byte 0x710000 23 --byte 0x70ffff 42 --word 0x700100 42
        --word 0x700102 149 --word 0x711000 46 --word 0x711002 103 --rambr 0 0 --register 6 0)
    pointer_fixture(signed [=[
far byte* step(far byte* pointer, word count) { return pointer + count; }
void main() {
    far byte* pointer = step((far byte*)0x710001, -2);
    *pointer = 42;
    *(word*)0x0100 = 149;
}
]=])
    pointer_run(signed --byte 0x70ffff 42 --word 0x700100 149 --register 6 0)
    pointer_fixture(multi_window [=[
far rom byte* step(far rom byte* pointer, unsigned word count) { return pointer + count; }
void main() {
    far rom byte* pointer = step((far rom byte*)0x018000, 65535);
    *(word*)0x0100 = *pointer;
}
]=])
    pointer_run(multi_window --rom-byte 0x02ffff 42 --word 0x700100 42 --rombr 0 0 --register 6 0)

elseif(CASE STREQUAL "pointer_rom")
    pointer_fixture(rom [=[
rom const byte header = 3;
rom const word answer = 42;
byte fetch(far const rom byte* pointer) { return *pointer; }
void main() {
    far rom byte* end = (far rom byte*)0x03FFFF;
    far rom byte* next = end + 1;
    far rom byte* previous = next - 1;
    *(word*)0x0100 = fetch(next) + fetch(previous);
    *(word*)0x0102 = *(far rom word*)0x038000;
    *(word*)0x0104 = answer + header;
    far const rom byte* near_pointer = (far const rom byte*)&header;
    *(word*)0x0106 = fetch(near_pointer);
    far rom byte* full = (far rom byte*)0x40FFFF;
    *(word*)0x0108 = fetch(full + 1);
}
]=])
    pointer_run(rom --rom-byte 0x03ffff 103 --rom-byte 0x048000 46 --rom-byte 0x038000 149
        --rom-byte 0x038001 1 --rom-byte 0x410000 23 --word 0x700100 149 --word 0x700102 405
        --word 0x700104 45 --word 0x700106 3 --word 0x700108 23 --rombr 0 0 --register 6 0)
    pointer_fixture(bank_context [=[
void main() {
    far rom word* foreign = (far rom word*)0x048000;
    rom word* original = (rom word*)0x8000;
    far rom word* widened = (far rom word*)original;
    rom word* narrowed = (rom word*)widened;
    *(word*)0x0100 = *foreign + *narrowed;
}
]=])
    foreach(suffix IN ITEMS "" "-asm")
        # Host-owned startup must accept explicit bank contexts without adding
        # bootstrap bytes; the far ROM read restores bank $03, not PBR=$70.
        run_command("${DISCLD}" "${TEST_DIR}/bank_context${suffix}.o" --origin 0x706000
            --ram-bank 1 --rom-bank 3 -o "${TEST_DIR}/bank_context${suffix}.bin")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/bank_context${suffix}.bin" 0x706000
            --initial-ram-bank 1 --rom-byte 0x048000 46 --rom-byte 0x038000 103
            --word 0x710100 149 --word 0x700100 0 --rambr 0 1 --rombr 0 3 --register 6 0)
    endforeach()

elseif(CASE STREQUAL "pointer_guards")
    pointer_fixture(scalar_entry [=[
word read(unsigned word address) { return *(word*)address; }
void main() { read(0x1000); }
]=])
    # A scalar-only main can call a checked callee. Its successful STOP must
    # not expose the callee's volatile R6 scratch as a spurious fault category.
    pointer_run(scalar_entry --ram-byte 0x701000 149 --register 0 149 --register 6 0)
    pointer_fixture(misaligned [=[
word read(unsigned word address) { return *(word*)address; }
void main() { *(word*)0x0100 = read(0x1001); }
]=])
    pointer_run(misaligned --register 6 1 --word 0x701000 0 --word 0x701001 0 --word 0x700100 0 --rambr 0 0)
    pointer_fixture(crossing [=[
far word* step(far word* pointer, word index) { return pointer + index; }
void main() { far word* invalid = step((far word*)0x70FFFE, 1); *invalid = 149; }
]=])
    pointer_run(crossing --register 6 3 --word 0x70fffe 0 --word 0x710000 0 --rambr 0 0)
    pointer_fixture(narrowing [=[
word* narrow(far word* pointer) { return (word*)pointer; }
void main() { word* invalid = narrow((far word*)0x711000); *invalid = 149; }
]=])
    pointer_run(narrowing --register 6 4 --word 0x711000 0 --word 0x701000 0 --rambr 0 0)
    pointer_fixture(domain [=[
far byte* step(far byte* pointer, word index) { return pointer + index; }
void main() { far byte* invalid = step((far byte*)0x71FFFF, 1); *invalid = 42; }
]=])
    pointer_run(domain --register 6 3 --byte 0x71ffff 0 --byte 0x700000 0)
    pointer_fixture(near_overflow [=[
byte* step(byte* pointer, word index) { return pointer + index; }
void main() { byte* invalid = step((byte*)0xFFFF, 1); *invalid = 42; }
]=])
    pointer_run(near_overflow --register 6 3 --byte 0x70ffff 0 --byte 0x700000 0)
    pointer_fixture(padding [=[
void main() {
    far word* far* storage = (far word* far*)0x710300;
    far word* malformed = *storage;
    *malformed = 149;
}
]=])
    pointer_run(padding --ram-byte 0x710300 0x71 --ram-byte 0x710301 1 --ram-byte 0x710302 0
        --ram-byte 0x710303 0x10 --register 6 2 --word 0x711000 0 --rambr 0 0)
    foreach(spec IN ITEMS "ram_borrow|0x710000|-1" "rom_carry|0x00FFFE|1" "rom_borrow|0x018000|-1")
        string(REPLACE "|" ";" fields "${spec}")
        list(GET fields 0 name)
        list(GET fields 1 address)
        list(GET fields 2 displacement)
        set(qualifier "")
        if(name MATCHES "^rom_")
            set(qualifier "rom ")
        endif()
        pointer_fixture(${name} "far ${qualifier}word* step(far ${qualifier}word* p, word n) { return p + n; }\nvoid main() { far ${qualifier}word* invalid = step((far ${qualifier}word*)${address}, ${displacement}); }\n")
        pointer_run(${name} --register 6 3 --rambr 0 0 --rombr 0 0)
    endforeach()
    # Nested aggregates make a 32 KiB stride without a huge source fixture.
    # LoROM subtraction must reject $8000-$8000, not wrap its guard threshold.
    set(large_struct "struct Block0 { word a; word b; };\n")
    foreach(level RANGE 1 13)
        math(EXPR previous "${level} - 1")
        string(APPEND large_struct "struct Block${level} { struct Block${previous} a; struct Block${previous} b; };\n")
    endforeach()
    set(large_step "far rom struct Block13* step(far rom struct Block13* p, word n) { return p + n; }\n")
    pointer_fixture(large_lorom "${large_struct}${large_step}void main() { far rom struct Block13* p = step((far rom struct Block13*)0x008000, -1); }\n")
    pointer_run(large_lorom --register 6 3 --rambr 0 0 --rombr 0 0)
    pointer_fixture(large_full_bank "${large_struct}${large_step}void main() { far rom struct Block13* p = step((far rom struct Block13*)0x408000, -1); }\n")
    pointer_run(large_full_bank --register 6 0 --rambr 0 0 --rombr 0 0)

elseif(CASE STREQUAL "pointer_diagnostics")
    function(reject_pointer name source diagnostic)
        file(WRITE "${TEST_DIR}/${name}.dc" "${source}")
        run_expected_failure_contains("${diagnostic}" "${DISCC}" "${TEST_DIR}/${name}.dc" -o "${TEST_DIR}/${name}.o")
    endfunction()
    reject_pointer(odd "void main() { word* p = (word*)0x1001; }" "Misaligned 16-bit")
    reject_pointer(rom_odd "void main() { far rom word* p = (far rom word*)0x00FFFF; }" "Misaligned 16-bit")
    reject_pointer(rom_low "void main() { far rom byte* p = (far rom byte*)0x037FFF; }" "outside GSU-visible ROM")
    reject_pointer(ram_domain "void main() { far byte* p = (far byte*)0x720000; }" "outside GSU-visible RAM")
    reject_pointer(rom_word_cross "void main() { far rom word* p = (far rom word*)0x00FFFE + 1; }" "bank boundary")
    reject_pointer(ram_word_cross "void main() { far word* p = (far word*)0x70FFFE + 1; }" "bank boundary")
    reject_pointer(rom_word_borrow "void main() { far rom word* p = (far rom word*)0x018000 - 1; }" "bank boundary")
    reject_pointer(ram_word_borrow "void main() { far word* p = (far word*)0x710000 - 1; }" "bank boundary")
    reject_pointer(implicit_narrow "void main() { far word* p = (far word*)0x711000; word* q = p; }" "Cannot implicitly convert")
    reject_pointer(inner_conversion "void main() { word* p = (word*)0x1000; word** q = &p; word* far* r = q; }" "Cannot implicitly convert")
    reject_pointer(inner_cast "void main() { word* p = (word*)0x1000; word** q = &p; word* far* r = (word* far*)q; }" "nested pointer representation")
    reject_pointer(word_index "void main() { word value = ((far word*)0x70FFFE)[1]; }" "bank boundary")
    reject_pointer(integer_pointer "void main() { word* pointer = 4096; }" "explicit cast")
    reject_pointer(pointer_negation "void main() { word* pointer = (word*)0x1000; pointer = -pointer; }" "integer operand")
    reject_pointer(function_pointer "word helper() { return 1; } void main() { void* pointer = &helper; }" "Function pointers")
    reject_pointer(void_dereference "void main() { far void* p = (far void*)0x711000; *p; }" "Cannot dereference a void pointer")
    reject_pointer(rom_store "void main() { *(far rom word*)0x038000 = 42; }" "ROM is read-only")

elseif(CASE STREQUAL "pointer_multifile")
    file(WRITE "${TEST_DIR}/main.dc" [=[
far word* configure(word a, far word* pointer, word b);
void main() {
    far word* pointer = configure(46, (far word*)0x711000, 103);
    *(word*)0x0100 = *pointer;
}
]=])
    file(WRITE "${TEST_DIR}/math.dc" [=[
far word* configure(word a, far word* pointer, word b) { *pointer = a + b; return pointer; }
]=])
    foreach(unit IN ITEMS main math)
        run_command("${DISCC}" "${TEST_DIR}/${unit}.dc" -o "${TEST_DIR}/${unit}.o")
        run_command("${DISCC}" --emit-asm "${TEST_DIR}/${unit}.dc" -o "${TEST_DIR}/${unit}.s")
        run_command("${DISCAS}" "${TEST_DIR}/${unit}.s" -o "${TEST_DIR}/${unit}-asm.o")
    endforeach()
    foreach(variant IN ITEMS direct assembly mixed)
        set(main "${TEST_DIR}/main.o")
        set(math "${TEST_DIR}/math.o")
        if(variant STREQUAL "assembly")
            set(main "${TEST_DIR}/main-asm.o")
        endif()
        if(NOT variant STREQUAL "direct")
            set(math "${TEST_DIR}/math-asm.o")
        endif()
        run_command("${DISCLD}" "${main}" "${math}" --origin 0x700900 --init-runtime
            -o "${TEST_DIR}/${variant}.bin")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/${variant}.bin" 0x700900 --initial-ram-bank 1
            --word 0x700100 149 --word 0x711000 149 --rambr 0 0 --register 6 0 --register 10 0x1ffc)
        file(SHA256 "${TEST_DIR}/${variant}.bin" ${variant}_hash)
    endforeach()
    if(NOT direct_hash STREQUAL assembly_hash OR NOT direct_hash STREQUAL mixed_hash)
        message(FATAL_ERROR "Mixed-object far ABI changed payload bytes")
    endif()

elseif(CASE STREQUAL "pointer_alignment")
    file(WRITE "${TEST_DIR}/a.s" [=[
.segment "CODE"
.export main
main:
    iwt r14, #value
    getb
    inc r14
    alt1
    getb
    stop
    nop
.segment "DATA"
    .byte $AA
]=])
    file(WRITE "${TEST_DIR}/b.s" [=[
.define __DISCO_DATA_ALIGNMENT 2
.segment "DATA"
.export value
value:
    .word 42
]=])
    foreach(unit IN ITEMS a b)
        run_command("${DISCAS}" "${TEST_DIR}/${unit}.s" -o "${TEST_DIR}/${unit}.o")
    endforeach()
    run_command("${DISCLD}" "${TEST_DIR}/a.o" "${TEST_DIR}/b.o" --emit-asm "${TEST_DIR}/final.s"
        -o "${TEST_DIR}/aligned.bin")
    file(READ "${TEST_DIR}/aligned.bin" actual HEX)
    if(NOT actual STREQUAL "fe0c80efde3def000101aa002a00")
        message(FATAL_ERROR "Wrong aligned DATA layout: ${actual}")
    endif()
    run_command("${GSU_RUNNER}" "${TEST_DIR}/aligned.bin" 0x008000 --register 0 42)
    run_command("${DISCAS}" "${TEST_DIR}/final.s" -o "${TEST_DIR}/final.o")
    run_command("${DISCLD}" "${TEST_DIR}/final.o" -o "${TEST_DIR}/final.bin")
    file(SHA256 "${TEST_DIR}/aligned.bin" expected)
    file(SHA256 "${TEST_DIR}/final.bin" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Final assembly lost section alignment")
    endif()
    file(WRITE "${TEST_DIR}/padding_entry.s" [=[
.define __DISCO_DATA_ALIGNMENT 2
.segment "CODE"
stop
.export end_entry
end_entry:
.segment "DATA"
.word 42
]=])
    run_command("${DISCAS}" "${TEST_DIR}/padding_entry.s" -o "${TEST_DIR}/padding_entry.o")
    run_expected_failure_contains("Runtime entry" "${DISCLD}" "${TEST_DIR}/padding_entry.o"
        --init-runtime --entry end_entry -o "${TEST_DIR}/padding_entry.bin")
    if(EXISTS "${TEST_DIR}/padding_entry.bin")
        message(FATAL_ERROR "DATA padding turned an end-of-CODE marker into an entry")
    endif()
else()
    message(FATAL_ERROR "Unknown pointer regression: ${CASE}")
endif()
