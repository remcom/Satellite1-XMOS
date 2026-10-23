import shutil
import subprocess
from pathlib import Path

import pytest

from tests.conftest import SAT1_FIRMWARE_SRC

CC = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
TEST_SRC = Path(__file__).resolve().parent / "audio_frontend_test.c"
AUDIO_SRC = SAT1_FIRMWARE_SRC / "src" / "audio"


@pytest.mark.skipif(CC is None, reason="a host C compiler is required")
def test_audio_frontend_host(tmp_path: Path):
    binary = tmp_path / "audio_frontend_test"
    subprocess.run(
        [
            CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
            f"-I{SAT1_FIRMWARE_SRC / 'src'}",
            str(TEST_SRC),
            str(AUDIO_SRC / "doa.c"),
            str(AUDIO_SRC / "mic_frontend.c"),
            "-lm",
            "-o", str(binary),
        ],
        check=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(result.stdout)
    assert result.returncode == 0, result.stdout
