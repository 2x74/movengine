#pragma once

#include <cstdint>
#include <vector>

namespace bsp {

struct DecodedTexture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba8888;  // width * height * 4 bytes
};

}  // namespace bsp
