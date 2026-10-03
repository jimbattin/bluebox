// Host-side WAV reading and writing for the lex224emu CLI (not built into the pedal).
#pragma once
#include <string>
#include <vector>

// Reads PCM 16/24/32-bit, float 32-bit or WAVE_FORMAT_EXTENSIBLE of those.
// Samples are normalised to [-1, 1).
bool readWav(const std::string& path, unsigned& rate, unsigned& channels,
             std::vector<std::vector<float>>& ch, std::string& err);

// Writes interleaved 16-bit PCM.
bool writeWav(const std::string& path, unsigned rate, const std::vector<std::vector<float>>& ch,
              std::string& err);
