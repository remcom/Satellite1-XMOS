"""Turns the simulator's JSON lines (one file per scenario) into markdown tables.

usage: python3 tests/sim_voice/summarize.py <results dir>
"""

import json
import statistics
import sys
from pathlib import Path


def load(d: Path, name: str) -> list[dict]:
    p = d / f"{name}.jsonl"
    if not p.exists():
        return []
    return [json.loads(line) for line in p.read_text().splitlines() if line.strip()]


def table(header: list[str], rows: list[list]) -> str:
    out = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    for r in rows:
        out.append("| " + " | ".join(f"{v:.1f}" if isinstance(v, float) else str(v) for v in r) + " |")
    return "\n".join(out)


def mean(xs) -> float:
    return statistics.mean(xs) if xs else float("nan")


def main() -> None:
    d = Path(sys.argv[1])
    parts = []

    r = load(d, "aec_ref")
    if r:
        parts.append("### Mono AEC reference: stereo music, mono speaker\n")
        parts.append(table(
            ["seed", "ERLE 2x2 mean", "ERLE 2x2 worst 10%", "ERLE 2x1 mean", "ERLE 2x1 worst 10%", "ERLE aec4 4x1 mean"],
            [[x["seed"], x["erle_2x2_mean"], x["erle_2x2_p10"], x["erle_2x1_mean"], x["erle_2x1_p10"], x["erle_4x1_mean"]]
             for x in r]))
    r = load(d, "aec_ref_doubletalk")
    if r:
        parts.append("\n\nch1 SNR of six utterances said over the music:\n")
        parts.append(table(["seed", "ch1 SNR 2x2", "ch1 SNR 2x1", "gain"],
                           [[x["seed"], x["snr_ch1_2x2"], x["snr_ch1_2x1"], x["snr_ch1_2x1"] - x["snr_ch1_2x2"]]
                            for x in r]))

    r = load(d, "ref_mix")
    if r:
        parts.append("\n\n### Reference mix: TAS2780 set to the left channel\n")
        parts.append(table(
            ["seed", "2-mic, (L+R)/2 ref", "2-mic, L ref", "aec4, (L+R)/2 ref", "aec4, L ref"],
            [[x["seed"], x["normal_downmix_erle"], x["normal_left_erle"], x["aec4_downmix_erle"], x["aec4_left_erle"]]
             for x in r]))

    r = load(d, "doa_playback")
    if r:
        parts.append("\n\n### aec4: wake words over music, DOA before vs after the AEC\n")
        parts.append(table(
            ["music (echo dBFS)", "talker az", "DOA results/wake before", "after",
             "beam on talker (of 3) before", "after", "beam error before", "after", "cmd SNR before", "after"],
            [[x["music_db"], x["talker_az"], x["pre_doa_updates_per_wake"], x["post_doa_updates_per_wake"],
              x["pre_beam_on_talker"], x["post_beam_on_talker"], x["pre_beam_err_deg"], x["post_beam_err_deg"],
              x["pre_cmd_snr"], x["post_cmd_snr"]] for x in r]))
        parts.append(f"\n\nMean command SNR change: {mean([x['post_cmd_snr'] - x['pre_cmd_snr'] for x in r]):+.1f} dB, "
                     f"wake word SNR change: {mean([x['post_wake_snr'] - x['pre_wake_snr'] for x in r]):+.1f} dB.")
    r = load(d, "doa_music_only")
    if r:
        parts.append("\n\nMusic only, nobody talking (post-AEC DOA, steering on any speech):\n")
        parts.append(table(["music (echo dBFS)", "DOA results", "frames", "beam moves"],
                           [[x["music_db"], x["doa_updates"], x["frames"], x["beam_steers"]] for x in r]))

    r = load(d, "mic_mode")
    if r:
        parts.append("\n\n### Mic mode: talker with a TV, ch1 SNR\n")
        parts.append(table(
            ["build", "talker az", "TV az", "wake PAIR_EW", "wake AUTO_PAIR", "wake BEAM", "cmd PAIR_EW", "cmd AUTO_PAIR", "cmd BEAM"],
            [[x["build"], x["talker_az"], x["tv_az"], x["wake_snr_pair_ew"], x["wake_snr_auto_pair"], x["wake_snr_beam"],
              x["cmd_snr_pair_ew"], x["cmd_snr_auto_pair"], x["cmd_snr_beam"]] for x in r]))

    r = load(d, "ic_reset")
    if r:
        parts.append("\n\n### Experiment: clear the IC filter on every re-steer (not adopted)\n")
        parts.append(table(
            ["mode", "build", "talker az", "1st cmd keep", "1st cmd reset", "cmds keep", "cmds reset"],
            [[x["mode"], x["build"], x["talker_az"], x["first_cmd_snr_keep"], x["first_cmd_snr_reset"],
              x["cmd_snr_keep"], x["cmd_snr_reset"]] for x in r]))

    r = load(d, "steer_playback")
    if r:
        parts.append("\n\n### Re-steering during playback: 2-mic vs aec4\n")
        parts.append(table(["seed", "talker az", "ERLE after wake, 2-mic", "ERLE after wake, aec4"],
                           [[x["seed"], x["talker_az"], x["erle_after_wake_normal"], x["erle_after_wake_aec4"]]
                            for x in r]))

    r = load(d, "tap")
    if r:
        parts.append("\n\n### Wake word channel tap (SNR only, no wake word model)\n")
        parts.append(table(["noise dBFS", "AEC+IC+NS", "AEC+IC", "AEC"],
                           [[x["noise_db"], x["wake_snr_ns"], x["wake_snr_ic"], x["wake_snr_aec"]] for x in r]))

    r = [x for x in load(d, "ic_check") if "mode" in x]
    if r:
        parts.append("\n\n### IC fast hold (existing feature), ch1 SNR of the wake words\n")
        parts.append(table(["mode", "tap", "fast hold off", "fast hold on"],
                           [[m, t,
                             next(x["wake_snr"] for x in r if x["mode"] == m and x["tap"] == t and x["fast_hold"] == 0),
                             next(x["wake_snr"] for x in r if x["mode"] == m and x["tap"] == t and x["fast_hold"] == 1)]
                            for m in ("pair_ew", "beam") for t in ("ic", "ns")]))
    print("\n".join(parts))


if __name__ == "__main__":
    main()
