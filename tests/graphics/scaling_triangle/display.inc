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
  lda #$3000           ; blank 4bpp tile 768 at VRAM byte $6000
  sta $2116
  lda #$0000
  ldx #16
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
  lda.l RainbowPalette,x
  sta $2122
  inx
  cpx #32
  bne PaletteByte
  lda #$01
  sta $2105            ; PPU mode 1: BG1 is 4bpp
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
  ; Only tile columns 9..23 and rows 9..16 can change. Fifteen 256-byte
  ; transfers total 3840 bytes, keeping the upload within NTSC VBlank.
  rep #$20
  .ACCU 16
  lda #$1C20           ; source: column 9 * 24 * 32 + row 9 * 32
  sta.l DMA_SOURCE
  lda #$0E10           ; VRAM word address = source byte address / 2
  sta.l DMA_DESTINATION
  sep #$20
  .ACCU 8
  ldx #15
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
  adc #$0300           ; 24 tiles per column, 32 bytes per tile
  sta.l DMA_SOURCE
  lda.l DMA_DESTINATION
  clc
  adc #$0180
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

RainbowPalette:
  ; BGR555: gray backdrop, red/orange/yellow/green/cyan/blue/violet/pink.
  .DW $0842, $001F, $021F, $03FF, $03E0, $7FE0, $7C00, $7C10, $7C1F
  .DW $0000, $0000, $0000, $0000, $0000, $0000, $0000
