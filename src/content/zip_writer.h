#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace content {

// writes a .zip one file at a time, stored (vtfs are already compressed, deflating them saves little and costs a lot of time) so a pack of gigabytes never needs to fit in memory. grows into zip64 when it passes 4 GB or 65535 files
class ZipWriter {
public:
    explicit ZipWriter(const std::string& path, bool forceZip64 = false);
    bool ok() const { return ok_; }
    bool add(const std::string& name, std::span<const std::byte> data);
    bool finish();  // writes the directory; the file is only valid after this

private:
    struct Entry {
        std::string name;
        uint32_t crc;
        uint64_t size;
        uint64_t offset;
    };
    std::ofstream out_;
    std::vector<Entry> entries_;
    uint64_t written_ = 0;
    bool ok_ = false;
    bool forceZip64_ = false;
};

uint32_t crc32(std::span<const std::byte> data);

}  // namespace content
