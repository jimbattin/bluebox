// Loads the 11 ROM files of a 224XL ROM set into the SBC and NVS images (romfile.h).
#include "romfile.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

struct RomFile {
    const char* name;
    uint16_t base;
    size_t size;
    uint8_t index;  // the firmware expects the 8-bit byte sum to equal this
};

// Mirrors tools/rom_map.py ROMS.
constexpr RomFile kRoms[] = {
    {"SBC1 2716.BIN", 0x0000, 0x800, 1},  {"SBC2 2716.BIN", 0x0800, 0x800, 2},
    {"SBC3 2716.BIN", 0x1000, 0x800, 3},  {"NVS1 2732.BIN", 0x8000, 0x1000, 1},
    {"NVS2 2732.BIN", 0x9000, 0x1000, 2}, {"NVS3 2732.BIN", 0xA000, 0x1000, 3},
    {"NVS4 2732.BIN", 0xB000, 0x1000, 4}, {"NVS5 2732.BIN", 0xC000, 0x1000, 5},
    {"NVS6 2732.BIN", 0xD000, 0x1000, 6}, {"NVS7 2732.BIN", 0xE000, 0x1000, 7},
    {"NVS8 2732.BIN", 0xF000, 0x1000, 8},
};

}  // namespace

bool loadRomFiles(const std::string& dir, uint8_t (&sbc)[0x1800], uint8_t (&nvs)[0x8000],
                  std::string& err) {
    for (const RomFile& rom : kRoms) {
        const std::string path = dir + "/" + rom.name;
        std::ifstream f(path, std::ios::binary);
        const std::vector<char> data{std::istreambuf_iterator<char>(f),
                                     std::istreambuf_iterator<char>()};
        if (!f.is_open() || data.size() != rom.size) {
            err = "missing or wrong-size ROM: " + path;
            return false;
        }
        uint8_t sum = 0;
        for (char ch : data) sum = uint8_t(sum + uint8_t(ch));
        if (sum != rom.index) {
            err = "ROM checksum mismatch: " + path;
            return false;
        }
        uint8_t* dst = rom.base < 0x8000 ? sbc + rom.base : nvs + (rom.base - 0x8000);
        std::memcpy(dst, data.data(), rom.size);
    }
    return true;
}
