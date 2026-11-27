## Add frameworks

## fwk_core, without add_subdirectory(core): its modules/CMakeLists.txt builds
## lib_xcore_math from its own nested submodule (v2.4.0), and lib_voice needs
## v3.0.1, which modules/lib_xcore_math provides. The rest is fwk_core's list.
set(FRAMEWORK_CORE_ROOT_PATH ${CMAKE_CURRENT_LIST_DIR}/core CACHE STRING "Root folder of framework_core in this cmake project tree")
foreach(_CORE_MODULE clock_control l2_cache legacy_compat multitile_support otpinfo random trycatch utils)
    add_subdirectory(core/modules/${_CORE_MODULE})
endforeach()
add_subdirectory(lib_xcore_math/lib_xcore_math)

if((${CMAKE_SYSTEM_NAME} STREQUAL XCORE_XS3A) OR (${CMAKE_SYSTEM_NAME} STREQUAL XCORE_XS2A))
    ## As fwk_core's core::general and core::all
    add_library(framework_core_general INTERFACE)
    target_link_libraries(framework_core_general
        INTERFACE
            framework_core_clock_control
            framework_core_legacy_compat
            framework_core_utils
            lib_l2_cache
            lib_otpinfo
            lib_random
            lib_trycatch
            lib_xcore_math
    )
    add_library(core::general ALIAS framework_core_general)

    add_library(framework_core_all INTERFACE)
    target_link_libraries(framework_core_all
        INTERFACE
            core::general
    )
    add_library(core::all ALIAS framework_core_all)
endif()

add_subdirectory(io)
add_subdirectory(lib_qspi_fast_read)
add_subdirectory(rtos)
add_subdirectory(fph)

if(${CMAKE_SYSTEM_NAME} STREQUAL XCORE_XS3A)
    ## Need to guard so host targets will not be built

    ## lib_voice's VNR links the ai_tools target: the TFLite Micro runtime of
    ## the xmos-ai-tools Python package (requirements.txt), as xmos/ai_tools'
    ## cfg_target.cmake defines it for xcommon_cmake builds.
    execute_process(
        COMMAND python -c "import os, xmos_ai_tools, xmos_ai_tools.runtime as rt; print(os.path.dirname(rt.__file__)); print(xmos_ai_tools.__version__)"
        OUTPUT_VARIABLE _AITOOLS_OUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _AITOOLS_RESULT
    )
    if(NOT _AITOOLS_RESULT EQUAL 0 OR _AITOOLS_OUT STREQUAL "")
        message(FATAL_ERROR "xmos-ai-tools is not installed for 'python'; install requirements.txt (see README)")
    endif()
    string(REPLACE "\n" ";" _AITOOLS_OUT "${_AITOOLS_OUT}")
    list(GET _AITOOLS_OUT 0 _AITOOLS_PATH)
    list(GET _AITOOLS_OUT 1 _AITOOLS_VERSION)
    add_library(ai_tools STATIC IMPORTED GLOBAL)
    target_compile_definitions(ai_tools
        INTERFACE
            TF_LITE_STATIC_MEMORY
            TF_LITE_STRIP_ERROR_STRINGS
            XCORE
            NO_INTERPRETER
    )
    set_target_properties(ai_tools PROPERTIES
        LINKER_LANGUAGE CXX
        IMPORTED_LOCATION ${_AITOOLS_PATH}/lib/libxtflitemicro_xs3a.a
        INTERFACE_INCLUDE_DIRECTORIES ${_AITOOLS_PATH}/include
    )

    ## lib_voice generates the VNR model source with the xcore-opt of
    ## xmos-ai-tools, and its rule only depends on the input model, so after a
    ## package upgrade the stale model fails to compile against the new runtime.
    ## Delete the generated files whenever the package version differs from the
    ## one this build directory last saw, so the rule runs again. A package
    ## upgrade re-runs this configure step on its own: the runtime library above
    ## is rewritten by pip.
    set(_VNR_MODEL_DIR ${CMAKE_CURRENT_BINARY_DIR}/voice/lib_voice/src.autogen/vnr_model)
    set(_VNR_STAMP ${CMAKE_BINARY_DIR}/vnr_model_xmos_ai_tools.version)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_AITOOLS_PATH}/lib/libxtflitemicro_xs3a.a)
    set(_VNR_STAMP_VERSION "")
    if(EXISTS ${_VNR_STAMP})
        file(READ ${_VNR_STAMP} _VNR_STAMP_VERSION)
        string(STRIP "${_VNR_STAMP_VERSION}" _VNR_STAMP_VERSION)
    endif()
    if(NOT _VNR_STAMP_VERSION STREQUAL _AITOOLS_VERSION)
        message(STATUS "xmos-ai-tools ${_AITOOLS_VERSION} (last: '${_VNR_STAMP_VERSION}'): regenerating VNR model")
        file(REMOVE
            ${_VNR_MODEL_DIR}/trained_model_xcore.tflite
            ${_VNR_MODEL_DIR}/trained_model_xcore.tflite.cpp
            ${_VNR_MODEL_DIR}/trained_model_xcore.tflite.h
        )
        file(WRITE ${_VNR_STAMP} "${_AITOOLS_VERSION}\n")
    endif()

    add_subdirectory(voice/lib_voice)
    add_subdirectory(sw_pll/lib_sw_pll)
endif()

## Add additional modules
add_subdirectory(sample_rate_conversion)
add_subdirectory(xscope_fileio)
