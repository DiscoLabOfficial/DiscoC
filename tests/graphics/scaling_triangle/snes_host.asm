; Scaling rainbow triangle: DiscoC renders, the CPU presents during VBlank.
; The host only selects the phase and copies finished tiles, never draws them.

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
  NAME "DISCOC SCALING PLOT"
  LOROM
  SLOWROM
  CARTRIDGETYPE $14 ; Super FX + RAM, no battery-backed persistence
  ROMSIZE $06       ; 64 KiB ROM
  SRAMSIZE $06      ; 64 KiB cartridge RAM
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

.DEFINE SUPERFX_R0   $3000
.DEFINE SUPERFX_SFR  $3030
.DEFINE SUPERFX_PBR  $3034
.DEFINE SUPERFX_CFGR $3037
.DEFINE SUPERFX_SCBR $3038
.DEFINE SUPERFX_CLSR $3039
.DEFINE SUPERFX_SCMR $303A
.DEFINE SUPERFX_R15  $301E
.DEFINE GSU_LOAD_ADDRESS $706000
.DEFINE GSU_START_PC $6000
.DEFINE FRAMEBUFFER_SIZE $6000 ; 256x192, 4bpp, column-major tiles
.DEFINE GSU_COUNT $70F000
.DEFINE GSU_PHASE $70F002
.DEFINE GSU_HEIGHT $70F004
.DEFINE GSU_ROWS $70F006
.DEFINE GSU_DONE_PHASE $70F008
.DEFINE TEST_STATUS $7E0000 ; 0=rendering, 1=completed-frame publication, 2=wrong, 3=timeout
.DEFINE TEST_RESULT $7E0002
.DEFINE TEST_SP $7E0004
.DEFINE TEST_PBR $7E0006
.DEFINE TEST_RAMBR $7E0007
.DEFINE TEST_SFR $7E0008
.DEFINE TEST_RAM_RESULT $7E000A
.DEFINE TEST_PHASE $7E000C
.DEFINE TEST_HEIGHT $7E000E
.DEFINE TEST_ROWS $7E0010
.DEFINE TEST_FRAME $7E0012
.DEFINE CURRENT_PHASE $7E0014
.DEFINE DMA_SOURCE $7E0016
.DEFINE DMA_DESTINATION $7E0018
.DEFINE LAST_DMA_LINE $7E001A
.DEFINE TEST_CBR $7E001C

.BANK 0 SLOT 0
.ORG 0
.SECTION "SNES host" FORCE
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
  stz $420B
  stz $420C
  lda #$80
  sta $2100            ; initialize the PPU under forced blank
  stz $212C
  stz $212D
  stz $2130
  stz $2131
  stz $2133
  stz $2106
  stz $2123
  stz $2124
  stz $2125
  stz $212E
  stz $212F
  stz SUPERFX_SFR
  stz SUPERFX_SFR+1
  stz SUPERFX_SCMR      ; CPU owns cartridge RAM during setup
  rep #$20
  .ACCU 16
  lda #$0000
  ldx #$0000
ClearInitialBitmap:
  sta.l $700000,x
  inx
  inx
  cpx #FRAMEBUFFER_SIZE
  bne ClearInitialBitmap
  sta.l CURRENT_PHASE
  sta.l TEST_FRAME
  sep #$20
  .ACCU 8
  jsr copy_gsu_to_bwram
  jsr initialize_display
  lda #$0F
  sta $2100

RenderNextFrame:
  lda #$00
  sta.l TEST_STATUS
  rep #$20
  .ACCU 16
  lda.l CURRENT_PHASE
  sta.l GSU_PHASE
  sep #$20
  .ACCU 8
  stz SUPERFX_SCBR      ; main_screen.base = $70:0000
  lda #$70
  sta SUPERFX_PBR
  lda #$80
  sta SUPERFX_CFGR     ; disable STOP IRQ; this host polls SFR.G
  lda #$01
  sta SUPERFX_CLSR
  lda #$29             ; 256x192, 4bpp, CPU ROM, GSU RAM
  sta SUPERFX_SCMR
  jsr start_gsu
  ldy #$0040           ; bounded wait, including software fixed-point helpers
WaitAnotherGsuRound:
  ldx #$FFFF
WaitForGsu:
  lda SUPERFX_SFR
  and #$20
  beq GsuStopped
  dex
  bne WaitForGsu
  dey
  bne WaitAnotherGsuRound
  stz SUPERFX_SFR
  stz SUPERFX_SCMR
  lda #$03
  jmp Failure

GsuStopped:
  lda SUPERFX_SFR+1
  stz SUPERFX_SCMR      ; reclaim CPU RAM access only after STOP
  lda SUPERFX_PBR
  sta.l TEST_PBR
  lda $303C
  sta.l TEST_RAMBR
  lda SUPERFX_SFR
  sta.l TEST_SFR
  rep #$20
  .ACCU 16
  lda $3014
  sta.l TEST_SP
  lda $303E            ; capture the last CACHE base only after STOP
  sta.l TEST_CBR
  lda.l GSU_COUNT
  sta.l TEST_RAM_RESULT
  lda.l GSU_DONE_PHASE
  sta.l TEST_PHASE
  lda.l GSU_HEIGHT
  sta.l TEST_HEIGHT
  lda.l GSU_ROWS
  sta.l TEST_ROWS
  lda SUPERFX_R0
  sta.l TEST_RESULT
  .IFDEF FORCE_BAD_RESULT
  inc a                ; deliberately corrupt the host's expected comparison
  .ENDIF
  cmp.l TEST_RAM_RESULT
  bne WrongResult16
  cmp #0
  beq WrongResult16
  lda.l TEST_HEIGHT
  inc a
  cmp.l TEST_ROWS
  bne WrongResult16
  lda.l TEST_PHASE
  cmp.l CURRENT_PHASE
  bne WrongResult16
  lda $300C            ; R6 runtime fault status
  bne WrongResult16
  sep #$20
  .ACCU 8
  jsr wait_next_vblank
  jsr upload_dirty_tiles
  rep #$20
  .ACCU 16
  lda.l TEST_FRAME
  inc a
  sta.l TEST_FRAME
  sep #$20
  .ACCU 8
  lda #$01
  sta.l TEST_STATUS

  ; Start the next render immediately. VRAM retains the completed image while
  ; the GSU reuses cartridge RAM. Missed refreshes repeat that image; only a
  ; complete, checked pose is uploaded at the next VBlank, never partial work.
  rep #$20
  .ACCU 16
  lda.l CURRENT_PHASE
  inc a
  and #63
  sta.l CURRENT_PHASE
  sep #$20
  .ACCU 8
  jmp RenderNextFrame

WrongResult16:
  sep #$20
  .ACCU 8
  lda #$02
Failure:
  sta.l TEST_STATUS
  stz $212C            ; red backdrop only; do not retain an old BG1 triangle
  stz $2121
  lda #$1F
  sta $2122
  lda #$00
  sta $2122
  lda #$0F
  sta $2100
Finished:
  bra Finished

copy_gsu_to_bwram:
  ldx #$0000
CopyNextByte:
  lda.l gsu_triangle_start,x
  sta.l GSU_LOAD_ADDRESS,x
  inx
  cpx #(gsu_triangle_end-gsu_triangle_start)
  bne CopyNextByte
  rts

start_gsu:
  ; R15 high starts the bootstrap; low must be written first.
  lda #<GSU_START_PC
  sta SUPERFX_R15
  lda #>GSU_START_PC
  sta SUPERFX_R15+1
  rts

wait_next_vblank:
  ; Leave the current VBlank before waiting for the next leading edge.
WaitVisibleLines:
  lda $4212
  bmi WaitVisibleLines
WaitVblankStart:
  lda $4212
  bpl WaitVblankStart
  rts

.INCLUDE "display.inc"
InterruptReturn:
  rti
.ENDS

.SECTION ".rodata_disco_triangle" SUPERFREE
gsu_triangle_start:
  .INCBIN "triangle.bin"
gsu_triangle_end:
.ENDS

.ASSERT gsu_triangle_end-gsu_triangle_start > 0, LDERROR, "Empty GSU payload"
.ASSERT gsu_triangle_end-gsu_triangle_start <= $9000, LDERROR, "Payload overlaps the mailbox at $70:F000"
