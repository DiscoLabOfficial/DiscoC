; Framebuffer clearing and SNES display routines for host.asm.
; Calls enter and leave with A8, X/Y16, DBR=$00 and GSU stopped (CPU RAM access).

clear_triangle_framebuffer:
  rep #$20
  .ACCU 16
  lda #$0000
  ldx #$0000
ClearTriangleWord:
  sta.l $700000,x
  inx
  inx
  cpx #FRAMEBUFFER_SIZE
  bne ClearTriangleWord
  sta.l GSU_RESULT
  sep #$20
  .ACCU 8
  rts

display_triangle:
  ; The GSU's 4bpp bitmap already uses SNES planar tile bytes. Only its
  ; column-major tile order needs conversion via the PPU's row-major tilemap.
  lda #$80
  sta $2115            ; VRAM increments by one word after the high-byte write
  stz $2116
  stz $2117
  lda #$01
  sta $4300            ; DMA mode 1: alternate $2118/$2119
  lda #$18
  sta $4301
  stz $4302            ; source $70:0000
  stz $4303
  lda #$70
  sta $4304
  stz $4305
  lda #>FRAMEBUFFER_SIZE
  sta $4306            ; copy exactly $6000 bytes of framebuffer
  lda #$01
  sta $420B

  ; Clear tile 768 (VRAM byte $6000), used below the 192-line bitmap.
  rep #$20
  .ACCU 16
  lda #$3000
  sta $2116
  lda #$0000
  ldx #$0010
ClearBlankTile:
  sta $2118
  dex
  bne ClearBlankTile

  lda #TILEMAP_WORD_ADDRESS
  sta $2116
  ldy #$0000           ; row 0..23; each GSU tile column contains 24 tiles
TriangleTilemapRow:
  tya                  ; tile index starts at row, then increases by 24
  ldx #$0020
TriangleTilemapColumn:
  sta $2118
  clc
  adc #24
  dex
  bne TriangleTilemapColumn
  iny
  cpy #24
  bne TriangleTilemapRow
  lda #BLANK_TILE
  ldx #256             ; remaining 8 tilemap rows (lines 192..255)
TriangleTilemapBlank:
  sta $2118
  dex
  bne TriangleTilemapBlank
  sep #$20
  .ACCU 8

  stz $2121
  ldx #$0000
TrianglePaletteByte:
  lda.l TrianglePalette,x
  sta $2122
  inx
  cpx #32
  bne TrianglePaletteByte

  lda #$01
  sta $2105            ; PPU mode 1, BG1 is 4bpp, 8x8 tiles
  lda #$38
  sta $2107            ; BG1 32x32 tilemap, VRAM word address $3800
  stz $210B            ; BG1 tile data at VRAM word address $0000
  stz $210D
  stz $210D            ; horizontal scroll zero (two writes)
  lda #$FF
  sta $210E
  lda #$03
  sta $210E            ; vertical scroll -1: SNES visible scanline 1 shows y=0
  lda #$01
  sta $212C            ; enable BG1 only on the main screen
  rts

TrianglePalette:
  ; BGR555: dark backdrop, then bright scanline bands (color indices 1..13).
  .DW $0842, $001F, $011F, $021F, $031F, $03FF, $03E0, $13E0, $33E0
  .DW $7FE0, $7F00, $7C00, $7C10, $7C1F, $7E1F, $7FFF
