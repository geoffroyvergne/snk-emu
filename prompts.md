# Step 1: Project Initialization & CMake Setup

Act according to the rules in .claude. Please generate a modern CMakeLists.txt file for our Neo-Geo emulator project. It must locate and link SDL2 and SDL2_mixer properly on Windows, macOS, and Linux. Also, provide a basic project tree structure along with the main.cpp entry point initializing an SDL2 window and a 60 FPS capped loop.

# Step 2: Memory Bus & ROM Loading Interface

Now, let's design the Memory Bus. Create a MemoryBus class that handles reads and writes for the 68000 address space. Include a method to load raw Neo-Geo binary ROMs (specifically the BIOS/BIOS-M, P ROMs for code, and S ROMs for text). Remember that Neo-Geo is Big-Endian, so provide inline helpers to swap bytes upon loading or reading 16-bit/32-bit words.

# Step 3: Integrating the 68000 CPU Core

We want to integrate a 68000 CPU core. Instead of writing it entirely from scratch, let's design a clean wrapper interface for a well-known open-source C core like Musashi or a C++ alternative. Show me how to bind our MemoryBus read/write functions to the CPU callbacks and execute instructions for a set number of cycles per frame.

# Step 4: Video Graphics System (Tile Rendering Architecture)

Let's implement the core structure of the Neo-Geo Video Graphics (LSPC). The Neo-Geo uses a unique system where everything is rendered as vertical bars of sprites (16x512 pixels max). Design a VideoSystem or PPU class that holds the VRAM (68KB) and processes tile data from the C ROMs. Write an SDL2-backed method to render a dummy frame using a flat structure based on the hardware specification.

# Step 5: Inputs & Controller Mapping

Create an InputManager class utilizing SDL2 events. We need to map keyboard or controller states to the Neo-Geo native Input Ports (Joystick 1, Joystick 2, System Ports for Coin/Start/Service). Generate the bitmask structures representing the state of these registers so the 68000 CPU can query them via the MemoryBus.

# Step 6: Audio System & Z80/YM2610 Structure

Let's establish the audio architecture framework. We need a SoundSystem class handling the Z80 CPU interface and the YM2610 sound chip emulator interface. Setup the SDL2 Audio Callback function or use SDL_QueueAudio to prepare for streaming 44100Hz stereo sound samples.

# Step 7: The ROM Loader (Parsing & Extraction)

Act according to our project rules. Write a complete C++ `RomLoader` class that targets a standard Neo-Geo zip archive or directory game set (MAME naming convention). 
The loader must:
1. Open and parse the zip file (you can use a lightweight library like 'miniz' or standard file streams if unzipped).
2. Load the P-ROM (Program Code) into an isolated buffer, applying a byte-swapping routing (Big-Endian to Little-Endian conversion for 16-bit words) so the host CPU can read it natively.
3. Load the M-ROM (Sound Z80 Code), S-ROM (Fix/Text layer), C-ROMs (Sprite graphics), and V-ROMs (ADPCM samples) into their respective byte arrays.
4. Provide clean getter methods or pointers (`get_p_rom()`, `get_c_rom()`, etc.) for the Memory Bus and PPU to use.
Provide the header and source implementation.

# Step 8: Boot Sequence & BIOS Memory Mapping

Now that we can load ROMs, let's implement the core system Boot Sequence and the Memory Map switching mechanism. 
In the `MemoryBus` class:
1. Implement a 16MB address space for the 68000.
2. Provide a BIOS memory array (typically 128KB) and a flag `bool bios_mapped = true;`.
3. Implement `read16(uint32_t address)` and `write16(uint32_t address, uint16_t value)`. When `bios_mapped` is true, reads between 0x000000 and 0x01FFFF must point to the BIOS.
4. Implement a write hook: when the 68000 writes to the specific Neo-Geo hardware register (0x3A0000 / 0x3A0012 depending on the exact mapping), toggle `bios_mapped = false` to let the game cartridge take over.
5. Write a `reset()` function that fetches the initial Stack Pointer (from address 0x000000) and Program Counter (from address 0x000004) to kick off the 68000 execution loop.

# Step 9: The 4bpp Graphics Decoder (C-ROM planar graphics)

Let's build the graphics tile decoder for the Neo-Geo C-ROMs. The tiles are 16x16 pixels and use a 4-bit-per-pixel (16 colors) planar format interleaved across pairs of C-ROMs (C1/C2, C3/C4).
1. Write a C++ helper function `decode_tile(const uint8_t* c_rom_ptr, uint32_t tile_index, uint8_t* out_buffer_4bpp)` that extracts a 16x16 pixel matrix where each byte contains a color index (0-15).
2. Explain the Neo-Geo layout bitplanes (how the 4 bits of a single pixel are fetched across the 64 bytes of a single tile data structure).
3. Implement a cache or optimization strategy to convert these 4bpp indexes into raw 32-bit RGBA pixels using the standard Neo-Geo color palette fetched from VRAM (where each color is encoded in RGB565 / RGB666 format with dark bit logic).

# Step 4: The Frame-Accurate Loop & VBlank/HBlank Interrupts

We need a robust emulation timing loop inside our main loop to synchronize the 68000 CPU with the display. The Neo-Geo runs at 60.19 Hz, with 264 total scanlines per frame (224 active, 40 blanking).
1. Implement an execution loop for one full frame. For each scanline (0 to 263), execute the 68000 CPU core for exactly 493 clock cycles (12MHz / 60.19Hz / 264 lines).
2. During the active scanline period, handle the Horizontal Blanking interval if required by the hardware.
3. When reaching the end of active lines (Scanline 224), trigger the Neo-Geo Vertical Blanking Interruption (68000 Auto-Vector IRQ 1).
4. Provide a mechanism using SDL2 (`SDL_GetTicks64` or high-resolution sleep) to cap the execution so that 1 frame takes exactly 16.61 milliseconds on the host machine.

