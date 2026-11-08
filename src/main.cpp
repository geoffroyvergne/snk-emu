// SNK Emu entry point: wires the SDL2 frontend to the emulated System and
// runs a fixed 60 FPS loop.
//
// Usage:
//   snkemu                                   test scene (no ROMs needed)
//   snkemu --bios neogeo.zip                 boot the BIOS alone
//   snkemu --bios neogeo.zip --game set.zip  boot a game
// Options:
//   --bios-file NAME   pick a 68000 BIOS inside the BIOS set (e.g. uni-bios_4_0.rom)
//   --parent PATH      parent set of a MAME clone
//   --fps N            host frame rate cap (default: the Neo-Geo's native 59.1856 Hz)
//   --frames N         quit after N frames and log statistics (smoke tests)
//   --no-nvram         don't load/save backup RAM (forces the BIOS's first-boot tests)
//   --headless         no window and no audio device (automated runs)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <SDL.h>  // SDL_MAIN_HANDLED is defined by the build system

#include "core/BackupRamFile.hpp"
#include "core/Hardware.hpp"
#include "core/System.hpp"
#include "platform/AudioOutput.hpp"
#include "platform/InputManager.hpp"
#include "platform/SdlPlatform.hpp"
#include "rom/BiosLoader.hpp"
#include "rom/RomLoader.hpp"
#include "video/TestScene.hpp"

namespace {

struct Options {
    uint64_t    maxFrames = 0;  // 0 = run until closed
    // Native refresh: 6 MHz pixel clock / (384 x 264) = 59.1856 Hz, 16.896 ms per frame.
    // --fps 60 trades exact speed (+1.37%) for judder-free output on 60 Hz displays.
    double      fps = snk::hw::kNativeRefreshHz;
    std::string bios, biosFile, game, parent;
    bool        nvram    = true;
    bool        headless = false;
};

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string(argv[i]) + " needs a value");
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--frames") == 0)         options.maxFrames = std::strtoull(value().c_str(), nullptr, 10);
        else if (std::strcmp(argv[i], "--fps") == 0)       options.fps = std::strtod(value().c_str(), nullptr);
        else if (std::strcmp(argv[i], "--bios") == 0)      options.bios = value();
        else if (std::strcmp(argv[i], "--bios-file") == 0) options.biosFile = value();
        else if (std::strcmp(argv[i], "--game") == 0)      options.game = value();
        else if (std::strcmp(argv[i], "--parent") == 0)    options.parent = value();
        else if (std::strcmp(argv[i], "--no-nvram") == 0)  options.nvram = false;
        else if (std::strcmp(argv[i], "--headless") == 0)  options.headless = true;
        else throw std::runtime_error(std::string("unknown option: ") + argv[i]);
    }
    if (!(options.fps >= 10.0 && options.fps <= 1000.0)) {
        throw std::runtime_error("--fps must be between 10 and 1000");
    }
    if (!options.game.empty() && options.bios.empty()) {
        throw std::runtime_error("--game needs --bios: the Neo-Geo always boots from its BIOS");
    }
    return options;
}

struct BootInfo {
    std::string title;
    std::string setName;  // NVRAM key, like MAME: the game set, or "neogeo" for the BIOS alone
};

// Per-user data folder (macOS: ~/Library/Application Support/snk-emu/snkemu/).
std::filesystem::path userDataDir() {
    char* base = SDL_GetPrefPath("snk-emu", "snkemu");
    if (base == nullptr) {
        return {};
    }
    std::filesystem::path dir(base);
    SDL_free(base);
    return dir;
}

// Backup RAM file for a set: <user data folder>/nvram/<set>/saveram (MAME's layout).
std::filesystem::path nvramPath(const std::string& setName) {
    const auto dir = userDataDir();
    return dir.empty() ? dir : dir / "nvram" / setName / "saveram";
}

// Loads BIOS (+ game) into the machine; the last install() resets it, so the
// 68000 starts from the BIOS vector table.
BootInfo bootFromRoms(const Options& options, snk::System& system) {
    snk::BiosLoader bios;
    bios.load(options.bios, options.biosFile);
    SDL_Log("BIOS: %s (%s, %s)", bios.selectedFile().c_str(),
            bios.systemType() == snk::SystemType::Mvs ? "MVS" : "AES",
            bios.wasWordSwapped() ? "word-swapped dump" : "big-endian dump");
    for (const auto& w : bios.warnings()) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "BIOS: %s", w.c_str());
    bios.install(system);

    std::string title   = "SNK Emu - BIOS";
    std::string setName = "neogeo";
    if (!options.game.empty()) {
        snk::RomLoader game;
        game.load(options.game, options.parent);
        for (const auto& f : game.files()) {
            SDL_Log("  %-16s %-18s %8zu bytes  crc %08X%s", f.name.c_str(), snk::romRoleName(f.role), f.size,
                    f.crc32, f.fromParent ? "  (parent)" : "");
        }
        for (const auto& w : game.warnings()) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Game: %s", w.c_str());
        game.install(system);  // loader buffers are freed when `game` goes out of scope
        if (const snk::CartridgeChip* chip = system.memory().cartridgeChip()) {
            SDL_Log("Game: cartridge hardware: %s", chip->name());
        }
        title   = "SNK Emu - " + game.setName();
        setName = game.setName();
    }

    const snk::M68000& cpu = system.mainCpu();
    SDL_Log("Reset: SSP=%06X PC=%06X (fetched from the BIOS vector table)", cpu.addressReg(7), cpu.pc());
    return {title, setName};
}

int run(const Options& options) {
    snk::platform::WindowConfig config;
    config.title    = "SNK Emu";
    config.logicalW = snk::hw::kScreenWidth;
    config.logicalH = snk::hw::kScreenHeight;
    config.scale    = 3;
    config.headless = options.headless;

    snk::platform::SdlPlatform platform{config};
    // Declared after platform: both unhook from SDL before SDL_Quit.
    snk::platform::InputManager input{userDataDir()};  // controls.ini, gamecontrollerdb.txt
    snk::platform::AudioOutput  audio{platform.audioAvailable()};

    // Emulated audio per wall-clock second scales with how fast frames are paced
    // (exactly the YM2610 rate at the native refresh, +1.37% at --fps 60).
    audio.setInputRate(snk::hw::kYm2610SampleRateExact * options.fps / snk::hw::kNativeRefreshHz);

    // Heap-allocated: the machine holds RAM/framebuffers that are too big for the stack.
    auto system = std::make_unique<snk::System>();

    // With ROMs: boot them. Without: drive the video pipeline with a synthetic scene.
    std::optional<snk::TestScene> scene;
    std::filesystem::path         nvram;
    std::optional<std::filesystem::file_time_type> nvramTime;  // file's timestamp when loaded
    if (!options.bios.empty()) {
        const BootInfo boot = bootFromRoms(options, *system);
        config.title = boot.title;
        platform.setTitle(config.title);

        // Battery-backed RAM: with valid contents the BIOS skips its destructive
        // backup-RAM test at power-on (about 0.66 s), as on a real MVS.
        if (options.nvram) {
            nvram     = nvramPath(boot.setName);
            nvramTime = snk::backup_ram::modificationTime(nvram);
            switch (snk::backup_ram::load(nvram, system->memory().backupRam())) {
                case snk::backup_ram::LoadResult::Loaded:
                    SDL_Log("NVRAM: loaded %s", nvram.string().c_str()); break;
                case snk::backup_ram::LoadResult::Missing:
                    SDL_Log("NVRAM: none yet (first boot), will save to %s", nvram.string().c_str()); break;
                case snk::backup_ram::LoadResult::WrongSize:
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "NVRAM: %s has the wrong size, ignored", nvram.string().c_str()); break;
            }
        }
    } else {
        system->setWatchdogEnabled(false);  // no 68000 program runs to kick it
        scene.emplace(*system);
    }
    bool biosVectors = system->memory().biosVectorsMapped();

    snk::platform::FrameLimiter limiter{options.fps};
    SDL_Log("Frame pacing: %.4f FPS (%.3f ms per frame)", options.fps, 1000.0 / options.fps);

    uint64_t fpsTimer  = SDL_GetTicks64();
    uint32_t fpsFrames = 0;

    uint32_t watchdogResets = 0;
    while (platform.pollEvents([&](const SDL_Event& e) { input.handleEvent(e); })) {
        // Latch this frame's controls into the machine's input registers.
        system->inputs().setState(input.sample());
        if (scene) scene->update(system->frameCount());
        system->runFrame();

        // Boot milestone: the program wrote REG_SWPROM, handing the vector table to the cartridge.
        if (const bool mapped = system->memory().biosVectorsMapped(); mapped != biosVectors) {
            biosVectors = mapped;
            SDL_Log("Frame %llu: %s vector table mapped (PC=%06X)",
                    static_cast<unsigned long long>(system->frameCount()), mapped ? "BIOS" : "cartridge",
                    system->mainCpu().pc());
        }
        if (const uint32_t resets = system->watchdogResets(); resets != watchdogResets) {
            watchdogResets = resets;
            SDL_Log("Frame %llu: watchdog reset (#%u)", static_cast<unsigned long long>(system->frameCount()), resets);
        }
        audio.push(system->audioSamples());
        platform.present(system->framebuffer());
        limiter.wait();

        // Once per second: FPS and audio buffer health in the title bar.
        ++fpsFrames;
        const uint64_t now = SDL_GetTicks64();
        if (now - fpsTimer >= 1000) {
            const double fps = fpsFrames * 1000.0 / static_cast<double>(now - fpsTimer);
            const auto   st  = audio.stats();
            char title[160];
            std::snprintf(title, sizeof(title), "%s - %.1f FPS - audio %.0f ms, rate %+.2f%%, underruns %llu",
                          config.title.c_str(), fps, st.bufferedMs, st.rateAdjust * 100.0,
                          static_cast<unsigned long long>(st.underruns));
            platform.setTitle(title);
            fpsTimer  = now;
            fpsFrames = 0;
        }

        if (options.maxFrames != 0 && system->frameCount() >= options.maxFrames) {
            break;
        }
    }

    // Where the 68000 is at exit, and what it last polled: points at the
    // hardware a hung BIOS is waiting for (e.g. 0x320000 = Z80 reply).
    const uint32_t lastIo = system->memory().lastIoRead();
    char lastIoText[16] = "none";
    if (lastIo != snk::MemoryBus::kNoIoRead) std::snprintf(lastIoText, sizeof(lastIoText), "%06X", lastIo);
    SDL_Log("68000: PC=%06X SR=%04X, last I/O read: %s", system->mainCpu().pc(), system->mainCpu().sr(), lastIoText);
    if (!nvram.empty()) {
        try {
            // Never overwrite a file that changed on disk while we were running
            // (another instance saved it, or it was repaired): keep ours aside.
            std::filesystem::path target = nvram;
            if (snk::backup_ram::modificationTime(nvram) != nvramTime) {
                target += ".unsaved";
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "NVRAM: %s changed on disk since it was loaded; keeping it, this session's data goes to %s",
                            nvram.string().c_str(), target.string().c_str());
            }
            snk::backup_ram::save(target, system->memory().backupRam());
            SDL_Log("NVRAM: saved %s", target.string().c_str());
        } catch (const std::exception& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "NVRAM: %s", e.what());
        }
    }
    const auto pace = limiter.stats();
    SDL_Log("Timing: %llu frames, avg %.3f ms (target %.3f), min %.3f, max %.3f, resyncs %llu",
            static_cast<unsigned long long>(pace.frames), pace.averageMs, pace.targetMs, pace.minMs, pace.maxMs,
            static_cast<unsigned long long>(pace.resyncs));
    const auto st = audio.stats();
    SDL_Log("Audio: %s, %d Hz, buffered %.1f ms, rate adjust %+.3f%%, underruns %llu, overruns %llu",
            audio.enabled() ? "on" : "off", audio.deviceRate(), st.bufferedMs, st.rateAdjust * 100.0,
            static_cast<unsigned long long>(st.underruns), static_cast<unsigned long long>(st.overruns));
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parseOptions(argc, argv));
    } catch (const std::exception& e) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Fatal: %s", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SNK Emu", e.what(), nullptr);
        return 1;
    }
}
