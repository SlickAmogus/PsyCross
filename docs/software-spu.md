# Software SPU backends

`PsyX_SPUAL_ConfigureRenderer` selects one of four renderers:

| Value | Backend | Description |
| ---: | --- | --- |
| 0 | Legacy | Original PsyCross OpenAL implementation; default |
| 1 | Authentic | Emulates the original PSX SPU algorithms, arithmetic, and quirks |
| 2 | High Precision | Preserves the original PSX DSP algorithms and character using modern numerical precision |
| 3 | Modern | Recreates the intended result using contemporary high-quality DSP designs |

The software SPU implements 24 voices, SPU ADPCM, ADSR, signed volume and
sweeps, pitch modulation, noise, key state, ENDX, CD/XA input, and the
RAM-backed PSX reverb algorithm. It does not currently implement SPU IRQ
behavior or the four hardware capture buffers.
