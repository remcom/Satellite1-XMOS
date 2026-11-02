##******************************************
## Create fixed_delay AEC+IC+NS+AGC 
##   2 mic input channels
#    2 reference input channels
##******************************************

add_library(fixed_delay_aec_ic_ns_agc_2mic_2ref INTERFACE)
target_sources(fixed_delay_aec_ic_ns_agc_2mic_2ref
    INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/fixed_delay/audio_pipeline_t0.c
        ${CMAKE_CURRENT_LIST_DIR}/fixed_delay/audio_pipeline_t1.c
        ${CMAKE_CURRENT_LIST_DIR}/fixed_delay/aec/aec_process_frame_1thread.c
)
target_include_directories(fixed_delay_aec_ic_ns_agc_2mic_2ref
    INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/fixed_delay
)
target_link_libraries(fixed_delay_aec_ic_ns_agc_2mic_2ref
    INTERFACE
        core::general
        rtos::freertos
        rtos::sw_services::generic_pipeline
        fwk_voice::aec
        fwk_voice::agc
        fwk_voice::ic
        fwk_voice::ns
        fwk_voice::vnr::features
        fwk_voice::vnr::inference
)

##*********************************************
## Create aliases for sln_voice example designs
##*********************************************

add_library(fph::ffva::ap::fixed_delay ALIAS fixed_delay_aec_ic_ns_agc_2mic_2ref)

##******************************************
## Create aec4 AEC+IC+NS+AGC
##   4 mic input channels into the AEC (the mic front end runs after it)
##   1 (mono) reference input channel
##
## lib_aec sizes its state structs for at most AEC_LIB_MAX_Y_CHANNELS (2) mics;
## its code only loops over the channel count given to aec_init(). This
## variant builds its own copy of lib_aec with the limit raised to 4, without
## touching the fwk_voice submodule, and a copy of lib_ic linked against it
## (lib_ic reuses lib_aec's FFT and filter helpers, which do not depend on the
## limit). The other variants keep the unmodified libraries.
##******************************************

get_target_property(SAT1_LIB_AEC_DIR fwk_voice_module_lib_aec SOURCE_DIR)
get_target_property(SAT1_LIB_IC_DIR fwk_voice_module_lib_ic SOURCE_DIR)

set(SAT1_AEC4_API_DIR ${CMAKE_CURRENT_BINARY_DIR}/aec4_lib_aec_api)
file(GLOB SAT1_LIB_AEC_API_HEADERS ${SAT1_LIB_AEC_DIR}/api/*.h)
file(COPY ${SAT1_LIB_AEC_API_HEADERS} DESTINATION ${SAT1_AEC4_API_DIR})
file(READ ${SAT1_LIB_AEC_DIR}/api/aec_defines.h SAT1_AEC_DEFINES)
# The state structs hold AEC_LIB_MAX_Y_CHANNELS x AEC_LIB_MAX_X_CHANNELS x 10
# filter phases of descriptors, so the reference limit drops to the one used.
string(REPLACE "#define AEC_LIB_MAX_Y_CHANNELS (2)" "#define AEC_LIB_MAX_Y_CHANNELS (4)"
       SAT1_AEC4_DEFINES "${SAT1_AEC_DEFINES}")
string(REPLACE "#define AEC_LIB_MAX_X_CHANNELS (2)" "#define AEC_LIB_MAX_X_CHANNELS (1)"
       SAT1_AEC4_DEFINES "${SAT1_AEC4_DEFINES}")
foreach(SAT1_AEC4_LIMIT "AEC_LIB_MAX_Y_CHANNELS (4)" "AEC_LIB_MAX_X_CHANNELS (1)")
    string(FIND "${SAT1_AEC4_DEFINES}" "#define ${SAT1_AEC4_LIMIT}" SAT1_AEC4_FOUND)
    if(SAT1_AEC4_FOUND EQUAL -1)
        message(FATAL_ERROR "aec4: could not set ${SAT1_AEC4_LIMIT} in ${SAT1_LIB_AEC_DIR}/api/aec_defines.h")
    endif()
endforeach()
file(WRITE ${SAT1_AEC4_API_DIR}/aec_defines.h "${SAT1_AEC4_DEFINES}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SAT1_LIB_AEC_API_HEADERS})

add_library(satellite1_lib_aec_4mic STATIC EXCLUDE_FROM_ALL
    ${SAT1_LIB_AEC_DIR}/src/aec_impl.c
    ${SAT1_LIB_AEC_DIR}/src/aec_l2_impl.c
    ${SAT1_LIB_AEC_DIR}/src/aec_priv_impl.c
)
# The patched copy of api/ comes first, so the sources (in src/) never see the original
target_include_directories(satellite1_lib_aec_4mic
    PUBLIC
        ${SAT1_AEC4_API_DIR}
        ${SAT1_LIB_AEC_DIR}/src
)
target_compile_options(satellite1_lib_aec_4mic PRIVATE -Os -g)
target_link_libraries(satellite1_lib_aec_4mic PUBLIC lib_xcore_math)

file(GLOB SAT1_LIB_IC_SOURCES ${SAT1_LIB_IC_DIR}/src/*.c)
add_library(satellite1_lib_ic_aec4 STATIC EXCLUDE_FROM_ALL ${SAT1_LIB_IC_SOURCES})
target_include_directories(satellite1_lib_ic_aec4 PUBLIC ${SAT1_LIB_IC_DIR}/api)
target_compile_options(satellite1_lib_ic_aec4 PRIVATE -Os -g)
target_link_libraries(satellite1_lib_ic_aec4
    PUBLIC
        lib_xcore_math
        satellite1_lib_aec_4mic
        fwk_voice::vnr::features
        fwk_voice::vnr::inference
)

add_library(fixed_delay_aec_ic_ns_agc_4mic_1ref INTERFACE)
target_sources(fixed_delay_aec_ic_ns_agc_4mic_1ref
    INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/fixed_delay/audio_pipeline_t0.c
        ${CMAKE_CURRENT_LIST_DIR}/fixed_delay/audio_pipeline_t1.c
        ${CMAKE_CURRENT_LIST_DIR}/fixed_delay/aec/aec_process_frame_1thread.c
)
target_include_directories(fixed_delay_aec_ic_ns_agc_4mic_1ref
    INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/fixed_delay
)
target_link_libraries(fixed_delay_aec_ic_ns_agc_4mic_1ref
    INTERFACE
        core::general
        rtos::freertos
        rtos::sw_services::generic_pipeline
        satellite1_lib_aec_4mic
        satellite1_lib_ic_aec4
        fwk_voice::agc
        fwk_voice::ns
        fwk_voice::vnr::features
        fwk_voice::vnr::inference
)

add_library(fph::ffva::ap::aec4 ALIAS fixed_delay_aec_ic_ns_agc_4mic_1ref)
