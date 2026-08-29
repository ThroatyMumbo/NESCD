CA65  ?= ca65
LD65  ?= ld65
PY    ?= python3
BUILD ?= build
PCM   := pcm
TOOLS := ../firmware/tools
SRC   := ../firmware/src

ROM    := $(BUILD)/demo.nes
TRACKS := $(BUILD)/track1.rom $(BUILD)/track2.rom $(BUILD)/track3.rom \
          $(BUILD)/track4.rom
LOOPS  := $(PCM)/loops.csv
IMG    := $(BUILD)/demo.img
TITLE  ?= NESCD DEMO

all: $(IMG)

$(ROM): demo.s gen_tiles.py game.cfg mkrom.py Makefile
	@mkdir -p $(BUILD)
	$(PY) gen_tiles.py $(BUILD)/tiles.chr
	$(CA65) demo.s -g --bin-include-dir $(BUILD) \
		-l $(BUILD)/demo.lst -o $(BUILD)/demo.o
	$(LD65) -C game.cfg $(BUILD)/demo.o -m $(BUILD)/demo.map \
		-Ln $(BUILD)/demo.sym -o $(BUILD)/prg.bin
	$(PY) mkrom.py $(BUILD)/prg.bin $@

$(BUILD)/track%.rom: $(PCM)/track%.wav $(LOOPS)
	@mkdir -p $(BUILD)
	$(PY) $(TOOLS)/mktrack.py $< -o $@ --loop-csv $(LOOPS) --verify

$(BUILD)/track1.rom: $(PCM)/track1.wav
	@mkdir -p $(BUILD)
	$(PY) $(TOOLS)/mktrack.py $< -o $@ --once --verify

$(BUILD)/track4.rom: $(PCM)/track4.wav
	@mkdir -p $(BUILD)
	$(PY) $(TOOLS)/mktrack.py $< -o $@ --once --verify

# The disc the drive boots: the ROM as item 0, then one track per $1FF9 value.
$(IMG): $(ROM) $(TRACKS) $(TOOLS)/mkgamedisc.py
	$(PY) $(TOOLS)/mkgamedisc.py -o $@ --title "$(TITLE)" --nes $(ROM) \
		$(foreach n,1 2 3 4,--track $(n)=$(BUILD)/track$(n).rom) --verify

disc: $(IMG)

burn: $(IMG)
	../firmware/burn.sh $(IMG)

# Read the image back with the firmware's own readers, no hardware attached.
check: $(IMG)
	gcc -O2 -I $(SRC) -o $(BUILD)/host_loop ../firmware/test/host_loop.c
	$(BUILD)/host_loop
	gcc -O2 -I $(SRC) -I ../firmware/test/stub -o $(BUILD)/host_game \
		../firmware/test/host_game.c $(addprefix $(SRC)/,catalog.c track.c \
		disc.c n8push.c cdcore.c)
	$(BUILD)/host_game $(IMG) --track 1=$(BUILD)/track1.rom

clean:
	rm -rf $(BUILD)

.PHONY: all disc burn check clean
