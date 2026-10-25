#**********************
# Gather Sources
#**********************

file(GLOB_RECURSE APP_SOURCES   ${CMAKE_CURRENT_LIST_DIR}/src/*.c)

set(APP_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/control
    ${CMAKE_CURRENT_LIST_DIR}/src/dfu_int
)

include(${CMAKE_CURRENT_LIST_DIR}/bsp_config/bsp_config.cmake)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/audio_pipelines)

set(VERSIONING_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/versioning.py)
option(USE_DEV_TRACKING "Enable dev-build tracking" OFF)
option(ALLOW_DIRTY_VERSIONING "Allow versioning.py to run from a dirty workspace for controlled CI inputs or local throwaway builds" OFF)

#**********************
# Flags
#**********************
set(APP_COMPILER_FLAGS
    -Os
    -g
    -report
    -mcmodel=large
    -Wno-xcore-fptrgroup
)

set(APP_COMPILE_DEFINITIONS
    DEBUG_PRINT_ENABLE=0
    PLATFORM_USES_TILE_0=1
    PLATFORM_USES_TILE_1=1
)

# Per-frame DSP code is built for speed; the rest stays -Os to save RAM (tile 1 is nearly full)
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/src/audio/doa.c
    ${CMAKE_CURRENT_LIST_DIR}/src/audio/mic_frontend.c
    ${CMAKE_CURRENT_LIST_DIR}/src/audio/beamformer.c
    PROPERTIES COMPILE_OPTIONS "-O2"
)

# The voice libraries build with -Os. Building them with -O2 trades RAM for
# speed; check the gain with the stats servicer's stage times before keeping it.
option(SATELLITE1_VOICE_LIBS_O2 "Build the AEC/IC/NS/AGC/VNR libraries with -O2" OFF)
if(SATELLITE1_VOICE_LIBS_O2)
    foreach(VOICE_LIB
            fwk_voice_module_lib_aec
            fwk_voice_module_lib_ic
            fwk_voice_module_lib_ns
            fwk_voice_module_lib_agc
            fwk_voice_module_lib_vnr_features
            fwk_voice_module_lib_vnr_inference)
        # Appended after the library's own -Os, so -O2 wins
        target_compile_options(${VOICE_LIB} PRIVATE -O2)
    endforeach()
endif()

set(APP_LINK_OPTIONS
    -lquadspi
    -report
    -lotp3
)

set(APP_COMMON_LINK_LIBRARIES
    fph::device_control
    lib_src
    lib_sw_pll
)


#**********************
# Firmware variants
#  fixed_delay: AEC+IC+NS+AGC pipeline
#  bypass:      fixed_delay build with all DSP stages skipped
#**********************
set(FFVA_PIPELINES_INT
    fixed_delay
    bypass
)

#**********************
# XMOS Firmware Targets
#**********************
include(${CMAKE_CURRENT_LIST_DIR}/satellite1.cmake)
