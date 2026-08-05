#pragma once
#include <vector>
#include <string>
#include <functional>
#include <SDL3/SDL_stdinc.h>
#include <glm/glm.hpp>

struct PosUVNormal;   // полное определение — PositionStructure.h (алиасу ниже хватает fwd)

// Генератор геометрии процедурной модели: заполняет переданные массивы вершин/индексов
// как угодно — от руками записанного квада до математической поверхности. MM это безразлично.
// Живёт здесь (а не в ModelManager.h), чтобы сигнатуры фасадов (EngineContext::CreateModel)
// не тянули весь ModelManager.h ради одного алиаса.
using ModelGeneratorFn = std::function<void(std::vector<PosUVNormal>&, std::vector<Uint32>&)>;

struct SubMeshData {
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
    Uint32 vertexCount = 0;
    Uint32 indexCount = 0;
	// Index of the material in the model's materials array, which is used for rendering this submesh. (See documentation for MaterialComponent for understanding how it works)
	uint32_t material_index = 0;
    glm::vec4 sphere;
    glm::vec3 aabb_center = glm::vec3(0.0f);  // центр локального AABB сабмеша (по min/max вершин)
    glm::vec3 aabb_half   = glm::vec3(0.0f);  // полу-размеры локального AABB
};

// Точка отсчёта (пивот) модели. Запекается в вершины один раз при CreateModel:
// геометрия сдвигается так, чтобы выбранный угол/центр локального AABB попал в origin.
// Keep — не сдвигать (поведение по умолчанию, обратная совместимость).
// L/R = X min/max, B/T = Y min/max (Bottom/Top), B/F = Z min/max (Back/Front).
enum class AnchorShift { Keep, Center, LBB, RBB, LTB, RTB, LBF, RBF, LTF, RTF };

struct ModelData {
    std::vector<SubMeshData> submeshes;
    // Как сдвинут пивот. Читается только в момент запекания (CreateModel);
    // дальше — информативная метка, рендер её не использует.
    AnchorShift anchor = AnchorShift::Keep;

    // Авторские данные (для редактора/сериализации): ресурс самоописываем. ПУСТЫ у процедурных
    // моделей (сгенерированы кодом) — из файла не пересоздаются, редактором не трогаются.
    std::string model_path;
    std::string index_path;
    // Не писать в models.json при SaveScene (движковые/кодовые дефолты — sphere/quad/cubes).
    // Процедурные и так скипались бы по пустым путям, но флаг — явный маркер намерения (как
    // TextureHandle::dont_save). Пересоздание/UI выставляют false → «тронул = сохраняемый».
    bool dont_save = false;
};