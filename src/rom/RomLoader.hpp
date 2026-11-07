#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace snk {

class System;

// What a file of a game set holds, decided from its MAME-style name.
enum class RomRole : uint8_t {
    Program,  // P ROMs (xxx-p1.p1, xxx-p2.sp2, ...): 68000 code
    Fix,      // S ROM  (xxx-s1.s1): fix layer tiles
    Music,    // M ROM  (xxx-m1.m1): Z80 code
    Sprite,   // C ROMs (xxx-c1.c1, xxx-c2.c2, ...): sprite tiles, in odd/even pairs
    AdpcmA,   // V ROMs (xxx-v1.v1, ... or xxx-v11.v11, ...): ADPCM-A samples
    AdpcmB,   // V ROMs (xxx-v21.v21, ...): separate ADPCM-B samples (older boards)
    Unknown,
};

[[nodiscard]] const char* romRoleName(RomRole role) noexcept;

struct RomFileInfo {
    std::string name;        // file name inside the archive / directory
    RomRole     role       = RomRole::Unknown;
    uint32_t    index      = 0;  // P1 -> 1, C4 -> 4, V21 -> 1 (within ADPCM-B) ...
    size_t      size       = 0;
    uint32_t    crc32      = 0;
    bool        fromParent = false;
};

// One C ROM pair: the odd ROM holds bitplanes 0-1, the even ROM bitplanes 2-3.
struct SpriteRomPair {
    std::vector<uint8_t> odd;   // C1, C3, C5, ...
    std::vector<uint8_t> even;  // C2, C4, C6, ...
};

// Loads a Neo-Geo game set named the MAME way, from a .zip archive or a
// directory, into one isolated buffer per chip:
//
//   pRom()   68000 program. Normalized to the 68000's big-endian byte order:
//            MAME dumps are usually word-swapped, so every 16-bit word is
//            byte-swapped at load (detected from the "NEO-GEO" header in P1).
//            MemoryBus keeps this layout and converts on access with
//            endian::read_be16, so no swap ever happens at run time.
//            P1 sits at offset 0, P2 and later at 0x100000 onwards.
//   mRom()   Z80 program              sRom()  fix layer tiles
//   cRoms()  sprite tile pairs        vRomA() / vRomB()  ADPCM samples
//
// Not supported yet: encrypted sets (CMC sprites/fix, SMA program, PCM2
// samples), which MAME decrypts per game. The loader reports a clear error
// when a set looks like one.
//
// The loader is a staging area: install() copies everything into a System,
// after which the loader can be destroyed to free its copies.
class RomLoader {
public:
    // Throws RomLoadError. `parent`: the parent set of a MAME clone (a clone
    // archive only contains the files that differ from its parent).
    void load(const std::filesystem::path& set, const std::filesystem::path& parent = {});
    void clear() noexcept;

    [[nodiscard]] std::span<const uint8_t>          pRom() const noexcept { return pRom_; }
    [[nodiscard]] std::span<const uint8_t>          mRom() const noexcept { return mRom_; }
    [[nodiscard]] std::span<const uint8_t>          sRom() const noexcept { return sRom_; }
    [[nodiscard]] const std::vector<SpriteRomPair>& cRoms() const noexcept { return cRoms_; }
    [[nodiscard]] std::span<const uint8_t>          vRomA() const noexcept { return vRomA_; }
    // Empty when the set has no separate ADPCM-B ROMs (ADPCM-B then reads vRomA()).
    [[nodiscard]] std::span<const uint8_t>          vRomB() const noexcept { return vRomB_; }

    [[nodiscard]] const std::string&              setName() const noexcept { return setName_; }
    [[nodiscard]] const std::vector<RomFileInfo>& files() const noexcept { return files_; }
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    [[nodiscard]] bool programWasWordSwapped() const noexcept { return programWordSwapped_; }
    [[nodiscard]] bool programHalvesSwapped() const noexcept { return programHalvesSwapped_; }
    [[nodiscard]] bool loaded() const noexcept { return !pRom_.empty(); }

    // Replaces the game ROMs of `system` (BIOS untouched) and resets it.
    void install(System& system) const;

private:
    std::string                setName_;
    std::vector<uint8_t>       pRom_, mRom_, sRom_, vRomA_, vRomB_;
    std::vector<SpriteRomPair> cRoms_;
    std::vector<RomFileInfo>   files_;
    std::vector<std::string>   warnings_;
    bool                       programWordSwapped_   = false;
    bool                       programHalvesSwapped_ = false;
};

}  // namespace snk
