function(copy_runtime_dlls target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/external/SDL3/bin/SDL3.dll"
            $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/external/SDL3_image/lib/x64/SDL3_image.dll"
            $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/external/SDL3_shadercross/bin/SDL3_shadercross.dll"
            $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/external/SDL3_ttf/lib/x64/SDL3_ttf.dll"
            $<TARGET_FILE_DIR:${target}>
    )
endfunction()