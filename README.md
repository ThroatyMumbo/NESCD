# NESCD-001

<img src="nescd.png" alt="NESCD-001" width="320">

An unofficial homemade CD add-on for the NES, just because!

This repo contains the schematics, PCB, 3D printable STLs for the shell, and sticker labels.

Firmware is on hold because it currently contains semi-reverse engineered code from Something Nerdy's 'Bad Apple' FMV demo, which I've chosen not to publish.
At some point I may push a stripped down version of the firmware that only includes the in-game CD audio feature. Need some time to work on that.

## How it works

It's basically an RP2350B hooked up to both an Everdrive N8 Pro and an IDE CD drive. It reads the game ROM off the CD, uploads it to the Everdrive over USB,
launches it, and then waits for audio or FMV requests from the game via CHR mailbox at 0x21FF8. The RP2350B is the coordinater in the middle that bridges
the NES/Everdrive and the CD drive.

The video frames for FMVs are streamed to the Everdrive over USB and read into VRAM using custom bitstream code for the Cyclone IV (the FPGA the Everdrive N8 Pro uses).
The audio is simply piped directly from a PCM5102 DAC to the audio mux in pin on the NES expansion port.

## Hardware
- The main PCB (see [hardware/](hardware/))
- A standard 40-pin IDE CD drive
- Everdrive N8 Pro

## FAQ

### What?
Great question! I came up with the idea for this project while reading about the cancelled SNES CD-ROM add-on. I was originally going to make my own
version of that add-on, but decided it'd be funnier and weirder if I took it a step further and made one for NES.

### Why?
Because it's fun.

### Who?
It's me! Your old pal - Throaty Mumbo!

### Why not stream audio from the Everdrive?
I originally wanted to do this, since that's how the Something Nerdy FMV demo handles it, but was hitting a ceiling on the Everdrive USB bandwidth.
I'd likely have to either add some form of aggressive compression or drop the video frame rate to make room. It's also a nice touch to use the expansion port instead.

In fact, I'd like to send the video over the expansion port as well if possible, since the USB cable poking out on the front is by far the biggest
blemish on the design of this contraption. Unfortunately the bandwidth on the expansion port is even more limited than the USB route.

Plus, the expansion port audio allows more flexibility with mixing and volume control - hence the volume knob on the front.

### How is this thing powered?
A single molex cable from a wall adapter ([this one](https://www.amazon.com/Coolerguys-100-240v-Molex-Power-Adapter/dp/B000MGG6SC) in particular).
The 5V/12V rails are split between the CD drive and the board + the NES output. I took the end of a spare NES power cable and hooked it up to
a buck converter that converts the 12V input to ~9.5V so you can power the NES as well (the plug pokes out of the top of the box). That way
you only need one wall outlet to get the entire thing running. You can also power the NES separately from its regular adapter - the plug
coming out of the NESCD is optional.

### How do you get back to the Everdrive menu once you've inserted a disc?
This is still something I'm trying to figure out. AFAIK there's no way to trigger a soft reset on the NES. However one guaranteed solution is to
hook up the NES power output rail to a relay switch. That way if the board ever needs the NES to reset, it can just turn it on and off from the
source. Not super elegant but it should work.
