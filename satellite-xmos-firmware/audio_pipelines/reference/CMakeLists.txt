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
