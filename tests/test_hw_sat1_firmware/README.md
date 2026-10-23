# Satellite1 hardware tests

These tests need a Satellite1 connected through an xTAG and the `orbit` test helpers.
They are skipped unless `HW_TESTS` is set:

```bash
HW_TESTS=build_and_xrun pytest tests/test_hw_sat1_firmware
```

With `HW_TESTS=build_and_xrun` the session builds the `satellite1_firmware_fixed_delay`
upgrade image and runs it with `xrun` before the tests start.

USB DFU was removed from the firmware, so there is no USB DFU test any more. DFU now
runs over SPI device control (see `docs/device-control-spi-protocol.md`).
