PPU_CTRL   = $2000
PPU_MASK   = $2001
PPU_STATUS = $2002
PPU_SCROLL = $2005
PPU_ADDR   = $2006
PPU_DATA   = $2007
OAM_ADDR   = $2003
OAM_DMA    = $4014
JOY1       = $4016
MBOX_PPU   = $1FF8
MBOX_BGM   = $1FF9
BGM_HOLD   = $80

BGM_TRACKS = 4
NTRIG      = 5

zVbl      = $10
zPad      = $11
zPX       = $12
zPY       = $13
zTrgOk    = $14
zRow      = $15
zPadP     = $16
zBgm      = $17
zBgmTrk   = $18
zHold     = $1A
zTmp      = $1B
zSrc      = $1C         ; and $1D

TILE_FLOOR = $01
TILE_WALL  = $02
TILE_TRIG  = $03
TILE_DIGIT = $04
PX_MIN = 8
PX_MAX = 240
PY_MIN = 8
PY_MAX = 224
TRIG_Y = 64

GAME_CTRL = $88
GAME_MASK = $1E

.segment "CODE"

reset:
    sei
    cld
    ldx #$FF
    txs
    inx
    stx PPU_CTRL
    stx PPU_MASK
    stx $4015
    stx $4010
    lda #$40
    sta $4017

    bit PPU_STATUS
:   bit PPU_STATUS
    bpl :-
:   bit PPU_STATUS
    bpl :-

    ; CHR RAM comes up undefined, so the tiles ship in PRG and go up here.
    ; PPU_CTRL/PPU_MASK are still 0 from reset: NMI off, rendering off, VRAM
    ; increment +1 -- all three load-bearing.
    bit PPU_STATUS
    lda #$00
    sta PPU_ADDR
    sta PPU_ADDR
    lda #<tiles
    sta zSrc
    lda #>tiles
    sta zSrc+1
    ldx #32                 ; 8192 bytes as 32 pages
    ldy #$00
@chr:
    lda (zSrc),y
    sta PPU_DATA
    iny
    bne @chr
    inc zSrc+1
    dex
    bne @chr

    lda #$00
    jsr set_mbox
    ldx #<MBOX_BGM
    jsr set_mbox_x
    lda #$00
    sta zVbl
    sta zPadP
    sta zBgm
    sta zHold
    lda #$01
    sta zTrgOk
    sta zBgmTrk

    lda #128
    sta zPX
    lda #120
    sta zPY

    jsr oam_clear
    jsr update_oam
    jsr draw_room
    jsr show_game

game:
:   lda zVbl
    beq :-
    lda #$00
    sta zVbl

    jsr read_pad
    jsr bgm_input
    jsr move_player
    jsr update_oam
    jsr check_trigger
    beq game

    tay
    dey
    lda trig_music,y
    sta zBgm
    beq @go
    sta zBgmTrk
@go:
    jmp game

set_mbox:
    ldx #<MBOX_PPU
set_mbox_x:
    pha
    bit PPU_STATUS
    lda #>MBOX_PPU
    sta PPU_ADDR
    stx PPU_ADDR
    pla
    sta PPU_DATA
    rts

bgm_input:
    lda zPadP
    eor #$FF
    and zPad
    tax
    lda zPad
    sta zPadP
    txa
    and #$80
    beq @b
    lda zBgm
    bne @off
    lda zBgmTrk
    bne @set
@off:
    lda #$00
@set:
    sta zBgm
@b:
    txa
    and #$40
    beq @sel
    ldy zBgmTrk
    iny
    cpy #BGM_TRACKS + 1
    bcc @keep
    ldy #$01
@keep:
    sty zBgmTrk
    lda zBgm
    beq @sel
    sty zBgm
@sel:
    txa
    and #$20
    beq @out
    lda zHold
    eor #BGM_HOLD
    sta zHold
@out:
    rts

show_game:
    bit PPU_STATUS
:   bit PPU_STATUS
    bpl :-
    lda #$00
    sta PPU_SCROLL
    sta PPU_SCROLL
    lda #GAME_CTRL
    sta PPU_CTRL
    lda #GAME_MASK
    sta PPU_MASK
    rts

read_pad:
    lda #$01
    sta JOY1
    sta zPad
    lsr a
    sta JOY1
:   lda JOY1
    lsr a
    rol zPad
    bcc :-
    rts

move_player:
    lda zPad
    and #$08
    beq @down
    lda zPY
    cmp #PY_MIN + 2
    bcc @down
    dec zPY
    dec zPY
@down:
    lda zPad
    and #$04
    beq @left
    lda zPY
    cmp #PY_MAX - 1
    bcs @left
    inc zPY
    inc zPY
@left:
    lda zPad
    and #$02
    beq @right
    lda zPX
    cmp #PX_MIN + 2
    bcc @right
    dec zPX
    dec zPX
@right:
    lda zPad
    and #$01
    beq @out
    lda zPX
    cmp #PX_MAX - 1
    bcs @out
    inc zPX
    inc zPX
@out:
    rts

check_trigger:
    ldy #$00
@next:
    lda zPX
    sec
    sbc trig_x,y
    clc
    adc #$07
    cmp #$0F
    bcs @miss
    lda zPY
    sec
    sbc #TRIG_Y
    clc
    adc #$07
    cmp #$0F
    bcs @miss
    lda zTrgOk
    beq @held
    iny
    tya
    rts
@miss:
    iny
    cpy #NTRIG
    bne @next
    lda #$01
    sta zTrgOk
@held:
    lda #$00
    rts

trig_col:   .byte 4, 10, 16, 22, 28
trig_x:     .byte 32, 80, 128, 176, 224
trig_music: .byte 1, 2, 3, 4, 0

trig_idx:
    stx zTmp
    lda #31
    sec
    sbc zTmp
    ldy #NTRIG - 1
@next:
    cmp trig_col,y
    beq @hit
    dey
    bpl @next
    lda #$FF
    rts
@hit:
    tya
    rts

oam_clear:
    ldx #$00
    lda #$FF
:   sta $0200,x
    inx
    bne :-
    rts

update_oam:
    lda zPY
    sec
    sbc #$01
    sta $0200
    lda #$00
    sta $0201
    ldx #$00
    lda zBgm
    beq :+
    ldx #$01
    lda zHold
    beq :+
    ldx #$02
:   stx $0202
    lda zPX
    sta $0203
    rts

draw_room:
    bit PPU_STATUS
    lda #$20
    sta PPU_ADDR
    lda #$00
    sta PPU_ADDR
    ldx #32
    lda #TILE_WALL
:   sta PPU_DATA
    dex
    bne :-
    lda #$01
    sta zRow
@row:
    lda #TILE_WALL
    sta PPU_DATA
    ldx #30
@col:
    lda zRow
    cmp #$07
    beq @digit
    cmp #$08
    bne @floor
    jsr trig_idx
    bmi @floor
    lda #TILE_TRIG
    bne @put
@digit:
    jsr trig_idx
    bmi @floor
    tay
    lda trig_music,y
    clc
    adc #TILE_DIGIT
    bne @put
@floor:
    lda #TILE_FLOOR
@put:
    sta PPU_DATA
    dex
    bne @col
    lda #TILE_WALL
    sta PPU_DATA
    inc zRow
    lda zRow
    cmp #29
    bne @row
    ldx #32
    lda #TILE_WALL
:   sta PPU_DATA
    dex
    bne :-
    ldx #64
    lda #$00
:   sta PPU_DATA
    dex
    bne :-
    lda #$3F
    sta PPU_ADDR
    lda #$00
    sta PPU_ADDR
    ldx #$00
:   lda game_pal,x
    sta PPU_DATA
    inx
    cpx #32
    bne :-
    rts

game_pal:
    .byte $0F,$0B,$17,$30,  $0F,$0F,$0F,$0F
    .byte $0F,$0F,$0F,$0F,  $0F,$0F,$0F,$0F
    .byte $0F,$16,$27,$30,  $0F,$1A,$2A,$30
    .byte $0F,$11,$21,$30,  $0F,$0F,$0F,$0F

; ---------------------------------------------------------------------------
.segment "CODE2"

nmi:
    pha
    txa
    pha
    tya
    pha
    lda #$00
    sta OAM_ADDR
    lda #$02
    sta OAM_DMA
    ; Rewritten every frame, not on change: a host CHR poll swaps the whole
    ; MemCtrl, so a one-shot write can be dropped outright rather than torn.
    bit PPU_STATUS
    lda #>MBOX_BGM
    sta PPU_ADDR
    lda #<MBOX_BGM
    sta PPU_ADDR
    lda zBgm
    beq @put
    ora zHold
@put:
    sta PPU_DATA
    lda #$00
    sta PPU_SCROLL
    sta PPU_SCROLL
    lda #GAME_CTRL
    sta PPU_CTRL
    inc zVbl
    pla
    tay
    pla
    tax
    pla
    rti

irq:
    rti

; ---------------------------------------------------------------------------
.segment "TILES"

tiles:
    .incbin "tiles.chr"

; ---------------------------------------------------------------------------
.segment "VECTORS"
    .word nmi
    .word reset
    .word irq
