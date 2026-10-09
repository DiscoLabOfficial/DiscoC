function(graphics_fixture name source)
    file(WRITE "${TEST_DIR}/${name}.dc" "${source}")
    check_round_trip("${name}" "${TEST_DIR}/${name}.dc" --init-runtime)
    foreach(suffix IN ITEMS "" "-asm")
        run_command("${GSU_RUNNER}" "${TEST_DIR}/${name}${suffix}.bin" 0x8000 ${ARGN} --register 6 0)
    endforeach()
endfunction()

if(CASE STREQUAL "graphics_state")
    graphics_fixture(cursor_updates [=[
void main() { plot {
    options; color 3; at (10, 20);
    word old_x = cursor.x++;
    word new_y = ++cursor.y;
    cursor.x += 2; cursor.y -= 1;
    pixel;
    *(word*)0x100 = old_x; *(word*)0x102 = new_y;
    *(word*)0x104 = cursor.x; *(word*)0x106 = cursor.y;
    flush;
} }
]=] --word 0x700100 10 --word 0x700102 21 --word 0x700104 14 --word 0x700106 20
    --plots 0 1 --rpix 0 1)
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
    set(control_modes 3)
    if(OPTIMIZATION STREQUAL "2" OR OPTIMIZATION STREQUAL "s")
        # Inlined identity does not clobber POR; the second dither is redundant.
        set(control_modes 2)
    endif()
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
]=] --word 0x700100 3 --plots 0 3 --rpix 0 1 --cmode 0 ${control_modes} --por 0 3)
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
                set(height 256)
            else()
                set(fields "mode bitmap; size ${mode};")
                if(mode STREQUAL "256x128")
                    set(height_bits 0)
                    set(height 128)
                elseif(mode STREQUAL "256x160")
                    set(height_bits 4)
                    set(height 160)
                else()
                    set(height_bits 32)
                    set(height 192)
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
            # The final logical pixel must occupy the final byte of this
            # independently calculated planar allocation, for every profile.
            math(EXPR last_y "${height} - 1")
            math(EXPR last_byte "0x706000 + 256 * ${height} * ${depth} / 8 - 1")
            graphics_fixture("${name}_edge" "bitmap screen { ${fields} depth ${depth}bpp; base 0x6000; } void main() { use bitmap screen; plot { options; draw at (255, ${last_y}) with color ${pixel_value}; *(byte*)0x100 = read_pixel at (255, ${last_y}); *(word*)0x102 = cursor.x; flush; } }"
                --screen-mode "${scm}" --byte 0x700100 "${pixel_value}" --word 0x700102 255
                --byte "${last_byte}" 1 --plots 0 1 --rpix 0 2)
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
    set(source "${ROOT_DIR}/examples/snes/triangle/main.dc")
    run_command("${DISCC}" build --config "${ROOT_DIR}/examples/snes/triangle/discoc.toml"
        --output-dir "${TEST_DIR}/project" -o "${TEST_DIR}/triangle-project.bin"
        --emit-asm "${TEST_DIR}/triangle-project.s")
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
    foreach(suffix IN ITEMS "" "-asm" "-roundtrip" "-project")
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
    string(FIND "${assembly}" "floor_div:" floor_start)
    string(FIND "${assembly}" "main:" main_start)
    if(floor_start LESS 0 OR main_start LESS_EQUAL floor_start)
        message(FATAL_ERROR "Missing rotation functions for CACHE accounting")
    endif()
    math(EXPR floor_length "${main_start} - ${floor_start}")
    string(SUBSTRING "${assembly}" ${floor_start} ${floor_length} floor_assembly)
    string(REGEX MATCHALL "\n[ \t]+cache[ \t]+" floor_caches "${floor_assembly}")
    list(LENGTH floor_caches divider_cache)
    math(EXPR expected_cache_instructions "2 + ${divider_cache}")
    if(divider_cache GREATER 1 OR NOT cache_count EQUAL expected_cache_instructions)
        message(FATAL_ERROR "Rotating triangle lost a cache hint or gained an unexpected CACHE")
    endif()
    string(SUBSTRING "${assembly}" ${main_start} -1 main_assembly)
    # CMake treats semicolons inside MATCHALL results as list separators.
    string(REPLACE ";" "|" main_assembly "${main_assembly}")
    string(REGEX MATCHALL "cache [|] CODE\\+\\$[0-9A-F]+" main_caches "${main_assembly}")
    list(LENGTH main_caches main_cache_count)
    if(NOT main_cache_count EQUAL 2)
        message(FATAL_ERROR "Rotating triangle must retain both explicit loop CACHE hints")
    endif()
    list(GET main_caches 0 clear_cache)
    list(GET main_caches 1 pixel_cache)
    string(FIND "${main_assembly}" "${clear_cache}" clear_start)
    string(FIND "${main_assembly}" "${pixel_cache}" pixel_start)
    math(EXPR clear_length "${pixel_start} - ${clear_start}")
    string(SUBSTRING "${main_assembly}" ${clear_start} ${clear_length} clear_body)
    string(SUBSTRING "${main_assembly}" ${pixel_start} -1 pixel_body)
    string(REGEX MATCHALL "\n[ \t]+loop [|]" clear_loops "${clear_body}")
    string(REGEX MATCHALL "\n[ \t]+loop [|]" pixel_loops "${pixel_body}")
    list(LENGTH clear_loops clear_hardware_loop)
    list(LENGTH pixel_loops pixel_hardware_loop)
    if(clear_hardware_loop GREATER 1 OR pixel_hardware_loop GREATER 1)
        message(FATAL_ERROR "Unexpected rotation LOOP topology")
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
            # A counted LOOP no longer revisits a failed header comparison.
            # Keep exact counts for the selected emitted speed/size strategy,
            # including the bounded divider kernel's optional CACHE request.
            math(EXPR occupied_rows "${cache_requests} - 14 * 113 - ${pixels}")
            math(EXPR cache_requests "${cache_requests} - 14 * ${clear_hardware_loop} - ${occupied_rows} * ${pixel_hardware_loop}")
            if(phase EQUAL 0 OR phase EQUAL 32)
                set(divisions 4)
            else()
                set(divisions 6)
            endif()
            math(EXPR cache_requests "${cache_requests} + ${divisions} * ${divider_cache}")
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
elseif(CASE STREQUAL "graphics_scaling")
    set(source "${ROOT_DIR}/tests/graphics/scaling_triangle/triangle.dc")
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
    if(NOT assembly MATCHES "__DISCO_BITMAP_SCMR[ \t]+\\$21" OR
       NOT assembly MATCHES "__DISCO_BITMAP_SCBR[ \t]+\\$00")
        message(FATAL_ERROR "Scaling triangle lost its 4bpp/192-line bitmap metadata")
    endif()
    file(SHA256 "${TEST_DIR}/triangle.bin" direct)
    foreach(suffix IN ITEMS "" "-asm" "-roundtrip")
        file(SHA256 "${TEST_DIR}/triangle${suffix}.bin" assembled)
        if(NOT assembled STREQUAL direct)
            message(FATAL_ERROR "Scaling triangle assembly is not byte-exact: ${suffix}")
        endif()
        # Growth, maximum, shrink, wrap and full-width volatile input masking.
        foreach(input IN ITEMS 0 1 16 31 32 33 48 63 64 255 511 65535)
            math(EXPR phase "${input} & 63")
            set(amplitude ${phase})
            if(phase GREATER 32)
                math(EXPR amplitude "64 - ${phase}")
            endif()
            math(EXPR height "24 + ${amplitude}")
            math(EXPR rows "${height} + 1")
            math(EXPR pixels "${rows} * ${rows}")
            math(EXPR low "${input} & 255")
            math(EXPR high "${input} >> 8")
            set(plots ${pixels})
            set(colors 8)
            if(phase EQUAL 0 OR phase GREATER 32)
                # The two old edges share one apex, erased twice deliberately.
                math(EXPR plots "${pixels} + 2 * (${height} + 2)")
                set(colors 9)
            endif()
            set(landmarks)
            # Independently reconstruct four planar bytes from the analytic
            # shape/color definition, not from the emitted code or its counters.
            foreach(y IN ITEMS 72 96 104 120 128)
                foreach(plane RANGE 0 3)
                    set(expected 0)
                    foreach(bit RANGE 0 7)
                        math(EXPR x "128 + 7 - ${bit}")
                        math(EXPR row "${y} - (128 - ${height})")
                        math(EXPR delta "${x} - 128")
                        if(row GREATER_EQUAL 0 AND row LESS_EQUAL height AND delta LESS_EQUAL row)
                            math(EXPR color "1 + 8 * ${row} / ${rows}")
                            math(EXPR expected "${expected} | (((${color} >> ${plane}) & 1) << ${bit})")
                        endif()
                    endforeach()
                    math(EXPR address "0x700000 + (16 * 24 + (${y} >> 3)) * 32 + (${y} & 7) * 2 + (${plane} >> 1) * 16 + (${plane} & 1)")
                    list(APPEND landmarks --byte "${address}" "${expected}")
                endforeach()
            endforeach()
            run_command("${GSU_RUNNER}" "${TEST_DIR}/triangle${suffix}.bin" 0x706000
                --screen-mode 0x21 --screen-base 0 --initial-ram-bank 1 --initial-sp 0x1234
                --ram-byte 0x70f002 "${low}" --ram-byte 0x70f003 "${high}"
                --ram-byte 0x703200 255 --ram-byte 0x700000 165
                --word 0x70f000 "${pixels}" --word 0x70f004 "${height}" --word 0x70f006 "${rows}"
                --word 0x70f008 "${phase}" --plots 0 "${plots}" --color 0 "${colors}" --rpix 0 1 --getc 0 0
                --register 0 "${pixels}" --register 6 0 --register 10 0xfffa --rambr 0 0
                --byte 0x700000 165 ${landmarks})
        endforeach()
    endforeach()
elseif(CASE STREQUAL "graphics_interactive")
    set(source "${ROOT_DIR}/tests/graphics/interactive_shapes/main.dc")
    set(origin 0x708000)
    set(execution_memory ram)
    if(NOT OPTIMIZATION MATCHES "^[2s]$")
        # O1 is semantically tested in the full-width GSU ROM mirror bank.
        # O0 additionally checks the explicit oversized-program diagnostic.
        set(origin 0x400000)
        set(execution_memory rom)
    endif()
    set(link_options --origin ${origin} --init-runtime --ram-bank 0 --stack-pointer 0xEFFE)
    run_command("${DISCC}" --target gsu --execution-memory ${execution_memory} "${source}" -o "${TEST_DIR}/shapes.o")
    run_command("${DISCC}" --target gsu --execution-memory ${execution_memory} --emit-asm "${source}" -o "${TEST_DIR}/shapes-compiler.s")
    run_command("${DISCAS}" "${TEST_DIR}/shapes-compiler.s" -o "${TEST_DIR}/shapes-asm.o")
    if(execution_memory STREQUAL "rom")
        run_expected_failure_contains("program-bank boundary" "${DISCLD}" "${TEST_DIR}/shapes.o"
            --origin 0x708000 --init-runtime --ram-bank 0 --stack-pointer 0xEFFE -o "${TEST_DIR}/too-large.bin")
    endif()
    if(NOT OPTIMIZATION MATCHES "^[12s]$")
        foreach(object IN ITEMS shapes.o shapes-asm.o)
            run_expected_failure_contains("program-bank boundary" "${DISCLD}" "${TEST_DIR}/${object}"
                ${link_options} -o "${TEST_DIR}/too-large.bin")
        endforeach()
        message(STATUS "O0 full renderer analyzes/assembles, and oversized RAM/ROM placements are rejected; execution coverage uses O1/O2/Os")
        return()
    endif()
    run_command("${DISCLD}" "${TEST_DIR}/shapes.o" ${link_options} --emit-asm "${TEST_DIR}/shapes.s" -o "${TEST_DIR}/shapes.bin")
    run_command("${DISCLD}" "${TEST_DIR}/shapes-asm.o" ${link_options} -o "${TEST_DIR}/shapes-asm.bin")
    run_command("${DISCAS}" "${TEST_DIR}/shapes.s" -o "${TEST_DIR}/shapes-final.o")
    run_command("${DISCLD}" "${TEST_DIR}/shapes-final.o" -o "${TEST_DIR}/shapes-roundtrip.bin")
    file(READ "${TEST_DIR}/shapes.s" assembly)
    if(NOT assembly MATCHES "__DISCO_BITMAP_SCMR[ \t]+\\$25" OR
       NOT assembly MATCHES "__DISCO_BITMAP_SCBR[ \t]+\\$00")
        message(FATAL_ERROR "Interactive shapes lost OBJ/4bpp bitmap metadata")
    endif()
    file(SHA256 "${TEST_DIR}/shapes.bin" direct)
    foreach(suffix IN ITEMS "" "-asm" "-roundtrip")
        file(SHA256 "${TEST_DIR}/shapes${suffix}.bin" assembled)
        if(NOT assembled STREQUAL direct)
            message(FATAL_ERROR "Interactive shapes assembly is not byte-exact: ${suffix}")
        endif()
        # Analytic major-axis edge points and outward face normals produce
        # these counts independently of the renderer's error accumulator.
        foreach(record IN ITEMS
            "0,0,6,0,42,8554,9" "48,48,6,0,42,8554,9" "8,8,16,0,42,9174,9" "63,63,16,0,42,9188,9"
            "0,0,16,1,9,11757,5" "16,0,6,1,9,8715,6" "0,16,16,1,9,11757,2" "32,32,16,1,9,11757,5"
            "63,0,16,1,12,12085,2" "0,63,6,1,10,8754,5" "8,8,6,1,13,8852,5" "12,1,16,1,13,12311,6"
            "9,7,16,1,13,12833,5" "63,63,6,1,13,8809,5"
            "255,511,0,255,13,8809,5" "65535,65535,32767,65534,42,9188,9"
            "65535,65535,65535,65535,13,8809,5")
            string(REPLACE "," ";" fields "${record}")
            list(GET fields 0 yaw)
            list(GET fields 1 pitch)
            list(GET fields 2 size)
            list(GET fields 3 mode)
            list(GET fields 4 primitives)
            list(GET fields 5 plots)
            list(GET fields 6 ink)
            set(inputs)
            foreach(field IN ITEMS yaw pitch size mode)
                if(field STREQUAL "yaw")
                    set(address 0x70f002)
                elseif(field STREQUAL "pitch")
                    set(address 0x70f004)
                elseif(field STREQUAL "size")
                    set(address 0x70f006)
                else()
                    set(address 0x70f008)
                endif()
                math(EXPR high_address "${address}+1")
                math(EXPR low "${${field}} & 255")
                math(EXPR high "${${field}} >> 8")
                list(APPEND inputs --ram-byte ${address} ${low} --ram-byte ${high_address} ${high})
            endforeach()
            math(EXPR yaw "${yaw}&63")
            math(EXPR pitch "${pitch}&63")
            math(EXPR mode "${mode}&1")
            if(size GREATER 32767)
                math(EXPR size "${size}-65536")
            endif()
            if(size LESS 6)
                set(size 6)
            elseif(size GREATER 16)
                set(size 16)
            endif()
            run_command("${GSU_RUNNER}" "${TEST_DIR}/shapes${suffix}.bin" ${origin}
                --screen-mode 0x25 --screen-base 0 --initial-ram-bank 1 --initial-sp 0x7770
                ${inputs}
                --ram-byte 0x700000 165 --ram-byte 0x702000 90
                --ram-byte 0x701080 165 --ram-byte 0x701090 90
                --ram-byte 0x703080 165 --ram-byte 0x703090 90
                --register 0 ${primitives} --register 6 0 --register 10 0xeffa --rambr 0 0
                --word 0x70f000 ${primitives} --word 0x70f010 ${yaw} --word 0x70f012 ${pitch}
                --word 0x70f014 ${size} --word 0x70f016 ${mode}
                --plots 0 ${plots} --rpix 0 1 --getc 0 0 --por 0 17 --colr 0 ${ink}
                --byte 0x700000 165 --byte 0x702000 90
                --byte 0x701080 0 --byte 0x701090 0 --byte 0x703080 0 --byte 0x703090 0)
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
