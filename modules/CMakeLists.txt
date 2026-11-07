## Add frameworks
add_subdirectory(core)

add_subdirectory(io)
add_subdirectory(lib_qspi_fast_read)
add_subdirectory(rtos)
add_subdirectory(fph)

if(${CMAKE_SYSTEM_NAME} STREQUAL XCORE_XS3A)
    ## Need to guard so host targets will not be built

    ## lib_vnr generates its model source into the (shared) source tree with the
    ## xcore-opt of xmos-ai-tools, and its rule only depends on the input model, so
    ## after a package upgrade the stale model fails to compile against the new
    ## runtime. Delete the generated files whenever the package version differs
    ## from the one this build directory last saw, so the rule runs again.
    ## A package upgrade re-runs this configure step on its own: lib_vnr include()s
    ## the package's aitoolslib.cmake, which pip rewrites.
    set(_VNR_MODEL_DIR ${CMAKE_CURRENT_LIST_DIR}/voice/modules/lib_vnr/src/inference/model)
    set(_VNR_STAMP ${CMAKE_BINARY_DIR}/vnr_model_xmos_ai_tools.version)
    execute_process(
        COMMAND python -c "import importlib.metadata as m; print(m.version('xmos-ai-tools'))"
        OUTPUT_VARIABLE _AITOOLS_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _AITOOLS_RESULT
    )
    if(NOT _AITOOLS_RESULT EQUAL 0 OR _AITOOLS_VERSION STREQUAL "")
        message(FATAL_ERROR "xmos-ai-tools is not installed for 'python'; install requirements.txt (see README)")
    endif()
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

    add_subdirectory(voice)
    add_subdirectory(inferencing)
    add_subdirectory(sw_pll/lib_sw_pll)

    ## The following alias is added to support in intermediate version of fwk_voice
    ##  This can be removed once fwk_voice is updated to use the new, core::lib_tflite_micro alias
    add_library(sdk::inferencing::lib_tflite_micro ALIAS inferencing_tflite_micro)
endif()

## Add additional modules
add_subdirectory(sample_rate_conversion)
add_subdirectory(xscope_fileio)