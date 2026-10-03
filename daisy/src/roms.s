@ Embeds the 224XL V8.2 ROMs. The files are exactly 2048 (2716) and 4096 (2732)
@ bytes, so each blob is the contiguous image at its 8080 base: lex_rom_sbc at
@ 0000-17FF and lex_rom_nvs at 8000-FFFF. .rodata is collected into .text,
@ which the BOOT_SRAM linker script places in SRAM.
    .section .rodata
    .align 2
    .global lex_rom_sbc
lex_rom_sbc:
    .incbin "SBC1 2716.BIN"
    .incbin "SBC2 2716.BIN"
    .incbin "SBC3 2716.BIN"
    .global lex_rom_nvs
lex_rom_nvs:
    .incbin "NVS1 2732.BIN"
    .incbin "NVS2 2732.BIN"
    .incbin "NVS3 2732.BIN"
    .incbin "NVS4 2732.BIN"
    .incbin "NVS5 2732.BIN"
    .incbin "NVS6 2732.BIN"
    .incbin "NVS7 2732.BIN"
    .incbin "NVS8 2732.BIN"
