##******************************************
## Create fixed_delay AEC+IC+NS+AGC
##   fixed_delay: 2 mic channels into the AEC (after the mic front end)
##   aec4:        4 mic channels into the AEC (the mic front end runs after it)
##   Both with 1 (mono) reference channel.
##
## The AEC and IC of lib_voice are INTERFACE libraries, compiled as part of
## each tile target, whose AEC state is sized by the aec_conf.h that
## lib_voice's generate_schedule() makes for that target (satellite1.cmake).
## So both variants link the same libraries.
##******************************************

foreach(SAT1_AP_NAME fixed_delay aec4)
    set(SAT1_AP_TARGET satellite1_ap_${SAT1_AP_NAME})
    add_library(${SAT1_AP_TARGET} INTERFACE)
    target_sources(${SAT1_AP_TARGET}
        INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/fixed_delay/audio_pipeline_t0.c
            ${CMAKE_CURRENT_LIST_DIR}/fixed_delay/audio_pipeline_t1.c
    )
    target_include_directories(${SAT1_AP_TARGET}
        INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}
            ${CMAKE_CURRENT_LIST_DIR}/fixed_delay
    )
    target_link_libraries(${SAT1_AP_TARGET}
        INTERFACE
            core::general
            rtos::freertos
            rtos::sw_services::generic_pipeline
            lib_voice::aec
            lib_voice::agc
            lib_voice::ic
            lib_voice::ns
            lib_voice::vnr
    )
    add_library(fph::ffva::ap::${SAT1_AP_NAME} ALIAS ${SAT1_AP_TARGET})
endforeach()
