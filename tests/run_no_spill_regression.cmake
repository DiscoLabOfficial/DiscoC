cmake_minimum_required(VERSION 3.15)
if(NOT DEFINED DISCC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "DISCC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")

function(no_spill_command)
    execute_process(COMMAND "${DISCC}" ${ARGV} WORKING_DIRECTORY "${TEST_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "No-spill command failed (${result}): ${ARGV}\n${output}\n${error}")
    endif()
    set(command_output "${output}" PARENT_SCOPE)
endfunction()
function(no_spill_failure pattern)
    execute_process(COMMAND "${DISCC}" ${ARGN} WORKING_DIRECTORY "${TEST_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
    if(result STREQUAL "0" OR NOT "${output}\n${error}" MATCHES "${pattern}" OR
       "${output}\n${error}" MATCHES "Internal Compiler Error|IR verifier:")
        message(FATAL_ERROR "Expected no-spill diagnostic '${pattern}': ${ARGN}\n${output}\n${error}")
    endif()
endfunction()
function(no_spill_equal left right)
    file(SHA256 "${left}" left_hash)
    file(SHA256 "${right}" right_hash)
    if(NOT left_hash STREQUAL right_hash)
        message(FATAL_ERROR "No-spill option changed spill-free output: ${left} and ${right}")
    endif()
endfunction()
function(no_spill_unchanged path expected)
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "No-spill validation clobbered ${path}")
    endif()
endfunction()

file(WRITE "${TEST_DIR}/simple.dc" "word main(){return 42;}\n")
# Explicit local storage, unused ABI parameters, and outgoing constant ABI
# arguments must not be mistaken for compiler-generated IR spill homes.
# Address-calculation intermediates can still require real compiler spill homes;
# use a direct scalar address here rather than assuming array indexing is free.
file(WRITE "${TEST_DIR}/allowed.dc" [=[
void opaque(word* address,word value);
void allowed(word argument){word local=42;opaque(&local,7);}
]=])
file(WRITE "${TEST_DIR}/array-storage.dc" "void array_storage(){word local[8];}\n")
file(WRITE "${TEST_DIR}/preserves.dc" [=[
void opaque(word value);
word preserves(word argument){word value=argument+1;opaque(7);return value;}
]=])
# All volatile arguments are evaluated before the call. Their simultaneous
# liveness exceeds the GSU allocator's register budget at every optimization.
set(pressure [=[
word sink(word a,word b,word c,word d,word e,word f,word g,word h,word i,word j,word k,word l);
word pressure(){return sink(
*(volatile word*)0x100,*(volatile word*)0x102,*(volatile word*)0x104,
*(volatile word*)0x106,*(volatile word*)0x108,*(volatile word*)0x10a,
*(volatile word*)0x10c,*(volatile word*)0x10e,*(volatile word*)0x110,
*(volatile word*)0x112,*(volatile word*)0x114,*(volatile word*)0x116);}
]=])
file(WRITE "${TEST_DIR}/pressure.dc" "${pressure}")
file(WRITE "${TEST_DIR}/divmod.dc" "word paired(word a,word b){return a/b+a%b;}\n")
file(WRITE "${TEST_DIR}/empty.dc" "")
file(WRITE "${TEST_DIR}/api.dci" "module API;void touch(word value);\n")

foreach(help IN ITEMS single project)
    if(help STREQUAL "project")
        no_spill_command(build --help)
    else()
        no_spill_command(--help)
    endif()
    if(NOT command_output MATCHES "--require-no-spills" OR NOT command_output MATCHES "prebuilt objects")
        message(FATAL_ERROR "${help} help omits the no-spill switch or source-unit scope")
    endif()
endforeach()

set(project "${TEST_DIR}/source project")
file(MAKE_DIRECTORY "${project}")
file(WRITE "${project}/api.dci" "module API;void touch(word value);\n")
file(WRITE "${project}/touch.dc" "import \"api.dci\";void touch(word value){}\n")
file(WRITE "${project}/main.dc" "import \"touch.dc\";void main(){touch(7);}\n")
file(WRITE "${project}/discoc.toml" "[project]\nname='no-spills'\nsources=['main.dc']\n[runtime]\ninitialize=true\n[output]\nbinary='payload.bin'\nassembly='payload.s'\n")
file(WRITE "${project}/bad.dc" "${pressure}")
# Even an unused imported implementation is analyzed and therefore constrained.
file(WRITE "${project}/bad-main.dc" "import \"bad.dc\";void main(){}\n")
file(WRITE "${project}/bad.toml" "[project]\nsources=['bad-main.dc']\n[output]\nbinary='payload.bin'\n")
# Also cover an explicitly listed implementation that is not imported.
file(WRITE "${project}/listed.toml" "[project]\nsources=['main.dc','bad.dc']\n[output]\nbinary='payload.bin'\n")

foreach(level IN ITEMS 0 1 2 s)
    set(flags "-O${level}")
    set(prefix "${TEST_DIR}/O${level}")
    file(MAKE_DIRECTORY "${prefix}")
    foreach(source IN ITEMS simple allowed array-storage)
        foreach(suffix IN ITEMS o s)
            set(mode)
            if(suffix STREQUAL "s")
                set(mode --emit-asm)
            endif()
            no_spill_command(${flags} ${mode} "${TEST_DIR}/${source}.dc" -o "${prefix}/${source}.${suffix}")
            no_spill_command(${flags} --require-no-spills ${mode} "${TEST_DIR}/${source}.dc" -o "${prefix}/${source}-strict.${suffix}")
            no_spill_equal("${prefix}/${source}.${suffix}" "${prefix}/${source}-strict.${suffix}")
        endforeach()
    endforeach()
    no_spill_command(${flags} --check "${TEST_DIR}/pressure.dc")
    no_spill_command(${flags} --emit-ir "${TEST_DIR}/pressure.dc")
    no_spill_command(${flags} "${TEST_DIR}/pressure.dc" -o "${prefix}/pressure-ordinary.o")
    foreach(mode IN ITEMS --check --emit-ir)
        no_spill_command(${flags} ${mode} "${TEST_DIR}/simple.dc")
        set(ordinary_ir "${command_output}")
        no_spill_command(${flags} --require-no-spills ${mode} "${TEST_DIR}/simple.dc"
            -o "${prefix}/inspection/no-output.o")
        if(NOT command_output STREQUAL ordinary_ir)
            message(FATAL_ERROR "Strict ${mode} changed successful inspection output at ${flags}")
        endif()
        if(EXISTS "${prefix}/inspection")
            message(FATAL_ERROR "Strict ${mode} created an output directory")
        endif()
        no_spill_command(${flags} --require-no-spills ${mode} "${TEST_DIR}/empty.dc")
        no_spill_command(${flags} --require-no-spills ${mode} "${TEST_DIR}/api.dci")
    endforeach()
    # Inspection and failed emission must leave existing destinations untouched.
    set(sentinel "${prefix}/preserved-output")
    file(WRITE "${sentinel}" "existing output\n")
    file(SHA256 "${sentinel}" sentinel_hash)
    foreach(mode IN ITEMS object --emit-asm --check --emit-ir)
        set(options)
        if(NOT mode STREQUAL "object")
            set(options "${mode}")
        endif()
        no_spill_failure("--require-no-spills" ${flags} --require-no-spills ${options}
            "${TEST_DIR}/pressure.dc" -o "${sentinel}")
        no_spill_unchanged("${sentinel}" "${sentinel_hash}")
    endforeach()
    no_spill_command(${flags} --require-no-spills --check "${TEST_DIR}/simple.dc" -o "${sentinel}")
    no_spill_unchanged("${sentinel}" "${sentinel_hash}")
    no_spill_failure("--require-no-spills.*--emit-ast" ${flags} --require-no-spills
        --emit-ast "${TEST_DIR}/simple.dc" -o "${sentinel}")
    no_spill_unchanged("${sentinel}" "${sentinel_hash}")
    # An unsupported target remains usable in unconstrained inspection modes.
    foreach(mode IN ITEMS --check --emit-ir --emit-ast)
        no_spill_command(${flags} --target spc700 ${mode} "${TEST_DIR}/simple.dc")
    endforeach()
    foreach(mode IN ITEMS --check --emit-ir object)
        set(options)
        if(NOT mode STREQUAL "object")
            set(options "${mode}")
        endif()
        no_spill_failure("spc700.*--require-no-spills.*backend" ${flags} --target spc700
            --require-no-spills ${options} "${TEST_DIR}/simple.dc")
    endforeach()
    no_spill_command(${flags} --emit-ast "${TEST_DIR}/simple.dc")

    foreach(mode IN ITEMS --check --emit-ir --emit-asm object)
        set(options)
        if(NOT mode STREQUAL "object")
            set(options "${mode}")
        endif()
        no_spill_failure("--require-no-spills" ${flags} --require-no-spills ${options}
            "${project}/bad-main.dc" -o "${sentinel}")
        no_spill_unchanged("${sentinel}" "${sentinel_hash}")
    endforeach()
    no_spill_command(build --config "${project}/discoc.toml" ${flags} --require-no-spills
        --check --output-dir "${prefix}/project-check")
    if(EXISTS "${prefix}/project-check")
        message(FATAL_ERROR "Strict project --check created output directories")
    endif()
    no_spill_command(build --config "${project}/discoc.toml" ${flags}
        --output-dir "${prefix}/ordinary-project")
    # A successful link proves the compiler-only switch was not sent to discld.
    no_spill_command(build --config "${project}/discoc.toml" ${flags} --require-no-spills
        --output-dir "${prefix}/strict-project")
    foreach(output IN ITEMS 0-main.o 1-touch.o payload.bin payload.s)
        no_spill_equal("${prefix}/ordinary-project/${output}" "${prefix}/strict-project/${output}")
    endforeach()
    no_spill_command(--project "${project}/discoc.toml" ${flags} --require-no-spills
        --check --output-dir "${prefix}/alias-check")
    if(EXISTS "${prefix}/alias-check")
        message(FATAL_ERROR "Strict --project --check created output directories")
    endif()
    foreach(manifest IN ITEMS bad listed)
        no_spill_failure("--require-no-spills" build --config "${project}/${manifest}.toml"
            ${flags} --require-no-spills --check --output-dir "${prefix}/${manifest}-check")
        if(EXISTS "${prefix}/${manifest}-check")
            message(FATAL_ERROR "Failed strict project --check created output directories")
        endif()
        no_spill_failure("--require-no-spills" build --config "${project}/${manifest}.toml"
            ${flags} --require-no-spills --output-dir "${prefix}/strict-project")
        no_spill_equal("${prefix}/ordinary-project/payload.bin" "${prefix}/strict-project/payload.bin")
    endforeach()
    no_spill_failure("spc700.*--require-no-spills.*backend" build --config "${project}/discoc.toml"
        ${flags} --target spc700 --require-no-spills --check)
endforeach()

# Register preservation across a call is ABI traffic, not a spill home.
no_spill_command(-O2 --require-no-spills "${TEST_DIR}/preserves.dc" -o "${TEST_DIR}/preserves.o")
# A fused divmod's auxiliary home is a spill even when its quotient has a register.
foreach(mode IN ITEMS --check --emit-ir --emit-asm object)
    set(options)
    if(NOT mode STREQUAL "object")
        set(options "${mode}")
    endif()
    no_spill_failure("--require-no-spills" -O2 --require-no-spills ${options}
        "${TEST_DIR}/divmod.dc" -o "${TEST_DIR}/unexpected-divmod-output")
endforeach()
if(EXISTS "${TEST_DIR}/unexpected-divmod-output" OR EXISTS "${TEST_DIR}/simple.o" OR
   EXISTS "${TEST_DIR}/empty.o" OR EXISTS "${TEST_DIR}/api.o")
    message(FATAL_ERROR "No-output no-spill validation created an implicit or failed output")
endif()
message(STATUS "No-spill CLI, inspection, output-preservation and project regressions passed")
