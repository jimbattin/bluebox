// Host-side loader for the 224XL ROM files (roms/224XL_v8_21).
#pragma once
#include <cstdint>
#include <string>

// Fills `sbc` (0000-17FF) and `nvs` (8000-FFFF) from `dir`, checking sizes and
// byte-sum checksums.
bool loadRomFiles(const std::string& dir, uint8_t (&sbc)[0x1800], uint8_t (&nvs)[0x8000],
                  std::string& err);
