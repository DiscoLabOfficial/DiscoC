.segment "CODE"
.export main
main:
    iwt r0, #149
    move r1, r0
    iwt r0, #0x100
    stw (r0), r1
    ldw r2, (r0)
    push r2
    iwt r2, #0
    pop r2
    add sp, #4
    sub sp, #4
    stop
    nop
