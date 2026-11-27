# DOA detector energy input

Task 136 addresses recommendation 5 of task 134's
`/data/reports/134-look-for-improvements.md` (reviewed 2026-11-21 at `33b430f`).
The remaining gap was the speech-energy/noise-floor calculation in
`doa_process()`: it read broadband raw microphones before the existing audio HPF.

## Repository and PR check

On 2026-11-23, the worktree and freshly fetched `origin/develop` both resolved to
`33b430fca80bed96891c6dc939c7f4bae630b83b`, matching the report. All repository
PRs were checked, including open, closed and merged states; none were open.

- Merged [PR #6](https://github.com/remcom/Satellite1-XMOS/pull/6) already added the
  configurable 100 Hz HPF, wake-word steering, channel-1 taps and fast IC hold.
- Merged [PR #8](https://github.com/remcom/Satellite1-XMOS/pull/8) already narrowed
  GCC-PHAT to 300–3000 Hz and added `aec4`. Its detector still preceded the HPF.
- Closed, unmerged [PR #7](https://github.com/remcom/Satellite1-XMOS/pull/7) was an
  audit, not a detector-ordering fix.

## Input correction

The detector copies the four raw microphone channels (North, South, East, West)
into its existing FFT scratch storage, then applies the existing second-order
100 Hz Butterworth HPF before calculating mean-square energy. The same filtered
energy drives both the speech threshold and the minimum-tracking noise floor.
Two `mic_hpf_t` instances provide four independent channel histories. Their shared
declarations are in `audio/mic_hpf.h`; the existing filter implementation and
coefficients are unchanged.

The detector filters every frame, including non-speech and playback frames, so
history remains continuous. `doa_init()` clears its history and noise floor; the
first frame initializes the floor without declaring speech. As with the existing
HPF, an abrupt DC step can create a startup transient before it decays.

This is a private detector input correction in `doa_process()`, shared by
`fixed_delay`, `bypass` and `aec4`. It leaves microphone inputs untouched.
GCC-PHAT still reads the original input and runs only on eligible speech frames;
beam delays, channel selection, capture/AEC ordering, playback gating and its
27-frame hangover retain their existing behavior. Scratch is reused before the
FFT overwrites it, avoiding another frame buffer. Added filter state is 104 bytes
(two 52-byte filter instances); the copy and four scalar biquads add frame work.

`AUDIO_SERVICER_CMD_HPF` (`7`) still defaults to `1`. It controls the audio HPF on
the two pipeline channels in `fixed_delay`/`bypass`, or all four raw microphones
before the AEC in `aec4`. Setting it to `0` bypasses that audio filter; re-enabling
resets its history. The detector HPF is always active and has separate state, so
toggling the audio HPF neither bypasses nor resets detection filtering. The main
audio path is not filtered twice.

## Validation and limits

The deterministic host regressions in `tests/test_audio_frontend` cover:

- 25 Hz energy attenuation and 1 kHz energy retention through `doa_process()`;
- a speech-band onset over stronger low-frequency rumble, and equal-level quiet
  rumble/speech-band inputs at the detector's absolute threshold;
- independent history and equal weighting of all four channels across frames;
- unchanged input samples, startup floor initialization, DC decay during playback,
  silence settling, reset reproducibility and independence from the audio HPF switch.

The existing direction, beam/crossfade, playback hangover, steering and HPF-response
tests also pass. The C harness compiles the changed firmware code against the
pinned lib_xcore_math reference kernels with `-Wall -Wextra -Werror`. The host
versioning and packaging tests pass: running
`.venv/bin/python -m pytest -q tests/test_audio_frontend tests/test_versioning tests/test_package_firmware_deb.py`
reports **23 passed**, with no skips. Validation copies that restore raw energy
or incorrectly share HPF history between channel pairs fail the new regressions.
No project-wide linter is configured;
`git diff --check` is used for whitespace validation.

Broad test collection also encounters the existing hardware suite's unconditional
`orbit` import, which is unavailable here. That suite was left unchanged and was
not run; the explicit host-suite command above avoids hardware tests.

The local firmware build command (`bash tools/build_local.sh -x fixed_delay bypass`)
stops because the XTC environment script is absent. Neither `xcc`, `xsim` nor
`cmake` is available here, so target builds for `fixed_delay`, `bypass` and `aec4`,
linker RAM reports and target timing checks remain unavailable. On a target or
equipped simulator, check tile-1 RAM and front-end maximum stage time with speech,
silence and playback, including wake-word steering, against the 15 ms frame budget.

Physical detection improvement remains unmeasured: synthetic host tests verify
the energy path and state behavior, not room acoustics, microphone response,
real HVAC/traffic/footstep recordings or wake-word recall/false-accept rates.
The 100 Hz filter attenuates low rumble but does not restrict detection to the
GCC-PHAT 300–3000 Hz band or reject every low-frequency onset. Hardware A/B tests
and representative recordings are still needed to quantify detection/localization
improvement. No hardware was deployed to, flashed or run for this change.
