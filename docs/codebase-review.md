# Satellite1 codebase review

Reviewed 2026-10-27 against `156df2b` on `develop`. Scope: first-party firmware,
active CMake targets, SPI control/DFU, audio front end, transport, tests and release
scripts. This is a source review and host test run, not a board timing or acoustic
qualification. It is not an exhaustive audit of the XMOS submodules.

Assumption: the supported product is Satellite1 with the `fixed_delay` and
`bypass` targets, 48 kHz I2S, four PDM mics and SPI device control, as selected by
[firmware.cmake](../satellite-xmos-firmware/firmware.cmake) and
[satellite1.cmake](../satellite-xmos-firmware/satellite1.cmake). Published audio
modes and SPI commands remain supported; unused example code is a separate
removal candidate. No host usage telemetry was available to justify removing a
working public feature.

Fix SPI error reporting and DFU recovery before spending time on more DSP
acceleration. Much of the audio path already uses vector kernels. The strongest
remaining optimization candidates are DOA scalar work and bypass memory use.

## Fixes to prioritize

All firmware findings below remain open. The accompanying changes repair test
discovery and documentation only. P1 means resolve before relying on the affected
control/update behavior in a release; P2 means schedule a follow-up.

### F1 — P1: SPI writes can report success after the handler rejects them

In [device_control_spi.c](../modules/fph/rtos_device_control/transport/spi/device_control_spi.c),
`device_control_spi_xfer_done_cb()` keeps the return value of
`device_control_request()`. That validates resource routing, not execution.
`device_control_payload_transfer_bidir()` writes the actual result to `tx_buf[0]`
and `ctx->last_status`, but the subsequent `spi_stage_status_response()` overwrites
it with the earlier routing result. The write response therefore says success
for invalid values and truncated payloads on a valid resource.

Concrete check: send audio gain `[230, 0, 1, 31]`, then a NOP. The gain handler
returns `SERVICER_WRONG_PAYLOAD` (67), but the transport stages `[1, 0, ...]`.
An incomplete `[230, 0, 1]` similarly loses `CONTROL_DATA_LENGTH_ERROR` (3).

Return the execution result explicitly from the bidirectional transfer helper
and use it for the status frame. Update the protocol document, which currently
describes the routing result. Test invalid values, short writes, bad commands,
valid writes, special-resource status reads and NOP consumption with a mocked
SPI driver, then an ESP32 host. Preserve the existing framing.

### F2 — P1: zero-length reads reach code that requires a status byte

The same SPI callback does not reject read requests with `payload_len == 0`.
The [device-control core](../modules/fph/rtos_device_control/src/device_control.c)
asserts `payload_len > 0` when allocating an off-tile read buffer. Audio resource
230 runs on that other tile, so `[230, 0x80, 0]` reaches this assertion. Local
servicer wrappers also subtract one from the length before validation.

Reject reads without room for a status byte before dispatch. Enforce the
documented 253-byte payload limit consistently: `SPI_MAX_PAYLOAD_LEN` is defined
but unused. A 255-byte read plus its marker fits the 256-byte TX array, so the
missing limit alone is not evidence of a TX overflow.

Also initialize failed read responses: special-resource errors can return
without writing `payload[0]`, and off-tile error replies copy the whole allocated
buffer even when a handler only wrote a status byte. Validate zero, one, maximum,
over-limit and malformed read lengths and verify deterministic error bytes.

### F3 — P1: an incomplete final DFU sector is discarded

In [dfu_state_machine.c](../satellite-xmos-firmware/src/dfu_int/dfu_state_machine.c),
the DNLOAD GETSTATUS path only writes when `frag_number == DFU_NUM_FRAGMENTS - 1`
(32 fragments of 128 bytes per 4096-byte sector). A zero-length DNLOAD calls
`dfu_int_reset_download_buffer()` before manifestation, clearing any incomplete
sector without writing it. A 4224-byte transfer followed by termination can
therefore report completion after writing only the first 4096 bytes.

Track buffered byte count, flush the final sector with a defined padding policy,
and reject invalid fragment sequences. Alternatively, if sector padding is a
mandatory host contract, document and enforce it instead of silently succeeding.
The current source accepts short fragments and does not establish that contract.
Test 128, 4095, 4096, 4097 and 4224-byte images, upload/readback and actual boot.
Host image padding may mask this problem in normal use; it does not fix the
device-side behavior.

### F4 — P1: abort/error recovery does not reset the cached DFU destination

[dfu_common.c](../satellite-xmos-firmware/src/dfu_int/dfu_common.c) caches
`dn_base_addr`, `bytes_avail` and `total_len`. Only `dfu_common_make_manifest()`
clears the base address. `dfu_int_reset_state()` resets the state-machine counters
but none of those cached values on abort, error clear or alternate change.

After writing a sector and aborting, a retry starts at block zero with the old
capacity accounting. Switching between writable alternates can retain the old
destination too: the common layer handles alternate 2 and the setter accepts
unchecked values, although the enum only declares factory and upgrade.

Give each transfer one explicit destination/length context, clear it on every
reset path, and validate alternate settings. Check actual address intervals
against partition bounds rather than only cumulative byte counts. For uploads,
`dfu_common_read_from_flash()` also needs to limit the final read to
`endaddr - addr`; checking only `addr < endaddr` allows reading past image end.
Test abort/retry after a committed sector, repeated resets, alternate changes,
partition boundaries and final short uploads with a fake flash map first.

### F5 — P1: malformed DFU inner lengths invoke an assertion

[dfu_servicer.c](../satellite-xmos-firmware/src/dfu_int/dfu_servicer.c) validates
the outer command length but forwards its first two bytes as `dnload_length`.
`dfu_int_download()` only checks this host-supplied value using
`xassert(length <= DFU_DATA_XFER_SIZE)` before copying. An otherwise valid
130-byte command with an embedded length of 129 can halt the firmware; builds
without the assertion would copy beyond the supplied 128-byte data field.

Return a command error for invalid embedded lengths before calling the state
machine. Validate state and available buffer space before copying too. Exercise
0, 1, 128, 129 and 65535 and confirm that audio continues after rejection.

### F6 — P2: dropped speaker-reference frames are invisible to the AEC

[main.c](../satellite-xmos-firmware/src/main.c), `speaker_pipeline_output()`,
drops the oldest reference when the two-frame queue fills, preserving playback.
`audio_pipeline_input()` later pairs a received reference with the next mic
frame. There is no sequence number, drop counter or explicit realignment.
Under overload the reference timeline can jump while the mic timeline continues,
potentially degrading echo cancellation. The acoustic effect needs measurement.

Add sequence/drop diagnostics and define a resynchronization policy. Stress
audio with beam steering, SPI bursts and DFU, then measure recovery and echo
suppression. Keep the nonblocking playback behavior.

## What can be removed or simplified

| Candidate | Evidence and benefit | Action / compatibility constraint |
| --- | --- | --- |
| Old USB DFU test and `orbit` fixtures | Firmware no longer builds USB DFU. The test still imported USB helpers and requested a missing `usb_dev_id` fixture; collection failed even with hardware tests disabled. | Removed in this change. No replacement hardware coverage is claimed. |
| `reference/adec`, `reference/adec_alt_arch`, `reference/empty`, `referenceless` | [Pipeline CMake](../satellite-xmos-firmware/audio_pipelines/CMakeLists.txt) adds reference and speaker; [reference CMake](../satellite-xmos-firmware/audio_pipelines/reference/CMakeLists.txt) exposes only fixed delay. Both supported targets use it. | Delete unused implementations in a follow-up, retaining git history. This reduces maintenance, not current firmware RAM or CPU. |
| USB transport sources and `modules/xua` | [Device-control CMake](../modules/fph/rtos_device_control/CMakeLists.txt) includes I2C/SPI, not USB; first-party CMake has no `modules/xua` consumer. | Remove USB-specific sources and submodule after checking a full recursive build. Do not remove SPI DFU because its state names come from USB DFU. |
| Filesystem demonstration WAVs, `ff_appconf.h`, filesystem helper macros | Current firmware image targets have no filesystem generation call; `filesystem_support` is about 1.1 MiB in this checkout. | Remove unused assets/helpers after checking tooling consumers. Savings are checkout/build maintenance, not runtime memory. Keep flash image/DFU partition handling until its consumers are resolved. |
| Dead `spi_read_payload_pending` bookkeeping | The variable in `device_control_spi.c` is assigned but never read; NOP handling is already unconditional. | Remove with SPI tests. Preserve one-time read consumption and startup status behavior. |
| DSP initialization in bypass | Both fixed-delay tile files unconditionally initialize AEC/IC/NS/AGC state even when stage processing is compiled out. Tile 0 still creates three stage tasks and seven pooled frames. | Guard state/init by skip flags; then consider collapsing bypass tasks and pool depth. Measure link-map/heap savings. Preserve left mic/right reference output, gain, HPF, DOA and watchdog progress. |

Keep four-mic acquisition: pair modes still use DOA history for direction reporting
and wake steering. Keep channel-1 taps, fast IC hold and the bypass diagnostic
variant absent usage evidence. The mic-array DC eliminator and 100 Hz HPF have
different purposes and positions in the chain; neither is proven redundant.

## Hardware acceleration and performance

XMOS provides vector/BFP, FFT and filtering APIs for the XS3 VPU; these are the
relevant acceleration facilities, as described in the
[XMOS library documentation](https://www.xmos.com/documentation/XM-015059-UG/html/doc/rst/src/introduction.html).
The proposals below are inferred from this checkout, not measured speedups. Use
the pinned library APIs; newer online documentation is not a reason by itself to
upgrade the dependencies.

Already implemented here:

- [doa.c](../satellite-xmos-firmware/src/audio/doa.c) uses `vect_s32_mul()` and
  `bfp_fft_forward_mono()` for windowing and four FFTs.
- [beamformer.c](../satellite-xmos-firmware/src/audio/beamformer.c) uses
  `vect_s32_dot()` for its fractional-delay filters.
- [mic_frontend.c](../satellite-xmos-firmware/src/audio/mic_frontend.c) uses vector
  scale/energy operations for gain and energy measurements.
- [voice_src.c](../satellite-xmos-firmware/src/audio/voice_src.c) already uses
  `fir_s32_24t()` / `conv_s32_24t()` for 48/16 kHz conversion.
- Frame pools, in-place DSP buffers and reduced tile-1-to-tile-0 frame payloads
  already remove several allocation/copy costs. IC/NS/AGC/VNR already receive
  targeted `-O2` flags in `firmware.cmake`.

Ranked experiments:

| Order | Change | Expected benefit and acceptance check |
| --- | --- | --- |
| 1 | Optimize the scalar part of DOA | Each active estimate has 52 bins, 208 `sqrtf()` calls and two direct-correlation passes. First share the lag phasor recurrence between NS/EW. Then prototype fixed-point/vector correlation or PHAT normalization if profiling warrants it. Compare angle, confidence, silence, reverberation and two-talker behavior against existing tests and recordings. |
| 2 | Reuse DOA FFT scratch by mic pair | `fft_buf[4][256]` occupies 4096 bytes. Process NS and EW sequentially using two buffers to potentially recover 2048 bytes without changing the FFT count. Verify cross-spectrum smoothing, buffer ownership and stack usage; the current four-spectrum lifetime requires a real restructuring. |
| 3 | Reduce beam steering worst-case work | A normal frame invokes 4 × 240 = 960 vector dot products; a crossfade doubles that to 1920. Profile call/setup overhead, block FIR alternatives and the per-sample 64-bit fade division. Preserve rounding, full-scale saturation, short frames and continuity; a generic FIR replacement is not automatically faster. |
| 4 | Reduce output intertile bandwidth if tile 1 has headroom | Output currently crosses at 48 kHz: 720 × 2 × 4 = 5760 bytes/frame. Crossing at 16 kHz and upsampling on tile 1 would send 1920 bytes/frame, saving 3840 bytes/frame (256 kB/s). This moves computation onto the AEC/I2S tile, so it may worsen timing despite lower traffic. Measure both tile loads and blocking time first. |
| 5 | Evaluate HPF kernels only if they are material in profiles | The two-channel Q28 biquad currently uses scalar 64-bit accumulation. Compare the pinned library's biquad implementation with matching coefficient, feedback and saturation conventions. There are only two independent channels and recurrence across samples, so a vector rewrite may not pay off. |

Do not trade several KiB of tile-1 RAM for a correlation lookup table without a
link map. Likewise, replacing the narrow 19-lag correlation with a 512-point
inverse FFT needs measurement. Avoid reducing DOA cadence until wake-history,
speech dwell and timeout semantics are expressed in elapsed time; several of
them currently count frames or DOA updates.

For DFU, `dfu_common_write_to_flash()` reads a whole sector into a heap buffer
and then overwrites every byte (`length == sector_size`). That preliminary read
is redundant under the current full-sector contract. Removing it is a modest
I/O improvement after the correctness fixes. Removing the heap copy also needs
confirmation of the asynchronous QSPI write buffer lifetime.

## Build and verification improvements

- Default `pytest` previously descended into initialized vendor submodules and
  imported `xmostest`/other vendor dependencies. `pytest.ini` now selects `tests/`.
  Removed obsolete USB fixtures rather than hiding their import failures.
- The only workflow, [release.yml](../.github/workflows/release.yml), runs on
  release publication or manual dispatch. Add PR host tests and builds of both
  supported firmware variants. The release list currently builds only fixed
  delay; bypass should at least receive compilation coverage.
- The host audio harness omits `voice_src.c`, SPI transport and DFU. Add golden
  SRC impulse/tone/full-scale tests and fake-SPI/fake-flash tests for F1–F5.
  Those are higher-value gaps than more tests of scalar helper functions.
- `satellite1.cmake` creates one versioning target per variant, both generating
  the same `src/version.h`. Before building variants in parallel, check for a
  shared-file race and consolidate generation or use per-target headers.
- `tools/ci/build_firmware.sh` uses a bare `exit` after printing a missing-input
  error, so it returns success. Change it to a nonzero exit and test this path.
- README now correctly identifies fixed delay, describes bypass outputs and
  fixes virtual-environment activation. It previously claimed automatic delay
  estimation for the active fixed-delay variant.

## Validation and next steps

Validation for this change: initialized the pinned `modules/core` dependencies,
installed `requirements_tests.txt`, and ran `python -m pytest -q`: **19 passed,
none skipped**, including the compiled audio front-end harness. Its first-party
C sources compile with `-Wall -Wextra -Werror`. No project-wide linter is
configured; Python syntax and diff whitespace are checked separately.

The container has no XTC compiler/simulator or attached Satellite1; firmware
linking, timing, flash operations and acoustic behavior remain unverified. No
firmware runtime code or public protocol was changed by this review.

Suggested implementation order:

1. Reproduce/fix F1–F2 with host transport tests, then validate with ESP32 SPI.
2. Fix F3–F5 together with fake-flash coverage and upgrade/abort/readback board
   tests. Decide and document whether alternate 2/data-partition writes remain
   part of the supported host contract.
3. Add PR checks and remove unused source/build paths. Build both variants and
   compare link maps before/after bypass simplification.
4. Capture stage timings, stack/heap minima and reference-drop counts under
   silence, speech, playback, beam crossfade, SPI traffic and DFU. The existing
   stats resource 250 provides a starting point. Measure queue/I2S/intertile wait
   time too: current stage timers do not cover all of it, and tasks share cores,
   so each stage being below 15 ms alone does not establish schedulability.
5. Implement the highest measured DSP bottleneck, preserving audio regression
   results and checking wake-word pickup/echo cancellation on real recordings.
