#include "content/zip_writer.h"

#include <array>

namespace content {

namespace {

const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void put16(std::string& b, uint16_t v) {
    b.push_back(static_cast<char>(v & 0xff));
    b.push_back(static_cast<char>(v >> 8));
}
void put32(std::string& b, uint32_t v) {
    put16(b, static_cast<uint16_t>(v & 0xffff));
    put16(b, static_cast<uint16_t>(v >> 16));
}
void put64(std::string& b, uint64_t v) {
    put32(b, static_cast<uint32_t>(v & 0xffffffffu));
    put32(b, static_cast<uint32_t>(v >> 32));
}

constexpr uint32_t kMax32 = 0xffffffffu;

}  // namespace

uint32_t crc32(std::span<const std::byte> data) {
    const auto& t = crcTable();
    uint32_t c = 0xffffffffu;
    for (std::byte b : data) c = t[(c ^ static_cast<uint8_t>(b)) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

ZipWriter::ZipWriter(const std::string& path, bool forceZip64)
    : out_(path, std::ios::binary | std::ios::trunc), ok_(static_cast<bool>(out_)), forceZip64_(forceZip64) {}

bool ZipWriter::add(const std::string& name, std::span<const std::byte> data) {
    if (!ok_) return false;
    Entry e{name, crc32(data), data.size(), written_};
    std::string h;
    put32(h, 0x04034b50);
    put16(h, 20);  // version needed
    put16(h, 0);   // flags
    put16(h, 0);   // stored
    put16(h, 0);   // time
    put16(h, 0x21);  // date: 1980-01-01
    put32(h, e.crc);
    put32(h, static_cast<uint32_t>(e.size));  // files here are always under 4 GB
    put32(h, static_cast<uint32_t>(e.size));
    put16(h, static_cast<uint16_t>(name.size()));
    put16(h, 0);
    h += name;
    out_.write(h.data(), static_cast<std::streamsize>(h.size()));
    out_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    written_ += h.size() + data.size();
    entries_.push_back(std::move(e));
    ok_ = static_cast<bool>(out_);
    return ok_;
}

bool ZipWriter::finish() {
    if (!ok_) return false;
    uint64_t dirStart = written_;
    bool zip64 = forceZip64_ || entries_.size() >= 0xffff;
    std::string d;
    for (const auto& e : entries_) {
        bool bigOffset = forceZip64_ || e.offset >= kMax32;
        put32(d, 0x02014b50);
        put16(d, bigOffset ? 45 : 20);  // made by
        put16(d, bigOffset ? 45 : 20);  // needed
        put16(d, 0);
        put16(d, 0);
        put16(d, 0);
        put16(d, 0x21);
        put32(d, e.crc);
        put32(d, static_cast<uint32_t>(e.size));
        put32(d, static_cast<uint32_t>(e.size));
        put16(d, static_cast<uint16_t>(e.name.size()));
        put16(d, bigOffset ? 12 : 0);  // extra: the zip64 offset
        put16(d, 0);                   // comment
        put16(d, 0);                   // disk
        put16(d, 0);                   // internal attributes
        put32(d, 0);                   // external attributes
        put32(d, bigOffset ? kMax32 : static_cast<uint32_t>(e.offset));
        d += e.name;
        if (bigOffset) {
            put16(d, 0x0001);
            put16(d, 8);
            put64(d, e.offset);
            zip64 = true;
        }
    }
    uint64_t dirSize = d.size();
    if (dirStart + dirSize >= kMax32) zip64 = true;
    if (zip64) {
        uint64_t recordAt = dirStart + dirSize;
        put32(d, 0x06064b50);  // zip64 end of central directory
        put64(d, 44);
        put16(d, 45);
        put16(d, 45);
        put32(d, 0);
        put32(d, 0);
        put64(d, entries_.size());
        put64(d, entries_.size());
        put64(d, dirSize);
        put64(d, dirStart);
        put32(d, 0x07064b50);  // its locator
        put32(d, 0);
        put64(d, recordAt);
        put32(d, 1);
    }
    put32(d, 0x06054b50);
    put16(d, 0);
    put16(d, 0);
    uint16_t count = zip64 ? 0xffff : static_cast<uint16_t>(entries_.size());
    put16(d, count);
    put16(d, count);
    put32(d, zip64 ? kMax32 : static_cast<uint32_t>(dirSize));
    put32(d, zip64 ? kMax32 : static_cast<uint32_t>(dirStart));
    put16(d, 0);
    out_.write(d.data(), static_cast<std::streamsize>(d.size()));
    out_.close();
    ok_ = !out_.fail();
    return ok_;
}

}  // namespace content
