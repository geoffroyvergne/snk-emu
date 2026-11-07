#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "input/InputPorts.hpp"

namespace snk {

class System;

// Loads the Neo-Geo system ROMs from a MAME-style BIOS set (neogeo.zip or a
// directory):
//
//   68000 BIOS   sp-s2.sp1, uni-bios_4_0.rom, ...  -> 0xC00000. Its vector
//                table is what the 68000 fetches at reset (REG_SWPBIOS).
//   sm1.sm1      Z80 sound BIOS
//   sfix.sfix    BIOS fix-layer tiles (the BIOS's own text and menus)
//   000-lo.lo    vertical-shrink lookup table (optional: approximated without it)
//
// A set usually holds several 68000 BIOS revisions. They are recognized by
// content, not name: the reset PC (offset 4) must point into 0xC00000-0xCFFFFF,
// which also reveals whether the dump is word-swapped. Images are normalized
// to 68000 (big-endian) byte order, like RomLoader does for P ROMs.
class BiosLoader {
public:
    // Throws RomLoadError. `preferredFile` picks a specific 68000 BIOS by file
    // name (case-insensitive); otherwise MAME's default order applies.
    void load(const std::filesystem::path& biosSet, std::string_view preferredFile = {});
    void clear() noexcept;

    [[nodiscard]] std::span<const uint8_t> mainBios() const noexcept { return mainBios_; }
    [[nodiscard]] std::span<const uint8_t> soundBios() const noexcept { return soundBios_; }
    [[nodiscard]] std::span<const uint8_t> fixBios() const noexcept { return fixBios_; }
    [[nodiscard]] std::span<const uint8_t> zoomRom() const noexcept { return zoomRom_; }

    [[nodiscard]] const std::string&              selectedFile() const noexcept { return selected_; }
    [[nodiscard]] const std::vector<std::string>& candidates() const noexcept { return candidates_; }
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    [[nodiscard]] SystemType systemType() const noexcept { return systemType_; }
    [[nodiscard]] bool wasWordSwapped() const noexcept { return wordSwapped_; }
    [[nodiscard]] bool loaded() const noexcept { return !mainBios_.empty(); }

    // Replaces the system ROMs of `system`, sets MVS/AES on the input
    // registers, and resets it (the 68000 then fetches the BIOS vectors).
    void install(System& system) const;

private:
    std::vector<uint8_t>     mainBios_, soundBios_, fixBios_, zoomRom_;
    std::string              selected_;
    std::vector<std::string> candidates_;
    std::vector<std::string> warnings_;
    SystemType               systemType_  = SystemType::Mvs;
    bool                     wordSwapped_ = false;
};

}  // namespace snk
