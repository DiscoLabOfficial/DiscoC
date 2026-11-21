; Minimal SNES host for SPC700 execution tests: uploads one payload through
; the APU IPL ROM protocol, starts it twice and records each result.
;
; Build with wla-65816 and these definitions (all numeric):
;   SPC_ORIGIN   load address of payload.bin in APU RAM
;   SPC_ENTRY    first instruction to execute
; payload.bin must be next to this file or on the include path.
;
; Mailbox in WRAM (read by the emulator after the run):
;   $7E0000 status: $01 = both runs returned to the IPL,
;                   $02 = run 1 stopped through the fault channel
;   $7E0002 result of run 1 ($2142/$2143 after the IPL handshake)
;   $7E0004 result of run 2
;   $7E0006 fault code ($2141) when status is $02
;
; IPL protocol: the IPL announces $AA/$BB on ports 0/1. A block starts with
; the address on ports 2/3, a nonzero byte on port 1 and $CC on port 0; each
; data byte is written to port 1 with its index on port 0 and is echoed on
; port 0. A zero port 1 with a new port 0 value starts execution at the
; address on ports 2/3. A program returning to the IPL makes it announce
; $AA/$BB again, which is the completion signal used here. A program that
; fails a run-time check writes its fault code to port 1 and $FA to port 0,
; then executes STOP; the host records it and does not run again.

.MEMORYMAP
  DEFAULTSLOT 0
  SLOTSIZE $8000
  SLOT 0 $8000
.ENDME
.ROMBANKMAP
  BANKSTOTAL 2
  BANKSIZE $8000
  BANKS 2
.ENDRO
.EMPTYFILL $FF

.SNESHEADER
  ID "DSCO"
  NAME "DISCOC SPC700 UPLOAD"
  LOROM
  SLOWROM
  CARTRIDGETYPE $00
  ROMSIZE $06
  SRAMSIZE $00
  COUNTRY $01
  LICENSEECODE $00
  VERSION $00
.ENDSNES

.SNESNATIVEVECTOR
  COP InterruptReturn
  BRK InterruptReturn
  ABORT InterruptReturn
  NMI InterruptReturn
  IRQ InterruptReturn
.ENDNATIVEVECTOR
.SNESEMUVECTOR
  COP InterruptReturn
  ABORT InterruptReturn
  NMI InterruptReturn
  RESET Reset
  IRQBRK InterruptReturn
.ENDEMUVECTOR

.DEFINE APUIO0 $2140
.DEFINE APUIO1 $2141
.DEFINE APUIO2 $2142
.DEFINE TEST_STATUS $7E0000
.DEFINE TEST_RESULT1 $7E0002
.DEFINE TEST_RESULT2 $7E0004
.DEFINE TEST_FAULT $7E0006

.BANK 0 SLOT 0
.ORG 0
.SECTION "SPC700 upload host" FORCE
.ACCU 8
.INDEX 8
Reset:
  sei
  clc
  xce
  rep #$30
  .ACCU 16
  .INDEX 16
  ldx #$1FFF
  txs
  lda #$0000
  tcd
  sep #$20
  .ACCU 8
  lda #$00
  pha
  plb
  stz $4200
  lda #$80
  sta $2100
  lda #$00
  sta.l TEST_STATUS

  jsr wait_ipl_ready
  ; Block header: destination, nonzero port 1, $CC on port 0.
  rep #$20
  .ACCU 16
  lda #SPC_ORIGIN
  sta APUIO2
  sep #$20
  .ACCU 8
  lda #$01
  sta APUIO1
  lda #$CC
  sta APUIO0
- cmp APUIO0
  bne -

  ; Data bytes: value on port 1, running index on port 0, wait for echo.
  ldx #$0000
upload_loop:
  lda.l payload,x
  sta APUIO1
  txa
  sta APUIO0
- cmp APUIO0
  bne -
  inx
  cpx #payload_end-payload
  bne upload_loop

  ; Start: entry address, zero port 1, index + 2 on port 0 (never $AA or
  ; $FA, so neither signal can be confused with the start echo).
  txa
  inc a                ; last index + 2
  beq bump_start
  cmp #$AA
  beq bump_start
  cmp #$FA
  bne +
bump_start:
  inc a
+ jsr start_and_wait
  bcs faulted
  rep #$20
  .ACCU 16
  lda APUIO2
  sta.l TEST_RESULT1
  sep #$20
  .ACCU 8

  ; Second run without re-uploading: a fresh $CC command with zero port 1
  ; jumps straight to the entry address.
  lda #$CC
  jsr start_and_wait
  bcs faulted
  rep #$20
  .ACCU 16
  lda APUIO2
  sta.l TEST_RESULT2
  sep #$20
  .ACCU 8

  lda #$01
  sta.l TEST_STATUS
done:
  bra done

faulted:
  lda APUIO1
  sta.l TEST_FAULT
  lda #$02
  sta.l TEST_STATUS
  bra done

; A = port 0 command value. Starts SPC_ENTRY and waits until the program
; has returned to the IPL (carry clear) or stopped with a fault (carry set).
start_and_wait:
  pha
  rep #$20
  .ACCU 16
  lda #SPC_ENTRY
  sta APUIO2
  sep #$20
  .ACCU 8
  stz APUIO1
  pla
  sta APUIO0
- cmp APUIO0
  bne -
  ; The IPL has echoed the command and jumped to the program. Clear port 0
  ; before the program returns: a restarted IPL that still saw $CC there
  ; would take it as a new command and run the program again.
  stz APUIO0
- lda APUIO0
  cmp #$FA
  beq +
  cmp #$AA
  bne -
  lda APUIO1
  cmp #$BB
  bne -
  clc
  rts
+ sec
  rts

wait_ipl_ready:
- lda APUIO0
  cmp #$AA
  bne -
  lda APUIO1
  cmp #$BB
  bne -
  rts

InterruptReturn:
  rti

payload:
.INCBIN "payload.bin"
payload_end:
.ENDS
