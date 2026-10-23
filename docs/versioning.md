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

## Releases

Releases are built by `.github/workflows/release.yml`. Publishing a GitHub release (or pre-release) triggers the workflow, which writes the release tag into `firmware_version.txt`, builds the firmware with that version and attaches the binaries and Debian package to the release. The tag must follow one of the formats above (e.g. `v1.2.0` or `v1.2.0-beta.1`), otherwise the build fails before compiling.

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