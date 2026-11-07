#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace snk {

// Persistence for the MVS battery-backed RAM (64 KiB at 0xD00000).
//
// File format: MAME's "saveram" NVRAM layout -- 16-bit words stored
// little-endian -- so files can be exchanged with MAME
// (<nvram dir>/<set>/saveram). In memory the RAM is in 68000 (big-endian)
// order, so every word is byte-swapped on load and save.
namespace backup_ram {

enum class LoadResult { Loaded, Missing, WrongSize };

// Fills `ram` from `path`. On Missing / WrongSize, `ram` is left untouched.
LoadResult load(const std::filesystem::path& path, std::span<uint8_t> ram);

// Writes atomically (temporary file + rename), creating parent folders.
// Throws std::runtime_error on failure.
void save(const std::filesystem::path& path, std::span<const uint8_t> ram);

}  // namespace backup_ram
}  // namespace snk
