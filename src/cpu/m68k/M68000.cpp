// Musashi-backed implementation of snk::M68000.

#include "cpu/m68k/M68000.hpp"

#include <cassert>

#include "memory/MemoryBus.hpp"

extern "C" {
#include "m68k.h"
}

namespace snk {

namespace {

// Musashi's memory callbacks are free functions with no user-data pointer, so
// they need to know which bus to talk to. These two pointers are routing only,
// not emulation state: they always refer to the instance whose context is
// currently loaded into Musashi, and are switched by M68000::activate().
MemoryBus* g_activeBus = nullptr;
M68000*    g_activeCpu = nullptr;

// Musashi state right after m68k_init(): the template for every new instance.
const std::vector<uint8_t>& pristineContext() {
    static const std::vector<uint8_t> context = [] {
        m68k_init();
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
        std::vector<uint8_t> ctx(m68k_context_size());
        m68k_get_context(ctx.data());
        return ctx;
    }();
    return context;
}

}  // namespace

// ---------------------------------------------------------------------------
// Instance management
// ---------------------------------------------------------------------------
M68000::M68000(MemoryBus& bus) : bus_(bus), context_(pristineContext()) {}

M68000::~M68000() {
    if (g_activeCpu == this) {
        g_activeCpu = nullptr;
        g_activeBus = nullptr;
    }
}

// Loads this instance's context into Musashi (saving the previous owner's).
// A no-op in the common single-System case.
void M68000::activate() {
    if (g_activeCpu == this) {
        return;
    }
    if (g_activeCpu != nullptr) {
        m68k_get_context(g_activeCpu->context_.data());
    }
    m68k_set_context(context_.data());
    g_activeCpu = this;
    g_activeBus = &bus_;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
void M68000::reset() {
    activate();
    m68k_pulse_reset();  // fetches SSP/PC through m68k_read_memory_32 -> MemoryBus
}

uint32_t M68000::run(int32_t cycles) {
    if (cycles <= 0) {
        return 0;
    }
    activate();
    executing_ = true;
    const int executed = m68k_execute(cycles);
    executing_ = false;
    const auto used = static_cast<uint32_t>(executed > 0 ? executed : 0);
    totalCycles_ += used;
    return used;
}

void M68000::endTimeslice() noexcept {
    if (executing_ && g_activeCpu == this) {
        m68k_end_timeslice();
    }
}

uint32_t M68000::cyclesThisSlice() const noexcept {
    if (!executing_ || g_activeCpu != this) {
        return 0;
    }
    const int run = m68k_cycles_run();
    return run > 0 ? static_cast<uint32_t>(run) : 0u;
}

void M68000::setInterruptLevel(uint8_t level) {
    assert(level <= 7);
    activate();
    m68k_set_irq(level);
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
uint32_t M68000::readRegister(int reg) const {
    // Musashi reads the live CPU when given nullptr, otherwise a saved context.
    void* ctx = (g_activeCpu == this) ? nullptr : const_cast<uint8_t*>(context_.data());
    return m68k_get_reg(ctx, static_cast<m68k_register_t>(reg));
}

uint32_t M68000::pc() const { return readRegister(M68K_REG_PC); }
uint16_t M68000::sr() const { return static_cast<uint16_t>(readRegister(M68K_REG_SR)); }

uint32_t M68000::dataReg(unsigned index) const {
    assert(index < 8);
    return readRegister(M68K_REG_D0 + static_cast<int>(index));
}

uint32_t M68000::addressReg(unsigned index) const {
    assert(index < 8);
    return readRegister(M68K_REG_A0 + static_cast<int>(index));
}

}  // namespace snk

// ---------------------------------------------------------------------------
// Musashi -> MemoryBus bindings. Musashi already masks addresses to 24 bits in
// 68000 mode and splits nothing itself: 32-bit accesses arrive here whole and
// MemoryBus performs them as two word cycles, high word first.
// ---------------------------------------------------------------------------
extern "C" {

unsigned int m68k_read_memory_8(unsigned int address) {
    return snk::g_activeBus->read8(address);
}

unsigned int m68k_read_memory_16(unsigned int address) {
    return snk::g_activeBus->read16(address);
}

unsigned int m68k_read_memory_32(unsigned int address) {
    return snk::g_activeBus->read32(address);
}

void m68k_write_memory_8(unsigned int address, unsigned int value) {
    snk::g_activeBus->write8(address, static_cast<uint8_t>(value));
}

void m68k_write_memory_16(unsigned int address, unsigned int value) {
    snk::g_activeBus->write16(address, static_cast<uint16_t>(value));
}

void m68k_write_memory_32(unsigned int address, unsigned int value) {
    snk::g_activeBus->write32(address, value);
}

// Used by Musashi's disassembler (m68kdasm.c): must never trigger I/O side
// effects, hence peek instead of read.
unsigned int m68k_read_disassembler_8(unsigned int address) {
    return snk::g_activeBus ? snk::g_activeBus->peek8(address) : 0xFFu;
}

unsigned int m68k_read_disassembler_16(unsigned int address) {
    return snk::g_activeBus ? snk::g_activeBus->peek16(address) : 0xFFFFu;
}

unsigned int m68k_read_disassembler_32(unsigned int address) {
    if (!snk::g_activeBus) {
        return 0xFFFF'FFFFu;
    }
    const unsigned int hi = snk::g_activeBus->peek16(address);
    return (hi << 16) | snk::g_activeBus->peek16(address + 2);
}

}  // extern "C"
