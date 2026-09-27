function(copy_runtime_dlls target)
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(_dxc_bin "${_root}/external/SDL3_shadercross/external/DirectXShaderCompiler-binaries/bin/x64")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>   # собирается из external/SDL3, а не лежит готовым в bin/
            $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_root}/external/SDL3_image/lib/x64/SDL3_image.dll"
            $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_dxc_bin}/dxcompiler.dll"
            "${_dxc_bin}/dxil.dll"
            $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_root}/external/SDL3_ttf/lib/x64/SDL3_ttf.dll"
            $<TARGET_FILE_DIR:${target}>
    )
endfunction()