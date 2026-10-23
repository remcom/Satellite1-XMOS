# Satellite1 XMOS firmware

## Variants
**satellite1_firmware_fixed_delay**

Uses the 'Automatic Delay Estimation and Correction' pipeline of the sln_voice example repository.

**satellite1_firmware_bypass**

A variant which bypasses the AEC/IC/NS/AGC stages. Hence, the mic signal (after the mic front end and mic gain) is streamed to the ESP32-S3.

## Audio front end

All four PDM microphones (North, South, East, West) are decimated. The mic front end
reduces them to the two channels the AEC/IC/NS/AGC pipeline processes. The mode, the
mic gain and the direction of arrival are accessible through the audio servicer
(device-control resource `230`, see [docs/device-control-spi-protocol.md](docs/device-control-spi-protocol.md)).

| Mode | Value | Channels |
| --- | --- | --- |
| `MIC_MODE_PAIR_EW` | `0` | East, West (default, the pre-4-mic behaviour) |
| `MIC_MODE_PAIR_NS` | `1` | North, South |
| `MIC_MODE_AVERAGE` | `2` | (North+East)/2, (South+West)/2 |
| `MIC_MODE_AUTO_PAIR` | `3` | The mic facing the talker (from the DOA estimate) and the mic opposite it |

The mic gain (default 20 dB, range 0..30 dB) is applied after the AEC, so loud echo is
not clipped before the AEC can cancel it.

The direction of arrival (DOA) is estimated with GCC-PHAT on the North/South and
East/West pairs during speech, and only while no audio is played back. It can be read
by the host and optionally shown on the LED ring (`LED_RING_SERVICER_CMD_DOA_INDICATOR`).

## Firmware Files

**variant_name.factory.bin**

A complete image of the flash memory: the boot partition, containing the flash loader and factory image.

**variant_name.upgrade.bin**

A firmware upgrade image that can be uploaded through the Device Firmware Update (DFU) service. Requires a factory image with DFU support running on the device.


**variant_name.xe**

The XMOS executable (XE) binary format stores programs for XMOS devices and includes information about the system it is intended to run on, allowing support for multiple program loads, configurations and debugging.


## Flashing via Satellite1 ESPHome Firmware
> **Note:** The Satellite1 does not come with a pre-flashed XMOS firmware. The initial firmware needs to be written directly to the flash memory via SPI.

The [Satellite1 ESPHome firmware](https://github.com/FutureProofHomes/Satellite1-ESPHome) running on the ESP32-S3 writes the XMOS flash over SPI. Use a `variant_name.factory.bin` for this.

## Upgrading via DFU over SPI
A device running a factory firmware with DFU support accepts `variant_name.upgrade.bin`
through the DFU servicer (device-control resource `240`) over SPI. See
[docs/device-control-spi-protocol.md](docs/device-control-spi-protocol.md) for the commands.
USB DFU (`dfu-util`) is no longer supported.


## Running / Flashing via xTAG
> **Note**: The Satellite1 does not include an xTAG debugger. This option applies only when testing the firmware with a developer board like the XCORE.AI EVALUATION KIT. 

When the XMOS board is connected as a USB xTag device, the firmware can be run or flashed as follows:

Running without flashing:

```bash
xrun --xscope variant_name.xe
```

Flashing:
```bash
xflash --quad-spi-clock 50MHz --factory variant_name.xe --boot-partition-size 0x100000
```


## Building the firmware locally

### Clone repository

```bash
git clone https://github.com/FutureProofHomes/Satellite1-XMOS.git
cd Satellite1-XMOS
git submodule update --init --recursive
```

### Setup XTC-Tools
Download XTX-15.3.1 from https://www.xmos.com/software-tools/

On Mac, the original software requires installing into `/Applications`. If you want to install into another directory, change `XMOS_TOOL_PATH` in `${INSTALL_DIR}/SetEnv.sh` to :
```bash
export XMOS_TOOL_PATH=${0:A:h};
```

### Setup Python Virtual Environment
The `xmos-ai-tools` python package is required for building the firmware modules.
The build generates the VNR model source (`modules/voice/modules/lib_vnr/src/inference/model/trained_model_xcore.tflite.*`) with the `xcore-opt` of this package, and does not regenerate it when the package version changes. After changing the `xmos-ai-tools` version, delete those generated files, or the old model fails to compile against the new runtime.
It is recommended to install it into a virtual environment:

```bash
python3.10 -m venv .venv
source activate .venv/bin/activate
pip install -r requirements.txt
```

for later builds simply activate the environment before calling cmake:
```bash
source activate .venv/bin/activate
```


### Creating factory and upgrade images

On Linux and Mac run:

```bash
cmake -B build --toolchain xmos_cmake_toolchain/xs3a.cmake
cd build

make create_flash_img_variant_name
make create_upgrade_img_variant_name
```

On Windows run:
```bash
cmake -G Ninja -B build --toolchain xmos_cmake_toolchain/xs3a.cmake
cd build

ninja create_flash_img_variant_name
ninja create_upgrade_img_variant_name
```

### Building the XMOS executable only (.xe file)
Run the following commands in the root folder to build the firmware.

On Linux and Mac run:

```bash
cmake -B build --toolchain xmos_cmake_toolchain/xs3a.cmake
cd build

make variant_name
```

On Windows run:
```bash
cmake -G Ninja -B build --toolchain xmos_cmake_toolchain/xs3a.cmake
cd build

ninja variant_name
```





