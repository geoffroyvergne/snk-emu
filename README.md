# SNK Emu

Modular SNK Neo-Geo (MVS/AES) emulator in C++20 with an SDL2 frontend.

## Dependencies

CMake ≥ 3.20, a C++20 compiler, SDL2 ≥ 2.0.18, SDL2_mixer ≥ 2.0.4.

| OS      | Install                                                                 |
|---------|-------------------------------------------------------------------------|
| macOS   | `brew install cmake sdl2 sdl2_mixer`                                    |
| Debian/Ubuntu | `sudo apt install cmake g++ libsdl2-dev libsdl2-mixer-dev`        |
| Fedora  | `sudo dnf install cmake gcc-c++ SDL2-devel SDL2_mixer-devel`            |
| Windows | `vcpkg install sdl2 sdl2-mixer` — or unzip the SDL2 / SDL2_mixer `-devel-*-VC.zip` releases |

68000 core: [Musashi](https://github.com/kstenerud/Musashi), built from source with the project:

```sh
git clone https://github.com/kstenerud/Musashi.git third_party/musashi
```

Zip ROM sets: [miniz](https://github.com/richgel999/miniz); Z80: [chips/z80.h](https://github.com/floooh/chips);
YM2610: [ymfm](https://github.com/aaronsgiles/ymfm). All built from source:

```sh
git clone --branch 3.1.2 https://github.com/richgel999/miniz.git third_party/miniz
git clone https://github.com/floooh/chips.git third_party/chips
git clone https://github.com/aaronsgiles/ymfm.git third_party/ymfm
```

No system packages? Add `-DSNKEMU_FETCH_DEPS=ON` and CMake downloads and builds SDL2, SDL2_mixer and all of the cores above from source.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bin/snkemu                                     # test scene, no ROMs needed
./build/bin/snkemu --bios neogeo.zip                   # boot the BIOS alone
./build/bin/snkemu --bios neogeo.zip --game mslug.zip  # boot a game (MAME set, .zip or folder)
```

Options: `--bios-file uni-bios_4_0.rom` picks a specific BIOS inside the BIOS set (default: MAME's
`sp-s2.sp1`), `--parent PATH` supplies the parent set of a MAME clone, `--fps N` overrides the frame rate (default:
the Neo-Geo's native 59.1856 Hz; `--fps 60` avoids judder on 60 Hz displays at +1.37% speed),
`--frames N` quits after N frames and logs timing and audio statistics, `--headless` runs without a window or audio device (automated tests).

Backup RAM (the MVS's battery-backed settings, bookkeeping and high scores) is saved per set on exit
and restored on the next boot, in MAME's `saveram` format, under the user data folder
(macOS: `~/Library/Application Support/snk-emu/snkemu/nvram/<set>/saveram`; files can be copied
to/from MAME's `nvram/<set>/saveram`). `--no-nvram` boots with blank backup RAM.

The green, garbled screen during the first seconds of boot is the MVS BIOS testing palette RAM and
VRAM in place (the test patterns are visible); MAME shows the same screen, frame for frame. ROMs are not included; use sets you own.

Windows (vcpkg):

```bat
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
build\bin\Release\snkemu.exe
```

Windows (SDL devel zips): `-DCMAKE_PREFIX_PATH="C:/libs/SDL2-2.30.9;C:/libs/SDL2_mixer-2.8.0"`.
SDL DLLs are copied next to `snkemu.exe` automatically.

Options: `SNKEMU_FETCH_DEPS` (OFF), `SNKEMU_WARNINGS_AS_ERRORS` (OFF).

## Controls

Keys are bound by physical position (scancodes), so they are the same on QWERTY and AZERTY.

| Neo-Geo         | Player 1           | Player 2           |
|-----------------|--------------------|--------------------|
| Joystick        | Arrow keys         | R F D G            |
| A B C D         | Z X C V            | A S Q W            |
| Start           | 1                  | 2                  |
| Coin            | 5                  | 6                  |
| Select (AES)    | Tab                |                    |

System: Service `9`, Test `F2`, Coin 3/4 `7`/`8`, quit `Esc`.

### Gamepads

Hot-pluggable; the first two pads become players 1 and 2, and each pad's **Coin** button inserts a
coin in that player's slot.

| Neo-Geo  | Game controller (Xbox / PlayStation / Switch / 8BitDo…) | Other joystick (arcade stick, generic USB) |
|----------|----------------------------------------------------------|--------------------------------------------|
| Joystick | D-pad or left stick                                      | hat or first stick                         |
| A B C D  | A B X Y (bottom, right, left, top)                       | buttons 0 1 2 3                            |
| Start    | Start                                                    | button 9                                   |
| **Coin** | **Back / Select / Share / Minus**                        | **button 8**                               |
| Select   | Left shoulder                                            | —                                          |

Remap any of these in `controls.ini` in the user data folder (macOS:
`~/Library/Application Support/snk-emu/snkemu/controls.ini`, created with the defaults on first
run). Pads SDL doesn't recognise as game controllers are read as raw joysticks; to have them
recognised, put [`gamecontrollerdb.txt`](https://github.com/mdqinc/SDL_GameControllerDB) in the
same folder.

## Game compatibility

MAME sets load from a zip or a folder. Cartridges with custom hardware are
recognised by the checksum of their first P ROM and handled automatically:

| Hardware | Games | What is emulated |
|---|---|---|
| Protection chip | Metal Slug X | bit-stream protection at 0x2FFFE0 |
| NEO-CMC42 + NEO-SMA | Metal Slug 3 | graphics/fix decryption; program decryption, `neo-sma` code, bank switching |
| NEO-CMC50 + NEO-PCM2 | Metal Slug 4, Samurai Shodown V | graphics/fix, Z80 (M1) and sample decryption; program shuffle (SS5) |
| NEO-PVC + CMC50 + PCM2 | Metal Slug 5, SNK vs. Capcom, KOF 2003 | all of the above, plus cartridge RAM, colour helpers and byte-granular bank switching |

Decryption happens once, at load time (about a second). The algorithms in
`src/cart/` are ported from MAME (`src/devices/bus/neogeo/`, BSD-3-Clause,
S. Smith, David Haywood, Fabio Priuli). Other encrypted cartridges (KOF 99-2002,
Garou, bootlegs such as `svcboot`...) are not supported yet: the loader rejects
them with a message, or warns that P1 has no NEO-GEO header.

The watchdog is emulated too: several games (Metal Slug 2/3, Last Blade,
Blazing Star...) deliberately let it reset the machine once on first boot, and
show a "WARNING ... ORIGINAL SNK BOARDS ONLY" screen on boards without one.

## Layout

```
CMakeLists.txt
cmake/SnkEmuDependencies.cmake   SDL2 / SDL2_mixer discovery -> snkemu::SDL2, snkemu::SDL2_mixer
cmake/SnkEmuMusashi.cmake        Musashi build (runs m68kmake codegen) -> snkemu_musashi
cmake/SnkEmuMiniz.cmake          miniz build -> miniz
cmake/SnkEmuSoundCores.cmake     chips z80.h + ymfm -> snkemu_z80core, snkemu_ymfm
third_party/                     Musashi, miniz, chips, ymfm sources (not committed; see above)
src/
  main.cpp                       entry point, 60 FPS loop
  core/      System (owns the machine, scanline loop), Interrupts, Hardware constants, Endian helpers
  memory/    MemoryBus: 68000 address map, ROM loading (BIOS, SM1, SFIX, P, S)
  cart/      Cartridge hardware: protection chips on the bus (Metal Slug X, NEO-PVC, NEO-SMA)
             and load-time decryption (NEO-CMC graphics/M1, NEO-PCM2 samples, program ciphers)
  cpu/m68k/  Motorola 68000 @ 12 MHz: backend-agnostic wrapper over Musashi
  cpu/z80/   Zilog Z80 @ 4 MHz: wrapper over chips z80.h (cycle-stepped)
  rom/       RomLoader (game sets), BiosLoader (neogeo.zip), RomSource (zip/folder access)
  rtc/       Upd4990a: MVS calendar chip (time pulse + date), needed by the MVS BIOS at boot
  input/     InputPorts: Neo-Geo input register bitmasks (active low)
  video/     VideoSystem (LSPC: 68 KiB VRAM, line renderer), TileCodec (C ROM decoding),
             Palette (color format, 64K lookup table), TestScene
  audio/     SoundSystem (Z80 memory/IO map, 68000<->Z80 latches), Ym2610 (ymfm),
             resampler, lock-free ring buffer
  platform/  SDL2 RAII wrapper, frame limiter, InputManager, AudioOutput (the only code that includes SDL)
```

`snkemu_core` (everything except `main.cpp` and `platform/`) is a static library with no SDL dependency.
