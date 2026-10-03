// WAV file I/O (audio.h).
#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {

uint32_t le16(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void put16(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}

}  // namespace

bool readWav(const std::string& path, unsigned& rate, unsigned& channels,
             std::vector<std::vector<float>>& ch, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        err = "cannot open " + path;
        return false;
    }
    const std::vector<uint8_t> buf{std::istreambuf_iterator<char>(f),
                                   std::istreambuf_iterator<char>()};
    if (buf.size() < 12 || std::memcmp(buf.data(), "RIFF", 4) != 0 ||
        std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    unsigned tag = 0, bits = 0;
    bool haveFmt = false;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    size_t p = 12;
    while (p + 8 <= buf.size()) {
        const uint8_t* hdr = buf.data() + p;
        const size_t size = le32(hdr + 4);
        const size_t body = p + 8;
        const size_t avail = std::min(size, buf.size() - body);
        if (std::memcmp(hdr, "fmt ", 4) == 0 && avail >= 16) {
            const uint8_t* fmt = buf.data() + body;
            tag = le16(fmt);
            channels = le16(fmt + 2);
            rate = le32(fmt + 4);
            bits = le16(fmt + 14);
            if (tag == 0xFFFE) tag = avail >= 26 ? le16(fmt + 24) : 0;  // SubFormat GUID
            haveFmt = true;
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            data = buf.data() + body;
            dataSize = avail;
        }
        p = body + size + (size & 1);  // chunks are padded to an even length
    }
    if (!haveFmt || !data) {
        err = "not a RIFF/WAVE file";
        return false;
    }
    const bool pcm = tag == 1 && (bits == 16 || bits == 24 || bits == 32);
    const bool flt = tag == 3 && bits == 32;
    if (!(pcm || flt) || channels == 0 || rate == 0) {
        err = "unsupported WAV format";
        return false;
    }

    const size_t bytes = bits / 8;
    const size_t frames = dataSize / (bytes * channels);
    const float scale = 1.0f / float(1u << (bits - 1));
    ch.assign(channels, std::vector<float>(frames));
    const uint8_t* s = data;
    for (size_t i = 0; i < frames; i++) {
        for (unsigned c = 0; c < channels; c++, s += bytes) {
            float v;
            if (flt) {
                const uint32_t u = le32(s);
                std::memcpy(&v, &u, 4);
            } else if (bits == 16) {
                v = float(int16_t(le16(s))) * scale;
            } else if (bits == 24) {
                // Assemble in the top 3 bytes, then shift down: the arithmetic shift
                // sign-extends.
                const uint32_t u = uint32_t(s[0]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 24;
                v = float(int32_t(u) >> 8) * scale;
            } else {
                v = float(int32_t(le32(s))) * scale;
            }
            ch[c][i] = v;
        }
    }
    return true;
}

bool writeWav(const std::string& path, unsigned rate, const std::vector<std::vector<float>>& ch,
              std::string& err) {
    const unsigned channels = unsigned(ch.size());
    const size_t frames = channels ? ch[0].size() : 0;
    const uint32_t dataSize = uint32_t(frames * channels * 2);
    std::vector<uint8_t> b;
    b.reserve(44 + dataSize);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put32(b, 36 + dataSize);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(b, 16);
    put16(b, 1);
    put16(b, channels);
    put32(b, rate);
    put32(b, rate * channels * 2);
    put16(b, channels * 2);
    put16(b, 16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, dataSize);
    for (size_t i = 0; i < frames; i++)
        for (unsigned c = 0; c < channels; c++) {
            const long v = std::clamp(std::lround(double(ch[c][i]) * 32768.0), -32768L, 32767L);
            put16(b, uint16_t(int16_t(v)));
        }
    std::ofstream f(path, std::ios::binary);
    if (!f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()))) {
        err = "cannot write " + path;
        return false;
    }
    return true;
}
