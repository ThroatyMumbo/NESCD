; cdplayer.s - an audio CD player. The host on the expansion port does the
; playing; this ROM shows its state and sends it the buttons, through the
; mailbox page in sprite tile $FF (firmware/src/player.h has the byte map).
;
; NROM-256 with 8 KiB CHR RAM, like demo.s. gen_gfx.py draws the screen; the
; font is at tile == ASCII. Left/Right pick an on-screen button, A presses it.
; The host's spectrum bands are sprite bars, which cost no nametable writes.

.include "gfx.inc"

PPU_CTRL   = $2000
PPU_MASK   = $2001
PPU_STATUS = $2002
PPU_SCROLL = $2005
PPU_ADDR   = $2006
PPU_DATA   = $2007
JOY1       = $4016

MBOX_VIZ   = $1FE8          ; the host's band levels, 0..255
MBOX_PAGE  = $1FF0          ; 16 bytes: ours and the host's
MBOX_REQ   = $1FF8
MBOX_PARAM = $1FF9
OAM_DMA    = $4014

; Request commands (low nibble), the sequence in the high nibble.
CMD_PLAY   = 1
CMD_PAUSE  = 2
CMD_STOP   = 3
CMD_NEXT   = 4
CMD_PREV   = 5
CMD_EJECT  = 6
CMD_LOAD   = 7
CMD_SEEK   = 8
ANS_FAIL   = $80
SEEK_SEC   = 10

; Host state byte.
ST_NO_DISC = 0
ST_TRAY    = 1
ST_LOADING = 2
ST_STOPPED = 3
ST_PLAYING = 4
ST_PAUSED  = 5
ST_NOAUDIO = 6

; Offsets into the page.
PG_LMIN    = 4
PG_LSEC    = 5
PG_ANS     = 10
PG_STATE   = 11
PG_TRACK   = 12
PG_NTRACK  = 13
PG_MIN     = 14
PG_SEC     = 15

GIVE_UP    = 600            ; fields an unanswered request waits
FLASH      = 45             ; fields the refused colour shows
REP_FIRST  = 24             ; fields A is held before a seek repeats
REP_NEXT   = 6
QMAX       = 48             ; queue bytes one vblank flushes, beside the OAM DMA
VIZ_DECAY  = 6              ; level units a field: a full bar falls in 0.7 s
PEAK_HOLD  = 30             ; fields a peak stays put
PEAK_FALL  = $03            ; then it drops a segment every fourth field

PAD_A      = $80
PAD_LEFT   = $02
PAD_RIGHT  = $01

GAME_CTRL  = $88            ; NMI on, sprites at $1000 (tile $FF = the mailbox)
GAME_MASK  = $1A            ; background and sprites

; Zero page.
zVbl     = $10
zPad     = $11
zPadP    = $12
zNew     = $13
zReq     = $14              ; what $1FF8 carries, kept after the answer
zParam   = $15              ; $1FF9
zSeq     = $16              ; 1..7 in the high nibble
zBusy    = $17              ; a request is outstanding
zWaitLo  = $18
zWaitHi  = $19
zFlash   = $1A
zQLock   = $1B              ; the queue is being appended to
zQ       = $1C              ; queue write index
zTmp     = $1D
zTmp2    = $1E
zIdx     = $1F
zPtr     = $20              ; and $21
zAddr    = $22              ; and $23: nametable address, hi first
zNum     = $24              ; 4: bytes for queue_bytes
zD       = $28              ; and $29: tens, ones
zSel     = $2A              ; the focused button
zRep     = $2B              ; A is held on a seek button
zRepT    = $2C
zFrame   = $2D
zHave    = $2F              ; the host has a disc to show
zPage    = $30              ; 16: this frame's read of $1FF0..$1FFF
zPrev    = $40              ; 16: last frame's
zSt      = $50              ; 16: the last two frames agreed on
zShState = $60              ; what is on screen, $FF = redraw
zShNum   = $61              ; 6: one per numeric field
zShIcon  = $67
zShMark  = $68
zShBar   = $69
zShRed   = $6A
NSHOWN   = 11
zW       = $70              ; 16-bit scratch for the bar
zT       = $72
zE       = $74              ; elapsed seconds
zL       = $76              ; track seconds
zN       = $78              ; 24-bit dividend, quotient in the low byte
zR       = $7B              ; 16-bit remainder
zVal     = $7D
zPx      = $7E
zViz     = $80              ; VIZ_BANDS each: this frame's read of the bands
zLvl     = zViz+VIZ_BANDS   ; decayed band levels
zPeak    = zLvl+VIZ_BANDS   ; in segments 1..12, 0 = none
zPeakT   = zPeak+VIZ_BANDS  ; hold countdown
zSeg     = zPeakT+VIZ_BANDS
zK       = zSeg+1
zBot     = zK+1

OAM      = $0200
QUEUE    = $0300            ; [len, hi, lo, bytes...]*, len 0 ends it

BTN_REW  = 1
BTN_FF   = 4
BTN_EJECT = 6

; An unnamed label, so render's @locals stay in one scope.
.macro NEED n
    lda #n
    jsr room
    bcc :+
    jmp render_end
:
.endmacro

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

    ; CHR RAM comes up undefined: the background table ships in PRG.
    ; PPU_CTRL/PPU_MASK are still 0: NMI off, rendering off, increment +1.
    lda #<tiles
    sta zPtr
    lda #>tiles
    sta zPtr+1
    lda #$00
    ldx #16
    jsr upload
    lda #<screen
    sta zPtr
    lda #>screen
    sta zPtr+1
    lda #$20
    ldx #4
    jsr upload
    lda #<sprites
    sta zPtr
    lda #>sprites
    sta zPtr+1
    lda #$10
    ldx #1
    jsr upload

    bit PPU_STATUS
    lda #$3F
    sta PPU_ADDR
    lda #$00
    sta PPU_ADDR
    tax
:   lda palette,x
    sta PPU_DATA
    inx
    cpx #$20
    bne :-

    ; The page and the bands start clear, including the host's half.
    lda #>MBOX_VIZ
    sta PPU_ADDR
    lda #<(MBOX_VIZ & $FFF0)
    sta PPU_ADDR
    lda #$00
    ldx #32
:   sta PPU_DATA
    dex
    bne :-

    ldx #$00
:   sta $00,x
    inx
    bne :-
    ldx #NSHOWN-1
    lda #$FF
:   sta zShState,x
    dex
    bpl :-
    lda #$00
    sta QUEUE

    ; The bars' sprites sit at fixed places; only their tiles change.
    lda #$FF
    ldx #$00
:   sta OAM,x
    inx
    bne :-
    ldy #$00
@vrow:
    lda #VIZ_X
    sta zTmp                ; the sprite's X
@vcol:
    tya
    asl a
    asl a
    asl a
    adc #VIZ_Y-1
    sta OAM,x
    lda #$00
    sta OAM+1,x
    lda viz_pal,y
    sta OAM+2,x
    lda zTmp
    sta OAM+3,x
    inx
    inx
    inx
    inx
    clc
    adc #VIZ_PITCH
    sta zTmp
    cmp #VIZ_X+VIZ_BANDS*VIZ_PITCH
    bne @vcol
    iny
    cpy #VIZ_ROWS
    bne @vrow

    ; Every dynamic field, drawn with rendering still off.
:   jsr render
    lda zQ
    beq :+
    jsr flush_queue
    jmp :-
:
    lda #$00
    sta PPU_SCROLL
    sta PPU_SCROLL
    lda #GAME_CTRL
    sta PPU_CTRL
    lda #GAME_MASK
    sta PPU_MASK

main:
:   lda zVbl
    beq :-
    lda #$00
    sta zVbl
    inc zFrame

    jsr read_pad
    lda zPadP
    eor #$FF
    and zPad
    sta zNew
    lda zPad
    sta zPadP

    jsr accept_page
    jsr handshake
    jsr input
    jsr viz
    jsr render
    jmp main

; A = PPU address high byte, zPtr = source, X = 256-byte pages.
upload:
    bit PPU_STATUS
    sta PPU_ADDR
    lda #$00
    sta PPU_ADDR
    tay
:   lda (zPtr),y
    sta PPU_DATA
    iny
    bne :-
    inc zPtr+1
    dex
    bne :-
    rts

; ---------------------------------------------------------------------------
; The host writes its bytes one at a time and a poll can land under a write,
; so a read is taken only once two frames agree.
accept_page:
    ldx #15
:   lda zPage,x
    cmp zPrev,x
    bne @copy
    dex
    bpl :-
    ldx #15
:   lda zPage,x
    sta zSt,x
    dex
    bpl :-
@copy:
    ldx #15
:   lda zPage,x
    sta zPrev,x
    dex
    bpl :-
    rts

; An outstanding request ends on its echo, its refusal, or the give-up count.
handshake:
    lda zBusy
    beq @done
    lda zSt+PG_ANS
    cmp zReq
    beq @answered
    eor #ANS_FAIL
    cmp zReq
    bne @wait
    lda #FLASH
    sta zFlash
@answered:
    lda #$00
    sta zBusy
    rts
@wait:
    lda zWaitLo
    bne :+
    dec zWaitHi
:   dec zWaitLo
    lda zWaitLo
    ora zWaitHi
    bne @done
    sta zBusy
@done:
    rts

; Left/Right move the focus, A presses the focused button. A held on a seek
; button presses it again each time the last one has been answered.
input:
    lda zNew
    and #PAD_LEFT
    beq @right
    dec zSel
    bpl @moved
    lda #NBTN-1
    sta zSel
    bpl @moved
@right:
    lda zNew
    and #PAD_RIGHT
    beq @hold
    inc zSel
    lda zSel
    cmp #NBTN
    bcc @moved
    lda #$00
    sta zSel
@moved:
    lda #$00
    sta zRep
    rts

@hold:
    lda zPad
    and #PAD_A
    bne :+
    sta zRep
:   lda zRepT
    beq :+
    dec zRepT
:   lda zBusy
    bne @none
    lda zNew
    and #PAD_A
    bne @press
    lda zRep
    beq @none
    lda zRepT
    bne @none
    lda #REP_NEXT
    sta zRepT
    jmp activate
@press:
    lda #REP_FIRST
    sta zRepT
    ldx zSel
    lda btn_repeats,x
    sta zRep
    jmp activate
@none:
    rts

activate:
    ldx zSel
    lda btn_param,x
    sta zParam
    lda btn_cmd,x
    tay
    cpx #BTN_PLAY
    bne @eject
    lda zSt+PG_STATE
    cmp #ST_PLAYING
    beq @pause
    cmp #ST_PAUSED
    bne @go
@pause:
    ldy #CMD_PAUSE
    bne @go
@eject:
    cpx #BTN_EJECT
    bne @go
    lda zSt+PG_STATE
    cmp #ST_TRAY
    bne @go
    ldy #CMD_LOAD
@go:
    tya
    tax
    ; falls through

; X = command. A new sequence (1..7, bit 7 is the host's refusal mark) makes
; the byte differ from the last request, which is what the host triggers on.
request:
    lda zSeq
    clc
    adc #$10
    and #$70
    bne :+
    lda #$10
:   sta zSeq
    stx zTmp
    ora zTmp
    sta zReq
    lda #$01
    sta zBusy
    lda #<GIVE_UP
    sta zWaitLo
    lda #>GIVE_UP
    sta zWaitHi
    rts

btn_cmd:     .byte CMD_PREV, CMD_SEEK, CMD_PLAY, CMD_STOP, CMD_SEEK, CMD_NEXT, CMD_EJECT
btn_param:   .byte 0, <(-SEEK_SEC), 0, 0, SEEK_SEC, 0, 0
btn_repeats: .byte 0, 1, 0, 0, 1, 0, 0

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

; ---------------------------------------------------------------------------
; Each band jumps up to the host's level and falls back by VIZ_DECAY a field,
; so a bar moves smoothly between the host's updates; a peak hangs above it.
viz:
    ldx #VIZ_BANDS-1
@band:
    lda zLvl,x
    sec
    sbc #VIZ_DECAY
    bcs :+
    lda #$00
:   cmp zViz,x
    bcs :+
    lda zViz,x
:   sta zLvl,x
    tay
    lda seg_of,y
    sta zSeg
    cmp zPeak,x
    bcc @fall
    sta zPeak,x
    lda #PEAK_HOLD
    sta zPeakT,x
    bne @draw
@fall:
    lda zPeakT,x
    beq :+
    dec zPeakT,x
    jmp @draw
:   lda zFrame
    and #PEAK_FALL
    bne @draw
    dec zPeak,x

    ; Bottom row first, two segments a sprite, tile = top * 3 + bottom.
@draw:
    txa
    asl a
    asl a
    adc #(VIZ_ROWS-1)*VIZ_BANDS*4+1
    tay
    lda #1
    sta zK
@row:
    lda zK
    jsr seg_state
    sta zBot
    inc zK
    lda zK
    jsr seg_state
    sta zTmp
    asl a
    adc zTmp
    adc zBot
    sta OAM,y
    inc zK
    tya
    sec
    sbc #VIZ_BANDS*4
    tay
    bcs @row
    dex
    bpl @band
    rts

.assert (VIZ_ROWS-1)*VIZ_BANDS*4+1 + (VIZ_BANDS-1)*4 < 256, error, "viz OAM offsets"

; A = segment 1..12 of band X: 1 lit, 2 the peak above the bar, 0 unlit.
seg_state:
    cmp zSeg
    beq @lit
    bcc @lit
    cmp zPeak,x
    bne @off
    lda #2
    rts
@lit:
    lda #1
    rts
@off:
    lda #0
    rts

seg_of:
.repeat 256, i
    .byte (i * VIZ_ROWS * 2 + 128) / 256
.endrepeat

; ---------------------------------------------------------------------------
; Only what changed goes to the queue, and no more than one vblank flushes;
; a field that does not fit stays stale and goes next frame.
render:
    lda #$01
    sta zQLock
    lda zFlash
    beq :+
    dec zFlash
:
    lda zSt+PG_STATE
    cmp #ST_STOPPED
    bcc @nodisc
    cmp #ST_NOAUDIO
    bcs @nodisc
    lda #$01
    bne :+
@nodisc:
    lda #$00
:   sta zHave

    ; A refusal turns every button face red for a moment.
    lda zFlash
    beq :+
    lda #$01
:   cmp zShRed
    beq @mark
    NEED 4
    lda zFlash
    beq :+
    lda #$01
:   sta zShRed
    tax
    lda faces,x
    sta zNum
    lda #>PAL_FACE
    sta zAddr
    lda #<PAL_FACE
    sta zAddr+1
    lda #1
    jsr queue_bytes

    ; The focus mark under the button, blinking while a request is out.
@mark:
    lda zSel
    ldx zBusy
    beq :+
    lda zFrame
    and #$08
    beq :+
    lda #$FF
    bne :++
:   lda zSel
:   cmp zShMark
    beq @state
    sta zVal
    NEED 10
    lda zShMark
    cmp #$FF
    beq :+
    lda #' '
    ldx zShMark
    jsr queue_mark
:   lda zVal
    sta zShMark
    cmp #$FF
    beq @state
    lda #T_MARK_L
    ldx zVal
    jsr queue_mark

@state:
    lda zSt+PG_STATE
    cmp #ST_NOAUDIO+1
    bcc :+
    lda #ST_NO_DISC
:   cmp zShState
    beq @icon
    sta zVal
    NEED 14
    lda zVal
    sta zShState
    asl a
    tax
    lda state_words,x
    sta zPtr
    lda state_words+1,x
    sta zPtr+1
    lda #>STATE_AT
    sta zAddr
    lda #<STATE_AT
    sta zAddr+1
    jsr queue_str

    ; The play button shows pause while there is something to pause.
@icon:
    ldx #$00
    lda zSt+PG_STATE
    cmp #ST_PLAYING
    bne :+
    ldx #$04
:   cpx zShIcon
    beq @nums
    stx zVal
    NEED 10
    ldx zVal
    stx zShIcon
    lda icons,x
    sta zNum
    lda icons+1,x
    sta zNum+1
    lda #>ICON_AT
    sta zAddr
    lda #<ICON_AT
    sta zAddr+1
    lda #2
    jsr queue_bytes
    ldx zShIcon
    lda icons+2,x
    sta zNum
    lda icons+3,x
    sta zNum+1
    lda #>(ICON_AT+32)
    sta zAddr
    lda #<(ICON_AT+32)
    sta zAddr+1
    lda #2
    jsr queue_bytes

@nums:
    ldx #$00
@field:
    stx zIdx
    ldy fld_src,x
    lda zSt,y
    ldy zHave
    bne :+
    lda #$FF
:   cmp zShNum,x
    beq @next
    sta zVal
    lda fld_size,x
    jsr room
    bcs render_end
    ldx zIdx
    lda zVal
    sta zShNum,x
    ldy fld_lo,x
    lda fld_big,x
    sta zTmp2
    lda fld_hi,x
    tax
    lda zVal
    bit zTmp2
    bmi :+
    jsr queue_num
    jmp @next
:   jsr queue_big
@next:
    ldx zIdx
    inx
    cpx #6
    bne @field

    jsr bar_px
    cmp zShBar
    beq render_end
    NEED BAR_TILES+3
    lda zPx
    sta zShBar
    jsr queue_bar

render_end:
    lda #$00
    sta zQLock
    rts

; Carry clear when A more bytes fit in this frame's queue.
room:
    clc
    adc zQ
    cmp #QMAX+1
    rts

fld_src:  .byte PG_TRACK, PG_NTRACK, PG_MIN, PG_SEC, PG_LMIN, PG_LSEC
fld_big:  .byte $80, 0, $80, $80, 0, 0
fld_size: .byte 10, 5, 10, 10, 5, 5
fld_hi:   .byte >TRACK_AT, >NTRACK_AT, >MIN_AT, >SEC_AT, >LMIN_AT, >LSEC_AT
fld_lo:   .byte <TRACK_AT, <NTRACK_AT, <MIN_AT, <SEC_AT, <LMIN_AT, <LSEC_AT

; A = tile for the left half (' ' erases), X = button.
queue_mark:
    sta zNum
    cmp #' '
    beq :+
    lda #T_MARK_R
:   sta zNum+1
    txa
    asl a
    asl a
    adc #<MARK_AT
    sta zAddr+1
    lda #>MARK_AT
    sta zAddr
    lda #2
    jmp queue_bytes

.assert <MARK_AT + (NBTN - 1) * BTN_PITCH + 1 < 256, error, "marker row crosses a page"

; A = 0..99, or $FF for blank, into zD (tens) and zD+1 (ones); blank is 10.
split:
    cmp #$FF
    bne :+
    lda #10
    sta zD
    sta zD+1
    rts
:   ldx #$00
:   cmp #10
    bcc :+
    sbc #10
    inx
    bne :-
:   cpx #10
    bcc :+
    ldx #9
:   stx zD
    sta zD+1
    rts

; A = value as two font digits ('-' when blank) at X:Y.
queue_num:
    stx zAddr
    sty zAddr+1
    jsr split
    ldx #1
:   lda zD,x
    cmp #10
    bcc :+
    lda #<('-'-'0')
:   clc
    adc #'0'
    sta zNum,x
    dex
    bpl :--
    lda #$02
    jmp queue_bytes

; A = value as two 7-segment digits, two rows tall, at X:Y.
queue_big:
    stx zAddr
    sty zAddr+1
    jsr split
    lda zD
    clc
    adc #T_BIGT
    sta zNum
    lda zD+1
    adc #T_BIGT
    sta zNum+1
    lda #$02
    jsr queue_bytes
    lda zAddr+1
    clc
    adc #32
    sta zAddr+1
    bcc :+
    inc zAddr
:   lda zD
    clc
    adc #T_BIGB
    sta zNum
    lda zD+1
    adc #T_BIGB
    sta zNum+1
    lda #$02
    ; falls through

; A bytes from zNum to zAddr.
queue_bytes:
    ldx zQ
    sta QUEUE,x
    sta zTmp
    inx
    lda zAddr
    sta QUEUE,x
    inx
    lda zAddr+1
    sta QUEUE,x
    inx
    ldy #$00
:   lda zNum,y
    sta QUEUE,x
    inx
    iny
    cpy zTmp
    bne :-
    lda #$00
    sta QUEUE,x
    stx zQ
    rts

; The string at zPtr (NUL-terminated) to zAddr.
queue_str:
    ldx zQ
    inx                     ; the length slot
    lda zAddr
    sta QUEUE,x
    inx
    lda zAddr+1
    sta QUEUE,x
    inx
    ldy #$00
:   lda (zPtr),y
    beq :+
    sta QUEUE,x
    inx
    iny
    bne :-
:   lda #$00
    sta QUEUE,x
    tya
    ldx zQ
    sta QUEUE,x
    tya
    clc
    adc zQ
    adc #$03
    sta zQ
    rts

; The whole bar as one run, zPx pixels lit.
queue_bar:
    ldx zQ
    lda #BAR_TILES
    sta QUEUE,x
    inx
    lda #>BAR_AT
    sta QUEUE,x
    inx
    lda #<BAR_AT
    sta QUEUE,x
    inx
    lda zPx
    sta zTmp
    ldy #BAR_TILES
@tile:
    lda zTmp
    cmp #9
    bcc :+
    lda #8
:   sta zTmp2
    clc
    adc #T_BAR
    sta QUEUE,x
    inx
    lda zTmp
    sec
    sbc zTmp2
    sta zTmp
    dey
    bne @tile
    lda #$00
    sta QUEUE,x
    stx zQ
    rts

; ---------------------------------------------------------------------------
; A = zPx = elapsed * bar width / length, 0 with no disc or no length.
bar_px:
    lda #$00
    sta zPx
    lda zHave
    beq @out
    lda zSt+PG_LMIN
    ldy zSt+PG_LSEC
    jsr seconds
    lda zW
    sta zL
    lda zW+1
    sta zL+1
    ora zL
    beq @out
    lda zSt+PG_MIN
    ldy zSt+PG_SEC
    jsr seconds
    lda zW
    cmp zL
    lda zW+1
    sbc zL+1
    bcc :+
    lda #BAR_TILES*8
    sta zPx
    bne @out
    ; elapsed * 5, then << 5, is * 160.
:   lda zW
    sta zN
    lda zW+1
    sta zN+1
    asl zW
    rol zW+1
    asl zW
    rol zW+1
    clc
    lda zW
    adc zN
    sta zN
    lda zW+1
    adc zN+1
    sta zN+1
    lda #$00
    sta zN+2
    ldx #5
:   asl zN
    rol zN+1
    rol zN+2
    dex
    bne :-
    jsr divide
    lda zN
    sta zPx
@out:
    lda zPx
    rts

.assert BAR_TILES * 8 = 160, error, "bar_px scales by 160"

; A = minutes, Y = seconds, into zW.
seconds:
    sty zTmp
    sta zW
    lda #$00
    sta zW+1
    asl zW
    rol zW+1
    asl zW
    rol zW+1
    lda zW
    sta zT
    lda zW+1
    sta zT+1
    ldx #4
:   asl zW
    rol zW+1
    dex
    bne :-
    sec
    lda zW
    sbc zT
    sta zW
    lda zW+1
    sbc zT+1
    sta zW+1
    clc
    lda zW
    adc zTmp
    sta zW
    bcc :+
    inc zW+1
:   rts

; zN (24-bit) / zL (16-bit): quotient in zN.
divide:
    lda #$00
    sta zR
    sta zR+1
    ldx #24
@bit:
    asl zN
    rol zN+1
    rol zN+2
    rol zR
    rol zR+1
    lda zR
    sec
    sbc zL
    tay
    lda zR+1
    sbc zL+1
    bcc :+
    sta zR+1
    sty zR
    inc zN
:   dex
    bne @bit
    rts

; Write every queued run through $2007 and empty the queue. Rendering off,
; or inside vblank.
flush_queue:
    ldx #$00
@run:
    lda QUEUE,x
    beq @end
    sta zTmp
    inx
    bit PPU_STATUS
    lda QUEUE,x
    sta PPU_ADDR
    inx
    lda QUEUE,x
    sta PPU_ADDR
    inx
:   lda QUEUE,x
    sta PPU_DATA
    inx
    dec zTmp
    bne :-
    jmp @run
@end:
    lda #$00
    sta QUEUE
    sta zQ
    rts

; Each 11 wide so a shorter word clears a longer one.
state_words:
    .word w_nodisc, w_tray, w_loading, w_stopped, w_playing, w_paused, w_noaudio
w_nodisc:  .byte $15, " NO DISC  ", 0
w_tray:    .byte $13, " TRAY OPEN", 0
w_loading: .byte $14, " LOADING  ", 0
w_stopped: .byte $12, " STOPPED  ", 0
w_playing: .byte $10, " PLAYING  ", 0
w_paused:  .byte $11, " PAUSED   ", 0
w_noaudio: .byte $15, " NOT AUDIO", 0

icons:
    .byte T_PLAY, T_PLAY+1, T_PLAY+2, T_PLAY+3
    .byte T_PAUSE, T_PAUSE+1, T_PAUSE+2, T_PAUSE+3
faces:
    .byte FACE_NORMAL, FACE_RED
palette:
    .incbin "palette.bin"
viz_pal:
    .incbin "vizpal.bin"

; ---------------------------------------------------------------------------
.segment "CODE2"

; Every frame: our bytes rewritten (a host access to the page drops a write
; that lands under it), the host's read back, the screen updated. The
; parameter goes first so a new request never shows with the old one.
nmi:
    pha
    txa
    pha
    tya
    pha

    lda #$00
    sta $2003
    lda #>OAM
    sta OAM_DMA

    bit PPU_STATUS
    lda #>MBOX_PAGE
    sta PPU_ADDR
    lda #<MBOX_PAGE
    sta PPU_ADDR
    lda #'N'
    sta PPU_DATA
    lda #'C'
    sta PPU_DATA
    lda #'D'
    sta PPU_DATA
    lda #'P'
    sta PPU_DATA
    lda #>MBOX_PARAM
    sta PPU_ADDR
    lda #<MBOX_PARAM
    sta PPU_ADDR
    lda zParam
    sta PPU_DATA
    lda #>MBOX_REQ
    sta PPU_ADDR
    lda #<MBOX_REQ
    sta PPU_ADDR
    lda zReq
    sta PPU_DATA

    lda #>MBOX_PAGE
    sta PPU_ADDR
    lda #<MBOX_PAGE
    sta PPU_ADDR
    lda PPU_DATA            ; fills the read buffer
    ldx #$00
:   lda PPU_DATA
    sta zPage,x
    inx
    cpx #16
    bne :-

    ; A torn read only shows for a field, so the bands skip accept_page.
    lda #>MBOX_VIZ
    sta PPU_ADDR
    lda #<MBOX_VIZ
    sta PPU_ADDR
    lda PPU_DATA
    ldx #$00
:   lda PPU_DATA
    sta zViz,x
    inx
    cpx #VIZ_BANDS
    bne :-

    lda zQLock
    bne :+
    jsr flush_queue
:
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
    .incbin "gfx.chr"
screen:
    .incbin "screen.bin"
sprites:
    .incbin "sprites.chr"

; ---------------------------------------------------------------------------
.segment "VECTORS"
    .word nmi
    .word reset
    .word irq
