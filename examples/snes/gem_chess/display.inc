; Two 24-KiB character buffers ($0000/$6000), one shared tilemap at $E000.
; Transfer only the inactive buffer, 4 KiB per fresh VBlank. Switch BG1's
; character base after all six chunks: the visible scene is never torn.
.ACCU 8
.INDEX 16
initialize_display:
  lda #$80
  sta $2115
  rep #$20
  .ACCU 16
  lda #$7000
  sta $2116
  ldy #0
MapRow:
  tya
  ldx #32
MapColumn:
  sta $2118
  clc
  adc #24
  dex
  bne MapColumn
  iny
  cpy #24
  bne MapRow
  ; Tile 0 is outside every projected geometry envelope and remains clear in
  ; both buffers. It also supplies blank rows 192..255 without a third tile.
  lda #0
  ldx #256
MapBlank:
  sta $2118
  dex
  bne MapBlank
  sep #$20
  .ACCU 8
  stz $2121
  ldx #0
PaletteByte:
  lda.l ScenePalette,x
  sta $2122
  inx
  cpx #32
  bne PaletteByte
  lda #1
  sta $2105
  lda #$70
  sta $2107
  stz $210B
  stz $210D
  stz $210D
  lda #$FF
  sta $210E
  lda #$03
  sta $210E
  lda #1
  sta $4300
  lda #$18
  sta $4301
  lda #$70
  sta $4304
  rts

upload_scene:
  rep #$20
  .ACCU 16
  stz DMA_SOURCE
  stz DMA_CHUNKS
  lda ACTIVE_BUFFER
  eor #1
  beq BufferZero
  lda #$3000
BufferZero:
  sta DMA_DESTINATION
  sep #$20
  .ACCU 8
UploadChunk:
  jsr wait_next_vblank
  rep #$20
  .ACCU 16
  lda DMA_DESTINATION
  sta $2116
  lda DMA_SOURCE
  sta $4302
  lda #$1000
  sta $4305
  sep #$20
  .ACCU 8
  lda #1
  sta $420B
  lda $4212
  bpl UploadOverrun
  lda $213F
  lda $2137
  lda $213D
  sta.l LAST_DMA_LINE
  lda $213D
  and #1
  sta.l LAST_DMA_LINE+1
  rep #$20
  .ACCU 16
  inc DMA_CHUNKS
  lda DMA_SOURCE
  clc
  adc #$1000
  sta DMA_SOURCE
  lda DMA_DESTINATION
  clc
  adc #$0800
  sta DMA_DESTINATION
  lda DMA_SOURCE
  cmp #$6000
  sep #$20
  .ACCU 8
  bne UploadChunk
  rep #$20
  .ACCU 16
  lda ACTIVE_BUFFER
  eor #1
  sta ACTIVE_BUFFER
  sep #$20
  .ACCU 8
  beq ShowZero
  lda #3
ShowZero:
  sta $210B
  lda #1
  sta $212C
  rts
UploadOverrun:
  lda #2
  jmp Failure

ScenePalette:
  ; Midnight backdrop, slate/ivory board, gold bevel, cyan/ruby gemstone ramps.
  .DW $0C41,$2CE5,$5692,$1616
  .DW $20A0,$4580,$6282,$7B88,$7FF8
  .DW $1027,$206F,$34B8,$499F,$66FF,$7FFF,$0841
