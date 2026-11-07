# Setting XMOS firmware version

## Overview

The XMOS firmware version is build into the firmware itself and can be received via DFU (SPI+USB).
Before actually building the firmware binaries, the versioning.py script is called which creates the `version.h` file which sets the current firmware version.

The following firmwares are supported (1 byte for each: MAJOR, MINOR, PATCH, PRE-RELEASE, COUNTER ):

```python
v{[0-255]}.{[0-255]}.{[0-255]}
v{[0-255]}.{[0-255]}.{[0-255]}-alpha
v{[0-255]}.{[0-255]}.{[0-255]}-beta
v{[0-255]}.{[0-255]}.{[0-255]}-rc
v{[0-255]}.{[0-255]}.{[0-255]}-dev
v{[0-255]}.{[0-255]}.{[0-255]}-alpha.{[1-255]}
v{[0-255]}.{[0-255]}.{[0-255]}-beta.{[1-255]}
v{[0-255]}.{[0-255]}.{[0-255]}-rc.{[1-255]}
v{[0-255]}.{[0-255]}.{[0-255]}-dev.{[1-255]}
```

The version does not say which build variant (`fixed_delay`, `aec4`, ...) is running, because all variants of a release share it. Read the variant with the separate `DFU_GETVARIANT` command (resid `92`); see [the device-control SPI protocol](device-control-spi-protocol.md) for the values.

## Releases

Releases are built by `.github/workflows/release.yml`. Publishing a GitHub release (or pre-release) triggers the workflow, which writes the release tag into `firmware_version.txt`, builds the firmware with that version and attaches the binaries and Debian package to the release. The tag must follow one of the formats above (e.g. `v1.2.0` or `v1.2.0-beta.1`), otherwise the build fails before compiling.

### Release assets

The factory images are attached under their plain name and again with the release tag appended (`-<tag>`), so images of different releases are easy to keep apart and to switch between. For a release `v1.2.0`:

| Asset | Versioned asset | Contents |
| --- | --- | --- |
| `satellite1_xmos.factory.bin` / `.md5` | `satellite1_xmos-v1.2.0.factory.bin` / `.md5` | Default firmware (fixed_delay) |
| `satellite1_xmos_aec4.factory.bin` / `.md5` | `satellite1_xmos_aec4-v1.2.0.factory.bin` / `.md5` | Experimental: AEC on all four mics |
| – | `satellite_firmware_apps-v1.2.0.zip` | All CI build outputs (`.xe`, factory and upgrade images) |
| – | `satellite1-xmos-firmware_1.2.0-1_all.deb` | Debian package with the default firmware |

The zip is only attached under its versioned name: the Documentation repo unzips `*.zip` from the release, which fails if a release holds more than one zip.

To switch firmware in ESPHome, change `xmos_fw_version` (and pick the variant in the file name):

```yaml
substitutions:
  xmos_fw_version: "v1.2.0"

memory_flasher:
  - platform: satellite1
    id: xflash
    embed_flash_image:
      image_version: ${xmos_fw_version}
      image_file: https://github.com/remcom/Satellite1-XMOS/releases/download/${xmos_fw_version}/satellite1_xmos-${xmos_fw_version}.factory.bin
      md5_file: https://github.com/remcom/Satellite1-XMOS/releases/download/${xmos_fw_version}/satellite1_xmos-${xmos_fw_version}.factory.md5
      # aec4 variant:
      # image_file: https://github.com/remcom/Satellite1-XMOS/releases/download/${xmos_fw_version}/satellite1_xmos_aec4-${xmos_fw_version}.factory.bin
      # md5_file: https://github.com/remcom/Satellite1-XMOS/releases/download/${xmos_fw_version}/satellite1_xmos_aec4-${xmos_fw_version}.factory.md5
```

The workflow can also be started manually (Actions → Build and Release Firmware → Run workflow) to build without publishing; it then uses the given version, or `<last tag>-dev` when left empty.

## firmware_version.txt
By default the versioning.py scripts obtains the actual firmware from the `firmware_version.txt` in the project root.

If it contains the string 'dev', ...


## Keeping Track of Dev-Builds


### Suggested workflow for developing/testing firmware for the Satellite1 hardware

```yaml
memory_flasher:
  - platform: satellite1
    id: xflash
    embed_flash_image:
      image_version: v1.0.1-dev.8
      image_file: /WS/Satellite1-XMOS/build/satellite1_firmware_fixed_delay.factory.bin
      md5_file: /WS/Satellite1-XMOS/build/satellite1_firmware_fixed_delay.factory.md5 
```

```yaml
packages:
  core_board: !include common/core_board.yaml
  wifi: !include common/wifi_improv.yaml
  sensors: !include common/hat_sensors.yaml
  buttons: !include common/buttons.yaml
  ha: !include common/home_assistant.yaml
  mp: !include common/media_player.yaml
  va: !include common/voice_assistant.yaml
  timer: !include common/timer.yaml
  led_ring: !include common/led_ring.yaml

  xmos: !include /WS/Satellite1-XMOS/build/embed_flash_imag.yaml  
    
  ## OPTIONAL COMPONENTS 
  # mmwave_ld2410: !include common/mmwave_ld2410.yaml
  # mmwave_ld2450: !include common/mmwave_ld2450.yaml
  debug: !include common/debug.yaml


memory_flasher:
  - platform: satellite1
    id: !extend xflash
    # embed_flash_image:
    #   image_version: ${xmos_fw_version}
    #   #image_file: https://raw.githubusercontent.com/FutureProofHomes/Documentation/refs/heads/main/assets/firmware/xmos/${xmos_fw_version}/satellite1_firmware_fixed_delay.factory.bin
    #   #md5_file: https://raw.githubusercontent.com/FutureProofHomes/Documentation/refs/heads/main/assets/firmware/xmos/${xmos_fw_version}/satellite1_firmware_fixed_delay.factory.md5
    

    on_flashing_start:    
      then:
        - lambda: id(xmos_flashing_state) = 1;
        - script.execute: control_leds

```