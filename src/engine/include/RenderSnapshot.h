#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <functional>
#include <SDL3/SDL_gpu.h>
#include "RenderCommandData.h"

struct BufferData;
struct PushConstantBinder;

// Пер-слотовые слепки «что рисуем»: sim готовит их в той же фазе и для того же слота, что и
// GPU-буферы. Замков нет и не нужно — sim пишет слепок, пока владеет слотом, рендер читает после
// того, как слот стал готовым, и happens-before даёт сам жизненный цикл слота (SlotController).
namespace RenderSnap {

    // Порядок в cams = порядок записи LIGHT_CAMERA_BUFFER, поэтому индекс здесь и есть
    // camera_index. Совпадение держится тем, что буфер и слепок пишет один модуль в один prepare.
    struct ShadowCam {
        float   max_range = 0.0f;
        uint8_t is_ortho = 0;
        uint8_t needs_render = 0;
    };

    struct LightCams {
        std::vector<ShadowCam> cams;
        // Число ИСТОЧНИКОВ, а не камер. Размером буфера его заменить нельзя: буфер умеет только
        // расти и переживает сцену с бо́льшим числом светов.
        uint32_t num_lights = 0;
    };

    struct TextureDraw {
        std::vector<UVL_Block> texture_uvl;
        VariantLayout variant_layout;
        // Невладеющий указатель в живой Material: содержимое UI правит на лету, под рендером —
        // это осознанный размен на мгновенный отклик слайдеров.
        const std::vector<uint8_t>* params = nullptr;
        uint32_t indirect_command_index = 0;   // первая команда мультидроу, ЛОКАЛЬНЫЙ индекс прохода
        uint32_t draw_count = 0;
    };

    struct AtlasGroup {
        std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
        std::vector<TextureDraw> draws;
    };

    struct ShaderGroup {
        std::shared_ptr<SDL_GPUGraphicsPipeline> pipeline;
        PushInstructions push_instructions;
        // Пустой список (или nullptr у индексного) = резолв имени сорвался, и весь шейдер-батч
        // пропускается: бинд не того стрима в слот пайплайна — UB, а не деградация картинки.
        std::vector<BufferData*> vertexBuffers;
        BufferData* indexBuffer = nullptr;
        std::vector<BufferData*> vertexStorageBuffers;
        std::vector<BufferData*> fragmentStorageBuffers;
        std::vector<AtlasGroup> atlases;
    };

    struct PassDrawList {
        std::vector<ShaderGroup> shaders;
        // Уже отрезолвленные биндинги: GPU-текстуру атласа могут пересоздать, поэтому копию,
        // снятую на setup, держать нельзя.
        std::vector<SDL_GPUTextureSamplerBinding> global_texture_bindings;
        uint32_t first_instance = 0;   // начало сегмента прохода во ВХОДНОМ PIB
        uint32_t num_instances = 0;
        uint32_t num_commands = 0;
    };

    // Неизменяема после сборки, слоты делят её через shared_ptr.
    struct BatchLayout {
        std::vector<PassDrawList> passes;   // индекс = RenderPassStep::ordinal
        BufferData* indirectBuffer = nullptr;
    };

}
