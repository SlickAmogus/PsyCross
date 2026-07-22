# This Psycross fork has been altered to make it more compatible with Silent Hill. In essence, it may also be more compatible with other games, but that hasn't been tested.  Will add documentation for all the changes at some point. 


# Psy-Cross (Psy-X)
![](https://i.ibb.co/PFNnw4G/PsyCross.jpg)

Compatibility framework for building and running Psy-Q SDK - based Playstation games across other platforms

### Implementation details
- High-level *Playstation API* reimplementation which translates it's calls into modern/compatible APIs
- Psy-Q - compatible headers
- Implements Geometry Transformation Engine (GTE) in software and adapts it's macros and calls
- Optimized Precise GTE Vertex Cache with *modern 3D hardware perspective transform* and *Z-buffer* support (PGXP-Z)
- *LibSPU* with the original OpenAL backend plus optional software SPU renderers
- *LibGPU* with Playstation-style polygon and image handling
- *LibCD* with ISO 9660 BIN/CUE image support by Playstation CD API
- Already proven to be *95% compatible* with the Psy-Q Playstation SDK - Psy-X game look almost identical to the Playstation game
- You can bring your game to *Web with Emscripten* support

### Folder structure
- `src/gpu`: PSX GPU linked lists and polygon handling routines
- `src/gte`: PSX GTE and PGXP-Z implementation
- `src/render`: OpenGL renderer and PSX VRAM emulation
- `src/pad`: Controller handling
- `src/psx`: Implementations of PsyQ - compatible libraries (**libgte, libgpu, libspu, libcd ...**)
- `include/psx`: Headers of PsyQ - compatible libraries (**libgte, libgpu, libspu, libcd ...**)
- `include/PsyX`: PsyCross interfaces (**window management, configuration, renderer, PGXP-Z**)

### Dependencies
- OpenAL-soft (1.21.x or newer)
- SDL2 (2.0.16 or newer)

### SPU backends

`PsyX_SPUAL_ConfigureRenderer` selects one of four backends:

| Value | Backend | Description |
| ---: | --- | --- |
| 0 | Legacy | Original PsyCross OpenAL implementation; default |
| 1 | Exact | Integer software SPU using the PSX Gaussian table and RAM reverb |
| 2 | Ideal | PSX Gaussian kernel and reverb topology with high-precision arithmetic |
| 3 | Reference | Adaptive sinc reconstruction, high-rate mixing, and modern reverb |

The software SPU implements 24 voices, SPU ADPCM, ADSR, signed volume and
sweeps, pitch modulation, noise, key state, ENDX, CD/XA input, and the
RAM-backed PSX reverb algorithm. It does not currently implement SPU IRQ
behavior or the four hardware capture buffers.

## TODO
- CMake dependency/build scripts
- Add some missing **LibGTE** functions
- MDEC implementation in **LibPress**
- CD Audio/XA decoding and playback

### Credits
- SoapyMan - more GTE functions, SPU-AL, PGXP-Z
- Gh0stBlade - original source/base [(link)](https://github.com/TOMB5/TOMB5/tree/master/EMULATOR)
