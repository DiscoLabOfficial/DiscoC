; Calls enter/leave in A8, X/Y16, DBR=$00, with the GSU stopped.
initialize_display:
  lda #$80
  sta $2115
  stz $2116
  stz $2117
  lda #$01
  sta $4300
  lda #$18
  sta $4301
  stz $4302
  stz $4303
  lda #$70
  sta $4304
  stz $4305
  lda #>FRAMEBUFFER_SIZE
  sta $4306
  lda #$01
  sta $420B            ; initial full upload is allowed under forced blank

  rep #$20
  .ACCU 16
  lda #$1800           ; blank 2bpp tile 768 at VRAM byte $3000
  sta $2116
  lda #$0000
  ldx #8
ClearBlankTile:
  sta $2118
  dex
  bne ClearBlankTile
  lda #$3800
  sta $2116
  ldy #0
TilemapRow:
  tya
  ldx #32
TilemapColumn:
  sta $2118
  clc
  adc #24
  dex
  bne TilemapColumn
  iny
  cpy #24
  bne TilemapRow
  lda #768
  ldx #256
TilemapBlank:
  sta $2118
  dex
  bne TilemapBlank
  sep #$20
  .ACCU 8
  stz $2121
  ldx #0
PaletteByte:
  lda.l CheckerPalette,x
  sta $2122
  inx
  cpx #8
  bne PaletteByte
  stz $2105            ; PPU mode 0: BG1 is 2bpp
  lda #$38
  sta $2107
  stz $210B
  stz $210D
  stz $210D
  lda #$FF
  sta $210E
  lda #$03
  sta $210E            ; scroll -1: first visible scanline corresponds to y=0
  lda #$01
  sta $212C
  rts

upload_dirty_tiles:
  ; Only tile columns 8..23 and rows 4..19 can change. Sixteen 256-byte
  ; transfers total 4096 bytes, keeping the upload within NTSC VBlank.
  rep #$20
  .ACCU 16
  lda #$0C40           ; source: column 8 * 24 * 16 + row 4 * 16
  sta.l DMA_SOURCE
  lda #$0620           ; VRAM word address = source byte address / 2
  sta.l DMA_DESTINATION
  sep #$20
  .ACCU 8
  ldx #16
DirtyTileColumn:
  rep #$20
  .ACCU 16
  lda.l DMA_DESTINATION
  sta $2116
  lda.l DMA_SOURCE
  sta $4302
  lda #$0100
  sta $4305
  sep #$20
  .ACCU 8
  lda #$01
  sta $420B
  rep #$20
  .ACCU 16
  lda.l DMA_SOURCE
  clc
  adc #$0180           ; 24 tiles per column, 16 bytes per tile
  sta.l DMA_SOURCE
  lda.l DMA_DESTINATION
  clc
  adc #$00C0
  sta.l DMA_DESTINATION
  sep #$20
  .ACCU 8
  dex
  bne DirtyTileColumn

  ; A runtime guard and test-visible snapshot catch an upload overrunning
  ; VBlank instead of silently accepting a torn or partially updated frame.
  lda $4212
  bpl UploadOverrun
  lda $213F            ; reset H/V counter read toggles
  lda $2137            ; reading SLHV latches the PPU beam counters
  lda $213D
  sta.l LAST_DMA_LINE
  lda $213D
  and #1
  sta.l LAST_DMA_LINE+1
  rts
UploadOverrun:
  lda #$02
  jmp Failure

CheckerPalette:
  ; A distinguishable dark gray backdrop; black and white checks, red error.
  .DW $0842, $0000, $7FFF, $001F
