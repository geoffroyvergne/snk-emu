#include "rom/BiosLoader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>

#include "audio/SoundSystem.hpp"
#include "core/Endian.hpp"
#include "core/System.hpp"
#include "rom/RomSource.hpp"

namespace snk {

namespace fs = std::filesystem;

namespace {

using romsrc::RomSource;
using romsrc::SourceEntry;
using romsrc::SourceError;
using romsrc::toLower;

// MAME's preference order for the MVS BIOS (its default is "euro": sp-s2.sp1).
// Anything not listed comes after, Universe BIOS first, then alphabetical.
constexpr std::array<std::string_view, 11> kPreferredMainBios = {
    "sp-s2.sp1", "sp-s3.sp1", "sp-s.sp1",   "sp-u2.sp1",      "sp-e.sp1",     "asia-s3.rom",
    "vs-bios.rom", "sp-j2.sp1", "sp-45.sp1", "sp1.jipan.1024", "japan-j3.bin",
};

bool isSystemFile(std::string_view lowerName) {
    return lowerName == "sm1.sm1" || lowerName == "sfix.sfix" || lowerName == "sfix.sfx" ||
           lowerName == "000-lo.lo";
}

// AES (home console) BIOS images; everything else is treated as MVS.
bool isAesBios(std::string_view lowerName) {
    return lowerName.starts_with("neo-po") || lowerName.starts_with("neo-epo") ||
           lowerName.starts_with("neodebug");
}

bool resetPcInBios(std::span<const uint8_t> data) {
    const uint32_t pc = endian::read_be32(&data[4]);
    return (pc & 0xFFF0'0000u) == 0x00C0'0000u;
}

// nullopt: not a 68000 BIOS. Otherwise: whether the dump is word-swapped.
std::optional<bool> mainBiosLayout(std::span<const uint8_t> data) {
    if (data.size() < 0x2'0000 || data.size() > 0x10'0000 || !std::has_single_bit(data.size())) {
        return std::nullopt;
    }
    if (resetPcInBios(data)) {
        return false;
    }
    const std::array<uint8_t, 8> swapped = {data[1], data[0], data[3], data[2], data[5], data[4], data[7], data[6]};
    if (resetPcInBios(swapped)) {
        return true;
    }
    return std::nullopt;
}

int preferenceRank(std::string_view lowerName) {
    const auto it = std::find(kPreferredMainBios.begin(), kPreferredMainBios.end(), lowerName);
    if (it != kPreferredMainBios.end()) {
        return static_cast<int>(it - kPreferredMainBios.begin());
    }
    if (lowerName.starts_with("uni-bios")) {
        return 100;
    }
    return isAesBios(lowerName) ? 300 : 200;
}

}  // namespace

void BiosLoader::clear() noexcept {
    mainBios_.clear();
    soundBios_.clear();
    fixBios_.clear();
    zoomRom_.clear();
    selected_.clear();
    candidates_.clear();
    warnings_.clear();
    systemType_  = SystemType::Mvs;
    wordSwapped_ = false;
}

void BiosLoader::load(const fs::path& biosSet, std::string_view preferredFile) {
    clear();
    try {
        const auto source = romsrc::openSource(biosSet);

        struct Candidate {
            const SourceEntry*   entry;
            std::string          lowerName;
            std::vector<uint8_t> data;
            bool                 wordSwapped;
        };
        std::vector<Candidate> found;
        const SourceEntry *sm1 = nullptr, *sfix = nullptr, *lo = nullptr;

        for (const SourceEntry& entry : source->entries()) {
            const std::string lower = toLower(entry.name);
            if (lower == "sm1.sm1") { sm1 = &entry; continue; }
            if (lower == "sfix.sfix" || lower == "sfix.sfx") { sfix = &entry; continue; }
            if (lower == "000-lo.lo") { lo = &entry; continue; }
            if (isSystemFile(lower) || entry.size < 0x2'0000 || entry.size > 0x10'0000) {
                continue;  // cheap size filter before reading anything
            }
            std::vector<uint8_t> data = source->read(entry);
            if (const auto layout = mainBiosLayout(data)) {
                found.push_back({&entry, lower, std::move(data), *layout});
            }
        }
        if (found.empty()) {
            throw RomLoadError("no 68000 BIOS image found (expected e.g. sp-s2.sp1 in neogeo.zip)");
        }

        std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
            const int ra = preferenceRank(a.lowerName), rb = preferenceRank(b.lowerName);
            if (ra != rb) return ra < rb;
            // Among Universe BIOS versions, prefer the newest (highest name).
            return ra == 100 ? a.lowerName > b.lowerName : a.lowerName < b.lowerName;
        });
        for (const Candidate& c : found) {
            candidates_.push_back(c.entry->name);
        }

        auto chosen = found.begin();
        if (!preferredFile.empty()) {
            const std::string want = toLower(preferredFile);
            chosen = std::find_if(found.begin(), found.end(), [&](const Candidate& c) { return c.lowerName == want; });
            if (chosen == found.end()) {
                std::string list;
                for (const auto& name : candidates_) list += (list.empty() ? "" : ", ") + name;
                throw RomLoadError("BIOS '" + std::string(preferredFile) + "' not found; available: " + list);
            }
        }

        mainBios_    = std::move(chosen->data);
        wordSwapped_ = chosen->wordSwapped;
        if (wordSwapped_) {
            endian::swap_words_in_place(mainBios_);  // -> 68000 big-endian order
        }
        selected_   = chosen->entry->name;
        systemType_ = isAesBios(chosen->lowerName) ? SystemType::Aes : SystemType::Mvs;

        if (sm1 != nullptr) {
            soundBios_ = source->read(*sm1);
        } else {
            warnings_.push_back("sm1.sm1 missing: the Z80 has no sound BIOS");
        }
        if (sfix != nullptr) {
            fixBios_ = source->read(*sfix);
        } else {
            warnings_.push_back("sfix.sfix missing: BIOS text will be invisible");
        }
        if (lo != nullptr) {
            zoomRom_ = source->read(*lo);
            if (zoomRom_.size() < 0x1'0000) {
                warnings_.push_back("000-lo.lo is too small; using the approximate shrink table");
                zoomRom_.clear();
            }
        } else {
            warnings_.push_back("000-lo.lo missing: using the approximate vertical-shrink table");
        }
    } catch (const SourceError& e) {
        const std::string message = e.what();
        clear();
        throw RomLoadError(message);
    } catch (const RomLoadError& e) {
        const std::string message = biosSet.string() + ": " + e.what();
        clear();
        throw RomLoadError(message);
    }
}

void BiosLoader::install(System& system) const {
    if (!loaded()) {
        throw RomLoadError("BiosLoader::install: no BIOS loaded");
    }
    MemoryBus& bus = system.memory();
    bus.unloadRom(RomType::Bios);
    bus.unloadRom(RomType::SFix);
    bus.loadRom(RomType::Bios, mainBios_, 0, RomByteOrder::BigEndian);  // already normalized
    if (!fixBios_.empty()) {
        bus.loadRom(RomType::SFix, fixBios_);
    }

    system.sound().unloadRom(SoundRom::BiosM);
    if (!soundBios_.empty()) {
        system.sound().loadRom(SoundRom::BiosM, soundBios_);
    }
    if (!zoomRom_.empty()) {
        system.video().loadZoomRom(zoomRom_);
    }
    system.inputs().setSystemType(systemType_);

    system.reset();  // REG_SWPBIOS on, 68000 fetches SSP/PC from the BIOS vectors
}

}  // namespace snk
