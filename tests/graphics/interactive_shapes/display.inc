; Real PPU OBJ presentation, not a bitmap BG pretending to be sprites.
.ACCU 8
.INDEX 16
initialize_display:
  lda #$80
  sta $2115
  stz $2116
  stz $2117
  lda #1
  sta $4300
  lda #$18
  sta $4301
  stz $4302
  stz $4303
  lda #$70
  sta $4304
  stz $4305
  lda #$80
  sta $4306
  lda #1
  sta $420B           ; full 32-KiB initial framebuffer, under forced blank
  stz $2121
  lda #$42
  sta $2122
  lda #$08
  sta $2122
  lda #$80
  sta $2121
  ldx #0
PaletteByte:
  lda.l ObjectPalette,x
  sta $2122
  inx
  cpx #32
  bne PaletteByte
  stz $2102
  stz $2103
  ; Two large sprites at the same screen positions as their GSU panels.
  ldx #0
VisibleOam:
  lda.l VisibleObjects,x
  sta $2104
  inx
  cpx #8
  bne VisibleOam
  ldx #126
HiddenOam:
  stz $2104
  lda #$E0
  sta $2104
  stz $2104
  stz $2104
  dex
  bne HiddenOam
  lda #$5A             ; first two: large/X-high=0, others: small/X-high=1
  sta $2104
  lda #$55
  ldx #31
OamHigh:
  sta $2104
  dex
  bne OamHigh
  lda #$40             ; 8/64-pixel sprites, base 0, second table byte $2000
  sta $2101
  stz $2105
  lda #$10
  sta $212C
  rts

upload_panels:
  rep #$20
  .ACCU 16
  ldx #$1080
  ldy #$0840
  lda #$2080
  sta $0016
  sep #$20
  .ACCU 8
  jsr upload_panel
  rep #$20
  .ACCU 16
  ldx #$3080
  ldy #$1840
  lda #$4080
  sta $0016
  sep #$20
  .ACCU 8
  jsr upload_panel
  lda $4212
  bpl UploadOverrun
  lda $213F
  lda $2137
  lda $213D
  sta.l LAST_DMA_LINE
  lda $213D
  and #1
  sta.l LAST_DMA_LINE+1
  rts
UploadOverrun:
  lda #2
  jmp Failure
upload_panel:
PanelRow:
  rep #$20
  .ACCU 16
  sty $2116
  stx $4302
  lda #$0100
  sta $4305
  sep #$20
  .ACCU 8
  lda #1
  sta $420B
  rep #$20
  .ACCU 16
  txa
  clc
  adc #$0200
  tax
  tya
  clc
  adc #$0100
  tay
  sep #$20
  .ACCU 8
  cpx $0016
  bne PanelRow
  rts
VisibleObjects:
  .DB 32,64,$84,$30, 160,64,$84,$31
ObjectPalette:
  .DW $0000,$001F,$7C00,$03E0,$03FF,$7C1F,$7FE0,$02FF
  .DW $7FFF,$7C1F,$0000,$0000,$0000,$0000,$0000,$0000
