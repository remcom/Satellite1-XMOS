query_tools_version()

foreach(FFVA_AP ${FFVA_PIPELINES_INT})

    set(FFVA_INT_COMPILE_DEFINITIONS
    ${APP_COMPILE_DEFINITIONS}
        appconfEXTERNAL_MCLK=0
        appconfI2S_ENABLED=1
        appconfI2S_MODE=appconfI2S_MODE_MASTER
        appconfI2S_AUDIO_SAMPLE_RATE=48000
        appconfDEVICE_CTRL_SPI=1
    )

    if(${FFVA_AP} STREQUAL bypass )
      set(PL_NAME fixed_delay)
      list(APPEND FFVA_INT_COMPILE_DEFINITIONS appconfPIPELINE_BYPASS=1)
    else()
      set(PL_NAME ${FFVA_AP})
      list(APPEND FFVA_INT_COMPILE_DEFINITIONS appconfPIPELINE_BYPASS=0)
    endif()

    #**********************
    # Tile Targets
    #**********************
    set(TARGET_NAME tile0_satellite1_firmware_${FFVA_AP})
    add_executable(${TARGET_NAME} EXCLUDE_FROM_ALL)
    target_sources(${TARGET_NAME} PUBLIC ${APP_SOURCES})
    target_include_directories(${TARGET_NAME} PUBLIC ${APP_INCLUDES})
    target_compile_definitions(${TARGET_NAME}
        PUBLIC
            ${FFVA_INT_COMPILE_DEFINITIONS}
            THIS_XCORE_TILE=0
    )
    target_compile_options(${TARGET_NAME} PRIVATE ${APP_COMPILER_FLAGS})
    target_link_libraries(${TARGET_NAME}
        PUBLIC
            ${APP_COMMON_LINK_LIBRARIES}
            fph::ffva::satellite1
            fph::ffva::ap::${PL_NAME}
            sln_voice::app::ffva::sp::passthrough
    )
    target_link_options(${TARGET_NAME} PRIVATE ${APP_LINK_OPTIONS})
    unset(TARGET_NAME)

    set(TARGET_NAME tile1_satellite1_firmware_${FFVA_AP})
    add_executable(${TARGET_NAME} EXCLUDE_FROM_ALL)
    target_sources(${TARGET_NAME} PUBLIC ${APP_SOURCES})
    target_include_directories(${TARGET_NAME} PUBLIC ${APP_INCLUDES})
    target_compile_definitions(${TARGET_NAME}
        PUBLIC
            ${FFVA_INT_COMPILE_DEFINITIONS}
            THIS_XCORE_TILE=1
    )
    target_compile_options(${TARGET_NAME} PRIVATE ${APP_COMPILER_FLAGS})
    target_link_libraries(${TARGET_NAME}
        PUBLIC
            ${APP_COMMON_LINK_LIBRARIES}
            fph::ffva::satellite1
            fph::ffva::ap::${PL_NAME}
            sln_voice::app::ffva::sp::passthrough
    )
    target_link_options(${TARGET_NAME} PRIVATE ${APP_LINK_OPTIONS})
    unset(TARGET_NAME)

    #*********************
    # Create version.h
    #*********************
    SET(VERSIONING_CMD "build")
    if(ALLOW_DIRTY_VERSIONING)
        list(APPEND VERSIONING_CMD "--allow-dirty")
    endif()
    if(USE_DEV_TRACKING)
        list(APPEND VERSIONING_CMD "--track")
    endif()

    add_custom_target(satellite1_firmware_${FFVA_AP}_versioning
        COMMAND ${Python3_EXECUTABLE} ${VERSIONING_SCRIPT} ${VERSIONING_CMD} satellite1_firmware_${FFVA_AP}
        COMMENT "Running versioning.py build satellite1_firmware_${FFVA_AP}"
        VERBATIM
    )
    add_dependencies(tile0_satellite1_firmware_${FFVA_AP} satellite1_firmware_${FFVA_AP}_versioning)
    add_dependencies(tile1_satellite1_firmware_${FFVA_AP} satellite1_firmware_${FFVA_AP}_versioning)

    #**********************
    # Merge binaries
    #**********************    
    merge_binaries(satellite1_firmware_${FFVA_AP} tile0_satellite1_firmware_${FFVA_AP} tile1_satellite1_firmware_${FFVA_AP} 1)

    #**********************
    # Create run and debug targets
    #**********************
    create_run_target(satellite1_firmware_${FFVA_AP})
    create_debug_target(satellite1_firmware_${FFVA_AP})
    create_upgrade_img_target(satellite1_firmware_${FFVA_AP} ${XTC_VERSION_MAJOR} ${XTC_VERSION_MINOR})
    
    #**********************
    # Create flash targets
    #**********************
    create_flash_image_target(
        #[[ Target ]]                  satellite1_firmware_${FFVA_AP}
        #[[ Boot Partition Size ]]     0x100000
    )
    create_flash_app_target(
        #[[ Target ]]                  satellite1_firmware_${FFVA_AP}
        #[[ Boot Partition Size ]]     0x100000
    )
endforeach()
