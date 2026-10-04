# Exercise source, object-assembly round trip, and instruction execution.
function(language_fixture name source)
    file(WRITE "${TEST_DIR}/${name}.dc" "${source}")
    check_round_trip(${name} "${TEST_DIR}/${name}.dc" --init-runtime)
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/${name}${suffix}.o" --init-runtime
            -o "${TEST_DIR}/${name}${suffix}.bin")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/${name}${suffix}.bin" 0x008000 ${ARGN})
    endforeach()
endfunction()

if(CASE STREQUAL "language_qualifiers")
    language_fixture(accesses [=[
word change(volatile word* port) { *port = 43; return *port; }
void main() {
    const word value = 42;
    const word* readonly = &value;
    volatile word* port = (volatile word*)0x100;
    *port = *readonly;
    *port;
    *(word*)0x102 = *port + change(port);
    *port = 44;
    *port = 44;
}
]=] --word 0x700100 44 --word 0x700102 85 --reads 0x700100 3 --writes 0x700100 4 --register 6 0)
    language_fixture(induction [=[
void main() { for (volatile word i = 3; i > 0; i = i - 1) {} }
]=] --register 6 0)
elseif(CASE STREQUAL "language_numeric")
    language_fixture(numeric [=[
word twice(word a) { return a * 2; }
void main() {
    word s = 32767;
    unsigned word u = 65535;
    byte b = -128;
    bool yes = true;
    bool no = false;
    *(word*)0x100 = s + 1;
    *(unsigned word*)0x102 = u + 1;
    *(word*)0x104 = twice(300);
    *(word*)0x106 = (word)((byte)255);
    *(word*)0x108 = b;
    *(word*)0x10a = (word)(u > 32767);
    *(word*)0x10c = 2 + 3;
    *(word*)0x10e = (word)((unsigned byte)257);
    *(word*)0x110 = -b;
    *(word*)0x112 = (word)(s + 1 < 0);
    *(word*)0x114 = (word)yes + (word)no;
    *(word*)0x116 = (word)((bool)42);
}
]=] --word 0x700100 32768 --word 0x700102 0 --word 0x700104 600
        --word 0x700106 65535 --word 0x700108 65408 --word 0x70010a 1
        --word 0x70010c 5 --word 0x70010e 1 --word 0x700110 65408
        --word 0x700112 1 --word 0x700114 1 --word 0x700116 1 --register 6 0)
elseif(CASE STREQUAL "language_operators")
    language_fixture(operators [=[
bool touch(volatile word* port) { *port = *port + 1; return true; }
word div(word a, word b) { return a / b; }
word rem(word a, word b) { return a % b; }
unsigned word udiv(unsigned word a, unsigned word b) { return a / b; }
void main() {
    volatile word* port = (volatile word*)0x100;
    *port = 0;
    false && touch(port);
    true || touch(port);
    true && touch(port);
    false || touch(port);
    *(word*)0x102 = (word)(!(false || (false && touch(port))));
    *(word*)0x104 = (0x12 | 0x41) ^ (0x0f & 0x06);
    *(word*)0x106 = (word)(~(byte)0x55);
    *(word*)0x108 = 3 << 4;
    *(word*)0x10a = -32768 >> 15;
    *(unsigned word*)0x10c = ((unsigned word)32768) >> 15;
    *(word*)0x10e = div(-149, 46);
    *(word*)0x110 = rem(-149, 46);
    *(word*)0x112 = div(149, -46);
    *(word*)0x114 = rem(149, -46);
    *(word*)0x116 = div(-32768, -1);
    *(unsigned word*)0x118 = udiv(65535, 32769);
    *(word*)0x11a = (word)(true && touch(port) && touch(port));
}
]=] --word 0x700100 4 --word 0x700102 1 --word 0x700104 0x55
        --word 0x700106 0xffaa --word 0x700108 48 --word 0x70010a 65535
        --word 0x70010c 1 --word 0x70010e 65533 --word 0x700110 65525
        --word 0x700112 65533 --word 0x700114 11 --word 0x700116 32768
        --word 0x700118 1 --word 0x70011a 1 --reads 0x700100 4 --writes 0x700100 5 --register 6 0)
    language_fixture(divzero [=[
void main() { word divisor = 0; 42 / divisor; }
]=] --register 6 6)
    language_fixture(shiftbad [=[
void main() { word count = -1; 42 << count; }
]=] --register 6 5)
    language_fixture(checked_division [=[
void main() {
    volatile word* out = (volatile word*)0x100;
    word a = -149;
    word b = 46;
    *out = a / b;
    plot {
    cursor.x = 10; cursor.y = 20;
    *(word*)0x102 = a % b;
    *(word*)0x104 = cursor.x;
    *(word*)0x106 = cursor.y;
    }
}
]=] --word 0x700100 65533 --word 0x700102 65525 --word 0x700104 10 --word 0x700106 20 --register 6 0)
    set(vectors "")
    set(expectations "")
    set(address 0x200)
    foreach(pair IN ITEMS "65535,1" "65535,65535" "65535,65534" "32768,32769" "32769,32768" "0,65535" "1,65535" "65535,2")
        string(REPLACE "," ";" parts "${pair}")
        list(GET parts 0 a)
        list(GET parts 1 b)
        math(EXPR q "${a}/${b}")
        math(EXPR r "${a}%${b}")
        string(APPEND vectors "*(unsigned word*)${address} = ((unsigned word)${a}) / ((unsigned word)${b});\n")
        math(EXPR full "0x700000+${address}")
        list(APPEND expectations --word ${full} ${q})
        math(EXPR address "${address}+2")
        string(APPEND vectors "*(unsigned word*)${address} = ((unsigned word)${a}) % ((unsigned word)${b});\n")
        math(EXPR full "0x700000+${address}")
        list(APPEND expectations --word ${full} ${r})
        math(EXPR address "${address}+2")
    endforeach()
    language_fixture(division_vectors "void main() { ${vectors} }" ${expectations} --register 6 0)
elseif(CASE STREQUAL "language_aggregates")
    language_fixture(aggregate [=[
struct S { byte tag; word value; bool valid; };
void fill(struct S* state) { (*state).tag = 3; (*state).value = 149; (*state).valid = true; }
word read(const struct S* state) { return (*state).value; }
void main() {
    struct S states[2];
    fill(&states[1]);
    *(word*)0x100 = read(&states[1]);
    *(word*)0x102 = (word)states[1].tag + (word)states[1].valid;
}
]=] --word 0x700100 149 --word 0x700102 4 --register 6 0)
elseif(CASE STREQUAL "language_globals")
    language_fixture(globals [=[
struct State { byte flags; word score; bool active; };
word counter;
const ram word limit = 149;
volatile word device_state;
byte sprite_buffer[128];
struct State game;
far word* remote = (far word*)0x711000;
void update() { counter = counter + 1; game.score = limit; game.active = true; }
void main() {
    *(word*)0x100 = counter;
    *(word*)0x102 = (word)sprite_buffer[127];
    update(); update();
    sprite_buffer[127] = 46;
    device_state = counter + game.score;
    *remote = device_state;
    *(word*)0x104 = (word)game.active;
    *(word*)0x106 = device_state;
}
]=] --ram-byte 0x700400 255 --ram-byte 0x70048f 255 --word 0x700100 0 --word 0x700102 0 --word 0x700104 1 --word 0x700106 151 --word 0x711000 151 --register 6 0)
    run_expected_failure_contains("require --init-runtime" "${DISCLD}" "${TEST_DIR}/globals.o" -o "${TEST_DIR}/uninitialized.bin")
    run_expected_failure_contains("bank boundary" "${DISCLD}" "${TEST_DIR}/globals.o" --init-runtime --ram-origin 0xfff0 -o "${TEST_DIR}/crossing.bin")
    run_expected_failure_contains("below the initial stack" "${DISCLD}" "${TEST_DIR}/globals.o" --init-runtime --ram-origin 0x2000 -o "${TEST_DIR}/stack.bin")
    run_expected_failure_contains("overlaps the linked payload" "${DISCLD}" "${TEST_DIR}/globals.o" --init-runtime --origin 0x700400 -o "${TEST_DIR}/overlap.bin")
    run_command("${DISCLD}" "${TEST_DIR}/globals.o" --origin 0x706000 --ram-bank 1 --ram-origin 0x0900 --init-runtime --emit-asm "${TEST_DIR}/globals-final.s" -o "${TEST_DIR}/globals-bank1.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/globals-bank1.bin" 0x706000 --word 0x710106 151 --word 0x711000 151 --register 6 0)
    run_command("${DISCAS}" "${TEST_DIR}/globals-final.s" -o "${TEST_DIR}/globals-final.o")
    run_command("${DISCLD}" "${TEST_DIR}/globals-final.o" -o "${TEST_DIR}/globals-final.bin")
    file(SHA256 "${TEST_DIR}/globals-bank1.bin" expected)
    file(SHA256 "${TEST_DIR}/globals-final.bin" actual)
    if(NOT expected STREQUAL actual)
        message(FATAL_ERROR "Initialized global payload did not round-trip through final assembly")
    endif()
    run_command("${DISCLD}" "${TEST_DIR}/globals.o" --init-runtime --stack-pointer 0x049a -o "${TEST_DIR}/globals-stack-guard.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/globals-stack-guard.bin" 0x008000 --register 6 2)
    file(WRITE "${TEST_DIR}/host-owned.dc" "word n = 149; void main() { *(word*)0x100 = n; }")
    run_command("${DISCC}" "${TEST_DIR}/host-owned.dc" -o "${TEST_DIR}/host-owned.o")
    run_command("${DISCLD}" "${TEST_DIR}/host-owned.o" --origin 0x706000 --host-initialized-globals -o "${TEST_DIR}/host-owned.bin")
    run_command("${GSU_RUNNER}" "${TEST_DIR}/host-owned.bin" 0x706000 --ram-byte 0x700400 149 --word 0x700100 149 --register 6 0)
    run_expected_failure_contains("not both" "${DISCLD}" "${TEST_DIR}/host-owned.o" --init-runtime --host-initialized-globals -o "${TEST_DIR}/double-init.bin")
    run_expected_failure_contains("word-aligned" "${DISCLD}" "${TEST_DIR}/host-owned.o" --init-runtime --ram-origin 0x401 -o "${TEST_DIR}/odd-globals.bin")
elseif(CASE STREQUAL "language_linkage")
    foreach(name IN ITEMS main math)
        set(source "${ROOT_DIR}/examples/language_core/${name}.dc")
        run_command("${DISCC}" "${source}" -o "${TEST_DIR}/core-${name}.o")
        run_command("${DISCC}" "${source}" --emit-asm -o "${TEST_DIR}/core-${name}.s")
        run_command("${DISCAS}" "${TEST_DIR}/core-${name}.s" -o "${TEST_DIR}/core-${name}-asm.o")
    endforeach()
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/core-main${suffix}.o" "${TEST_DIR}/core-math${suffix}.o" --origin 0x706000 --init-runtime -o "${TEST_DIR}/core${suffix}.bin")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/core${suffix}.bin" 0x706000 --word 0x700100 149 --word 0x700102 21 --word 0x700104 1 --word 0x700106 1 --register 6 0)
    endforeach()
    file(SHA256 "${TEST_DIR}/core.bin" direct_core)
    file(SHA256 "${TEST_DIR}/core-asm.bin" assembled_core)
    run_command("${DISCLD}" "${TEST_DIR}/core-main.o" "${TEST_DIR}/core-math-asm.o" --origin 0x706000 --init-runtime -o "${TEST_DIR}/core-mixed.bin")
    file(SHA256 "${TEST_DIR}/core-mixed.bin" mixed_core)
    if(NOT direct_core STREQUAL assembled_core OR NOT direct_core STREQUAL mixed_core)
        message(FATAL_ERROR "Language core example changed bytes across object workflows")
    endif()
    file(WRITE "${TEST_DIR}/a.dc" [=[
internal word counter = 0;
internal rom const byte salt = 3;
export word shared = 7;
internal word helper(word a) { counter = counter + 1; return a + counter + salt; }
export word alpha() { return helper(30); }
]=])
    file(WRITE "${TEST_DIR}/b.dc" [=[
internal word counter = 100;
internal rom const byte salt = 5;
internal word helper(word a) { counter = counter + 1; return a + counter + salt; }
export word beta() { return helper(10); }
]=])
    file(WRITE "${TEST_DIR}/entry.dc" [=[
extern word shared;
word alpha(); word beta();
void main() {
    *(word*)0x100 = alpha();
    *(word*)0x102 = beta();
    *(word*)0x104 = shared;
    shared = 19;
    *(word*)0x106 = shared;
}
]=])
    foreach(name IN ITEMS a b entry)
        run_command("${DISCC}" "${TEST_DIR}/${name}.dc" -o "${TEST_DIR}/${name}.o")
        run_command("${DISCC}" --emit-asm "${TEST_DIR}/${name}.dc" -o "${TEST_DIR}/${name}.s")
        run_command("${DISCAS}" "${TEST_DIR}/${name}.s" -o "${TEST_DIR}/${name}-asm.o")
    endforeach()
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${DISCLD}" "${TEST_DIR}/entry${suffix}.o" "${TEST_DIR}/a${suffix}.o" "${TEST_DIR}/b${suffix}.o" --init-runtime -o "${TEST_DIR}/project${suffix}.bin")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/project${suffix}.bin" 0x8000 --word 0x700100 34 --word 0x700102 116 --word 0x700104 7 --word 0x700106 19 --register 6 0)
    endforeach()
    file(SHA256 "${TEST_DIR}/project.bin" direct)
    file(SHA256 "${TEST_DIR}/project-asm.bin" assembled)
    if(NOT direct STREQUAL assembled)
        message(FATAL_ERROR "Linkage changed during assembly round trip")
    endif()
    run_command("${DISCLD}" "${TEST_DIR}/entry.o" "${TEST_DIR}/a-asm.o" "${TEST_DIR}/b.o" --init-runtime -o "${TEST_DIR}/mixed.bin")
    file(SHA256 "${TEST_DIR}/mixed.bin" mixed)
    if(NOT direct STREQUAL mixed)
        message(FATAL_ERROR "Mixed backends changed private symbol resolution")
    endif()
    file(WRITE "${TEST_DIR}/private.dc" "word helper(word a); void main() { helper(1); }")
    run_command("${DISCC}" "${TEST_DIR}/private.dc" -o "${TEST_DIR}/private.o")
    run_expected_failure_contains("Undefined symbol 'helper'" "${DISCLD}" "${TEST_DIR}/private.o" "${TEST_DIR}/a.o" --init-runtime -o "${TEST_DIR}/leaked.bin")
    run_expected_failure_contains("Duplicate global symbol" "${DISCLD}" "${TEST_DIR}/entry.o" "${TEST_DIR}/a.o" "${TEST_DIR}/a.o" "${TEST_DIR}/b.o" --init-runtime -o "${TEST_DIR}/duplicates.bin")
elseif(CASE STREQUAL "language_plot")
    language_fixture(plot_scope [=[
word divide(word a, word b) { return a / b; }
word in_plot(bool choose) {
    if (choose) {
        plot { cursor.x = 46; cursor.y = 103; return cursor.x + cursor.y + divide(149, 1); }
    }
    return 7;
}
void main() {
    word result = 0;
    if (true) { plot { cursor.x = 12; cursor.y = 30; word n = divide(149, 7); result = cursor.x + cursor.y + n; } }
    *(word*)0x100 = result;
    *(word*)0x102 = in_plot(true);
    *(word*)0x104 = in_plot(false);
    while (true) { plot { word local = 5; result = local; break; } }
    *(word*)0x106 = result + divide(42, 2);
    switch (1) { case 1: plot { cursor.x = 9; result = cursor.x; break; } default: result = 0; }
    *(word*)0x108 = result;
}
]=] --word 0x700100 63 --word 0x700102 298 --word 0x700104 7 --word 0x700106 26 --word 0x700108 9 --register 6 0)
    file(WRITE "${TEST_DIR}/drawing.dc" "void main() { plot { for (word y = 0; y < 4; y = y + 1) { for (word x = 0; x < 8; x = x + 1) { color x & 15; draw at (x, y); } } } flush; }")
    check_round_trip(drawing "${TEST_DIR}/drawing.dc" --init-runtime)
    language_fixture(boolean_empty [=[
bool yes() { return true && !false; }
rom const bool flag = true;
void main() {
    volatile bool* status = (volatile bool*)0x120;
    *(word*)0x100 = (word)(yes() || false);
    *(word*)0x102 = (word)flag;
    *(word*)0x104 = (word)!*status;
}
]=] --ram-byte 0x700120 128 --word 0x700100 1 --word 0x700102 1 --word 0x700104 0 --reads 0x700120 1 --register 6 0)
endif()
