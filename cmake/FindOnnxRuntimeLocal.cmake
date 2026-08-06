function(cloakframe_onnxruntime_from_root)
    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        HINTS
            "${ONNXRUNTIME_ROOT}/include"
            "$ENV{ONNXRUNTIME_ROOT}/include"
    )
    if(WIN32)
        find_library(ONNXRUNTIME_LIBRARY
            NAMES onnxruntime
            HINTS
                "${ONNXRUNTIME_ROOT}/lib"
                "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native"
                "$ENV{ONNXRUNTIME_ROOT}/lib"
        )
        find_file(ONNXRUNTIME_RUNTIME
            NAMES onnxruntime.dll
            HINTS
                "${ONNXRUNTIME_ROOT}/lib"
                "${ONNXRUNTIME_ROOT}/bin"
                "${ONNXRUNTIME_ROOT}/runtimes/win-x64/native"
                "$ENV{ONNXRUNTIME_ROOT}/lib"
        )
    else()
        find_library(ONNXRUNTIME_LIBRARY
            NAMES onnxruntime
            HINTS
                "${ONNXRUNTIME_ROOT}/lib"
                "$ENV{ONNXRUNTIME_ROOT}/lib"
        )
    endif()

    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY OR
       (WIN32 AND NOT ONNXRUNTIME_RUNTIME))
        message(FATAL_ERROR "ONNX Runtime was not found. Configure with -DONNXRUNTIME_ROOT=/path/to/onnxruntime")
    endif()

    if(WIN32)
        add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
        set_target_properties(onnxruntime::onnxruntime PROPERTIES
            IMPORTED_IMPLIB "${ONNXRUNTIME_LIBRARY}"
            IMPORTED_LOCATION "${ONNXRUNTIME_RUNTIME}"
            INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
        )
    else()
        add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED GLOBAL)
        set_target_properties(onnxruntime::onnxruntime PROPERTIES
            IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
        )
    endif()
endfunction()
