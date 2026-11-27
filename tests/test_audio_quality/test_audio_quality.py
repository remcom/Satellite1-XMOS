import shutil
import subprocess
from pathlib import Path

import pytest

from tests.conftest import PROJ_ROOT, SAT1_FIRMWARE_SRC

CC = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
HERE = Path(__file__).resolve().parent
XMATH = PROJ_ROOT / "modules/lib_xcore_math/lib_xcore_math/api"


@pytest.mark.skipif(CC is None, reason="a host C compiler is required")
@pytest.mark.skipif(not XMATH.is_dir(), reason="lib_xcore_math submodule is not checked out")
def test_audio_quality_host(tmp_path):
    binary = tmp_path / "audio_quality_test"
    subprocess.run(
        [CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
         f"-I{HERE / 'stubs'}", f"-I{SAT1_FIRMWARE_SRC / 'src'}", f"-I{XMATH}",
         str(HERE / "audio_quality_test.c"),
         str(SAT1_FIRMWARE_SRC / "src/audio/audio_quality.c"),
         str(SAT1_FIRMWARE_SRC / "src/audio/audio_quality_snapshot.c"),
         "-lm", "-o", str(binary)],
        check=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.skipif(CC is None, reason="a host C compiler is required")
@pytest.mark.skipif(not XMATH.is_dir(), reason="lib_xcore_math submodule is not checked out")
def test_reference_queue_accounting(tmp_path):
    # Compile the production function without the rest of main.c's platform
    # startup. Keep the test on the real queue branch rather than a copy of it.
    source = (SAT1_FIRMWARE_SRC / "src/main.c").read_text()
    start = source.index("void speaker_pipeline_output(")
    end = source.index("void audio_pipeline_input(", start)
    (tmp_path / "speaker_output.inc").write_text(source[start:end])
    binary = tmp_path / "reference_queue_test"
    subprocess.run(
        [CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
         f"-I{tmp_path}", f"-I{SAT1_FIRMWARE_SRC / 'src'}", f"-I{XMATH}",
         str(HERE / "reference_queue_test.c"),
         str(SAT1_FIRMWARE_SRC / "src/audio/audio_quality.c"),
         "-lm", "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True, timeout=30)


@pytest.mark.skipif(CC is None, reason="a host C compiler is required")
@pytest.mark.skipif(not XMATH.is_dir(), reason="lib_xcore_math submodule is not checked out")
def test_stats_dispatch_compatibility(tmp_path):
    source = (SAT1_FIRMWARE_SRC / "src/stats_servicer/stats_servicer.c").read_text()
    start = source.index("static control_cmd_info_t stats_servicer_resid_cmd_map")
    end = source.index("void stats_servicer(void *args)")
    (tmp_path / "stats_dispatch.inc").write_text(source[start:end])
    helpers = (SAT1_FIRMWARE_SRC / "src/control/servicer.c").read_text()
    start = helpers.index("control_cmd_info_t* get_cmd_info(")
    end = helpers.index("// Process write commands directed to the servicer resource", start)
    (tmp_path / "servicer_helpers.inc").write_text(helpers[start:end])
    binary = tmp_path / "stats_dispatch_test"
    subprocess.run(
        [CC, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-sign-compare",
         f"-I{tmp_path}", f"-I{HERE / 'stubs'}", f"-I{SAT1_FIRMWARE_SRC / 'src'}",
         f"-I{SAT1_FIRMWARE_SRC / 'src/control'}", f"-I{XMATH}",
         f"-I{PROJ_ROOT / 'modules/fph/rtos_device_control/api'}",
         str(HERE / "stats_dispatch_test.c"), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True, timeout=30)
