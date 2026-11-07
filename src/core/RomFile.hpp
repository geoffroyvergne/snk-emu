#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace snk {

class RomLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Reads a whole ROM file, refusing anything larger than `maxSize` before allocating.
inline std::vector<uint8_t> readRomFile(const std::filesystem::path& path, size_t maxSize) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw RomLoadError(path.string() + ": " + ec.message());
    }
    if (size > maxSize) {
        throw RomLoadError(path.string() + ": file is " + std::to_string(size) +
                           " bytes, limit is " + std::to_string(maxSize));
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size))) {
        throw RomLoadError(path.string() + ": read failed");
    }
    return buffer;
}

}  // namespace snk
