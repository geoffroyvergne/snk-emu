#include "audio/SoundSystem.hpp"

#include <algorithm>
#include <string>

#include "core/Hardware.hpp"
#include "core/RomFile.hpp"

namespace snk {

namespace {

struct BankWindow {
    uint16_t base;
    uint16_t size;
    uint8_t  resetEntry;  // power-on mapping: each window shows its own address range
};

// Indexed by (port & 3): ports 0x08, 0x09, 0x0A, 0x0B.
constexpr std::array<BankWindow, 4> kBankWindows = {{
    {0xF000, 0x0800, 0x1E},
    {0xE000, 0x1000, 0x0E},
    {0xC000, 0x2000, 0x06},
    {0x8000, 0x4000, 0x02},
}};

struct SoundRomInfo {
    const char* name;
    size_t      maxSize;
};

constexpr SoundRomInfo soundRomInfo(SoundRom rom) noexcept {
    switch (rom) {
        case SoundRom::BiosM:  return {"SM1", 0x2'0000};
        case SoundRom::M1:     return {"M1", 0x40'0000};          // 256 x 16 KiB banks
        case SoundRom::AdpcmA: return {"V (ADPCM-A)", 0x100'0000};
        case SoundRom::AdpcmB: return {"V (ADPCM-B)", 0x100'0000};
    }
    return {"?", 0};
}

}  // namespace

SoundSystem::SoundSystem() : cpu_(static_cast<Z80Bus&>(*this)) {
    ym_.setIrqHandler([this](bool asserted) { cpu_.setIrqLine(asserted); });
    samples_.reserve(1024);  // ~939 per frame
    reset();
}

void SoundSystem::reset() {
    ram_.fill(0);
    for (size_t i = 0; i < kBankWindows.size(); ++i) {
        banks_[i] = kBankWindows[i].resetEntry;
    }
    cartridgeRom_ = false;
    nmiEnabled_   = false;
    nmiPending_   = false;
    command_      = 0;
    reply_        = 0;
    cycleBalance_   = 0;
    ymClockBalance_ = 0;
    samples_.clear();
    cpu_.reset();
    ym_.reset();
}

// ---------------------------------------------------------------------------
// ROMs
// ---------------------------------------------------------------------------
void SoundSystem::loadRom(SoundRom rom, std::span<const uint8_t> data, uint32_t offset) {
    const SoundRomInfo info = soundRomInfo(rom);
    const size_t       end  = size_t{offset} + data.size();
    if (data.empty()) {
        throw RomLoadError(std::string(info.name) + ": image is empty");
    }
    if (end > info.maxSize) {
        throw RomLoadError(std::string(info.name) + ": image would end at byte " + std::to_string(end) +
                           ", limit is " + std::to_string(info.maxSize));
    }

    std::vector<uint8_t>* target = nullptr;
    switch (rom) {
        case SoundRom::BiosM:  target = &sm1_; break;
        case SoundRom::M1:     target = &m1_; break;
        case SoundRom::AdpcmA: target = &adpcmA_; break;
        case SoundRom::AdpcmB: target = &adpcmB_; break;
    }
    if (target->size() < end) {
        target->resize(end, 0xFF);
    }
    std::copy(data.begin(), data.end(), target->begin() + static_cast<std::ptrdiff_t>(offset));
    updateAdpcmRoms();  // vectors may have moved
}

void SoundSystem::loadRomFile(SoundRom rom, const std::filesystem::path& path, uint32_t offset) {
    const auto buffer = readRomFile(path, soundRomInfo(rom).maxSize);
    try {
        loadRom(rom, buffer, offset);
    } catch (const RomLoadError& e) {
        throw RomLoadError(path.string() + ": " + e.what());
    }
}

void SoundSystem::unloadRom(SoundRom rom) {
    switch (rom) {
        case SoundRom::BiosM:  sm1_.clear(); break;
        case SoundRom::M1:     m1_.clear(); break;
        case SoundRom::AdpcmA: adpcmA_.clear(); break;
        case SoundRom::AdpcmB: adpcmB_.clear(); break;
    }
    updateAdpcmRoms();
}

void SoundSystem::updateAdpcmRoms() noexcept {
    ym_.setAdpcmRoms(adpcmA_, adpcmB_.empty() ? std::span<const uint8_t>(adpcmA_) : adpcmB_);
}

// ---------------------------------------------------------------------------
// 68000 side
// ---------------------------------------------------------------------------
// Latch the byte and raise NMI. If NMIs are disabled the request stays
// pending and fires as soon as the Z80 enables them.
void SoundSystem::writeCommand(uint8_t command) {
    command_    = command;
    nmiPending_ = true;
    updateNmi();
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------
void SoundSystem::runScanline() {
    cycleBalance_ += hw::kZ80CyclesPerLine;
    if (cycleBalance_ > 0) {
        cycleBalance_ -= cpu_.run(static_cast<int32_t>(cycleBalance_));
    }

    // 512 YM2610 clocks per line, one sample per 144 clocks: 3 or 4 samples,
    // 938.67 per frame on average, with no drift.
    ymClockBalance_ += hw::kYm2610ClocksPerLine;
    const uint32_t count = ymClockBalance_ / hw::kYm2610ClocksPerSample;
    ymClockBalance_ %= hw::kYm2610ClocksPerSample;
    if (count != 0) {
        const size_t first = samples_.size();
        samples_.resize(first + count);
        ym_.generate(std::span(samples_).subspan(first));
    }
}

// ---------------------------------------------------------------------------
// Z80 bus
// ---------------------------------------------------------------------------
uint8_t SoundSystem::read(uint16_t address) {
    if (address >= 0xF800) {
        return ram_[address & 0x07FFu];
    }
    const auto& rom = activeProgramRom();
    if (rom.empty()) {
        return 0xFF;
    }
    size_t offset = address;
    if (address >= 0x8000) {
        const size_t window = address >= 0xF000 ? 0 : address >= 0xE000 ? 1 : address >= 0xC000 ? 2 : 3;
        const BankWindow& w = kBankWindows[window];
        offset = size_t{banks_[window]} * w.size + (address - w.base);
    }
    return rom[offset % rom.size()];
}

void SoundSystem::write(uint16_t address, uint8_t value) {
    if (address >= 0xF800) {
        ram_[address & 0x07FFu] = value;
    }
}

uint8_t SoundSystem::in(uint16_t port) {
    switch (port & 0x0F) {
        case 0x00:
            nmiPending_ = false;
            updateNmi();
            return command_;
        case 0x04: case 0x05: case 0x06: case 0x07:
            return ym_.read(static_cast<uint8_t>(port & 3));
        case 0x08: case 0x09: case 0x0A: case 0x0B:
            banks_[port & 3] = static_cast<uint8_t>(port >> 8);
            return 0;
        default:
            return 0xFF;
    }
}

void SoundSystem::out(uint16_t port, uint8_t value) {
    switch (port & 0x0F) {
        case 0x00:
            command_ = 0;
            break;
        case 0x04: case 0x05: case 0x06: case 0x07:
            ym_.write(static_cast<uint8_t>(port & 3), value);
            break;
        case 0x08:
            nmiEnabled_ = (port & 0x10) == 0;  // 0x08 enables, 0x18 disables
            updateNmi();
            break;
        case 0x0C:
            reply_ = value;
            break;
        default:
            break;
    }
}

}  // namespace snk
