// CRC32, FNV-1a hash and little-endian byte reader/writer (Part 2, 2.2).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gdfwr {

uint32_t crc32(const uint8_t* data, size_t n);

inline uint64_t fnv1a64(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    auto b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
template <class T> inline uint64_t hashPod(uint64_t h, T v) { return fnv1a64(&v, sizeof(T), h); }

class ByteWriter {
public:
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; i++) buf.push_back(uint8_t(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) buf.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; i++) buf.push_back(uint8_t(v >> (8 * i))); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void i64(int64_t v) { u64(uint64_t(v)); }
    void f32(float v) { uint32_t b; std::memcpy(&b, &v, 4); u32(b); }
    void f64(double v) { uint64_t b; std::memcpy(&b, &v, 8); u64(b); }
    void str(const std::string& s) { u32(uint32_t(s.size())); buf.insert(buf.end(), s.begin(), s.end()); }
    void bytes(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }
    std::vector<uint8_t> buf;
};

class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    bool atEnd() const { return pos_ >= n_; }
    size_t left() const { return ok_ ? n_ - pos_ : 0; }
    uint8_t u8() { return uint8_t(take(1)); }
    uint16_t u16() { return uint16_t(take(2)); }
    uint32_t u32() { return uint32_t(take(4)); }
    uint64_t u64() { return take(8); }
    int32_t i32() { return int32_t(u32()); }
    int64_t i64() { return int64_t(u64()); }
    float f32() { uint32_t b = u32(); float v; std::memcpy(&v, &b, 4); return v; }
    double f64() { uint64_t b = u64(); double v; std::memcpy(&v, &b, 8); return v; }
    std::string str() {
        uint32_t len = u32();
        if (!ok_ || len > left()) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        return s;
    }
    // a count that must be satisfiable by the remaining bytes (guards against hostile sizes)
    uint32_t count(size_t minBytesPerItem) {
        uint32_t c = u32();
        if (!ok_ || (minBytesPerItem && size_t(c) > left() / minBytesPerItem)) { ok_ = false; return 0; }
        return c;
    }
    void fail() { ok_ = false; }
private:
    uint64_t take(int n) {
        if (!ok_ || size_t(n) > n_ - pos_) { ok_ = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < n; i++) v |= uint64_t(p_[pos_ + i]) << (8 * i);
        pos_ += size_t(n);
        return v;
    }
    const uint8_t* p_;
    size_t n_, pos_ = 0;
    bool ok_ = true;
};

}  // namespace gdfwr
