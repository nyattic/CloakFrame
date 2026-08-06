function(cloakframe_harden target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /guard:cf
            /GS
            /sdl
        )
        if(CLOAKFRAME_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_link_options(${target} PRIVATE
            /guard:cf
            /DYNAMICBASE
            /NXCOMPAT
            /HIGHENTROPYVA
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -fstack-protector-strong
            -fno-strict-aliasing
            $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
        )
        if(CLOAKFRAME_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if(NOT APPLE)
            target_link_options(${target} PRIVATE
                -Wl,-z,relro
                -Wl,-z,now
                -Wl,-z,noexecstack
            )
        endif()
    endif()
endfunction()
