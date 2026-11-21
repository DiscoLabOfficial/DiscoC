; SPC700 runtime/ABI fixture, object A. Used by spc700_runtime and
; spc700_luna_runtime; main returns $9DCF in YA.
;
; Frames follow docs/spc700-target.md: with L bytes of locals, FP+0..1 hold
; the caller's FP, FP+2..FP+L+1 the locals and FP+L+2.. the arguments (first
; argument lowest). Callers push arguments last to first, one word slot each
; (a byte argument uses the slot's low byte), and pop them after the call. Values live across calls stay in the frame; $04-$0B are scratch.
.setcpu "SPC700"
.export main
.segment "CODE"

; main: L = 2, accumulator word at FP+2.
main:
    movw ya,$02
    movw $04,ya
    mov x,#4
main_alloc:
    decw $00
    dec x
    bne main_alloc
    mov y,#0
    mov a,$04
    mov [$00]+y,a
    inc y
    mov a,$05
    mov [$00]+y,a
    movw ya,$00
    movw $02,ya

    mov a,#8                ; acc = fact(8) = $9D80
    decw $00
    decw $00
    mov y,#0
    mov [$00]+y,a
    call !fact
    incw $00
    incw $00
    movw $04,ya
    mov y,#2
    mov a,$04
    mov [$02]+y,a
    inc y
    mov a,$05
    mov [$02]+y,a

    mov a,#7                ; acc += f(7) = 22
    decw $00
    decw $00
    mov y,#0
    mov [$00]+y,a
    call !f
    incw $00
    incw $00
    call !acc_add

    call !bump              ; acc += counter after increment (1), object B
    mov y,#0
    call !acc_add

    mov a,!t3               ; acc += initialized static byte $33
    mov y,#0
    call !acc_add

    ; Record the stack limit for 16 bytes: lowest software-stack byte + 16.
    mov a,#<__disco_stack_limit_16
    mov !limit,a
    mov a,#>__disco_stack_limit_16
    mov !limit_hi,a

    ; acc = add_word(acc, 5): push b, then the word a (low byte first).
    mov a,#5
    decw $00
    decw $00
    mov y,#0
    mov [$00]+y,a
    mov y,#2
    mov a,[$02]+y
    mov $04,a
    inc y
    mov a,[$02]+y
    mov $05,a
    decw $00
    decw $00
    mov y,#0
    mov a,$04
    mov [$00]+y,a
    inc y
    mov a,$05
    mov [$00]+y,a
    call !add_word
    incw $00
    incw $00
    incw $00
    incw $00
    mov x,#4
    jmp !epilogue           ; $9D80 + 22 + 1 + $33 + 5 = $9DCF

; acc_add: internal helper of main, not an ABI function: it has no frame of
; its own and adds YA to main's accumulator at main's FP+2.
acc_add:
    movw $04,ya
    mov y,#2
    mov a,[$02]+y
    mov $06,a
    inc y
    mov a,[$02]+y
    mov $07,a
    movw ya,$06
    addw ya,$04
    movw $06,ya
    mov y,#2
    mov a,$06
    mov [$02]+y,a
    inc y
    mov a,$07
    mov [$02]+y,a
    ret

; f(byte x): L = 0, returns g(x) + 1.
f:
    movw ya,$02
    movw $04,ya
    mov x,#2
f_alloc:
    decw $00
    dec x
    bne f_alloc
    mov y,#0
    mov a,$04
    mov [$00]+y,a
    inc y
    mov a,$05
    mov [$00]+y,a
    movw ya,$00
    movw $02,ya
    mov y,#2
    mov a,[$02]+y           ; x
    decw $00
    decw $00
    mov y,#0
    mov [$00]+y,a
    call !g
    incw $00
    incw $00
    movw $04,ya
    incw $04
    movw ya,$04
    mov x,#2
    jmp !epilogue

; g(byte x): L = 0, returns x * 3.
g:
    movw ya,$02
    movw $04,ya
    mov x,#2
g_alloc:
    decw $00
    dec x
    bne g_alloc
    mov y,#0
    mov a,$04
    mov [$00]+y,a
    inc y
    mov a,$05
    mov [$00]+y,a
    movw ya,$00
    movw $02,ya
    mov y,#2
    mov a,[$02]+y
    mov y,#3
    mul ya
    mov x,#2
    jmp !epilogue

; fact(byte n): L = 1, local copy of n at FP+2, argument at FP+3.
fact:
    movw ya,$02
    movw $04,ya
    mov x,#3
fact_alloc:
    decw $00
    dec x
    bne fact_alloc
    mov y,#0
    mov a,$04
    mov [$00]+y,a
    inc y
    mov a,$05
    mov [$00]+y,a
    movw ya,$00
    movw $02,ya
    mov y,#3
    mov a,[$02]+y           ; argument n
    mov y,#2
    mov [$02]+y,a           ; local n
    cmp a,#2
    bcs fact_recurse
    mov a,#1
    mov y,#0
    mov x,#3
    jmp !epilogue
fact_recurse:
    dec a
    decw $00
    decw $00
    mov y,#0
    mov [$00]+y,a
    call !fact              ; YA = (n - 1)!
    incw $00
    movw $06,ya
    mov y,#2
    mov a,[$02]+y           ; local n
    mov $08,a
    mov a,$06
    mov y,$08
    mul ya
    movw $0A,ya             ; low byte * n
    mov a,$07
    mov y,$08
    mul ya                  ; high byte * n; only its low byte survives
    clrc
    adc a,$0B
    mov y,a
    mov a,$0A
    mov x,#3
    jmp !epilogue

; add_word(word a, byte b): L = 2, sum stored in a local word at FP+2;
; a at FP+4..5, b at FP+6.
add_word:
    movw ya,$02
    movw $04,ya
    mov x,#4
add_alloc:
    decw $00
    dec x
    bne add_alloc
    mov y,#0
    mov a,$04
    mov [$00]+y,a
    inc y
    mov a,$05
    mov [$00]+y,a
    movw ya,$00
    movw $02,ya
    mov y,#6
    mov a,[$02]+y
    mov $04,a
    mov $05,#0
    mov y,#4
    mov a,[$02]+y
    mov $06,a
    inc y
    mov a,[$02]+y
    mov $07,a
    movw ya,$06
    addw ya,$04
    movw $06,ya
    mov y,#2
    mov a,$06
    mov [$02]+y,a
    inc y
    mov a,$07
    mov [$02]+y,a
    mov y,#2                ; reload the local sum as the result
    mov a,[$02]+y
    mov $06,a
    inc y
    mov a,[$02]+y
    mov y,a
    mov a,$06
    mov x,#4
    jmp !epilogue

; Epilogue: YA = result, X = L + 2. SSP = FP, FP = caller's FP, SSP += X.
epilogue:
    movw $04,ya
    movw ya,$02
    movw $00,ya
    mov y,#0
    mov a,[$00]+y
    mov $06,a
    inc y
    mov a,[$00]+y
    mov $07,a
    movw ya,$06
    movw $02,ya
epilogue_pop:
    incw $00
    dec x
    bne epilogue_pop
    movw ya,$04
    ret
