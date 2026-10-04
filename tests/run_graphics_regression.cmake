function(graphics_fixture name source)
    file(WRITE "${TEST_DIR}/${name}.dc" "${source}")
    check_round_trip("${name}" "${TEST_DIR}/${name}.dc" --init-runtime)
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/${name}${suffix}.bin" 0x8000 ${ARGN} --register 6 0)
    endforeach()
endfunction()

if(CASE STREQUAL "graphics_state")
    graphics_fixture(cursor [=[
word identity(word x) { return x; }
void main() {
    plot {
        options;
        at (10, 20);
        word before = cursor.x;
        color identity(5);
        pixel; pixel; pixel;
        *(word*)0x100 = before;
        *(word*)0x102 = cursor.x;
        *(word*)0x104 = cursor.y;
        byte c = read_pixel at (10, 20);
        *(byte*)0x106 = c;
        read_pixel; // The unused result is still a cache-flushing hardware read.
        flush;
        *(word*)0x108 = cursor.x;
    }
}
]=] --word 0x700100 10 --word 0x700102 13 --word 0x700104 20 --byte 0x700106 5 --word 0x700108 10 --plots 0 3 --rpix 0 3 --color 0 1)
    graphics_fixture(scanline [=[
void main() {
    plot {
        options; options;
        color 3;
        at (0, 1);
        for (word i = 0; i < 12; i++) { pixel; }
        *(word*)0x100 = cursor.x;
        *(word*)0x102 = cursor.y;
        flush;
    }
}
]=] --word 0x700100 12 --word 0x700102 1 --plots 0 12 --rpix 0 1 --cmode 0 1 --byte 0x706002 255 --byte 0x706003 255)
    execute_process(COMMAND "${DISCC}" "${TEST_DIR}/scanline.dc" --emit-ir OUTPUT_VARIABLE ir RESULT_VARIABLE result)
    string(REGEX MATCHALL "cursor.write 0" x_writes "${ir}")
    list(LENGTH x_writes count)
    if(result OR NOT count EQUAL 1 OR NOT ir MATCHES "rpix discard")
        message(FATAL_ERROR "Scanline must initialize X once and preserve RPIX discard: ${ir}")
    endif()
    file(READ "${TEST_DIR}/scanline.s" assembly)
    if(assembly MATCHES "inc r1\n")
        message(FATAL_ERROR "Backend manually increments R1 after PLOT")
    endif()
    graphics_fixture(sugar [=[
void main() { plot {
    draw at (2, 3) with color 9;
    *(byte*)0x100 = read_pixel at (2, 3);
    *(word*)0x102 = cursor.x;
    draw at (3, 3);
    *(word*)0x104 = cursor.x;
    flush;
} }
]=] --byte 0x700100 9 --word 0x700102 2 --word 0x700104 4 --plots 0 2 --rpix 0 2)
    graphics_fixture(sugar_order [=[
ram word counter;
word next() { counter++; return counter; }
void main() { plot {
    options;
    draw at (next(), next()) with color next();
    *(word*)0x100 = cursor.x; *(word*)0x102 = cursor.y;
    byte c = read_pixel at (next(), next());
    *(word*)0x104 = counter; *(word*)0x106 = cursor.x;
    *(word*)0x108 = cursor.y; *(byte*)0x10a = c;
    flush;
} }
]=] --word 0x700100 3 --word 0x700102 3 --word 0x700104 5
    --word 0x700106 4 --word 0x700108 5 --byte 0x70010a 0 --colr 0 1 --plots 0 1 --rpix 0 2)
    graphics_fixture(cursor_wrap [=[
void main() { plot {
    options; color 5; at (-1, 0); pixel;
    *(word*)0x100 = cursor.x;
    *(byte*)0x102 = read_pixel at (-1, 0);
    *(word*)0x104 = cursor.x; flush;
} }
]=] --word 0x700100 0 --byte 0x700102 5 --word 0x700104 65535 --plots 0 1)
    graphics_fixture(control [=[
word identity(word v) { return v; }
void main() { plot {
    at (0, 0); options; color 6;
    for (word i = 0; i < 5; i++) {
        if (i == 1) { continue; }
        switch (i) { case 4: break; default: pixel; break; }
    }
    *(word*)0x100 = cursor.x;
    options dither;
    color identity(3);
    options dither;
    flush;
} }
]=] --word 0x700100 3 --plots 0 3 --rpix 0 1 --cmode 0 3)
elseif(CASE STREQUAL "graphics_colors")
    graphics_fixture(palette [=[
rom const byte original[] = { 5, 7, 9 };
void main() {
    byte working[3];
    working[0] = original[0]; working[0] = 11;
    plot {
        options;
        at (0, 0);
        color original[1]; pixel;
        color working[0]; pixel;
        color original[0] + 1; pixel;
        byte c = original[2]; c = (byte)(c ^ 1); color c; pixel;
        *(byte*)0x100 = read_pixel at (0, 0);
        *(byte*)0x101 = read_pixel at (1, 0);
        *(byte*)0x102 = read_pixel at (2, 0);
        *(byte*)0x103 = read_pixel at (3, 0);
        flush;
    }
}
]=] --byte 0x700100 7 --byte 0x700101 11 --byte 0x700102 6 --byte 0x700103 8 --getc 0 1 --color 0 3 --plots 0 4)
    graphics_fixture(far_color [=[
void main() {
    far rom byte* p = (far rom byte*)0x018120;
    plot { at (1, 1); options; color *p; pixel; flush; }
}
]=] --rom-byte 0x018120 13 --getc 0 1 --colr 0 13 --rombr 0 0 --reads 0x018120 1)
    foreach(path IN ITEMS immediate rom ram)
        if(path STREQUAL "immediate")
            set(declarations "")
            set(value "0xB4")
            set(counts --getc 0 0 --color 0 3)
        elseif(path STREQUAL "rom")
            set(declarations "rom const byte palette[] = { (byte)0xB4 };")
            set(value "palette[0]")
            set(counts --getc 0 2 --color 0 1)
        else()
            set(declarations "byte palette[1]; palette[0] = (byte)0xB4;")
            set(value "palette[0]")
            set(counts --getc 0 0 --color 0 3)
        endif()
        if(path STREQUAL "rom")
            set(prefix "${declarations} void main() {")
        else()
            set(prefix "void main() { ${declarations}")
        endif()
        graphics_fixture("transform_${path}" "${prefix} plot { color 0xA2; options high_nibble; color ${value}; *(byte*)0x100 = read_pixel; options freeze_high; color ${value}; draw at (1, 0); flush; } }"
            --byte 0x700100 0 --colr 0 164 ${counts})
    endforeach()
    graphics_fixture(dither [=[
void main() { plot { at (0, 0); options dither; color 0xA3; pixel; pixel;
    *(byte*)0x100 = read_pixel at (0, 0); *(byte*)0x101 = read_pixel at (1, 0); flush; } }
]=] --byte 0x700100 3 --byte 0x700101 10)
    graphics_fixture(object_option [=[
void main() { plot { options object; draw at (130, 130) with color 14;
    *(byte*)0x100 = read_pixel at (130, 130); flush; } }
]=] --byte 0x700100 14 --byte 0x70c004 0 --byte 0x70c005 32 --por 0 17)
    graphics_fixture(transparent [=[
void main() { plot { at (0, 0); options; color 7; pixel; at (0, 0);
    options transparent; color 0; pixel; *(word*)0x102 = cursor.x;
    *(byte*)0x100 = read_pixel at (0, 0); options; color 0; pixel;
    *(byte*)0x101 = read_pixel at (0, 0); flush; } }
]=] --byte 0x700100 7 --byte 0x700101 0 --word 0x700102 1)
    # Seed the near-ROM byte outside CODE; $8100 overlaps this fixture's
    # instructions. Program fetches and ROM data now share physical storage.
    graphics_fixture(boundaries [=[
ram byte working[2];
void main() {
    working[0] = 12;
    far volatile rom byte* port = (far volatile rom byte*)0x028100;
    far ram byte* remote = (far ram byte*)0x710100;
    plot {
        options; at (0, 0);
        color *port; pixel;
        color *port; pixel;
        color *remote; pixel;
        color working[0]; pixel;
        color (byte)(*(rom byte*)0x00F100); pixel;
        flush;
    }
}
]=] --rom-byte 0x00F100 5 --rom-byte 0x028100 7 --ram-byte 0x710100 9
    --reads 0x028100 2 --reads 0x710100 1 --getc 0 0 --color 0 5 --rambr 0 0 --rombr 0 0)
elseif(CASE STREQUAL "graphics_bitmaps")
    foreach(mode IN ITEMS 256x128 256x160 256x192 obj)
        foreach(depth IN ITEMS 2 4 8)
            if(mode STREQUAL "obj")
                set(fields "mode obj;")
                set(height_bits 36)
            else()
                set(fields "mode bitmap; size ${mode};")
                if(mode STREQUAL "256x128")
                    set(height_bits 0)
                elseif(mode STREQUAL "256x160")
                    set(height_bits 4)
                else()
                    set(height_bits 32)
                endif()
            endif()
            if(depth EQUAL 2)
                set(depth_bits 0)
                set(pixel_value 3)
            elseif(depth EQUAL 4)
                set(depth_bits 1)
                set(pixel_value 15)
            else()
                set(depth_bits 3)
                set(pixel_value 255)
            endif()
            math(EXPR scm "${height_bits} + ${depth_bits}")
            set(name "screen_${mode}_${depth}")
            graphics_fixture("${name}" "bitmap screen { ${fields} depth ${depth}bpp; base 0x6000; } void main() { use bitmap screen; plot { options; draw at (130, 3) with color ${pixel_value}; *(byte*)0x100 = read_pixel at (130, 3); flush; } }"
                --screen-mode "${scm}" --byte 0x700100 "${pixel_value}")
        endforeach()
    endforeach()
    graphics_fixture(last_pixel "bitmap last { mode obj; depth 8bpp; base 0x10000; } void main() { use bitmap last; plot { options; draw at (255, 255) with color 255; *(byte*)0x100 = read_pixel at (255, 255); flush; } }"
        --screen-mode 0x27 --screen-base 64 --byte 0x700100 255 --byte 0x71ffff 1)
    # Metadata must survive the complete linked assembly round trip too.
    run_command("${DISCLD}" "${TEST_DIR}/screen_256x128_4.o" --init-runtime --emit-asm "${TEST_DIR}/final.s" -o "${TEST_DIR}/final.bin")
    run_command("${DISCAS}" "${TEST_DIR}/final.s" -o "${TEST_DIR}/final.o")
    run_command("${DISCLD}" "${TEST_DIR}/final.o" -o "${TEST_DIR}/roundtrip.bin")
    file(SHA256 "${TEST_DIR}/final.bin" direct)
    file(SHA256 "${TEST_DIR}/roundtrip.bin" assembled)
    if(NOT direct STREQUAL assembled)
        message(FATAL_ERROR "Linked graphics assembly is not byte-exact")
    endif()
    file(WRITE "${TEST_DIR}/screen_decl.dc" "bitmap screen { mode bitmap; size 256x160; depth 2bpp; base 0x6000; }")
    graphics_fixture(imported "import \"screen_decl.dc\"; void main() { use bitmap screen; plot { options; draw at (4, 5) with color 3; *(byte*)0x100 = read_pixel at (4, 5); flush; } }"
        --screen-mode 4 --byte 0x700100 3)
    check_round_trip(triangle "${ROOT_DIR}/examples/plot.dc" --init-runtime --stack-pointer 0x8000)
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/triangle${suffix}.bin" 0x8000
            --screen-mode 0x21 --screen-base 0 --plots 0 4225 --getc 0 4225
            --color 0 0 --rpix 0 1 --register 6 0)
    endforeach()
elseif(CASE STREQUAL "graphics_triangle")
    set(source "${ROOT_DIR}/tests/graphics/triangle/triangle.dc")
    set(link_options --origin 0x706000 --init-runtime --ram-bank 0 --stack-pointer 0xFFFE)
    run_command("${DISCC}" --target gsu --execution-memory ram "${source}" -o "${TEST_DIR}/triangle.o")
    run_command("${DISCC}" --target gsu --execution-memory ram --emit-asm "${source}" -o "${TEST_DIR}/triangle-compiler.s")
    run_command("${DISCAS}" "${TEST_DIR}/triangle-compiler.s" -o "${TEST_DIR}/triangle-asm.o")
    run_command("${DISCLD}" "${TEST_DIR}/triangle.o" ${link_options}
        --emit-asm "${TEST_DIR}/triangle.s" -o "${TEST_DIR}/triangle.bin")
    run_command("${DISCLD}" "${TEST_DIR}/triangle-asm.o" ${link_options} -o "${TEST_DIR}/triangle-asm.bin")
    run_command("${DISCAS}" "${TEST_DIR}/triangle.s" -o "${TEST_DIR}/triangle-final.o")
    run_command("${DISCLD}" "${TEST_DIR}/triangle-final.o" -o "${TEST_DIR}/triangle-roundtrip.bin")
    file(SHA256 "${TEST_DIR}/triangle.bin" direct)
    foreach(suffix IN ITEMS "" "-asm" "-roundtrip")
        file(SHA256 "${TEST_DIR}/triangle${suffix}.bin" assembled)
        if(NOT direct STREQUAL assembled)
            message(FATAL_ERROR "Triangle assembly round trip is not byte-exact: ${suffix}")
        endif()
        run_command("${GSU_RUNNER}" "${TEST_DIR}/triangle${suffix}.bin" 0x706000
            --screen-mode 0x21 --screen-base 0 --initial-ram-bank 1 --initial-sp 0x1234
            --plots 0 9409 --color 0 97 --getc 0 0 --rpix 0 1
            --word 0x70f000 9409 --register 0 9409 --register 6 0 --register 10 0xfffa --rambr 0 0
            # Independent planar landmarks: apex, left/right base, background.
            --byte 0x7030c0 128 --byte 0x7030c1 0 --byte 0x7030d0 0 --byte 0x7030d1 0
            --byte 0x700e40 255 --byte 0x700e41 0 --byte 0x700e50 255 --byte 0x700e51 255
            --byte 0x705640 128 --byte 0x705641 0 --byte 0x705650 128 --byte 0x705651 128
            --byte 0x705642 0 --byte 0x700000 0)
    endforeach()
    file(READ "${TEST_DIR}/triangle.s" assembly)
    if(NOT assembly MATCHES "__DISCO_BITMAP_SCBR[ \t]+\\$00" OR
       NOT assembly MATCHES "__DISCO_BITMAP_SCMR[ \t]+\\$21")
        message(FATAL_ERROR "Triangle's final export lost its 256x192 4bpp host bitmap contract")
    endif()
elseif(CASE STREQUAL "graphics_rotation")
    set(source "${ROOT_DIR}/tests/graphics/rotating_triangle/triangle.dc")
    set(link_options --origin 0x706000 --init-runtime --ram-bank 0 --stack-pointer 0xFFFE)
    run_command("${DISCC}" --target gsu --execution-memory ram "${source}" -o "${TEST_DIR}/triangle.o")
    run_command("${DISCC}" --target gsu --execution-memory ram --emit-asm "${source}" -o "${TEST_DIR}/triangle-compiler.s")
    run_command("${DISCAS}" "${TEST_DIR}/triangle-compiler.s" -o "${TEST_DIR}/triangle-asm.o")
    run_command("${DISCLD}" "${TEST_DIR}/triangle.o" ${link_options}
        --emit-asm "${TEST_DIR}/triangle.s" -o "${TEST_DIR}/triangle.bin")
    run_command("${DISCLD}" "${TEST_DIR}/triangle-asm.o" ${link_options} -o "${TEST_DIR}/triangle-asm.bin")
    run_command("${DISCAS}" "${TEST_DIR}/triangle.s" -o "${TEST_DIR}/triangle-final.o")
    run_command("${DISCLD}" "${TEST_DIR}/triangle-final.o" -o "${TEST_DIR}/triangle-roundtrip.bin")
    file(READ "${TEST_DIR}/triangle.s" assembly)
    string(REGEX MATCHALL "\n[ \t]+cache[ \t]+" cache_instructions "${assembly}")
    list(LENGTH cache_instructions cache_count)
    if(NOT cache_count EQUAL 2)
        message(FATAL_ERROR "Rotating triangle must retain both instruction-cache requests")
    endif()
    string(REGEX REPLACE ".*cache ; CODE\\+\\$([0-9A-F]+).*" "\\1" cache_offset "${assembly}")
    math(EXPR final_cache_base "(0x6000 + 0x${cache_offset} + 1) & 0xfff0")
    file(SHA256 "${TEST_DIR}/triangle.bin" direct)
    # Independent per-pixel half-plane reference vectors. Fields: host input,
    # masked phase, total/white/black counts, four planar byte landmarks and
    # CACHE executions (14*113 clear headers + pixels + occupied scanlines).
    set(vectors
        "0,0,3281,1641,1640,255,0,255,0,4944"
        "5,5,3249,1633,1616,28,14,128,127,4921"
        "8,8,3249,1624,1625,0,0,128,127,4916"
        "16,16,3281,1641,1640,0,0,128,127,4944"
        "23,23,3213,1604,1609,24,255,255,0,4877"
        "32,32,3281,1641,1640,127,127,128,127,4944"
        "48,48,3281,1641,1640,0,0,255,0,4944"
        "63,63,3177,1591,1586,252,3,255,0,4841"
        "64,0,3281,1641,1640,255,0,255,0,4944"
        "255,63,3177,1591,1586,252,3,255,0,4841"
    )
    foreach(suffix IN ITEMS "" "-asm" "-roundtrip")
        file(SHA256 "${TEST_DIR}/triangle${suffix}.bin" assembled)
        if(NOT direct STREQUAL assembled)
            message(FATAL_ERROR "Rotating triangle assembly is not byte-exact: ${suffix}")
        endif()
        foreach(vector IN LISTS vectors)
            string(REPLACE "," ";" fields "${vector}")
            list(GET fields 0 input)
            list(GET fields 1 phase)
            list(GET fields 2 pixels)
            list(GET fields 3 white)
            list(GET fields 4 black)
            list(GET fields 5 byte1888)
            list(GET fields 6 byte1890)
            list(GET fields 7 byte18c0)
            list(GET fields 8 byte18c1)
            list(GET fields 9 cache_requests)
            run_command("${GSU_RUNNER}" "${TEST_DIR}/triangle${suffix}.bin" 0x706000
                --screen-mode 0x20 --screen-base 0 --initial-ram-bank 1 --initial-sp 0x1234
                --ram-byte 0x70f002 "${input}" --ram-byte 0x701000 255 --ram-byte 0x704000 165
                --word 0x70f000 "${pixels}" --word 0x70f004 "${white}" --word 0x70f006 "${black}"
                --word 0x70f008 "${phase}" --plots 0 "${pixels}" --color 0 "${pixels}"
                --rpix 0 1 --getc 0 0 --register 0 "${pixels}" --register 6 0 --register 10 0xfffa
                --cbr 0 "${final_cache_base}" --cache-count 0 "${cache_requests}"
                --rambr 0 0 --byte 0x704000 165 --byte 0x700dd0 0 --byte 0x70228f 0
                --byte 0x701888 "${byte1888}" --byte 0x701890 "${byte1890}"
                --byte 0x7018c0 "${byte18c0}" --byte 0x7018c1 "${byte18c1}")
        endforeach()
    endforeach()
elseif(CASE STREQUAL "graphics_diagnostics")
    function(graphics_bad source)
        file(WRITE "${TEST_DIR}/invalid.dc" "${source}")
        run_expected_failure_contains(":[0-9]+:[0-9]+: error:" "${DISCC}" "${TEST_DIR}/invalid.dc" --check)
    endfunction()
    graphics_bad("void main() { pixel; }")
    graphics_bad("void main() { color 1; }")
    graphics_bad("void main() { byte c = read_pixel; }")
    graphics_bad("void main() { options dither; }")
    graphics_bad("void main() { at (1, 2); }")
    graphics_bad("void main() { plot { options invalid; } }")
    graphics_bad("void main() { plot { options dither, dither; } }")
    graphics_bad("void main() { plot { color (word*)0x100; } }")
    graphics_bad("void main() { plot { plot { pixel; } } }")
    graphics_bad("void main() { plot_begin; plot_end; }")
    graphics_bad("void main() { set_color(1); }")
    graphics_bad("void main() { plot(1, 2); }")
    graphics_bad("void main() { plot { cursor.z = 0; } }")
    graphics_bad("void main() { use bitmap unknown; }")
    graphics_bad("bitmap s { mode obj; size 256x128; depth 4bpp; base 0; }")
    graphics_bad("bitmap s { mode bitmap; size 256x200; depth 4bpp; base 0; }")
    graphics_bad("bitmap s { mode bitmap; size 256x128; depth 3bpp; base 0; }")
    graphics_bad("bitmap s { mode bitmap; size 256x128; depth 4bpp; base 1; }")
    graphics_bad("bitmap s { mode bitmap; size 256x192; depth 8bpp; base 0x1fc00; }")
    graphics_bad("bitmap s { mode bitmap; depth 4bpp; base 0; }")
    graphics_bad("bitmap s { mode obj; depth 4bpp; depth 2bpp; base 0; }")
    graphics_bad("@packed bitmap s { mode obj; depth 4bpp; base 0; }")
    graphics_bad("bitmap s { mode obj; depth 4bpp; base 0; } bitmap s { mode obj; depth 4bpp; base 0; }")
    graphics_bad("bitmap a { mode obj; depth 4bpp; base 0x6000; } bitmap b { mode obj; depth 2bpp; base 0x6000; } void main() { use bitmap a; use bitmap b; }")
    file(WRITE "${TEST_DIR}/screen.dc" "bitmap s { mode bitmap; size 256x128; depth 4bpp; base 0; } void main() { use bitmap s; }")
    run_command("${DISCC}" "${TEST_DIR}/screen.dc" -o "${TEST_DIR}/screen.o")
    run_expected_failure_contains("initial stack word" "${DISCLD}" "${TEST_DIR}/screen.o" --init-runtime -o "${TEST_DIR}/bad.bin")
    run_expected_failure_contains("RAM payload" "${DISCLD}" "${TEST_DIR}/screen.o" --origin 0x700900 -o "${TEST_DIR}/bad.bin")
    file(WRITE "${TEST_DIR}/static.dc" "ram byte table[16]; void helper() { table[0] = 1; }")
    run_command("${DISCC}" "${TEST_DIR}/static.dc" -o "${TEST_DIR}/static.o")
    run_expected_failure_contains("static RAM" "${DISCLD}" "${TEST_DIR}/screen.o" "${TEST_DIR}/static.o" --init-runtime --stack-pointer 0x8000 -o "${TEST_DIR}/bad.bin")
    file(WRITE "${TEST_DIR}/other.dc" "bitmap s { mode bitmap; size 256x160; depth 4bpp; base 0x6000; } void helper() { use bitmap s; }")
    run_command("${DISCC}" "${TEST_DIR}/other.dc" -o "${TEST_DIR}/other.o")
    run_expected_failure_contains("conflicting host bitmap" "${DISCLD}" "${TEST_DIR}/screen.o" "${TEST_DIR}/other.o" -o "${TEST_DIR}/bad.bin")
    foreach(defines IN ITEMS
        ".define __DISCO_BITMAP_SCBR 0"
        ".define __DISCO_BITMAP_SCBR 0\n.define __DISCO_BITMAP_SCMR 2"
        ".define __DISCO_BITMAP_SCBR 1\n.define __DISCO_BITMAP_SCMR 8")
        file(WRITE "${TEST_DIR}/invalid.s" "${defines}\n.segment \"CODE\"\n.export main\nmain:\nstop\nnop\n")
        run_expected_failure_contains("bitmap|Bitmap" "${DISCAS}" "${TEST_DIR}/invalid.s" -o "${TEST_DIR}/bad.o")
    endforeach()
    file(WRITE "${TEST_DIR}/spc.dc" "void main() { plot { pixel; } }")
    run_expected_failure_contains("graphics" "${DISCC}" "${TEST_DIR}/spc.dc" --target spc700 --check)
    file(WRITE "${TEST_DIR}/spc.dc" "bitmap s { mode obj; depth 4bpp; base 0; } void main() {}")
    run_expected_failure_contains("graphics" "${DISCC}" "${TEST_DIR}/spc.dc" --target spc700 --check)
    file(WRITE "${TEST_DIR}/conditional.dc" "@cfg(gsu) bitmap s { mode obj; depth 4bpp; base 0x6000; } @cfg(gsu) void main() { use bitmap s; } @cfg(spc700) void main() {}")
    run_command("${DISCC}" "${TEST_DIR}/conditional.dc" --check)
    run_command("${DISCC}" "${TEST_DIR}/conditional.dc" --target spc700 --check)
endif()
