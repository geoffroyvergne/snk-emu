#include "memory/MemoryBus.hpp"

#include <algorithm>
#include <bit>
#include <string>
#include <string_view>

#include "audio/SoundSystem.hpp"
#include "core/Interrupts.hpp"
#include "input/InputPorts.hpp"
#include "rtc/Upd4990a.hpp"
#include "video/VideoSystem.hpp"

namespace snk {

namespace {

constexpr size_t kMiB = 0x10'0000;

struct RomInfo {
    const char* name;
    size_t      maxSize;
    bool        wordWide;  // 16-bit ROM on the 68000 bus -> subject to byte-order handling
};

constexpr RomInfo romInfo(RomType type) noexcept {
    switch (type) {
        case RomType::Bios:  return {"BIOS", 1 * kMiB, true};
        case RomType::SFix:  return {"SFIX", 0x2'0000, false};
        case RomType::PRom:  return {"P ROM", 9 * kMiB, true};  // 1 MiB fixed + 8 banks
        case RomType::SRom:  return {"S ROM", 1 * kMiB, false};
    }
    return {"unknown ROM", 0, false};
}

bool bytesMatch(std::span<const uint8_t> data, size_t at, std::string_view text) noexcept {
    if (data.size() < at + text.size()) {
        return false;
    }
    return std::equal(text.begin(), text.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                      [](char c, uint8_t b) { return static_cast<uint8_t>(c) == b; });
}

RomByteOrder resolveOrder(RomType type, std::span<const uint8_t> data, RomByteOrder requested) {
    if (requested != RomByteOrder::Auto) {
        return requested;
    }
    if (!romInfo(type).wordWide) {
        return RomByteOrder::BigEndian;  // 8-bit data: byte order is irrelevant
    }
    // Every cartridge P1 carries "NEO-GEO" at 0x100. In a word-swapped dump the
    // bytes of each pair are exchanged: "EN-OEG".
    if (type == RomType::PRom) {
        if (bytesMatch(data, 0x100, "NEO-GE")) return RomByteOrder::BigEndian;
        if (bytesMatch(data, 0x100, "EN-OEG")) return RomByteOrder::WordSwapped;
    }
    return RomByteOrder::WordSwapped;  // standard layout for BIOS and P2+ dumps
}

// BIOS and the fixed P window are mirrored, so they are padded to a power of
// two and addressed with a mask. Beyond 1 MiB the P ROM is banked in 1 MiB
// units, so it is padded to a whole number of banks instead.
size_t paddedSize(RomType type, size_t used) {
    switch (type) {
        case RomType::Bios:
            return std::bit_ceil(std::max(used, size_t{0x1'0000}));
        case RomType::PRom:
            return used <= kMiB ? std::bit_ceil(std::max(used, size_t{0x1'0000}))
                                : (used + kMiB - 1) / kMiB * kMiB;
        default:
            return used;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / reset
// ---------------------------------------------------------------------------
MemoryBus::MemoryBus() {
    reset();
}

void MemoryBus::reset() {
    workRam_.fill(0);
    paletteRam_.fill(0);
    biosVectors_ = true;
    setCartridgeFix(false);
    sramLocked_  = true;
    shadow_      = false;
    paletteBank_ = 0;
    programBank_ = 0;
    mapAll();
}

// ---------------------------------------------------------------------------
// ROM loading
// ---------------------------------------------------------------------------
MemoryBus::RomImage& MemoryBus::image(RomType type) noexcept {
    switch (type) {
        case RomType::Bios:  return bios_;
        case RomType::SFix:  return sFix_;
        case RomType::PRom:  return pRom_;
        case RomType::SRom:  return sRom_;
    }
    return pRom_;
}

const MemoryBus::RomImage& MemoryBus::image(RomType type) const noexcept {
    return const_cast<MemoryBus*>(this)->image(type);
}

void MemoryBus::loadRom(RomType type, std::span<const uint8_t> data, uint32_t offset,
                        RomByteOrder order) {
    const RomInfo info = romInfo(type);
    const size_t  end  = size_t{offset} + data.size();

    if (data.empty()) {
        throw RomLoadError(std::string(info.name) + ": image is empty");
    }
    if (end > info.maxSize) {
        throw RomLoadError(std::string(info.name) + ": image would end at byte " +
                           std::to_string(end) + ", limit is " + std::to_string(info.maxSize));
    }
    if (info.wordWide && ((offset | data.size()) & 1u) != 0) {
        throw RomLoadError(std::string(info.name) + ": 16-bit ROM needs an even size and offset");
    }

    RomImage& rom = image(type);
    if (rom.data.size() < end) {
        rom.data.resize(end, kOpenBus);
    }
    const auto dst = std::span(rom.data).subspan(offset, data.size());
    std::copy(data.begin(), data.end(), dst.begin());

    // Convert to the 68000's big-endian layout once, at load time, so the hot
    // read path never has to care how the dump was stored.
    if (resolveOrder(type, data, order) == RomByteOrder::WordSwapped) {
        endian::swap_words_in_place(dst);
    }

    rom.used = std::max(rom.used, end);
    rom.data.resize(std::max(rom.data.size(), paddedSize(type, rom.used)), kOpenBus);
    mapAll();
}

void MemoryBus::loadRomFile(RomType type, const std::filesystem::path& path, uint32_t offset,
                            RomByteOrder order) {
    const auto buffer = readRomFile(path, romInfo(type).maxSize);
    try {
        loadRom(type, buffer, offset, order);
    } catch (const RomLoadError& e) {
        throw RomLoadError(path.string() + ": " + e.what());
    }
}

void MemoryBus::unloadRom(RomType type) {
    image(type) = RomImage{};
    mapAll();
}

// ---------------------------------------------------------------------------
// Page table
// ---------------------------------------------------------------------------
void MemoryBus::mapRange(uint32_t firstPage, uint32_t lastPage, const uint8_t* read,
                         uint8_t* write, uint32_t mask) noexcept {
    for (uint32_t p = firstPage; p <= lastPage; ++p) {
        pages_[p] = Page{read, write, mask};
    }
}

void MemoryBus::mapAll() {
    pages_.fill(Page{});  // everything unmapped -> slow path -> open bus

    // P ROM fixed window, mirrored if smaller than 1 MiB.
    if (pRom_.used != 0) {
        const auto window = static_cast<uint32_t>(std::min(pRom_.data.size(), kMiB));
        mapRange(0x00, 0x0F, pRom_.data.data(), nullptr, window - 1);
    }
    mapVectors();

    mapRange(0x10, 0x1F, workRam_.data(), workRam_.data(), kWorkRamSize - 1);
    mapBankedProgram();
    mapPalette();

    if (bios_.used != 0) {
        mapRange(0xC0, 0xCF, bios_.data.data(), nullptr,
                 static_cast<uint32_t>(bios_.data.size() - 1));
    }
    mapBackupRam();
}

// While REG_SWPBIOS is active, 0x000000-0x00007F shows the BIOS vector table
// and the rest of page 0 still shows the cartridge. Page 0 then points at a
// pre-composed copy so the fast path stays branch-free.
void MemoryBus::mapVectors() {
    if (!biosVectors_ && pRom_.used != 0) {
        return;  // page 0 already maps the P ROM directly
    }
    vectorPage_.fill(kOpenBus);
    if (pRom_.used != 0) {
        std::copy_n(pRom_.data.begin(), vectorPage_.size(), vectorPage_.begin());
    }
    if (biosVectors_ && bios_.used != 0) {
        std::copy_n(bios_.data.begin(), kVectorTableSize, vectorPage_.begin());
    }
    pages_[0] = Page{vectorPage_.data(), nullptr, static_cast<uint32_t>(vectorPage_.size() - 1)};
}

void MemoryBus::mapBankedProgram() noexcept {
    if (pRom_.data.size() <= kMiB) {
        mapRange(0x20, 0x2F, nullptr, nullptr, 0);  // no P2: open bus
        return;
    }
    const size_t banks = (pRom_.data.size() - kMiB) / kMiB;
    const size_t base  = kMiB + (programBank_ % banks) * kMiB;
    // Reads are fast-path; writes stay on the slow path to catch bank selects.
    mapRange(0x20, 0x2F, pRom_.data.data() + base, nullptr, kMiB - 1);
}

void MemoryBus::mapPalette() noexcept {
    uint8_t* bank = paletteRam_.data() + size_t{paletteBank_} * kPaletteBankSize;
    mapRange(0x40, 0x7F, bank, bank, kPaletteBankSize - 1);
}

void MemoryBus::mapBackupRam() noexcept {
    mapRange(0xD0, 0xDF, backupRam_.data(), sramLocked_ ? nullptr : backupRam_.data(),
             kBackupRamSize - 1);
}

// ---------------------------------------------------------------------------
// Slow path: I/O and registers
// ---------------------------------------------------------------------------
// I/O decodes in 128 KiB blocks; each register is mirrored across its block.
uint8_t MemoryBus::readSlow8(uint32_t address) {
    lastIoRead_ = address;
    const bool odd = (address & 1u) != 0;
    switch (address >> 17) {
        case 0x300000 >> 17:
            // Address bit 7 selects REG_SYSTYPE (0x300081) over P1/DIP (0x300000/1).
            if (address & 0x80u) {
                return odd && inputs_ ? inputs_->sysType() : uint8_t{0xFF};
            }
            if (!inputs_) return 0xFF;
            return odd ? inputs_->dipSwitches() : inputs_->p1();       // REG_DIPSW / REG_P1CNT
        case 0x320000 >> 17:
            // Even: REG_SOUND (Z80 reply). Odd: REG_STATUS_A.
            if (!odd) return sound_ ? sound_->reply() : uint8_t{0xFF};
            {
                uint8_t status = inputs_ ? inputs_->statusA() : uint8_t{0x3F};
                if (rtc_ != nullptr) {
                    status = static_cast<uint8_t>((status & 0x3F) | (rtc_->tp() ? StatusABit::RtcTimePulse : 0) |
                                                  (rtc_->dataOut() ? StatusABit::RtcData : 0));
                }
                return status;
            }
        case 0x340000 >> 17:
            return !odd && inputs_ ? inputs_->p2() : uint8_t{0xFF};    // REG_P2CNT
        case 0x380000 >> 17:
            return !odd && inputs_ ? inputs_->statusB() : uint8_t{0xFF};  // REG_STATUS_B
        case 0x3C0000 >> 17: {
            // LSPC: the chip only decodes accesses that include the upper byte
            // lane, so a byte read of an odd address is open bus.
            if (odd) return kOpenBus;
            const uint16_t word = video_ ? video_->readRegister(address) : uint16_t{0xFFFF};
            return static_cast<uint8_t>(word >> 8);
        }
        default:
            return kOpenBus;
    }
}

uint16_t MemoryBus::readSlow16(uint32_t address) {
    lastIoRead_ = address;
    if ((address & 0xFE'0000) == 0x3C'0000) {
        return video_ ? video_->readRegister(address) : uint16_t{0xFFFF};
    }
    const uint16_t hi = readSlow8(address);
    return static_cast<uint16_t>((hi << 8) | readSlow8(address + 1));
}

void MemoryBus::writeSlow8(uint32_t address, uint8_t value) {
    if ((address & 0xFE'0000) == 0x3C'0000) {
        // LSPC registers are 16-bit. A byte write to the even address drives
        // the upper lane and the register stores that byte in both halves; a
        // byte write to the odd address (lower lane only) is not decoded.
        if ((address & 1u) == 0) {
            writeLspc(address, static_cast<uint16_t>(value * 0x0101u));
        }
    } else if ((address & 0xFF'FFF0) == 0x2F'FFF0) {
        programBank_ = static_cast<uint8_t>(value & 0x07);
        mapBankedProgram();
    } else if ((address & 0xFE'0000) == 0x3A'0000 && (address & 1u) != 0) {
        writeSystemLatch(address);  // latch responds on the low byte lane only
    }
    else if ((address & 0xFE'0000) == 0x32'0000 && (address & 1u) == 0) {
        if (sound_ != nullptr) sound_->writeCommand(value);  // REG_SOUND: command to the Z80
    }
    else if ((address & 0xFE'0000) == 0x38'0000 && (address & 1u) != 0) {
        writeIoControl(static_cast<uint8_t>((address & 0xFF) >> 1), value);
    }
    // Everything else (ROM, locked SRAM, unmapped) ignores writes.
}

void MemoryBus::writeSlow16(uint32_t address, uint16_t value) {
    if ((address & 0xFE'0000) == 0x32'0000) {
        // REG_SOUND sits on the upper byte lane.
        if (sound_ != nullptr) sound_->writeCommand(static_cast<uint8_t>(value >> 8));
        return;
    }
    if ((address & 0xFE'0000) == 0x3C'0000) {
        writeLspc(address, value);
        return;
    }
    // A word write drives both byte lanes: route it as a write to the odd
    // (low) byte, which is where 8-bit registers live.
    writeSlow8(address | 1u, static_cast<uint8_t>(value));
}

// LSPC registers, mirrored every 16 bytes across 0x3C0000-0x3DFFFF.
void MemoryBus::writeLspc(uint32_t address, uint16_t value) {
    switch (address & 0x0E) {
        case 0x0C:  // REG_IRQACK
            if (irq_ != nullptr) {
                irq_->acknowledge(static_cast<uint8_t>(value & 0x07));
            }
            break;
        default:
            if (video_ != nullptr) {
                video_->writeRegister(address, value);
            }
            break;
    }
}

// I/O control (0x380000-0x39FFFF, odd bytes, mirrored every 256 bytes).
void MemoryBus::writeIoControl(uint8_t offset, uint8_t value) {
    switch (offset & 0x78) {
        case 0x28:  // 0x380051: uPD4990A DATA IN / CLK / STB
            if (rtc_ != nullptr) rtc_->write(value);
            break;
        default:
            // 0x380001 controller select, 0x380011 memory card bank, 0x380021 slot select,
            // 0x380031/41 cabinet outputs, 0x380061/E1 coin counters and lockouts: TODO.
            break;
    }
}

// One latch selects both the fix-layer ROM and the Z80 program ROM.
void MemoryBus::setCartridgeFix(bool cartridge) noexcept {
    cartFix_ = cartridge;
    if (sound_ != nullptr) sound_->selectCartridgeRom(cartridge);
}

// System latch: the register is selected by the address alone (A1-A4);
// the data value is ignored.
void MemoryBus::writeSystemLatch(uint32_t address) {
    switch (address & 0x1F) {
        case 0x01: shadow_ = false; break;                                  // REG_NOSHADOW
        case 0x11: shadow_ = true;  break;                                  // REG_SHADOW
        case 0x03: biosVectors_ = true;  mapAll(); break;                   // REG_SWPBIOS
        case 0x13: biosVectors_ = false; mapAll(); break;                   // REG_SWPROM
        case 0x0B: setCartridgeFix(false); break;                           // REG_BRDFIX (SFIX + SM1)
        case 0x1B: setCartridgeFix(true);  break;                           // REG_CRTFIX (S ROM + M1)
        case 0x0D: sramLocked_ = true;  mapBackupRam(); break;              // REG_SRAMLOCK
        case 0x1D: sramLocked_ = false; mapBackupRam(); break;              // REG_SRAMUNLOCK
        case 0x0F: paletteBank_ = 1; mapPalette(); break;                   // REG_PALBANK1
        case 0x1F: paletteBank_ = 0; mapPalette(); break;                   // REG_PALBANK0
        default: break;  // memory-card lock/select registers: TODO
    }
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------
std::span<const uint8_t> MemoryBus::fixRom() const noexcept {
    const RomImage& rom = cartFix_ ? sRom_ : sFix_;
    return std::span(rom.data).first(rom.used);
}

std::span<const uint8_t> MemoryBus::paletteRam() const noexcept {
    return std::span(paletteRam_).subspan(size_t{paletteBank_} * kPaletteBankSize, kPaletteBankSize);
}

}  // namespace snk
