; Freestanding original host. No Nintendo SDK, BIOS, game code or game assets.
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
  NAME "DISCOC GEM CHESS"
  LOROM
  SLOWROM
  CARTRIDGETYPE $14
  ROMSIZE $06
  SRAMSIZE $06
  COUNTRY $01
  LICENSEECODE $00
  VERSION $00
.ENDSNES
.SNESNATIVEVECTOR
  COP InterruptReturn
  BRK InterruptReturn
  ABORT InterruptReturn
  NMI ReadController
  IRQ InterruptReturn
.ENDNATIVEVECTOR
.SNESEMUVECTOR
  COP InterruptReturn
  ABORT InterruptReturn
  NMI InterruptReturn
  RESET Reset
  IRQBRK InterruptReturn
.ENDEMUVECTOR

.INCLUDE "scene-config.inc"
.DEFINE TEST_STATUS $7E0000
.DEFINE TEST_RESULT $7E0002
.DEFINE TEST_SP $7E0004
.DEFINE TEST_PBR $7E0006
.DEFINE TEST_RAMBR $7E0007
.DEFINE TEST_SFR $7E0008
.DEFINE TEST_RAM_RESULT $7E000A
.DEFINE TEST_YAW $7E000C
.DEFINE TEST_PITCH $7E000E
.DEFINE TEST_ZOOM $7E0010
.DEFINE TEST_MODE $7E0012
.DEFINE TEST_FRAME $7E0014
.DEFINE DMA_SOURCE $0016
.DEFINE DMA_DESTINATION $0018
.DEFINE LAST_DMA_LINE $7E001A
.DEFINE TEST_CBR $7E001C
.DEFINE INPUT_VERSION $0020
.DEFINE INPUT_HELD $0022
.DEFINE INPUT_YAW $0024
.DEFINE INPUT_PITCH $0026
.DEFINE INPUT_ZOOM $0028
.DEFINE INPUT_MODE $002A
.DEFINE INPUT_PREVIOUS $002C
.DEFINE INPUT_POLLS $002E
.DEFINE ACTIVE_BUFFER $0030
.DEFINE DMA_CHUNKS $0032
.DEFINE GSU_RESULT $70F000
.DEFINE FRAMEBUFFER_SIZE $6000

.BANK 0 SLOT 0
.ORG 0
.SECTION "Gem chess SNES host" FORCE
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
  lda #0
  tcd
  sep #$20
  .ACCU 8
  pha
  plb
  stz $4200
  stz $420B
  stz $420C
  lda #$80
  sta $4201
  sta $2100
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
  stz $3030
  stz $3031
  stz $303A
  rep #$20
  .ACCU 16
  lda #0
  ldx #0
ClearWorkRam:
  sta $0000,x
  inx
  inx
  cpx #$0040
  bne ClearWorkRam
  ldx #0
ClearFramebuffer:
  sta.l $700000,x
  inx
  inx
  cpx #FRAMEBUFFER_SIZE
  bne ClearFramebuffer
  lda #8
  sta INPUT_YAW
  lda #7
  sta INPUT_PITCH
  lda #12
  sta INPUT_ZOOM
  sep #$20
  .ACCU 8
  jsr copy_payload
  jsr initialize_display
  lda $4210
  lda #$80
  sta $4200
  lda #$0F
  sta $2100

RenderNextFrame:
  lda #0
  sta.l TEST_STATUS
  rep #$20
  .ACCU 16
InputSnapshot:
  lda INPUT_VERSION
  and #1
  bne InputSnapshot
  ldx INPUT_VERSION
  lda INPUT_YAW
  sta.l TEST_YAW
  lda INPUT_PITCH
  sta.l TEST_PITCH
  lda INPUT_ZOOM
  sta.l TEST_ZOOM
  lda INPUT_MODE
  sta.l TEST_MODE
  txa
  cmp INPUT_VERSION
  bne InputSnapshot
  lda.l TEST_YAW
  sta.l $70F002
  lda.l TEST_PITCH
  sta.l $70F004
  lda.l TEST_ZOOM
  sta.l $70F006
  lda.l TEST_MODE
  sta.l $70F008
  sep #$20
  .ACCU 8
  lda #BITMAP_SCBR
  sta $3038
  lda #$70
  sta $3034
  lda #$80
  sta $3037
  lda #1
  sta $3039            ; ~21.477 MHz NTSC, normal multiply timing
  lda #(BITMAP_SCMR | $08)
  sta $303A            ; GSU RAM / SNES CPU ROM access
  rep #$20
  .ACCU 16
  lda #GSU_START_PC
  sta $301E
  sep #$20
  .ACCU 8
  ldy #$0100
WaitRound:
  ldx #$FFFF
WaitGsu:
  lda $3030
  and #$20
  beq GsuStopped
  dex
  bne WaitGsu
  dey
  bne WaitRound
  stz $3030
  stz $303A
  lda #3
  jmp Failure
GsuStopped:
  lda $3031
  stz $303A
  lda $3034
  sta.l TEST_PBR
  lda $303C
  sta.l TEST_RAMBR
  lda $3030
  sta.l TEST_SFR
  rep #$20
  .ACCU 16
  lda $3014
  sta.l TEST_SP
  lda $303E
  sta.l TEST_CBR
  lda.l GSU_RESULT
  sta.l TEST_RAM_RESULT
  lda $3000
  sta.l TEST_RESULT
  bra CheckResult
WrongResult:
  sep #$20
  .ACCU 8
  lda #2
  jmp Failure
CheckResult:
  .ACCU 16
  .IFDEF FORCE_BAD_RESULT
  inc a
  .ENDIF
  cmp.l TEST_RAM_RESULT
  bne WrongResult
  cmp #0
  beq WrongResult
  lda $300C
  bne WrongResult
  lda $3014
  cmp #$EFFA
  bne WrongResult
  lda.l $70F010
  cmp.l TEST_YAW
  bne WrongResult
  lda.l $70F012
  cmp.l TEST_PITCH
  bne WrongResult
  lda.l $70F014
  cmp.l TEST_ZOOM
  bne WrongResult
  lda.l $70F016
  cmp.l TEST_MODE
  bne WrongResult
  sep #$20
  .ACCU 8
  jsr upload_scene
  rep #$20
  .ACCU 16
  lda.l TEST_FRAME
  inc a
  sta.l TEST_FRAME
  sep #$20
  .ACCU 8
  lda #1
  sta.l TEST_STATUS
WaitForChangedInput:
  rep #$20
  .ACCU 16
  lda INPUT_YAW
  cmp.l TEST_YAW
  bne InputChanged
  lda INPUT_PITCH
  cmp.l TEST_PITCH
  bne InputChanged
  lda INPUT_ZOOM
  cmp.l TEST_ZOOM
  bne InputChanged
  lda INPUT_MODE
  cmp.l TEST_MODE
  bne InputChanged
  sep #$20
  .ACCU 8
  wai
  bra WaitForChangedInput
InputChanged:
  sep #$20
  .ACCU 8
  jmp RenderNextFrame
Failure:
  sta.l TEST_STATUS
  stz $212C
  stz $2121
  lda #$1F
  sta $2122
  lda #0
  sta $2122
  lda #$0F
  sta $2100
Finished:
  bra Finished

copy_payload:
  ldx #0
CopyByte:
  lda.l payload_start,x
  sta.l GSU_LOAD_ADDRESS,x
  inx
  cpx #(payload_end-payload_start)
  bne CopyByte
  rts

wait_next_vblank:
WaitVisible:
  lda $4212
  bmi WaitVisible
WaitBlank:
  lda $4212
  bpl WaitBlank
  rts

ReadController:
  php
  rep #$30
  .ACCU 16
  .INDEX 16
  pha
  phx
  phy
  phb
  sep #$20
  .ACCU 8
  lda #0
  pha
  plb
  lda $4210
  lda #1
  sta $4016
  stz $4016
  rep #$20
  .ACCU 16
  inc INPUT_VERSION
  stz INPUT_HELD
  ldx #16
ReadButton:
  lda $4016
  and #1
  lsr a
  rol INPUT_HELD
  dex
  bne ReadButton
  lda INPUT_HELD
  and #$1000
  beq NoReset
  lda #8
  sta INPUT_YAW
  lda #7
  sta INPUT_PITCH
  lda #12
  sta INPUT_ZOOM
  stz INPUT_MODE
  jmp InputDone
NoReset:
  lda INPUT_PREVIOUS
  eor INPUT_HELD
  and INPUT_HELD
  and #$0080
  beq NoToggle
  lda INPUT_MODE
  eor #1
  sta INPUT_MODE
NoToggle:
  lda INPUT_HELD
  and #$0300
  cmp #$0100
  bne NotRight
  lda INPUT_YAW
  inc a
  bra StoreYaw
NotRight:
  cmp #$0200
  bne Vertical
  lda INPUT_YAW
  dec a
StoreYaw:
  and #63
  sta INPUT_YAW
Vertical:
  lda INPUT_HELD
  and #$2000
  beq TiltCamera
  lda INPUT_HELD
  and #$0C00
  cmp #$0800
  bne NotGrow
  lda INPUT_ZOOM
  cmp #16
  beq InputDone
  inc a
  sta INPUT_ZOOM
  bra InputDone
NotGrow:
  cmp #$0400
  bne InputDone
  lda INPUT_ZOOM
  cmp #6
  beq InputDone
  dec a
  sta INPUT_ZOOM
  bra InputDone
TiltCamera:
  lda INPUT_HELD
  and #$0C00
  cmp #$0800
  bne NotUp
  lda INPUT_PITCH
  cmp #10
  beq InputDone
  inc a
  sta INPUT_PITCH
  bra InputDone
NotUp:
  cmp #$0400
  bne InputDone
  lda INPUT_PITCH
  cmp #5
  beq InputDone
  dec a
  sta INPUT_PITCH
InputDone:
  lda INPUT_HELD
  sta INPUT_PREVIOUS
  inc INPUT_POLLS
  inc INPUT_VERSION
  plb
  ply
  plx
  pla
  plp
  rti

.INCLUDE "display.inc"
InterruptReturn:
  rti
.ENDS
.SECTION ".rodata_gem_chess_payload" SUPERFREE
payload_start:
  .INCBIN "gem-chess.bin"
payload_end:
.ENDS
.ASSERT payload_end-payload_start > 0, LDERROR, "Empty payload"
.ASSERT payload_end-payload_start <= $8000, LDERROR, "Payload must fit its LoROM source bank"
