"""Smoke test of the voice pipeline simulator (tests/sim_voice, docs/voice-simulation.md)."""

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from tests.conftest import PROJ_ROOT, SAT1_FIRMWARE_SRC

HERE = Path(__file__).resolve().parent
XMATH = PROJ_ROOT / "modules" / "core" / "modules" / "xcore_math" / "lib_xcore_math" / "lib_xcore_math"
VOICE = PROJ_ROOT / "modules" / "voice" / "modules"


@pytest.mark.skipif(shutil.which("cc") is None, reason="a host C compiler is required")
@pytest.mark.skipif(not (XMATH.is_dir() and VOICE.is_dir()), reason="submodules are not checked out")
def test_sim_voice_smoke(tmp_path: Path):
    out = subprocess.run([str(HERE / "build.sh"), str(tmp_path)], check=True, capture_output=True, text=True)
    binary = out.stdout.strip().splitlines()[-1]
    run = subprocess.run(
        [binary, str(SAT1_FIRMWARE_SRC / "filesystem_support"), "smoke"],
        check=True, capture_output=True, text=True, timeout=600,
    )
    result = json.loads(run.stdout.strip().splitlines()[-1])
    print(result)
    # The mono reference matches the mono speaker; L/R references do not converge to one solution
    assert result["erle_2x1"] > result["erle_2x2"] + 6.0
    # The echo gate keeps the post-AEC DOA from steering the beam at the speaker
    assert result["music_only_beam_steers"] == 0
