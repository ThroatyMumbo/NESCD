; cdplayer.s - an audio CD player. The host on the expansion port does the
; playing; this ROM shows its state and sends it the buttons, through the
; mailbox page in sprite tile $FF (firmware/src/player.h has the byte map).
;
; NROM-256 with 8 KiB CHR RAM, like demo.s. The font is at tile == ASCII.

PPU_CTRL   = $2000
PPU_MASK   = $2001
PPU_STATUS = $2002
PPU_SCROLL = $2005
PPU_ADDR   = $2006
PPU_DATA   = $2007
JOY1       = $4016

MBOX_PAGE  = $1FF0          ; 16 bytes: ours and the host's
MBOX_REQ   = $1FF8

; Request commands (low nibble), the sequence in the high nibble.
CMD_PLAY   = 1
CMD_PAUSE  = 2
CMD_STOP   = 3
CMD_NEXT   = 4
CMD_PREV   = 5
CMD_EJECT  = 6
CMD_LOAD   = 7
ANS_FAIL   = $80

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
FLASH      = 45             ; fields the refused mark shows

PAD_A      = $80
PAD_B      = $40
PAD_SELECT = $20
PAD_START  = $10
PAD_LEFT   = $02
PAD_RIGHT  = $01

GAME_CTRL  = $88            ; NMI on, sprites at $1000 (tile $FF = the mailbox)
GAME_MASK  = $0A            ; background only

; Screen positions (nametable $2000 + row * 32 + col).
TITLE_AT   = $2000 + 3 * 32 + 9
STATE_LBL  = $2000 + 8 * 32 + 4
STATE_AT   = $2000 + 8 * 32 + 13
BUSY_AT    = $2000 + 8 * 32 + 23
TRACK_LBL  = $2000 + 11 * 32 + 4
TRACK_AT   = $2000 + 11 * 32 + 13
NTRACK_AT  = $2000 + 11 * 32 + 18
TIME_LBL   = $2000 + 14 * 32 + 4
TIME_AT    = $2000 + 14 * 32 + 13
LEN_AT     = $2000 + 14 * 32 + 21
HELP1_AT   = $2000 + 19 * 32 + 9
HELP2_AT   = $2000 + 21 * 32 + 9
HELP3_AT   = $2000 + 23 * 32 + 9

; Zero page.
zVbl     = $10
zPad     = $11
zPadP    = $12
zNew     = $13
zReq     = $14              ; what $1FF8 carries, kept after the answer
zParam   = $15              ; $1FF9
zSeq     = $16              ; 1..15
zBusy    = $17              ; a request is outstanding
zWaitLo  = $18
zWaitHi  = $19
zFlash   = $1A
zQLock   = $1B              ; the queue is being appended to
zQ       = $1C              ; queue write index
zTmp     = $1D
zNum     = $1E              ; and $1F: two digit tiles
zPtr     = $20              ; and $21
zAddr    = $22              ; and $23: nametable address, hi first
zIdx     = $24
zPage    = $30              ; 16: this frame's read of $1FF0..$1FFF
zPrev    = $40              ; 16: last frame's
zSt      = $50              ; 16: the last two frames agreed on
zShState = $60              ; what is on screen
zShTrack = $61
zShNTrk  = $62
zShMin   = $63
zShSec   = $64
zShLMin  = $65
zShLSec  = $66
zShBusy  = $67

QUEUE    = $0300            ; [len, hi, lo, bytes...]*, len 0 ends it

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

    ; CHR RAM comes up undefined: the font ships in PRG. PPU_CTRL/PPU_MASK
    ; are still 0 from reset: NMI off, rendering off, VRAM increment +1.
    bit PPU_STATUS
    lda #$00
    sta PPU_ADDR
    sta PPU_ADDR
    lda #<tiles
    sta zPtr
    lda #>tiles
    sta zPtr+1
    ldx #32
    ldy #$00
@chr:
    lda (zPtr),y
    sta PPU_DATA
    iny
    bne @chr
    inc zPtr+1
    dex
    bne @chr

    ; The page starts clear, including the host's half.
    bit PPU_STATUS
    lda #>MBOX_PAGE
    sta PPU_ADDR
    lda #<MBOX_PAGE
    sta PPU_ADDR
    lda #$00
    ldx #16
:   sta PPU_DATA
    dex
    bne :-

    ldx #$00
:   sta $00,x
    inx
    bne :-
    ldx #$07
    lda #$FF
:   sta zShState,x          ; everything redraws on the first frame
    dex
    bpl :-
    lda #$00
    sta QUEUE

    jsr clear_screen
    jsr draw_static
    jsr flush_queue

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
    jsr render
    jmp main

; ---------------------------------------------------------------------------
; The host writes its bytes one at a time and a poll can land under a write,
; so a read is taken only once two frames agree.
accept_page:
    ldx #15
:   lda zPage,x
    cmp zPrev,x
    bne @no
    dex
    bpl :-
    ldx #15
:   lda zPage,x
    sta zSt,x
    dex
    bpl :-
@no:
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

input:
    lda zBusy
    bne @none
    lda zNew
    beq @none
    ldx #CMD_PLAY
    and #PAD_A
    bne request
    lda zNew
    ldx #CMD_STOP
    and #PAD_B
    bne request
    lda zNew
    ldx #CMD_PAUSE
    and #PAD_START
    bne request
    lda zNew
    ldx #CMD_NEXT
    and #PAD_RIGHT
    bne request
    lda zNew
    ldx #CMD_PREV
    and #PAD_LEFT
    bne request
    lda zNew
    and #PAD_SELECT
    beq @none
    ldx #CMD_EJECT
    lda zSt+PG_STATE
    cmp #ST_TRAY
    bne request
    ldx #CMD_LOAD
    bne request
@none:
    rts

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
; Only what changed goes to the queue.
render:
    lda #$01
    sta zQLock

    lda zSt+PG_STATE
    cmp zShState
    beq @track
    sta zShState
    cmp #ST_NOAUDIO+1
    bcc :+
    lda #ST_NO_DISC
:   asl a
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

@track:
    lda zSt+PG_TRACK
    cmp zShTrack
    beq @ntrack
    sta zShTrack
    ldx #>TRACK_AT
    ldy #<TRACK_AT
    jsr queue_num
@ntrack:
    lda zSt+PG_NTRACK
    cmp zShNTrk
    beq @time
    sta zShNTrk
    ldx #>NTRACK_AT
    ldy #<NTRACK_AT
    jsr queue_num
@time:
    lda zSt+PG_MIN
    cmp zShMin
    beq @sec
    sta zShMin
    ldx #>TIME_AT
    ldy #<TIME_AT
    jsr queue_num
@sec:
    lda zSt+PG_SEC
    cmp zShSec
    beq @lmin
    sta zShSec
    ldx #>(TIME_AT+3)
    ldy #<(TIME_AT+3)
    jsr queue_num
@lmin:
    lda zSt+PG_LMIN
    cmp zShLMin
    beq @lsec
    sta zShLMin
    ldx #>LEN_AT
    ldy #<LEN_AT
    jsr queue_num
@lsec:
    lda zSt+PG_LSEC
    cmp zShLSec
    beq @busy
    sta zShLSec
    ldx #>(LEN_AT+3)
    ldy #<(LEN_AT+3)
    jsr queue_num
@busy:
    lda zFlash
    beq :+
    dec zFlash
    lda #'!'
    bne @mark
:   lda #' '
    ldx zBusy
    beq @mark
    lda #'*'
@mark:
    cmp zShBusy
    beq @done
    sta zShBusy
    sta zNum
    lda #>BUSY_AT
    sta zAddr
    lda #<BUSY_AT
    sta zAddr+1
    lda #$01
    jsr queue_bytes
@done:
    lda #$00
    sta zQLock
    rts

; A = 0..99 as two digit tiles at X:Y.
queue_num:
    stx zAddr
    sty zAddr+1
    ldx #'0'
:   cmp #10
    bcc :+
    sbc #10
    inx
    bne :-
:   stx zNum
    clc
    adc #'0'
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

; ---------------------------------------------------------------------------
clear_screen:
    bit PPU_STATUS
    lda #$20
    sta PPU_ADDR
    lda #$00
    sta PPU_ADDR
    ldx #$03                ; 960 tiles of space, then the 64 attribute bytes
    ldy #$00
    lda #' '
:   sta PPU_DATA
    iny
    bne :-
    dex
    bne :-
    ldy #192
:   sta PPU_DATA
    dey
    bne :-
    ldy #$40                ; attribute table: palette 0 everywhere
    lda #$00
:   sta PPU_DATA
    dey
    bne :-
    lda #$3F
    sta PPU_ADDR
    lda #$00
    sta PPU_ADDR
    ldx #$00
:   lda palette,x
    sta PPU_DATA
    inx
    cpx #$20
    bne :-
    rts

draw_static:
    ldx #$00
@next:
    lda statics,x
    beq @done
    sta zAddr
    lda statics+1,x
    sta zAddr+1
    lda statics+2,x
    sta zPtr
    lda statics+3,x
    sta zPtr+1
    stx zIdx
    jsr queue_str
    jsr flush_queue
    ldx zIdx
    inx
    inx
    inx
    inx
    bne @next
@done:
    rts

statics:
    .byte >TITLE_AT, <TITLE_AT
    .word s_title
    .byte >STATE_LBL, <STATE_LBL
    .word s_state
    .byte >TRACK_LBL, <TRACK_LBL
    .word s_track
    .byte >(TRACK_AT+3), <(TRACK_AT+3)
    .word s_slash
    .byte >TIME_LBL, <TIME_LBL
    .word s_time
    .byte >(TIME_AT+2), <(TIME_AT+2)
    .word s_colon
    .byte >(TIME_AT+6), <(TIME_AT+6)
    .word s_slash
    .byte >(LEN_AT+2), <(LEN_AT+2)
    .word s_colon
    .byte >HELP1_AT, <HELP1_AT
    .word s_help1
    .byte >HELP2_AT, <HELP2_AT
    .word s_help2
    .byte >HELP3_AT, <HELP3_AT
    .word s_help3
    .byte 0

s_title:  .byte "NES CD PLAYER", 0
s_state:  .byte "STATUS", 0
s_track:  .byte "TRACK", 0
s_time:   .byte "TIME", 0
s_slash:  .byte "/", 0
s_colon:  .byte ":", 0
; Padded so the dashes line up under each other.
s_help1:  .byte "    A - PLAY", 0
s_help2:  .byte "    B - STOP", 0
s_help3:  .byte "START - PAUSE", 0

state_words:
    .word w_nodisc, w_tray, w_loading, w_stopped, w_playing, w_paused, w_noaudio
w_nodisc:  .byte "NO DISC  ", 0
w_tray:    .byte "TRAY OPEN", 0
w_loading: .byte "LOADING  ", 0
w_stopped: .byte "STOPPED  ", 0
w_playing: .byte "PLAYING  ", 0
w_paused:  .byte "PAUSED   ", 0
w_noaudio: .byte "NOT AUDIO", 0

palette:
    .byte $0F,$30,$10,$21,  $0F,$30,$10,$21
    .byte $0F,$30,$10,$21,  $0F,$30,$10,$21
    .byte $0F,$0F,$0F,$0F,  $0F,$0F,$0F,$0F
    .byte $0F,$0F,$0F,$0F,  $0F,$0F,$0F,$0F

; ---------------------------------------------------------------------------
.segment "CODE2"

; Every frame: our bytes rewritten (a host access to the page drops a write
; that lands under it), the host's read back, the screen updated.
nmi:
    pha
    txa
    pha
    tya
    pha

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
    lda #>MBOX_REQ
    sta PPU_ADDR
    lda #<MBOX_REQ
    sta PPU_ADDR
    lda zReq
    sta PPU_DATA
    lda zParam
    sta PPU_DATA

    ldx #15
:   lda zPage,x
    sta zPrev,x
    dex
    bpl :-
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
    .incbin "font.chr"

; ---------------------------------------------------------------------------
.segment "VECTORS"
    .word nmi
    .word reset
    .word irq
