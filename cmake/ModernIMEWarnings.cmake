function(modernime_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # PUBLIC, not INTERFACE: the flags must also apply to the target
        # itself, which INTERFACE-only flags never reach (MODULE libraries
        # have no consumers to inherit them).
        target_compile_options(${target} PUBLIC
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    elseif(MSVC)
        target_compile_options(${target} PUBLIC /W4)
    endif()
endfunction()
