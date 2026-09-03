#pragma once
#include <vector>
#include <string>
#include <functional>
#include <cstddef>
#include <cstring>
#include <SDL3/SDL_stdinc.h>
#include <glm/glm.hpp>
#include "RangeAllocator.h"

// Генератор геометрии процедурной модели: заполняет вершины БАЙТАМИ в раскладке своего пула и
// индексы. Тип вершины движку не нужен нигде — заливка копирует count × VertexSize() пула, поэтому
// раскладку задаёт ПУЛ, а не сигнатура; иначе процедурная модель была бы навсегда привязана к одной
// структуре. Типизированную структуру генератор объявляет у себя и укладывает хелпером ниже.
// Живёт здесь (а не в ModelManager.h), чтобы сигнатуры фасадов (EngineContext::CreateModel)
// не тянули весь ModelManager.h ради одного алиаса.
using ModelGeneratorFn = std::function<void(std::vector<std::byte>&, std::vector<Uint32>&)>;

// Укладка типизированных вершин в байтовый выход. Деталь ТИПИЗИРОВАННОЙ обёртки
// (EngineContext::CreateModel<V>), а не то, что зовёт сам генератор: тот просто заполняет свой
// вектор, как раньше, а стиранием типа занимается обёртка — ровно как ShaderManager::CreatePushInstruction<T>.
template<class V>
inline void WriteVertices(std::vector<std::byte>& out, const std::vector<V>& src)
{
    const size_t base = out.size();
    out.resize(base + src.size() * sizeof(V));
    if (!src.empty()) std::memcpy(out.data() + base, src.data(), src.size() * sizeof(V));
}

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

    // Пул геометрии (раскладка вершин), в котором живёт геометрия модели. Имя, а не указатель:
    // ссылка уезжает в models.json, резолв — в ModelManager. Пусто = дефолтный пул.
    std::string pool_name;

    // Занятые моделью диапазоны в буферах её пула, В ЭЛЕМЕНТАХ (вершинный — общий на все стримы
    // пула, они растут в ногу; индексный свой). count==0 — геометрия ещё в стейджинге, места на
    // GPU не занимает. Проставляет финализатор заливки; нужны, чтобы вернуть место аллокатору при
    // перезагрузке модели — иначе каждая загрузка сцены дописывала бы копию в конец буфера.
    RangeAllocator::Range vertex_range;
    RangeAllocator::Range index_range;

    // Авторские данные (для редактора/сериализации): ресурс самоописываем. ПУСТЫ у процедурных
    // моделей (сгенерированы кодом) — из файла не пересоздаются, редактором не трогаются.
    std::string model_path;
    std::string index_path;
    // Не писать в models.json при SaveScene (движковые/кодовые дефолты — sphere/quad/cubes).
    // Процедурные и так скипались бы по пустым путям, но флаг — явный маркер намерения (как
    // TextureHandle::dont_save). Пересоздание/UI выставляют false → «тронул = сохраняемый».
    bool dont_save = false;
};