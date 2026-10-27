# Satellite1 hardware tests

There are currently no automated Satellite1 hardware tests in this repository.
The obsolete USB DFU test and its `orbit` fixtures have been removed: the firmware
supports DFU over SPI device control, not USB.

A replacement suite needs a Satellite1, an SPI host and a recovery/flashing path.
It should exercise upgrade/upload round trips, incomplete final sectors,
abort/retry, alternate-setting changes and malformed commands. See the
[SPI protocol](../../docs/device-control-spi-protocol.md) and
[code review](../../docs/codebase-review.md) for the outstanding cases.

The host suite runs from the repository root with `python -m pytest`; it does not
validate flash operations, RTOS scheduling or real-time audio on the board.
