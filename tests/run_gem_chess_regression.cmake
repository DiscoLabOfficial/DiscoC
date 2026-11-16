# Included by run_toolchain_regression.cmake, including its checked commands,
# optimization selection and case-specific output directory.
if(NOT DEFINED GSU_RUNNER OR NOT OPTIMIZATION MATCHES "^[2s]$")
    message(FATAL_ERROR "Gem-chess regressions require GSU_RUNNER and O2 or Os")
endif()

# Compile the actual scene functions unchanged. The declaration boundaries are
# checked so a scene reorganization fails visibly instead of testing stale code.
# The kernel starts at the scene's static-RAM state, which project/paint/collect
# use instead of pointer parameters, and ends before the projection state. Omit its
# framebuffer at zero and scene main: this fixture's stack and inputs live below
# a separate framebuffer at $6000.
set(scene_source "${ROOT_DIR}/examples/snes/gem_chess/main.dc")
file(READ "${scene_source}" scene)
string(REPLACE "\r\n" "\n" scene "${scene}")
# No ';' in these patterns: CMake would split the list item there.
foreach(boundary IN ITEMS "word camera\\[5\\]" "word q6\\(" "word inverse\\[512\\]")
    string(REGEX MATCHALL "\ninternal ${boundary}" declarations "${scene}")
    list(LENGTH declarations declaration_count)
    if(NOT declaration_count EQUAL 1)
        message(FATAL_ERROR "Expected one internal ${boundary} declaration in ${scene_source}")
    endif()
endforeach()
string(FIND "${scene}" "\ninternal word camera[5];" kernel_begin)
string(FIND "${scene}" "\ninternal word q6(" kernel_functions)
string(FIND "${scene}" "\ninternal word inverse[512]" kernel_end)
if(kernel_begin LESS 0 OR kernel_functions LESS_EQUAL kernel_begin OR kernel_end LESS_EQUAL kernel_functions)
    message(FATAL_ERROR "Gem-chess kernel declaration boundaries are missing or reordered")
endif()
math(EXPR kernel_begin "${kernel_begin} + 1")
math(EXPR kernel_length "${kernel_end} - ${kernel_begin}")
string(SUBSTRING "${scene}" ${kernel_begin} ${kernel_length} kernel)
foreach(declaration IN ITEMS "internal word spans[512]" "internal word poly_x[4]" "internal word poly_y[4]"
        "internal void collect_clipped(" "@cache internal void collect(" "internal void line("
        "internal word raster(" "internal word paint(")
    string(FIND "${kernel}" "\n${declaration}" declaration_position)
    if(declaration_position LESS 0)
        message(FATAL_ERROR "Current gem-chess kernel is missing declaration: ${declaration}")
    endif()
endforeach()

set(bitmap "")
if(CASE STREQUAL "gem_chess_paint")
    set(runner_mode --gem-paint)
    set(bitmap "bitmap fixture { mode bitmap; size 256x192; depth 4bpp; base 0x6000; }\n")
    # The runner seeds the polygon at $0108/$0110 and dirties every span word
    # at $1000. Copy both into the scene's globals so vertex-only span
    # initialization still cannot pass by reading zero-initialized rows. The
    # scene interleaves rows: [2y] is the left minimum, [2y+1] the negated
    # right maximum; the runner's layout is left[0..191], right[192..383].
    set(entry [=[
word main() {
    use bitmap fixture;
    word* input = (word*)0x100;
    word* xs = (word*)0x108; word* ys = (word*)0x110;
    word* outside = (word*)0x1000;
    for (word i = 0; i < 4; i++) { poly_x[i] = xs[i]; poly_y[i] = ys[i]; }
    for (word y = 0; y < 192; y++) { spans[y+y] = outside[y]; spans[y+y+1] = -outside[192+y]; }
    plot { options; }
    paint(input[0], input[1], input[2] != 0);
    flush;
    return 42;
}
]=])
else()
    set(entry [=[
word main() {
    word* input = (word*)0x100;
    word* outside = (word*)0x1000;
    @cache for (word y = 0; y < 192; y++) { spans[y+y] = 256; spans[y+y+1] = 1; }
]=])
    if(CASE STREQUAL "gem_chess_raster_private")
        set(runner_mode --gem-raster-private)
        # A private edge writes one side according to its orientation. Walk
        # both directions to compare both boundaries to the widened oracle.
        string(APPEND entry [=[
    collect(input[0], input[1], input[2], input[3]);
    collect(input[2], input[3], input[0], input[1]);
]=])
    elseif(CASE STREQUAL "gem_chess_raster")
        set(runner_mode --gem-raster)
        string(APPEND entry "    collect_clipped(input[0], input[1], input[2], input[3]);\n")
    else()
        message(FATAL_ERROR "Unknown gem-chess regression case: ${CASE}")
    endif()
    # The runner compares all 384 span words at $1000, in its own layout.
    string(APPEND entry "    for (word y = 0; y < 192; y++) { outside[y] = spans[y+y]; outside[192+y] = -spans[y+y+1]; }\n    return 42;\n}\n")
endif()

set(source "${TEST_DIR}/kernel.dc")
file(WRITE "${source}" "${bitmap}${kernel}\n${entry}")
# Static RAM must end below the $2000 stack. The ~2 KiB of scene globals fit
# between the runner's inputs ($0100..$0117) and its $0FFE span sentinel,
# away from every runner-checked span, input, sentinel and stack address.
set(link_options --origin 0x008000 --init-runtime --ram-bank 0 --ram-origin 0x0200 --stack-pointer 0x2000)
check_round_trip(kernel "${source}" ${link_options})

# Export the genuinely linked payload, with bootstrap and resolved metadata.
# Reassemble it without injecting another runtime or overriding its placement.
run_command("${DISCLD}" "${TEST_DIR}/kernel.o" ${link_options}
    --emit-asm "${TEST_DIR}/kernel-final.s" -o "${TEST_DIR}/kernel.bin")
run_command("${DISCAS}" "${TEST_DIR}/kernel-final.s" -o "${TEST_DIR}/kernel-final.o")
run_command("${DISCLD}" "${TEST_DIR}/kernel-final.o" -o "${TEST_DIR}/kernel-final.bin")
file(SHA256 "${TEST_DIR}/kernel.bin" direct_hash)
foreach(payload IN ITEMS kernel-asm kernel-final)
    file(SHA256 "${TEST_DIR}/${payload}.bin" round_trip_hash)
    if(NOT round_trip_hash STREQUAL direct_hash)
        message(FATAL_ERROR "${CASE} O${OPTIMIZATION}: ${payload} is not byte-exact")
    endif()
endforeach()
if(CASE STREQUAL "gem_chess_paint")
    foreach(listing IN ITEMS kernel.s kernel-final.s)
        file(READ "${TEST_DIR}/${listing}" assembly)
        if(NOT assembly MATCHES "__DISCO_BITMAP_SCBR[ \t]+\\$18" OR
           NOT assembly MATCHES "__DISCO_BITMAP_SCMR[ \t]+\\$21")
            message(FATAL_ERROR "${listing} lost the fixture's 256x192/4bpp framebuffer metadata")
        endif()
    endforeach()
endif()

# The runner owns the full deterministic edge/reversal/private/polygon corpus,
# independent widened oracle, instruction cap, ABI checks and memory sentinels.
# Byte equality above makes all three production paths execute this same code.
run_command("${GSU_RUNNER}" ${runner_mode} "${TEST_DIR}/kernel.bin" 0x008000)
message(STATUS "${CASE} O${OPTIMIZATION}: current scene kernel, byte-exact assembly paths and full oracle corpus passed")
