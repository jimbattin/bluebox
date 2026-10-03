# 224XL Emulator

An experiment to see how we can replicate this legendary reverb unit for historic posterity

## Requirements

Requires a legal set of 224XL ROMs. 
This project is researched, built and tested against 224XL v8.21 ROMs 

Place a complete set of v8.21 rom images in a folder called `roms/224XL_v8_21/`
There should be 8 `NVSx 2732.BIN` images and 3 `SBCx 2716.BIN` images in this folder.

## Usage

Comes in two flavors:
- Stand alone application in [emu/](emu) that takes a .wav input and renders an output wav file
- Daisy Seed hosted in a PedalPCB Terrarium guitar pedal (Mono only) in [daisy/](daisy), with the Bluebox BARC Web MIDI editor for it in [editor/](editor)

For the locally-running emulator that can ingest a wav file and spit out a 100% wet output see the [emulator build docs](emu/README.md)

For the Daisy pedal instructions [see this README.md](daisy/README.md)




