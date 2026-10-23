import os
from pathlib import Path

PROJ_ROOT = Path(__file__).resolve().parent.parent
SAT1_FIRMWARE_SRC = PROJ_ROOT / "satellite-xmos-firmware"

# Hardware tests run only when HW_TESTS is set, e.g. HW_TESTS=build_and_xrun
HW_TESTS = os.environ.get("HW_TESTS")
