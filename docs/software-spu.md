# Software SPU backends

`PsyX_SPUAL_ConfigureRenderer` selects one of four renderers:

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
