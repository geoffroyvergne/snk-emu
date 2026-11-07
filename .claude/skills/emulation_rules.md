# Skill: Neo-Geo Architecture & Emulation Roadmap

## Hardware Core Specifications to Enforce
- **Main CPU:** Motorola 68000 running at 12 MHz (16/32-bit).
- **Sound CPU:** Zilog Z80 running at 4 MHz (8-bit).
- **Resolution:** 320x224 pixels (with specific horizontal/vertical blanking periods).
- **Color Palette:** 65,536-color space; 256 palettes of 16 colors each (4,096 colors on screen, color 0 of each palette is transparent).
- **Graphics:** Tile-based layout with line-shrinking capability (No traditional background layers, everything is a sprite).
- **Audio:** YM2610 (14 channels: 4 FM, 3 SSG/PSG, 6 ADPCM-A, 1 ADPCM-B).

## Code Style Rule
- Always use explicit size types from `<cstdint>` (`uint8_t`, `uint16_t`, `uint32_t`).
- Implement Endianness swapping helpers (`bit_cast`, network-to-host macros, or custom shifts) since Neo-Geo ROMs are Big-Endian and modern host systems are Little-Endian.
