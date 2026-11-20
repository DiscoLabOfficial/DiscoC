; SPC700 runtime/ABI fixture, object B: zeroed and initialized static RAM
; and a helper called from object A. See runtime_program.s.
.setcpu "SPC700"
.export bump, t3, limit, limit_hi
.segment "CODE"
bump:
    inc !counter
    mov a,!counter
    ret
.segment "RAM"
counter:
    .byte $00, $00
t0:
    .byte $11
t1:
    .byte $22
t2:
    .byte $00
t3:
    .byte $33
limit:
    .byte $00
limit_hi:
    .byte $00
