#include "rom/RomLoader.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>
#include <utility>

#include <miniz.h>

#include "audio/SoundSystem.hpp"
#include "core/Endian.hpp"
#include "core/RomFile.hpp"
#include "core/System.hpp"
#include "rom/RomSource.hpp"

namespace snk {

namespace fs = std::filesystem;

namespace {

using romsrc::openSource;
using romsrc::RomSource;
using romsrc::SourceEntry;
using romsrc::SourceError;
using romsrc::toLower;

constexpr size_t kMiB            = 0x10'0000;
constexpr size_t kMaxSetSize     = 768 * kMiB;  // guards against zip bombs
constexpr size_t kMaxProgramSize = 9 * kMiB;    // MemoryBus limit: 1 MiB fixed + 8 banks

// ---------------------------------------------------------------------------
// MAME naming: role and index from "xxx-c3.c3", "xxx-p2.sp2", "xxx-v21.v21"...
// The extension is the primary key; "foo-p1.bin"-style names fall back to the
// part of the stem after the last '-'.
// ---------------------------------------------------------------------------
struct Classification {
    RomRole  role  = RomRole::Unknown;
    uint32_t index = 0;
};

Classification classifyToken(std::string_view token) {
    size_t split = 0;
    while (split < token.size() && std::isalpha(static_cast<unsigned char>(token[split]))) {
        ++split;
    }
    const std::string_view prefix = token.substr(0, split);
    const std::string_view digits = token.substr(split);
    if (prefix.empty() || digits.empty() || digits.size() > 2 ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        return {};
    }
    const auto number = static_cast<uint32_t>(std::stoul(std::string(digits)));
    if (number == 0) {
        return {};
    }

    if (prefix == "p" || prefix == "sp" || prefix == "ep" || prefix == "pg" || prefix == "pn" || prefix == "hp") {
        return {RomRole::Program, number};
    }
    if (prefix == "s") return {RomRole::Fix, number};
    if (prefix == "m") return {RomRole::Music, number};
    if (prefix == "c") return {RomRole::Sprite, number};
    if (prefix == "v") {
        // Two-digit "vGN": group G (1 = ADPCM-A, 2 = ADPCM-B), file N.
        if (number >= 11 && number <= 29 && number % 10 != 0) {
            return {number < 20 ? RomRole::AdpcmA : RomRole::AdpcmB, number % 10};
        }
        return {RomRole::AdpcmA, number};
    }
    return {};
}

Classification classifyName(std::string_view fileName) {
    const std::string lower = toLower(fileName);
    const size_t      dot   = lower.rfind('.');
    if (dot == std::string::npos) {
        return {};
    }
    if (const auto byExtension = classifyToken(std::string_view(lower).substr(dot + 1));
        byExtension.role != RomRole::Unknown) {
        return byExtension;
    }
    const std::string_view stem = std::string_view(lower).substr(0, dot);
    const size_t           dash = stem.rfind('-');
    return classifyToken(dash == std::string_view::npos ? stem : stem.substr(dash + 1));
}

// ---------------------------------------------------------------------------
// P ROM byte order: every cartridge program has "NEO-GEO" at 0x100.
// ---------------------------------------------------------------------------
enum class HeaderLayout { NotFound, BigEndian, WordSwapped };

HeaderLayout headerAt(std::span<const uint8_t> data, size_t offset) {
    const auto matches = [&](std::string_view text) {
        return data.size() >= offset + text.size() &&
               std::equal(text.begin(), text.end(), data.begin() + static_cast<std::ptrdiff_t>(offset),
                          [](char c, uint8_t b) { return static_cast<uint8_t>(c) == b; });
    };
    if (matches("NEO-GE")) return HeaderLayout::BigEndian;
    if (matches("EN-OEG")) return HeaderLayout::WordSwapped;
    return HeaderLayout::NotFound;
}

}  // namespace

const char* romRoleName(RomRole role) noexcept {
    switch (role) {
        case RomRole::Program: return "P (68000 program)";
        case RomRole::Fix:     return "S (fix layer)";
        case RomRole::Music:   return "M (Z80 program)";
        case RomRole::Sprite:  return "C (sprites)";
        case RomRole::AdpcmA:  return "V (ADPCM-A)";
        case RomRole::AdpcmB:  return "V (ADPCM-B)";
        case RomRole::Unknown: break;
    }
    return "unknown";
}

void RomLoader::clear() noexcept {
    setName_.clear();
    pRom_.clear();
    mRom_.clear();
    sRom_.clear();
    vRomA_.clear();
    vRomB_.clear();
    cRoms_.clear();
    files_.clear();
    warnings_.clear();
    programWordSwapped_   = false;
    programHalvesSwapped_ = false;
}

void RomLoader::load(const fs::path& set, const fs::path& parent) {
    clear();
    try {
        std::vector<std::unique_ptr<RomSource>> sources;
        sources.push_back(openSource(set));
        if (!parent.empty()) {
            sources.push_back(openSource(parent));
        }
        setName_ = set.stem().string();

        // ---- Pick one file per (role, index); the set wins over its parent ----
        struct Pick {
            RomSource*         source;
            const SourceEntry* entry;
            bool               fromParent;
        };
        std::map<std::pair<RomRole, uint32_t>, Pick> picks;
        for (size_t s = 0; s < sources.size(); ++s) {
            for (const SourceEntry& entry : sources[s]->entries()) {
                const Classification c = classifyName(entry.name);
                if (c.role == RomRole::Unknown) {
                    if (s == 0) warnings_.push_back("ignored unrecognized file: " + entry.name);
                    continue;
                }
                picks.try_emplace({c.role, c.index}, Pick{sources[s].get(), &entry, s != 0});
            }
        }
        const auto collect = [&](RomRole role) {
            std::vector<std::pair<uint32_t, Pick>> list;
            for (const auto& [key, pick] : picks) {
                if (key.first == role) list.emplace_back(key.second, pick);
            }
            return list;  // sorted by index (std::map order)
        };

        size_t totalBytes = 0;
        const auto readPick = [&](RomRole role, uint32_t index, const Pick& pick) {
            if (totalBytes + pick.entry->size > kMaxSetSize) {
                throw RomLoadError("set is larger than " + std::to_string(kMaxSetSize / kMiB) + " MiB");
            }
            std::vector<uint8_t> data = pick.source->read(*pick.entry);
            totalBytes += data.size();
            const uint32_t crc = pick.entry->crcKnown
                                     ? pick.entry->crc
                                     : static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data.data(), data.size()));
            files_.push_back({pick.entry->name, role, index, data.size(), crc, pick.fromParent});
            return data;
        };

        const auto programs = collect(RomRole::Program);
        const auto fixes    = collect(RomRole::Fix);
        const auto musics   = collect(RomRole::Music);
        const auto sprites  = collect(RomRole::Sprite);
        const auto adpcmA   = collect(RomRole::AdpcmA);
        const auto adpcmB   = collect(RomRole::AdpcmB);

        // ---- Validate the set's shape before reading megabytes ----
        if (programs.empty() || programs.front().first != 1) {
            throw RomLoadError("no P1 program ROM found (expected e.g. xxx-p1.p1)");
        }
        if (musics.empty()) {
            throw RomLoadError("no M1 Z80 ROM found (expected e.g. xxx-m1.m1)");
        }
        if (sprites.empty()) {
            throw RomLoadError("no C sprite ROMs found (expected xxx-c1.c1, xxx-c2.c2, ...)");
        }
        if (fixes.empty()) {
            throw RomLoadError("no S1 fix ROM: this looks like a CMC-encrypted set (fix tiles stored in "
                               "the C ROMs), which is not supported yet");
        }
        if (adpcmA.empty()) {
            throw RomLoadError("no V sample ROMs found (expected xxx-v1.v1, ...)");
        }
        for (size_t i = 0; i < sprites.size(); ++i) {
            if (sprites[i].first != i + 1) {
                throw RomLoadError("C ROMs must be numbered C1..Cn without gaps (C" + std::to_string(i + 1) +
                                   " is missing)");
            }
        }
        if (sprites.size() % 2 != 0) {
            throw RomLoadError("C ROMs come in odd/even pairs, found an odd count (" +
                               std::to_string(sprites.size()) + ")");
        }

        // ---- P ROMs ----
        std::vector<uint8_t> p1 = readPick(RomRole::Program, 1, programs.front().second);
        if (p1.empty() || p1.size() % 2 != 0) {
            throw RomLoadError(programs.front().second.entry->name + ": P ROM size must be even");
        }
        HeaderLayout layout     = headerAt(p1, 0x100);
        bool         headerHigh = false;
        if (layout == HeaderLayout::NotFound && p1.size() >= 2 * kMiB) {
            layout     = headerAt(p1, kMiB + 0x100);
            headerHigh = layout != HeaderLayout::NotFound;
        }
        if (layout == HeaderLayout::NotFound) {
            warnings_.push_back("P1 has no NEO-GEO header (encrypted or bootleg program?); "
                                "assuming the usual word-swapped layout");
        }
        programWordSwapped_ = layout != HeaderLayout::BigEndian;
        if (programWordSwapped_) {
            endian::swap_words_in_place(p1);  // -> 68000 big-endian order
        }
        if (headerHigh) {
            // 2 MiB P1 stored with its halves exchanged: the vectors and header
            // must end up at 0x000000.
            std::rotate(p1.begin(), p1.begin() + static_cast<std::ptrdiff_t>(kMiB), p1.end());
            programHalvesSwapped_ = true;
        }
        pRom_ = std::move(p1);

        for (size_t i = 1; i < programs.size(); ++i) {
            std::vector<uint8_t> px = readPick(RomRole::Program, programs[i].first, programs[i].second);
            if (px.size() % 2 != 0) {
                throw RomLoadError(programs[i].second.entry->name + ": P ROM size must be even");
            }
            if (programWordSwapped_) {
                endian::swap_words_in_place(px);
            }
            // P2 and later live in the banked area, starting at 0x100000.
            const size_t offset = std::max(pRom_.size(), kMiB);
            pRom_.resize(offset, 0xFF);
            pRom_.insert(pRom_.end(), px.begin(), px.end());
        }
        if (pRom_.size() > kMaxProgramSize) {
            throw RomLoadError("P ROMs total " + std::to_string(pRom_.size()) + " bytes, more than 9 MiB");
        }

        // ---- M, S ----
        mRom_ = readPick(RomRole::Music, musics.front().first, musics.front().second);
        sRom_ = readPick(RomRole::Fix, fixes.front().first, fixes.front().second);
        if (musics.size() > 1 || fixes.size() > 1) {
            warnings_.push_back("more than one M or S ROM; only the first of each is used");
        }

        // ---- C pairs ----
        for (size_t i = 0; i < sprites.size(); i += 2) {
            SpriteRomPair pair;
            pair.odd  = readPick(RomRole::Sprite, sprites[i].first, sprites[i].second);
            pair.even = readPick(RomRole::Sprite, sprites[i + 1].first, sprites[i + 1].second);
            if (pair.odd.size() != pair.even.size()) {
                throw RomLoadError(sprites[i].second.entry->name + " and " + sprites[i + 1].second.entry->name +
                                   " differ in size");
            }
            cRoms_.push_back(std::move(pair));
        }

        // ---- V: concatenated in file order ----
        const auto concat = [&](RomRole role, const auto& list, std::vector<uint8_t>& out) {
            for (const auto& [index, pick] : list) {
                std::vector<uint8_t> data = readPick(role, index, pick);
                out.insert(out.end(), data.begin(), data.end());
            }
        };
        concat(RomRole::AdpcmA, adpcmA, vRomA_);
        concat(RomRole::AdpcmB, adpcmB, vRomB_);
    } catch (const SourceError& e) {
        const std::string message = e.what();
        clear();
        throw RomLoadError(message);
    } catch (const RomLoadError& e) {
        const std::string message = set.string() + ": " + e.what();
        clear();
        throw RomLoadError(message);
    }
}

void RomLoader::install(System& system) const {
    if (!loaded()) {
        throw RomLoadError("RomLoader::install: no set loaded");
    }
    MemoryBus& bus = system.memory();
    bus.unloadRom(RomType::PRom);
    bus.unloadRom(RomType::SRom);
    bus.loadRom(RomType::PRom, pRom_, 0, RomByteOrder::BigEndian);  // already normalized
    bus.loadRom(RomType::SRom, sRom_);

    SoundSystem& sound = system.sound();
    sound.unloadRom(SoundRom::M1);
    sound.unloadRom(SoundRom::AdpcmA);
    sound.unloadRom(SoundRom::AdpcmB);
    sound.loadRom(SoundRom::M1, mRom_);
    sound.loadRom(SoundRom::AdpcmA, vRomA_);
    if (!vRomB_.empty()) {
        sound.loadRom(SoundRom::AdpcmB, vRomB_);
    }

    VideoSystem& video = system.video();
    video.clearSpriteRoms();
    for (const SpriteRomPair& pair : cRoms_) {
        video.addSpriteRomPair(pair.odd, pair.even);
    }

    system.reset();
}

}  // namespace snk
