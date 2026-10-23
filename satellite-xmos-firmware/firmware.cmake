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
