cmake_minimum_required(VERSION 3.20)
project(XOR_Engine)

include("${CMAKE_SOURCE_DIR}/cmake/RuntimeDlls.cmake")

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
#  SDL3_image / SDL3_ttf / SDL3_shadercross остаются бинарными: они грузят SDL3.dll
#  динамически, ABI 3.4.14 совпадает с тем, что собираем. Если бампнешь версию SDL —
#  обнови и их.
# ============================================================
set(SDL_SHARED       ON  CACHE BOOL "" FORCE)   # нужен именно SDL3.dll: к нему прилинкованы image/ttf/shadercross
set(SDL_STATIC       OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)   # SDL3_test не нужен; заодно гасит SDL_TESTS
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)   # install() у нас свой, в src/game и src/mygame
add_subdirectory(external/SDL3)

# Проекты SDL — в отдельную папку решения, чтобы не мешались с таргетами движка.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
foreach(_sdl_target SDL3-shared SDL_uclibc SDL3-collect-sources)
    if(TARGET ${_sdl_target})
        set_target_properties(${_sdl_target} PROPERTIES FOLDER "external/SDL3")
    endif()
endforeach()

add_subdirectory(src/engine)
add_subdirectory(src/physics)
add_subdirectory(src/game)
add_subdirectory(src/mygame)
add_subdirectory(src/sandbox)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Game)
