# ============================================================
#  Sheaf — формат сцены (docs/sheaf.md). Самостоятельный лист: ни движка, ни SDL, только
#  стандартная библиотека. Его линкуют EngineEcs (сохранение сцены) и SheafDump, которому
#  движок не нужен.
# ============================================================
add_library(Sheaf STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Sheaf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SheafReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Sheaf.h
)
target_compile_features(Sheaf PUBLIC cxx_std_23)
target_include_directories(Sheaf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(Sheaf PROPERTIES FOLDER "sheaf")

add_subdirectory(dump)
