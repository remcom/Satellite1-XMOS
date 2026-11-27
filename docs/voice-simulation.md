# Voice pipeline simulation

`tests/sim_voice` runs simulated rooms through a host build of the voice
pipeline, to compare firmware configurations before trying them on hardware.
It was used to decide which of the wake word and voice changes proposed in
November 2026 to adopt. The method and its limits come first; the numbers
follow.

## What is simulated

- **The DSP is the firmware's own code**, compiled for the host: the
  lib_voice AEC, IC and NS, lib_xcore_math's portable C kernels, and the
  firmware's DOA, mic front end, beamformer, high-pass, VNR hold and echo
  gate (`satellite-xmos-firmware/src/audio`). `sim_pipeline.c` calls them in
  the order `audio_pipeline_t1.c` and `audio_pipeline_t0.c` do.
- **Room**: a 6 x 5 x 2.7 m shoebox, RT60 ~0.39 s, image source method,
  high-passed at 60 Hz as in Allen & Berkley. The four mics sit on a 7.1 cm
  ring (N, S, E, W); the speaker is 4 cm below its centre.
- **Signals**: the talker and the "TV" are the recorded numbers in
  `satellite-xmos-firmware/filesystem_support` (English and Mandarin). Music is
  synthetic stereo, three panned instruments and percussion with the panning
  redrawn every 3 s. Noise comes from three far pink noise sources, plus a fan
  (pink noise with a 120 Hz hum) in some scenes, plus mic self noise. The
  speaker has a soft saturation (tanh), so the echo is not perfectly linear.
- **Levels** at the mics: talker -50 dBFS RMS while talking, TV -54..-60, music
  echo -36..-24, room noise -68..-58.
- **Metrics**:
  - *ERLE*: AEC input over output energy, while only the speaker plays.
  - *SNR* of the talker on channel 1 (the wake word channel), by phase
    inversion (Hagerman & Olofsson 2004). Each configuration runs twice, the
    second time with the talker's polarity inverted. The talker at the output
    is half the difference of the two outputs; noise, echo and TV are half the
    sum. Linear filtering of the talker (the beam, the IC filter) therefore
    does not count against it, but the NS's spectral changes are not
    penalised either.
  - *DOA*: results per wake word, and how far from the talker the beam
    pointed after each wake word.

## What is not simulated

- **Wake word detection.** No microWakeWord model runs here. The SNR on
  channel 1 is a proxy; it says nothing about how the model reacts to NS
  artefacts, so the `CH1_TAP` choice is left to tests on hardware.
- **VNR**: the IC is driven by an oracle (the frame's true speech-to-noise
  ratio through a sigmoid, two frames late), not by the VNR model.
- **AGC**, the 48 kHz resamplers, I2S and SPI.
- **Real acoustics**: the device housing, a speaker that is not centred,
  furniture, moving talkers. With the speaker centred, the echo reaches all
  four mics almost equally, which flatters any steering-related echo effect.
- **CPU and memory**: measured with the XTC build (below), not here.

## Reproducing

```sh
tests/sim_voice/build.sh /tmp/sim                      # builds sim_voice
/tmp/sim/sim_voice satellite-xmos-firmware/filesystem_support all /tmp/sim/csv > /tmp/sim/all.jsonl
```

Or one scenario at a time (`aec_ref`, `aec_ref_doubletalk`, `ref_mix`,
`mic_mode`, `doa_playback`, `doa_music_only`, `ic_reset`, `steer_playback`,
`tap`, `ic_check`), each into `<name>.jsonl`, then
`python3 tests/sim_voice/summarize.py <dir>` for the tables below. A full run
takes ~15 minutes on 11 cores with the scenarios in parallel.
`tests/sim_voice/test_sim_voice.py` is a ~20 s pytest smoke check of the two
main results.

## Decisions

| Change | Simulated effect | Decision |
|---|---|---|
| One (mono) AEC reference in the 2-mic build, instead of L and R | ERLE 11-18 dB -> 27-31 dB; ch1 SNR during double talk +6.5..+12.4 dB; tile 1 RAM -92.9 KB | Adopted (default) |
| `AUDIO_SERVICER_CMD_REF_MIX`: reference mixed like the speaker plays | Speaker on the left channel: ERLE 7-10 dB -> 27-30 dB | Adopted; the host must set it to match the TAS2780 channel |
| aec4: DOA after the AEC, echo gate instead of the playback gate | Wake words over music located: beam on the talker for 19 of 24 wake words instead of 0; no steering at the speaker with music alone; command SNR on ch1 mean +0.6 dB (-2.1..+2.5), wake words -0.2 dB | Adopted. CPU of the AEC stage not yet measured on hardware |
| Clear the IC filter on every re-steer | -0.6..+2.2 dB without music (table below); -0.3..-2.7 dB in all 8 music scenes of `doa_playback` (run once with it on) | Not adopted |
| BEAM as the default mic mode | -0.6..+0.5 dB against PAIR_EW: a 7.1 cm ring has little directivity in the speech band | Not adopted |
| Soft limiter and XMOS gain on ch1 | Fewer full scale samples, same distortion against the unclipped signal; +4.9 KB RAM on tile 0 | Not adopted |
| aec4 to avoid AEC re-convergence after a 2-mic re-steer | No ERLE difference after a re-steer (centred speaker) | No change needed; to check on hardware |

The existing IC fast hold (`AUDIO_SERVICER_CMD_IC_FAST_HOLD`, on by default)
is confirmed: it raises the ch1 SNR of wake words by 1.3..2.8 dB in pair and
beam modes.

## Build sizes (XTC 15.3.1, tile 1 / tile 0 bytes)

| Variant | develop | this change |
|---|---|---|
| fixed_delay | 500,460 / 424,668 | 407,588 / 424,668 |
| aec4 | 508,108 / 424,668 | 508,340 / 424,668 |

## lib_voice 2.0.0 against fwk_voice 0.8.0

The move from fwk_voice 0.8.0 to lib_voice 2.0.0 (with lib_xcore_math 3.0.1
and xmos-ai-tools 1.4.5), every scenario run with both. The firmware keeps
the fwk_voice behaviour where lib_voice changed an interface: the AGC still
gets the smoothed VNR of the IC output, and the IC fast hold still feeds
`ic_adapt()`.

| | fwk_voice 0.8.0 | lib_voice 2.0.0 |
|---|---|---|
| ERLE 2x1, mean of 3 seeds (worst 10%) | 29.6 (27.2) dB | 29.2 (26.4) dB |
| ERLE aec4 4x1 (worst 10%) | 29.5 (27.3) dB | 29.3 (26.8) dB |
| ch1 SNR, double talk, 2x1 | 14.5 dB | 13.8 dB |
| ERLE 2x2 (not used) | 14.7 dB | 12.5 dB |
| aec4 post-AEC DOA, 8 music scenes: beam error after a wake word | 17.7 deg | 24.7 deg (one scene +49, the others -2..+4) |
| aec4 post-AEC DOA: ch1 wake word / command SNR | 15.9 / 16.4 dB | 15.4 / 15.9 dB (that scene -6.9 / -4.8) |
| Command SNR after a re-steer during playback, 2x1 / aec4 | 15.6 / 18.0 dB | 15.8 / 18.7 dB |
| ERLE with a reference that does not match the speaker (`ref_mix` downmix), 2x1 | 8.3 dB | 7.3 dB |
| IC, NS, mic modes, taps | | within 0.1 dB |

The AEC loses a few tenths of a dB on average and up to 1.4 dB in its worst
frames. A likely cause, not confirmed, is that lib_voice 2.0.0 stores the
adaptive filter as 16-bit time-domain taps. The DOA result rests on one
scene and needs checking on hardware, as does all of this.

| Variant, tile 0 / tile 1 bytes (XTC 15.3.1) | fwk_voice 0.8.0 | lib_voice 2.0.0 |
|---|---|---|
| fixed_delay | 424,668 / 407,588 | 397,676 / 368,052 |
| bypass | 382,876 / 384,252 | 355,372 / 334,700 |
| aec4 | 424,668 / 508,340 | 397,676 / 421,996 |

## Results
### Mono AEC reference: stereo music, mono speaker

| seed | ERLE 2x2 mean | ERLE 2x2 worst 10% | ERLE 2x1 mean | ERLE 2x1 worst 10% | ERLE aec4 4x1 mean |
|---|---|---|---|---|---|
| 1 | 15.0 | 10.2 | 30.7 | 29.1 | 30.5 |
| 2 | 17.8 | 14.5 | 27.3 | 24.6 | 27.4 |
| 3 | 11.3 | 6.2 | 30.9 | 27.8 | 30.7 |


ch1 SNR of six utterances said over the music:

| seed | ch1 SNR 2x2 | ch1 SNR 2x1 | gain |
|---|---|---|---|
| 1 | 2.5 | 14.9 | 12.4 |
| 2 | 6.6 | 13.2 | 6.5 |
| 3 | 2.9 | 15.3 | 12.4 |


### Reference mix: TAS2780 set to the left channel

| seed | 2-mic, (L+R)/2 ref | 2-mic, L ref | aec4, (L+R)/2 ref | aec4, L ref |
|---|---|---|---|---|
| 1 | 9.7 | 28.7 | 9.8 | 28.8 |
| 2 | 6.9 | 26.6 | 6.6 | 26.5 |
| 3 | 8.3 | 29.9 | 8.3 | 29.7 |


### aec4: wake words over music, DOA before vs after the AEC

| music (echo dBFS) | talker az | DOA results/wake before | after | beam on talker (of 3) before | after | beam error before | after | cmd SNR before | after |
|---|---|---|---|---|---|---|---|---|---|
| -36 | 30 | 0.0 | 27.0 | 0 | 2 | 30.0 | 15.7 | 19.4 | 19.4 |
| -36 | 120 | 0.0 | 25.3 | 0 | 3 | 120.0 | 2.0 | 15.8 | 17.1 |
| -36 | 200 | 0.0 | 33.7 | 0 | 3 | 160.0 | 1.0 | 16.1 | 18.6 |
| -36 | 300 | 0.0 | 28.0 | 0 | 1 | 60.0 | 28.3 | 18.3 | 16.2 |
| -30 | 30 | 0.0 | 24.7 | 0 | 3 | 30.0 | 11.0 | 16.5 | 16.0 |
| -30 | 120 | 0.0 | 15.7 | 0 | 2 | 120.0 | 49.3 | 11.2 | 13.6 |
| -30 | 200 | 0.0 | 20.7 | 0 | 3 | 160.0 | 11.0 | 13.8 | 15.2 |
| -30 | 300 | 0.0 | 20.7 | 0 | 2 | 60.0 | 23.3 | 15.1 | 14.8 |


Mean command SNR change: +0.6 dB, wake word SNR change: -0.2 dB.


Music only, nobody talking (post-AEC DOA, steering on any speech):

| music (echo dBFS) | DOA results | frames | beam moves |
|---|---|---|---|
| -30 | 13 | 2000 | 0 |
| -24 | 0 | 2000 | 0 |


### Mic mode: talker with a TV, ch1 SNR

| build | talker az | TV az | wake PAIR_EW | wake AUTO_PAIR | wake BEAM | cmd PAIR_EW | cmd AUTO_PAIR | cmd BEAM |
|---|---|---|---|---|---|---|---|---|
| normal | 60 | 220 | 3.5 | 3.1 | 3.2 | 4.0 | 3.5 | 3.4 |
| aec4 | 60 | 220 | 3.5 | 3.1 | 3.2 | 4.0 | 3.5 | 3.4 |
| normal | 150 | 330 | 2.6 | 3.0 | 2.7 | 3.1 | 3.5 | 2.9 |
| aec4 | 150 | 330 | 2.6 | 3.0 | 2.7 | 3.1 | 3.5 | 2.9 |
| normal | 300 | 120 | 3.8 | 3.8 | 4.3 | 4.1 | 4.1 | 4.1 |
| aec4 | 300 | 120 | 3.8 | 3.8 | 4.3 | 4.1 | 4.1 | 4.1 |


### Experiment: clear the IC filter on every re-steer (not adopted)

| mode | build | talker az | 1st cmd keep | 1st cmd reset | cmds keep | cmds reset |
|---|---|---|---|---|---|---|
| beam | normal | 120 | 8.0 | 8.0 | 6.6 | 6.5 |
| beam | aec4 | 120 | 8.0 | 8.0 | 6.6 | 6.5 |
| auto_pair | normal | 120 | 5.7 | 7.9 | 6.8 | 8.2 |
| auto_pair | aec4 | 120 | 5.8 | 8.0 | 6.8 | 8.2 |
| beam | normal | 210 | 7.7 | 7.8 | 7.9 | 8.0 |
| beam | aec4 | 210 | 7.7 | 7.8 | 7.9 | 8.0 |
| auto_pair | normal | 210 | 8.8 | 8.2 | 8.6 | 8.5 |
| auto_pair | aec4 | 210 | 8.8 | 8.2 | 8.7 | 8.5 |
| beam | normal | 300 | 6.7 | 7.6 | 8.4 | 8.1 |
| beam | aec4 | 300 | 6.6 | 7.6 | 8.4 | 8.0 |
| auto_pair | normal | 300 | 7.7 | 7.7 | 8.1 | 8.1 |
| auto_pair | aec4 | 300 | 7.7 | 7.7 | 8.1 | 8.1 |


### Re-steering during playback: 2-mic vs aec4

| seed | talker az | ERLE after wake, 2-mic | ERLE after wake, aec4 |
|---|---|---|---|
| 1 | 150 | 18.9 | 19.0 |
| 2 | 210 | 18.9 | 19.1 |
| 3 | 270 | 19.2 | 19.1 |


### Wake word channel tap (SNR only, no wake word model)

| noise dBFS | AEC+IC+NS | AEC+IC | AEC |
|---|---|---|---|
| -68 | 15.5 | 13.2 | 13.6 |
| -58 | 4.5 | 1.6 | 3.5 |


### IC fast hold (existing feature), ch1 SNR of the wake words

| mode | tap | fast hold off | fast hold on |
|---|---|---|---|
| pair_ew | ic | 3.1 | 5.8 |
| pair_ew | ns | 7.0 | 9.8 |
| beam | ic | 3.9 | 5.2 |
| beam | ns | 7.2 | 8.6 |
