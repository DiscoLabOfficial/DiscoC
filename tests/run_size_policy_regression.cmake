# Independent objects own identically named private division kernels; mixed
# optimization levels and RAM banks must retain the public ABI and relocations.
file(WRITE "${TEST_DIR}/first.dc" "word first(word a,word b){return a/b+(a+1)/b;}")
file(WRITE "${TEST_DIR}/second.dc" "word second(word a,word b){return a%b+(a+1)%b;}")
file(WRITE "${TEST_DIR}/main.dc" [=[
word first(word a,word b); word second(word a,word b);
word main(){word a=*(volatile word*)0x100;word b=*(volatile word*)0x102;return first(a,b)+second(a,b);}
]=])
foreach(level IN ITEMS 0 1 2 s)
    foreach(name IN ITEMS first second main)
        run_command("${DISCC}" "-O${level}" "${TEST_DIR}/${name}.dc" -o "${TEST_DIR}/${name}-${level}.o")
    endforeach()
endforeach()
foreach(other IN ITEMS 0 1 2 s)
    foreach(bank IN ITEMS 0 1)
        set(payload "${TEST_DIR}/mixed-${other}-${bank}")
        run_command("${DISCLD}" "${TEST_DIR}/main-s.o" "${TEST_DIR}/first-s.o" "${TEST_DIR}/second-${other}.o"
            --origin 0x706000 --init-runtime --ram-bank ${bank} --stack-pointer 0x2000 --emit-asm "${payload}.s" -o "${payload}.bin")
        run_command("${DISCAS}" "${payload}.s" -o "${payload}-final.o")
        run_command("${DISCLD}" "${payload}-final.o" -o "${payload}-final.bin")
        file(SHA256 "${payload}.bin" expected)
        file(SHA256 "${payload}-final.bin" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Os multi-object kernels did not reconstruct byte exactly")
        endif()
        foreach(pair IN ITEMS "42:3" "-42:3" "42:-3" "-42:-3" "32767:7" "-32768:-1" "-32768:3")
            string(REPLACE ":" ";" values "${pair}")
            list(GET values 0 a)
            list(GET values 1 b)
            math(EXPR next "(${a}+1)&65535")
            if(next GREATER_EQUAL 32768)
                math(EXPR next "${next}-65536")
            endif()
            math(EXPR result "(${a}/${b}+${next}/${b}+${a}%${b}+${next}%${b})&65535")
            math(EXPR base "0x700000+${bank}*65536")
            math(EXPR al "${a}&255")
            math(EXPR ah "(${a}>>8)&255")
            math(EXPR bl "${b}&255")
            math(EXPR bh "(${b}>>8)&255")
            math(EXPR address "${base}+256")
            math(EXPR address1 "${address}+1")
            math(EXPR address2 "${address}+2")
            math(EXPR address3 "${address}+3")
            run_command("${GSU_RUNNER}" "${payload}.bin" 0x706000 --initial-sp 0x7777 --initial-ram-bank 1
                --ram-byte ${address} ${al} --ram-byte ${address1} ${ah} --ram-byte ${address2} ${bl} --ram-byte ${address3} ${bh}
                --reads ${address} 1 --reads ${address2} 1 --register 0 ${result} --register 6 0 --register 10 0x1ffc --rambr 0 ${bank})
        endforeach()
    endforeach()
endforeach()

foreach(level IN ITEMS 2 s)
    run_command("${DISCLD}" "${TEST_DIR}/main-${level}.o" "${TEST_DIR}/first-${level}.o" "${TEST_DIR}/second-${level}.o"
        --origin 0x706000 --init-runtime --ram-bank 0 --stack-pointer 0x2000 -o "${TEST_DIR}/size-report-${level}.bin")
    file(SIZE "${TEST_DIR}/size-report-${level}.bin" bytes_${level})
endforeach()
if(NOT bytes_s LESS bytes_2)
    message(FATAL_ERROR "Os repeated divmod kernels did not amortize calls/preservation")
endif()
message(STATUS "Repeated divisions: O2 ${bytes_2} bytes, Os ${bytes_s} bytes; mixed-level result/bank/stack checks passed")

file(WRITE "${TEST_DIR}/tails.dc" [=[
void tick(word a){*(volatile word*)0x120=a;}
word choose(word s,word a){
    if(s){*(volatile word*)0x122=1;tick(a);return 42;}
    else{*(volatile word*)0x122=2;tick(a);return 42;}
}
word main(){return choose(*(volatile word*)0x100,*(volatile word*)0x102);}
]=])
foreach(level IN ITEMS 2 s)
    set(OPTIMIZATION ${level})
    check_round_trip("tails-${level}" "${TEST_DIR}/tails.dc" --init-runtime)
    foreach(seed IN ITEMS 0 1)
        math(EXPR witness "2-${seed}")
        foreach(suffix IN ITEMS "" "-asm")
            run_command("${GSU_RUNNER}" "${TEST_DIR}/tails-${level}${suffix}.bin" 0x8000
                --ram-byte 0x700100 ${seed} --ram-byte 0x700102 19
                --word 0x700120 19 --word 0x700122 ${witness} --writes 0x700120 1 --writes 0x700122 1
                --reads 0x700100 1 --reads 0x700102 1 --register 0 42 --register 6 0 --register 10 0x1ffc)
        endforeach()
    endforeach()
endforeach()
file(SIZE "${TEST_DIR}/tails-2.bin" speed_bytes)
file(SIZE "${TEST_DIR}/tails-s.bin" size_bytes)
if(size_bytes GREATER speed_bytes)
    message(FATAL_ERROR "Os byte selection grew a payload beyond its O2 candidate")
endif()

file(WRITE "${TEST_DIR}/discoc.toml" "[project]\nsources=['project.dc']\n[compiler]\noptimization_level='s'\n[runtime]\ninitialize=true\n[output]\ndirectory='project'\nbinary='payload.bin'\n")
file(WRITE "${TEST_DIR}/project.dc" "import \"first.dc\";import \"second.dc\";word main(){return first(40,3)+second(40,3);}")
unset(OPTIMIZATION)
run_command("${DISCC}" build --config "${TEST_DIR}/discoc.toml")
run_command("${GSU_RUNNER}" "${TEST_DIR}/project/payload.bin" 0x8000 --register 0 29 --register 6 0)
foreach(order IN ITEMS before after)
    if(order STREQUAL "before")
        set(flags -O2 --config "${TEST_DIR}/discoc.toml")
    else()
        set(flags --config "${TEST_DIR}/discoc.toml" -O2)
    endif()
    run_command("${DISCC}" ${flags} "${TEST_DIR}/project.dc" -o "${TEST_DIR}/configured-${order}.o")
endforeach()
run_command("${DISCC}" -O2 "${TEST_DIR}/project.dc" -o "${TEST_DIR}/cli.o")
file(SHA256 "${TEST_DIR}/cli.o" expected)
foreach(order IN ITEMS before after)
    file(SHA256 "${TEST_DIR}/configured-${order}.o" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "CLI did not override Os manifest configuration")
    endif()
endforeach()
