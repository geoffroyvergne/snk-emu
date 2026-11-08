#include "rom/RomLoader.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>
#include <utility>

#include <miniz.h>

#include "audio/SoundSystem.hpp"
#include "cart/Cmc.hpp"
#include "cart/Kof2k2Cipher.hpp"
#include "cart/Pcm2.hpp"
#include "cart/PvcProtection.hpp"
#include "cart/SmaProtection.hpp"
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
    const std::string_view stem   = std::string_view(lower).substr(0, dot);
    const size_t           dash   = stem.rfind('-');
    const Classification   byStem = classifyToken(dash == std::string_view::npos ? stem : stem.substr(dash + 1));
    const Classification   byExt  = classifyToken(std::string_view(lower).substr(dot + 1));
    if (byExt.role == RomRole::Unknown) {
        return byStem;
    }
    // "250-p2.ep1": the extension says "EPROM 1" but the stem holds the real
    // position (P2). When both name the same role, the stem's number wins.
    if (byStem.role == byExt.role) {
        return byStem;
    }
    return byExt;
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
    cart_                 = nullptr;
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
                    if (s == 0 && !isSmaCodeName(entry.name)) warnings_.push_back("ignored unrecognized file: " + entry.name);
                    continue;
                }
                const auto [it, inserted] =
                    picks.try_emplace({c.role, c.index}, Pick{sources[s].get(), &entry, s != 0});
                if (!inserted && !it->second.fromParent && s == 0) {
                    warnings_.push_back("ignored " + entry.name + ": same slot as " + it->second.entry->name);
                }
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
        cart_ = cart::findCartridge(files_.back().crc32);  // custom hardware, keyed by the P1 dump
        if (p1.empty() || p1.size() % 2 != 0) {
            throw RomLoadError(programs.front().second.entry->name + ": P ROM size must be even");
        }
        if (fixes.empty() && !(cart_ && cart_->gfx)) {
            throw RomLoadError("no S1 fix ROM: this looks like a CMC-encrypted set (fix tiles stored in "
                               "the C ROMs) that this emulator does not know yet");
        }
        if (cart_ && cart_->layout != cart::ProgramLayout::Standard) {
            std::vector<std::vector<uint8_t>> rest;
            for (size_t i = 1; i < programs.size(); ++i) {
                rest.push_back(readPick(RomRole::Program, programs[i].first, programs[i].second));
            }
            std::vector<uint8_t> smaCode;
            if (cart_->smaCode) {
                smaCode = readSmaCode(sources);
            }
            pRom_ = assembleProgram(*cart_, std::move(p1), rest, smaCode);
            programWordSwapped_ = true;
        } else {
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
        }  // standard layout
        if (pRom_.size() > kMaxProgramSize) {
            throw RomLoadError("P ROMs total " + std::to_string(pRom_.size()) + " bytes, more than 9 MiB");
        }

        // ---- M, S ----
        mRom_ = readPick(RomRole::Music, musics.front().first, musics.front().second);
        if (cart_ && cart_->m1Encrypted) {
            mRom_ = cart::cmc::decryptM1(mRom_);
        }
        if (!fixes.empty()) {
            sRom_ = readPick(RomRole::Fix, fixes.front().first, fixes.front().second);
        }
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
        if (cart_ && cart_->gfx) {
            decryptSprites(*cart_);  // also rebuilds the fix ROM from the C data
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
        if (cart_ && cart_->samples == cart::SampleCipher::Pcm2Decrypt) {
            cart::pcm2::decrypt(vRomA_, cart_->sampleParam);
        } else if (cart_ && cart_->samples == cart::SampleCipher::Pcm2Swap) {
            if (vRomA_.size() < 0x100'0000) {
                throw RomLoadError("NEO-PCM2: expected 16 MiB of V ROMs, found " + std::to_string(vRomA_.size()));
            }
            cart::pcm2::swap(vRomA_, cart_->sampleParam);
        }
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

// Program image of an encrypted cartridge. Like every P ROM dump the files are
// word-swapped: each is turned into 68000 byte order first, placed where
// MAME's ROM definition puts it, then decrypted. MAME's routines work on that
// byte order (checked: the results carry the NEO-GEO header and valid vectors).
std::vector<uint8_t> RomLoader::assembleProgram(const cart::CartridgeInfo& info, std::vector<uint8_t> p1,
                                                const std::vector<std::vector<uint8_t>>& rest,
                                                std::vector<uint8_t> smaCode) {
    const std::string name = info.setName;
    std::vector<uint8_t> image(info.programRegionSize, 0);
    const auto place = [&](std::vector<uint8_t> data, size_t at, const char* what) {
        endian::swap_words_in_place(data);
        if (at + data.size() > image.size()) {
            throw RomLoadError(name + ": " + what + " does not fit the " +
                               std::to_string(image.size() / kMiB) + " MiB program image");
        }
        std::copy(data.begin(), data.end(), image.begin() + static_cast<std::ptrdiff_t>(at));
    };

    if (info.layout == cart::ProgramLayout::Interleaved32) {
        // P1/P2 alternate word by word (MAME ROM_LOAD32_WORD_SWAP), optional P3 at 0x800000.
        if (rest.empty() || rest[0].size() != p1.size() || 2 * p1.size() > image.size()) {
            throw RomLoadError(name + ": expected P1 and P2 of the same size (" +
                               std::to_string(info.programRegionSize / 2 / kMiB) + " MiB each)");
        }
        std::vector<uint8_t> p2 = rest[0];
        endian::swap_words_in_place(p1);
        endian::swap_words_in_place(p2);
        for (size_t w = 0; w < p1.size() / 2; ++w) {
            image[4 * w + 0] = p1[2 * w];
            image[4 * w + 1] = p1[2 * w + 1];
            image[4 * w + 2] = p2[2 * w];
            image[4 * w + 3] = p2[2 * w + 1];
        }
        if (rest.size() > 1) {
            place(rest[1], 2 * p1.size(), "P3");
        }
    } else {  // Linear
        size_t at = info.programStart;
        const size_t p1Size = p1.size();
        place(std::move(p1), at, "P1");
        at += p1Size;
        for (size_t i = 0; i < rest.size(); ++i) {
            place(rest[i], at, "P2+");
            at += rest[i].size();
        }
    }
    if (info.smaCode) {
        if (smaCode.size() != 0x4'0000) {
            throw RomLoadError(name + ": the neo-sma dump (SMA chip code) must be 256 KiB");
        }
        place(std::move(smaCode), 0x0C'0000, "neo-sma");
    }

    switch (info.program) {
        case cart::ProgramCipher::PvcMslug5:  PvcProtection::decryptMslug5(image); break;
        case cart::ProgramCipher::PvcSvc:     PvcProtection::decryptSvc(image); break;
        case cart::ProgramCipher::PvcKof2003: PvcProtection::decryptKof2003(image); break;
        case cart::ProgramCipher::Samsho5:    cart::kof2k2::decryptSamsho5(image); break;
        case cart::ProgramCipher::SmaMslug3:  SmaMslug3Protection::decrypt(image); break;
        case cart::ProgramCipher::None:       break;
    }
    if (headerAt(image, 0x100) != HeaderLayout::BigEndian) {
        throw RomLoadError(name + ": program decryption produced no NEO-GEO header (different dump of the set?)");
    }
    return image;
}

// The SMA chip's internal code, dumped as "neo-sma" (MAME: green.neo-sma).
bool RomLoader::isSmaCodeName(const std::string& fileName) {
    const std::string lower = toLower(fileName);
    return lower.size() >= 7 && lower.compare(lower.size() - 7, 7, "neo-sma") == 0;
}

std::vector<uint8_t> RomLoader::readSmaCode(const std::vector<std::unique_ptr<RomSource>>& sources) {
    for (const auto& source : sources) {
        for (const SourceEntry& entry : source->entries()) {
            if (isSmaCodeName(entry.name)) {
                return source->read(entry);
            }
        }
    }
    throw RomLoadError("missing neo-sma (the SMA chip's code, e.g. green.neo-sma)");
}

// CMC: rebuild MAME's sprite region (C1 even bytes, C2 odd bytes, pair after
// pair), decrypt it, take the fix tiles from its end, split it back into pairs.
void RomLoader::decryptSprites(const cart::CartridgeInfo& info) {
    std::vector<size_t> pairSizes;
    size_t total = 0;
    for (const SpriteRomPair& pair : cRoms_) {
        pairSizes.push_back(pair.odd.size());
        total += 2 * pair.odd.size();
    }
    std::vector<uint8_t> region(total);
    size_t at = 0;
    for (SpriteRomPair& pair : cRoms_) {
        for (size_t i = 0; i < pair.odd.size(); ++i) {
            region[at + 2 * i]     = pair.odd[i];
            region[at + 2 * i + 1] = pair.even[i];
        }
        at += 2 * pair.odd.size();
        pair.odd  = {};  // free as we go: the decryption needs a second full copy
        pair.even = {};
    }

    cart::cmc::decryptSprites(region, *info.gfx, info.gfxKey);
    sRom_ = cart::cmc::extractFix(region, info.fixSize);

    at = 0;
    for (size_t p = 0; p < cRoms_.size(); ++p) {
        SpriteRomPair& pair = cRoms_[p];
        const size_t   size = pairSizes[p];
        pair.odd.resize(size);
        pair.even.resize(size);
        for (size_t i = 0; i < size; ++i) {
            pair.odd[i]  = region[at + 2 * i];
            pair.even[i] = region[at + 2 * i + 1];
        }
        at += 2 * size;
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
    bus.attachCartridgeChip(makeCartridgeChip(cartridgeChip()));

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
    video.setFixBanking(cart_ ? cart_->fixBanking : VideoSystem::FixBanking::None);
    video.clearSpriteRoms();
    for (const SpriteRomPair& pair : cRoms_) {
        video.addSpriteRomPair(pair.odd, pair.even);
    }

    system.reset();
}

}  // namespace snk
