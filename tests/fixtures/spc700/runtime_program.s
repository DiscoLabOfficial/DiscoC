; SPC700 runtime/ABI fixture, object A: main, a nested call chain f -> g
; and a recursive factorial. Used by spc700_runtime and spc700_luna_runtime;
; main returns $9DCA in YA. See tests/run_spc700_runtime_regression.cmake.
.setcpu "SPC700"
.export main
.segment "CODE"
; Push the byte in A as an argument (caller pops with incw $00).
main:
    mov a,#8
    decw $00
    mov y,#0
    mov [$00]+y,a
    call !fact              ; YA = 8! = $9D80
    incw $00
    movw $10,ya             ; f, g, fact and bump only use $04-$0B, so $10-$17 survive
    mov a,#7
    decw $00
    mov y,#0
    mov [$00]+y,a
    call !f                 ; YA = g(7) + 1 = 22
    incw $00
    addw ya,$10
    movw $10,ya
    call !bump              ; object B: counter = counter + 1, returns it in A
    mov y,#0
    movw $14,ya
    mov a,!t3               ; initialized static byte $33
    mov y,#0
    movw $16,ya
    movw ya,$14
    addw ya,$16
    addw ya,$10
    movw $10,ya
    ; Record the stack limit for 16 bytes: lowest software-stack byte + 16.
    mov a,#<__disco_stack_limit_16
    mov !limit,a
    mov a,#>__disco_stack_limit_16
    mov !limit_hi,a
    movw ya,$10
    ret                     ; $9D80 + 22 + 1 + $33 = $9DCA

; Prologue: push the caller's FP, then FP = SSP. Epilogue: SSP = FP, pop FP.
; The first argument is at FP+2.
f:
    movw ya,$02
    movw $04,ya
    decw $00
    decw $00
    mov y,#0
    mov a,$04
    mov [$00]+y,a
    inc y
    mov a,$05
    mov [$00]+y,a
    movw ya,$00
    movw $02,ya
    mov y,#2
    mov a,[$02]+y           ; argument
    decw $00
    mov y,#0
    mov [$00]+y,a
    call !g                 ; YA = arg * 3
    incw $00
    movw $04,ya
    incw $04                ; + 1
    movw ya,$04
    bra epilogue

g:
    movw ya,$02
    movw $04,ya
    decw $00
    decw $00
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
    bra epilogue

fact:
    movw ya,$02
    movw $04,ya
    decw $00
    decw $00
    mov y,#0
    mov a,$04
    mov [$00]+y,a
    inc y
    mov a,$05
    mov [$00]+y,a
    movw ya,$00
    movw $02,ya
    mov y,#2
    mov a,[$02]+y           ; n
    cmp a,#2
    bcs fact_recurse
    mov a,#1
    mov y,#0
    bra epilogue
fact_recurse:
    dec a
    decw $00
    mov y,#0
    mov [$00]+y,a
    call !fact              ; YA = (n - 1)!
    incw $00
    movw $06,ya
    mov y,#2
    mov a,[$02]+y
    mov $08,a               ; n
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

epilogue:
    movw $04,ya             ; return value
    movw ya,$02
    movw $00,ya             ; SSP = FP
    mov y,#0
    mov a,[$00]+y
    mov $02,a
    inc y
    mov a,[$00]+y
    mov $03,a               ; FP = caller's FP
    incw $00
    incw $00
    movw ya,$04
    ret
