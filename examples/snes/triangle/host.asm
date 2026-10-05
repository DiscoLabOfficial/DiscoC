; Standalone SNES host: ROM .incbin -> cartridge RAM -> GSU -> VRAM -> BG1.
; Build with build-snes.cmake; triangle.bin is a fixed-origin $70:6000 payload.
; This is 65816 host assembly, not a WLA GSU re-export.

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
  NAME "DISCOC TRIANGLE DEMO"
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
.INCLUDE "triangle-config.inc" ; Generated from the linked assembly metadata.
  .DEFINE GSU_RESULT $70F000
  .DEFINE FRAMEBUFFER_SIZE $6000 ; 256*192*4/8 bytes, column-major tiles
  .DEFINE TILEMAP_WORD_ADDRESS $3800 ; byte address $7000, BG1SC=$38
  .DEFINE BLANK_TILE 768 ; one cleared 4bpp tile after the 768 bitmap tiles
.DEFINE TEST_STATUS $7E0000 ; $01=pass, $02=wrong result, $03=timeout
.DEFINE TEST_RESULT $7E0002 ; CPU-read R0 after STOP
.DEFINE TEST_SP $7E0004     ; R10 snapshot after STOP
.DEFINE TEST_PBR $7E0006
.DEFINE TEST_RAMBR $7E0007
.DEFINE TEST_SFR $7E0008
.DEFINE TEST_RAM_RESULT $7E000A ; CPU-read RAM diagnostic after STOP
.IFNDEF EXPECTED_RESULT
  .DEFINE EXPECTED_RESULT 9409
.ENDIF

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
  plb                  ; DBR=$00 for SNES and Super FX registers

  stz $4200            ; no NMI or CPU timer IRQ
  stz $420B
  stz $420C
  lda #$80
  sta $2100            ; forced blank while setting backdrop color
  stz $212C
  stz $212D
  stz $2130
  stz $2131
  stz $2133
  stz $2106            ; no mosaic
  stz $2123            ; no window masking or color-window effects
  stz $2124
  stz $2125
  stz $212E
  stz $212F
  lda #$00
  sta.l TEST_STATUS
  sta.l TEST_RESULT
  sta.l TEST_RESULT+1

  stz SUPERFX_SFR       ; keep GSU stopped during CPU RAM copy
  stz SUPERFX_SFR+1
  stz SUPERFX_SCMR      ; CPU owns RAM before launching the GSU
  jsr clear_triangle_framebuffer
  jsr copy_gsu_to_bwram

  ; Match main.dc's bitmap metadata and the linker's execution origin.
  lda #BITMAP_SCBR      ; bitmap base, SCBR in 1024-byte units
  sta SUPERFX_SCBR
  lda #$70
  sta SUPERFX_PBR
  lda #%00000000       ; normal multiply speed; GSU STOP IRQ enabled
  sta SUPERFX_CFGR     ; CPU SEI prevents that IRQ from interrupting this test
  lda #%00000001
  sta SUPERFX_CLSR
  lda #(BITMAP_SCMR | $08) ; mode bits + RAN=1 (GSU RAM), RON=0 (CPU ROM)
  sta SUPERFX_SCMR
  jsr start_gsu

  ldy #$0010           ; bounded extra wait for thousands of pixel operations
WaitAnotherGsuRound:
  ldx #$FFFF
WaitForGsu:
  lda SUPERFX_SFR
  and #$20             ; SFR.G: GSU running
  beq GsuStopped
  dex
  bne WaitForGsu
  dey
  bne WaitAnotherGsuRound
  stz SUPERFX_SFR       ; timeout: stop before reclaiming RAM
  stz SUPERFX_SCMR
  lda #$03
  sta.l TEST_STATUS
  bra ShowFailure

GsuStopped:
  lda SUPERFX_SFR+1     ; acknowledge STOP IRQ
  stz SUPERFX_SCMR      ; reclaim CPU RAM access before reading results
  ; Capture real CPU register reads; debugger Peek of GSU I/O can return zero.
  lda SUPERFX_PBR
  sta.l TEST_PBR
  lda $303C            ; RAMBR is readable but initialized by GSU RAMB
  sta.l TEST_RAMBR
  lda SUPERFX_SFR
  sta.l TEST_SFR
  rep #$20
  .ACCU 16
  lda $3014            ; R10 low/high
  sta.l TEST_SP
  lda.l GSU_RESULT
  sta.l TEST_RAM_RESULT
  lda SUPERFX_R0        ; triangle's word main() returns its pixel count in R0
  sta.l TEST_RESULT
  cmp #EXPECTED_RESULT
  sep #$20             ; SEP does not change comparison Z
  .ACCU 8
  bne WrongResult
  rep #$20
  .ACCU 16
  lda.l TEST_RAM_RESULT
  cmp #EXPECTED_RESULT
  sep #$20
  .ACCU 8
  bne WrongResult
  lda $300C            ; R6: compiler runtime address/arithmetic fault status
  ora $300D
  bne WrongResult
  jsr display_triangle ; CPU owns RAM; DMA only after STOP and the GSU flush
  lda #$01
  sta.l TEST_STATUS
  bra ShowScreen

WrongResult:
  lda #$02
  sta.l TEST_STATUS
ShowFailure:
  stz $2121
  lda #$1F             ; CGRAM color 0 = $001F (red)
  sta $2122
  lda #$00
  sta $2122
ShowScreen:
  lda #$0F
  sta $2100
Finished:
  bra Finished

copy_gsu_to_bwram:
  ; gsu_code_size = end - start, exactly the issue's label-size calculation.
  ; The long ROM operand works even if SUPERFREE chooses another ROM bank.
  ldx #$0000
CopyNextByte:
  lda.l gsu_triangle_start,x
  sta.l GSU_LOAD_ADDRESS,x
  inx
  cpx #(gsu_triangle_end-gsu_triangle_start)
  bne CopyNextByte
  rts

start_gsu:
  ; Writing R15 high starts execution; write low first and high last.
  ; Enter the bootstrap, not main directly: it initializes RAMBR and R10.
  lda #<GSU_START_PC
  sta SUPERFX_R15
  lda #>GSU_START_PC
  sta SUPERFX_R15+1
  rts

  .INCLUDE "triangle-host.inc"

InterruptReturn:
  rti
.ENDS

; Store the fixed-origin GSU payload in ROM, then copy it before execution.
.SECTION ".rodata_disco_triangle" SUPERFREE
gsu_triangle_start:
  .INCBIN "triangle.bin"
gsu_triangle_end:
.ENDS

.ASSERT gsu_triangle_end-gsu_triangle_start > 0, LDERROR, "Empty GSU payload"
.ASSERT GSU_LOAD_ADDRESS+(gsu_triangle_end-gsu_triangle_start) <= $710000, LDERROR, "GSU payload crosses the RAM program bank"
.ASSERT GSU_LOAD_ADDRESS+(gsu_triangle_end-gsu_triangle_start) <= GSU_RESULT, LDERROR, "Triangle payload overlaps the diagnostic word at $70:F000"
