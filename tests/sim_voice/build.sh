#!/usr/bin/env bash
# Builds the voice pipeline simulator for the host: the firmware's audio code,
# lib_voice's AEC, IC and NS and lib_xcore_math's portable C kernels.
# The AEC is built with room for 4 mics and 2 references (an aec_conf.h and
# task schedule from lib_voice's generator, as generate_schedule() in
# satellite1.cmake makes for each firmware target), so one binary runs every
# AEC configuration.
#
# usage: tests/sim_voice/build.sh <out dir>
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:?out dir}"
mkdir -p "$OUT/obj" "$OUT/aec_conf"
CC="${CC:-cc}"
# The AEC schedule generator needs numpy, which xmos-ai-tools brings (requirements.txt)
if [[ -z "${PYTHON:-}" ]]; then
  PYTHON=python3
  [[ -x "$ROOT/.venv/bin/python" ]] && PYTHON="$ROOT/.venv/bin/python"
fi

XMATH="$ROOT/modules/lib_xcore_math/lib_xcore_math"
VOICE="$ROOT/modules/voice/lib_voice"
FW="$ROOT/satellite-xmos-firmware"

# <threads> <mics> <references> <main phases> <shadow phases>
"$PYTHON" -I "$ROOT/modules/voice/tests/shared/python/generate_task_distribution_scheme.py" \
  --config "1 4 2 10 5" --out-dir "$OUT/aec_conf" > /dev/null

INC=(
  -I"$HERE"
  -I"$OUT/aec_conf"
  -I"$XMATH/api"
  -I"$VOICE/api"
  -I"$VOICE/api/aec" -I"$VOICE/src/aec"
  -I"$VOICE/api/ic" -I"$VOICE/src/ic"
  -I"$VOICE/api/ns" -I"$VOICE/src/ns"
  -I"$VOICE/api/vnr"
  -I"$FW/src"
  -I"$FW/audio_pipelines/reference/fixed_delay"
)

objs=()
compile() {
  local src="$1" flags="$2"
  local obj="$OUT/obj/$(echo "$src" | sed "s|$ROOT/||; s|/|_|g").o"
  if [[ ! -f "$obj" || "$src" -nt "$obj" || "$HERE/build.sh" -nt "$obj" ]]; then
    # shellcheck disable=SC2086
    "$CC" -std=gnu99 -O2 -D__aec_conf_h_exists__ $flags "${INC[@]}" -c "$src" -o "$obj"
  fi
  objs+=("$obj")
}

for d in vect bfp dct fft filter scalar arch/ref etc/xmath_fft_lut; do
  while IFS= read -r f; do
    compile "$f" "-w -I$XMATH/src/vect -I$XMATH/src/etc/xmath_fft_lut"
  done < <(find "$XMATH/src/$d" -name '*.c' | sort)
done
for f in "$VOICE"/src/aec/*.c "$VOICE"/src/ic/*.c "$VOICE"/src/ns/*.c "$OUT/aec_conf/aec_task_distribution.c"; do
  compile "$f" "-w"
done
for f in doa mic_frontend beamformer vnr_hold echo_gate; do
  compile "$FW/src/audio/$f.c" "-Wall -Wextra -Werror"
done
for f in sim_room sim_pipeline sim_main; do
  compile "$HERE/$f.c" "-Wall -Wextra -Wno-unused-parameter"
done

"$CC" "${objs[@]}" -lm -o "$OUT/sim_voice"
echo "$OUT/sim_voice"
