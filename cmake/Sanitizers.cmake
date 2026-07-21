function(gamenet_configure_sanitizers target_name)
    if(GAMENET_ENABLE_ASAN_UBSAN AND GAMENET_ENABLE_TSAN)
        message(FATAL_ERROR "ASan/UBSan and TSan cannot be enabled together.")
    endif()

    if(MSVC)
        if(GAMENET_ENABLE_ASAN_UBSAN)
            # MSVC's compiler switch both instruments objects and emits the
            # ASan runtime-link directives. Passing the compiler switch
            # directly to link.exe only produces LNK4044 and has no effect.
            target_compile_options(${target_name} PUBLIC /fsanitize=address)
        endif()
        return()
    endif()

    if(GAMENET_ENABLE_ASAN_UBSAN)
        target_compile_options(${target_name} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target_name} PUBLIC -fsanitize=address,undefined)
    endif()

    if(GAMENET_ENABLE_TSAN)
        target_compile_options(${target_name} PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target_name} PUBLIC -fsanitize=thread)
    endif()
endfunction()
