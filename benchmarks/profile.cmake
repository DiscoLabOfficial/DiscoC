# Versioned measurement contract. Changing workloads/profile/model requires
# remeasuring both toolchains, including the historical v0.1.0 tools.
set(BENCHMARK_NAMES triangle_fill horizontal_span rom_palette_plot ram_palette_plot
    memcpy function_call switch_dense switch_sparse arithmetic)
set(BENCHMARK_METRICS code_bytes data_bytes payload_bytes instructions_executed
    loads ram_loads rom_reads stores stack_accesses stack_loads stack_stores
    plot color getc rpix branches taken_branches jumps calls cache)
set(BENCHMARK_PROFILE [=[{"target":"gsu","mapping":"lorom","origin":32768,"ram_bank":0,"ram_origin":1024,"stack_pointer":65534,"stack_begin":7397376,"stack_end":7405568,"initial_sp":4660,"initial_ram_bank":1,"runtime_initialization":true,"instruction_limit":10000000,"framebuffer_base":24576,"screen_mode":33}]=])

function(benchmark_sources name output)
    set(sources "${name}/main.dc")
    if(name STREQUAL "memcpy")
        list(APPEND sources "memcpy/copy.dc")
    elseif(name STREQUAL "function_call")
        list(APPEND sources "function_call/add.dc")
    endif()
    set(${output} "${sources}" PARENT_SCOPE)
endfunction()

function(benchmark_text_hash path output)
    file(SIZE "${path}" size)
    if(size GREATER 1048576)
        message(FATAL_ERROR "Benchmark source/report exceeds 1 MiB: ${path}")
    endif()
    file(READ "${path}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 digest "${source}")
    set(${output} "${digest}" PARENT_SCOPE)
endfunction()
