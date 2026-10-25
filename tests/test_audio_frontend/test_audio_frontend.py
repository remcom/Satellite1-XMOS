import shutil
import subprocess
from pathlib import Path

import pytest

from tests.conftest import PROJ_ROOT, SAT1_FIRMWARE_SRC

CC = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
TEST_SRC = Path(__file__).resolve().parent / "audio_frontend_test.c"
AUDIO_SRC = SAT1_FIRMWARE_SRC / "src" / "audio"

# The DOA and gain code run on the VPU through lib_xcore_math, which ships
# portable C reference implementations of its kernels for host builds.
XMATH = PROJ_ROOT / "modules" / "core" / "modules" / "xcore_math" / "lib_xcore_math" / "lib_xcore_math"
XMATH_SRC_DIRS = ["vect", "bfp", "dct", "fft", "filter", "scalar", "arch/ref", "etc/xmath_fft_lut"]
XMATH_INCLUDES = [XMATH / "api", XMATH / "src" / "vect", XMATH / "src" / "etc" / "xmath_fft_lut"]


def build_xmath(out_dir: Path) -> list[Path]:
    sources = sorted(p for d in XMATH_SRC_DIRS for p in (XMATH / "src" / d).rglob("*.c"))
    objects = []
    for i, src in enumerate(sources):
        obj = out_dir / f"xmath_{i}.o"
        subprocess.run(
            [CC, "-std=c99", "-O2", "-w", *(f"-I{inc}" for inc in XMATH_INCLUDES),
             "-c", str(src), "-o", str(obj)],
            check=True,
        )
        objects.append(obj)
    return objects


@pytest.mark.skipif(CC is None, reason="a host C compiler is required")
@pytest.mark.skipif(not XMATH.is_dir(), reason="lib_xcore_math submodule is not checked out")
def test_audio_frontend_host(tmp_path: Path):
    xmath_objects = build_xmath(tmp_path)
    binary = tmp_path / "audio_frontend_test"
    subprocess.run(
        [
            CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
            f"-I{SAT1_FIRMWARE_SRC / 'src'}",
            *(f"-I{inc}" for inc in XMATH_INCLUDES[:1]),
            str(TEST_SRC),
            str(AUDIO_SRC / "doa.c"),
            str(AUDIO_SRC / "mic_frontend.c"),
            str(AUDIO_SRC / "beamformer.c"),
            *(str(o) for o in xmath_objects),
            "-lm",
            "-o", str(binary),
        ],
        check=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(result.stdout)
    assert result.returncode == 0, result.stdout
