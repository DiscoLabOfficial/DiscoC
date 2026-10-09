; Original, minimal 65816 benchmark host. No Nintendo SDK/proprietary code.
; Payload executes in ROM at $00:8000; the host waits in SNES WRAM so it
; never competes for the GSU-owned ROM/RAM during the measured interval.
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
  NAME "DISCOC GSU BENCHMARK"
  LOROM
  SLOWROM
  CARTRIDGETYPE $14
  ROMSIZE $06
  SRAMSIZE $07 ; 128 KiB GSU RAM; both banks exist in this fixed profile.
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
.IFNDEF HOST_DELAY
  .DEFINE HOST_DELAY 0
.ENDIF

.BANK 0 SLOT 0
.ORG 0
Payload:
  .IFDEF CALIBRATION
    .INCLUDE "payload.inc"
  .ELSE
    .INCBIN "payload.bin"
  .ENDIF
PayloadEnd:
.ASSERT PayloadEnd-Payload < $6000, LDERROR, "Benchmark payload overlaps the host at $00:E000"

.ORG $6000
Reset:
  sei
  clc
  xce
  rep #$30
  .ACCU 16
  .INDEX 16
  lda #$1FFF
  tcs
  lda #0
  tcd
  sep #$20
  .ACCU 8
  stz $4200
  stz $420B
  stz $420C
  lda #$80
  sta $2100
  stz $3030 ; Stop and invalidate instruction cache.
  stz $303A
  stz $3034 ; ROM program bank 0; invalidates cache again.
  stz $3037 ; Normal multiply speed, IRQ enabled; CPU interrupts stay masked.
  lda #$18
  sta $3038 ; SCBR $18 = framebuffer base $6000.
  lda #1
  sta $3039 ; CLSR=1: fast GSU clock, one master clock per GSU clock tick.
  lda #$39
  sta $303A ; 4bpp, 256x192, RON=1, RAN=1.
  lda #$34
  sta $3014
  lda #$12
  sta $3015 ; Deliberately incorrect initial R10; payload initializes it.
  ldx #0
CopyHost:
  lda.l RamHost,x
  sta.l $7E2000,x
  inx
  cpx #(RamHostEnd-RamHost)
  bne CopyHost
  jml $7E2000

RamHost:
  .IF HOST_DELAY > 0
    ldx #HOST_DELAY
DelayBefore:
    dex
    bne DelayBefore
  .ENDIF
  lda #$11
  sta.l $7E0000 ; Lua seeds inputs while stopped, before the start register write.
  stz $301E
  lda #$80
  sta $301F ; Start $00:8000 from WRAM, not from GSU-owned ROM.
Poll:
  lda $3030
  and #$20
  bne Poll
  .IF HOST_DELAY > 0
    ldx #HOST_DELAY
DelayAfter:
    dex
    bne DelayAfter
  .ENDIF
  lda #$22
  sta.l $7E0000 ; STOP confirmed. No VRAM/DMA/display work in this harness.
Idle:
  bra Idle
RamHostEnd:
InterruptReturn:
  rti
