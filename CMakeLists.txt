cmake_minimum_required(VERSION 3.20)
project(XOR_Engine)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RuntimeDlls.cmake")

# /MP: параллельная компиляция TU ВНУТРИ одного таргета (MSBuild сам параллелит только
# проекты между собой; без /MP ~25 cpp Engine собираются по одному → это и было главным
# множителем «правка одного заголовка = 30 секунд»).
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/MP>")

# ============================================================
#  SDL3 — собирается ИЗ ИСХОДНИКОВ (external/SDL3, форк release-3.4.14).
#  Раньше тут лежал бинарный дистрибутив (include/ + libSDL3.dll.a + bin/SDL3.dll);
#  перешли на исходники, чтобы править GPU-бэкенд (вторая vulkan-очередь: загрузка
#  отдельно от рендера).
#
#  Наружу торчит цель SDL3::SDL3 (алиас SDL3-shared) — она несёт И include-пути,
#  И линк. Руками пути прописывать НЕЛЬЗЯ: SDL_revision.h генерируется в бинарный
#  каталог, одного external/SDL3/include не хватит.
#
#  SDL3_image / SDL3_ttf остаются бинарными: они грузят SDL3.dll динамически, ABI 3.4.14
#  совпадает с тем, что собираем. Если бампнешь версию SDL — обнови и их.
# ============================================================
set(SDL_SHARED       ON  CACHE BOOL "" FORCE)   # нужен именно SDL3.dll: к нему прилинкованы image/ttf
set(SDL_STATIC       OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)   # SDL3_test не нужен; заодно гасит SDL_TESTS
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)
add_subdirectory(external/SDL3)

# Проекты SDL — в отдельную папку решения, чтобы не мешались с таргетами движка.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
foreach(_sdl_target SDL3-shared SDL_uclibc SDL3-collect-sources)
    if(TARGET ${_sdl_target})
        set_target_properties(${_sdl_target} PROPERTIES FOLDER "external/SDL3")
    endif()
endforeach()

# ============================================================
#  SDL3_shadercross — тоже из исходников (external/SDL3_shadercross, см. SHADERCROSS_FORK.md).
#  Статическая библиотека внутри EngineGpu. SPIRV-Cross собирается из исходников рядом;
#  DXC — готовые бинарники релиза Microsoft (сборка DXC из исходников = LLVM, не окупается).
#  dxcompiler.dll/dxil.dll кладёт рядом с exe copy_runtime_dlls — иначе Windows найдёт
#  чужой dxcompiler по PATH (например, из Vulkan SDK) другой версии.
# ============================================================
set(SPIRV_CROSS_STATIC        ON  CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED        OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI           OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS  OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL  ON  CACHE BOOL "" FORCE)
add_subdirectory(external/SDL3_shadercross/external/SPIRV-Cross EXCLUDE_FROM_ALL)
# shadercross делает export(TARGETS SDL3_shadercross-static), а CMake требует, чтобы и его
# зависимости лежали в каком-нибудь export set. Его собственный vendored-режим делает то же самое.
set(_spirv_cross_targets)
foreach(_t spirv-cross-c spirv-cross-core spirv-cross-glsl spirv-cross-hlsl spirv-cross-msl
           spirv-cross-cpp spirv-cross-reflect spirv-cross-util)
    if(TARGET ${_t})
        list(APPEND _spirv_cross_targets ${_t})
    endif()
endforeach()
export(TARGETS ${_spirv_cross_targets} NAMESPACE "SDL3_shadercross::" FILE "spirv-cross-targets.cmake")

set(SDLSHADERCROSS_VENDORED          OFF CACHE BOOL "" FORCE)   # ON собирал бы DXC из исходников
set(SDLSHADERCROSS_DXC               ON  CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_SHARED            OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_STATIC            ON  CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_SPIRVCROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_CLI               OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_INSTALL           OFF CACHE BOOL "" FORCE)
set(SDLSHADERCROSS_TESTS             OFF CACHE BOOL "" FORCE)
add_subdirectory(external/SDL3_shadercross EXCLUDE_FROM_ALL)

foreach(_sc_target SDL3_shadercross-static spirv-cross-c spirv-cross-core spirv-cross-glsl
                   spirv-cross-hlsl spirv-cross-msl spirv-cross-cpp spirv-cross-reflect spirv-cross-util)
    if(TARGET ${_sc_target})
        set_target_properties(${_sc_target} PROPERTIES FOLDER "external/SDL3_shadercross")
    endif()
endforeach()

add_subdirectory(src/engine)
add_subdirectory(src/physics)
