#!/usr/bin/env bash
# Builds the voice pipeline simulator for the host: the firmware's audio code,
# fwk_voice lib_aec/lib_ic/lib_ns and lib_xcore_math's portable C kernels.
# lib_aec is built with room for 4 mics and 2 references (as
# audio_pipelines/reference/CMakeLists.txt does for aec4), so one binary runs
# every AEC configuration.
#
# usage: tests/sim_voice/build.sh <out dir>
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${1:?out dir}"
mkdir -p "$OUT/obj" "$OUT/aec_api"
CC="${CC:-cc}"

XMATH="$ROOT/modules/core/modules/xcore_math/lib_xcore_math/lib_xcore_math"
VOICE="$ROOT/modules/voice/modules"
FW="$ROOT/satellite-xmos-firmware"

cp "$VOICE"/lib_aec/api/*.h "$OUT/aec_api/"
sed -i.bak -e 's/#define AEC_LIB_MAX_Y_CHANNELS (2)/#define AEC_LIB_MAX_Y_CHANNELS (4)/' "$OUT/aec_api/aec_defines.h"
grep -q "AEC_LIB_MAX_Y_CHANNELS (4)" "$OUT/aec_api/aec_defines.h"

INC=(
  -I"$HERE"
  -I"$OUT/aec_api"
  -I"$XMATH/api"
  -I"$VOICE/lib_aec/src"
  -I"$VOICE/lib_ic/api" -I"$VOICE/lib_ic/src"
  -I"$VOICE/lib_ns/api" -I"$VOICE/lib_ns/src"
  -I"$VOICE/lib_vnr/api/common" -I"$VOICE/lib_vnr/api/features" -I"$VOICE/lib_vnr/api/inference"
  -I"$FW/src"
  -I"$FW/audio_pipelines/reference/fixed_delay"
)

objs=()
compile() {
  local src="$1" flags="$2"
  local obj="$OUT/obj/$(echo "$src" | sed "s|$ROOT/||; s|/|_|g").o"
  if [[ ! -f "$obj" || "$src" -nt "$obj" || "$HERE/build.sh" -nt "$obj" ]]; then
    # shellcheck disable=SC2086
    "$CC" -std=gnu99 -O2 $flags "${INC[@]}" -c "$src" -o "$obj"
  fi
  objs+=("$obj")
}

for d in vect bfp dct fft filter scalar arch/ref etc/xmath_fft_lut; do
  while IFS= read -r f; do
    compile "$f" "-w -I$XMATH/src/vect -I$XMATH/src/etc/xmath_fft_lut"
  done < <(find "$XMATH/src/$d" -name '*.c' | sort)
done
for f in "$VOICE"/lib_aec/src/*.c "$VOICE"/lib_ic/src/*.c "$VOICE"/lib_ns/src/*.c; do
  compile "$f" "-w"
done
compile "$FW/audio_pipelines/reference/fixed_delay/aec/aec_process_frame_1thread.c" "-w"
for f in doa mic_frontend beamformer vnr_hold echo_gate; do
  compile "$FW/src/audio/$f.c" "-Wall -Wextra -Werror"
done
for f in sim_room sim_pipeline sim_main; do
  compile "$HERE/$f.c" "-Wall -Wextra -Wno-unused-parameter"
done

"$CC" "${objs[@]}" -lm -o "$OUT/sim_voice"
echo "$OUT/sim_voice"
