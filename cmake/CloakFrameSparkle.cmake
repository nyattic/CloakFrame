include(FetchContent)

set(CLOAKFRAME_SPARKLE_VERSION "2.9.4")
set(CLOAKFRAME_SPARKLE_SHA256
    "ce89daf967db1e1893ed3ebd67575ed82d3902563e3191ca92aaec9164fbdef9")

function(cloakframe_enable_sparkle target)
    FetchContent_Declare(sparkle
        URL "https://github.com/sparkle-project/Sparkle/releases/download/${CLOAKFRAME_SPARKLE_VERSION}/Sparkle-${CLOAKFRAME_SPARKLE_VERSION}.tar.xz"
        URL_HASH SHA256=${CLOAKFRAME_SPARKLE_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sparkle)

    # FetchContent sets sparkle_SOURCE_DIR in this function's scope only; the packaging
    # module needs it from the top-level directory.
    set(CLOAKFRAME_SPARKLE_DIR "${sparkle_SOURCE_DIR}" CACHE INTERNAL
        "Directory holding the fetched Sparkle.framework")

    target_link_libraries(${target} PRIVATE "${sparkle_SOURCE_DIR}/Sparkle.framework")
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${sparkle_SOURCE_DIR}")
    target_compile_definitions(${target} PRIVATE CLOAKFRAME_HAVE_SPARKLE)
endfunction()
