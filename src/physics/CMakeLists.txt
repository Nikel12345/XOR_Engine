# ============================================================
#  Physics — модуль физики.
#  Линкует EngineGpu (GPU-кор) + EngineEcs (ECS): без Engine, рендера, ImGui, SDL_image.
#  Своего PCH не использует — доказательство, что зависимость чистая.
# ============================================================
add_library(Physics STATIC)
target_compile_features(Physics PUBLIC cxx_std_23)

set(PHYSICS_SRC
    src/PhysicsBufferSet.cpp
    src/PhysicsComputeSet.cpp
    src/CollisionShapes.cpp
    src/ContactSystem.cpp
    src/DebugColliderSystem.cpp
)
set(PHYSICS_HDR
    include/PhysicsBufferSet.h
    include/PhysicsComputeSet.h
    include/Colliders.h
    include/ColliderQuery.h
    include/CollisionShapes.h
    include/ContactSystem.h
    include/DebugColliderSystem.h
)
list(TRANSFORM PHYSICS_SRC PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM PHYSICS_HDR PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
target_sources(Physics PRIVATE ${PHYSICS_SRC} ${PHYSICS_HDR})
source_group("Source"  FILES ${PHYSICS_SRC})
source_group("Headers" FILES ${PHYSICS_HDR})

target_include_directories(Physics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# EngineEcs PUBLIC: ObjectManager и компоненты стоят в ЗАГОЛОВКАХ физики (ColliderQuery.h), то
# есть входят в её контракт. EngineGpu PRIVATE: GpuContext появляется только в .cpp, в заголовках
# от него форвард-декларация — физика берёт GPU для своих задач, но потребитель физики GPU-кор
# вместе с ней не получает.
target_link_libraries(Physics PUBLIC  EngineEcs)
target_link_libraries(Physics PRIVATE EngineGpu)
