#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/RomFile.hpp"

// Internal to src/rom: uniform access to the files of a ROM set, whether it
// is a .zip archive or a plain directory.
namespace snk::romsrc {

inline constexpr size_t kMaxFileSize = 128 * 0x10'0000;  // largest single Neo-Geo ROM is 64 MiB

std::string toLower(std::string_view s);

// Errors raised by a source already name the exact archive/file (which may be
// a parent set), so loaders pass them through without another prefix.
class SourceError : public RomLoadError {
public:
    using RomLoadError::RomLoadError;
};

struct SourceEntry {
    std::string name;          // file name, without any folder inside the archive
    uint64_t    size     = 0;
    uint32_t    crc      = 0;
    bool        crcKnown = false;
    uint32_t    handle   = 0;  // zip file index / directory entry index
};

class RomSource {
public:
    virtual ~RomSource() = default;
    [[nodiscard]] const std::vector<SourceEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::string& label() const noexcept { return label_; }
    // Throws SourceError. Zip entries are CRC-checked while inflating.
    virtual std::vector<uint8_t> read(const SourceEntry& entry) = 0;

protected:
    std::vector<SourceEntry> entries_;
    std::string              label_;
};

// A directory -> DirectorySource, anything else -> ZipSource. Throws SourceError.
std::unique_ptr<RomSource> openSource(const std::filesystem::path& path);

}  // namespace snk::romsrc
