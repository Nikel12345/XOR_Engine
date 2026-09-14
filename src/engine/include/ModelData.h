#pragma once
#include <vector>
#include <string>
#include <functional>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <SDL3/SDL_stdinc.h>
#include <glm/glm.hpp>
// Диапазон в буферах пула, В ЭЛЕМЕНТАХ. Размечает их аллокатор внутри ModelManager, но живёт
// результат здесь — как UVL текстуры живёт в её хэндле.
struct GeometryRange {
    uint32_t first = 0;
    uint32_t count = 0;
};

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

// Диапазон ЭКРАННЫХ размеров объекта, на котором сабмеш рисуется. Ступени лесенки, а не пиксели:
// 0 = граница не задана, иначе порог = 0.5 * 2^(L-1) px (0.5, 1, 2, … 8192). Грубость намеренная —
// это ручка детализации, точность в ней не значит ничего, зато пара ступеней влезает в свободные
// биты слова EntityToCmd и не заводит второго GPU-буфера.
//
// Нижняя граница убирает мелкую ДЕТАЛЬ: у накладного сабмеша (окна поверх сплошного фасада) за
// порогом каждый квад субпиксельный и вдобавок шейдит стену второй раз, а выбросить его можно без
// дырок — он наложен, а не вырезан. Верхняя нужна LOD-у: уровень модели = ещё один сабмеш с
// соседним диапазоном, и две границы вместе оставляют видимым ровно один из них.
//
// Меряется размер ВСЕГО объекта (BoundSpheres — по строке трансформа, то есть по модели целиком),
// а не самого сабмеша: у накладки сфера почти совпадает с фасадом, и её радиус ничего не сказал бы
// о том, разрешимы ли отдельные окна.
//
// Имя = ключ "screen_size_span" в models.json: значение приходит из манифеста сцены, а не из .bin —
// это настройка, а не свойство геометрии файла.
struct SubMeshSpan {
    uint8_t lod_min = 0;
    uint8_t lod_max = 0;
};

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
    SubMeshSpan screen_size_span;   // см. объявление типа
};

// Слово буфера EntityToCmd; разбирает его culling_pib.comp.hlsl. Незаполненный диапазон даёт
// нулевые ступени, то есть слово из одного индекса команды.
inline constexpr uint32_t kCmdIndexMask = 0x00FFFFFFu;
inline uint32_t MakeEntityToCmdWord(uint32_t cmd_index, SubMeshSpan span) {
    assert(cmd_index <= kCmdIndexMask);
    return (cmd_index & kCmdIndexMask)
         | (static_cast<uint32_t>(span.lod_min & 0xFu) << 24)
         | (static_cast<uint32_t>(span.lod_max & 0xFu) << 28);
}

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

    // Место модели в буферах пула. ДО размещения (placed == false) отсчитывается от начала
    // стейджинга, после — от начала буфера; абсолютным его делает ModelManager::PackModels.
    // Возвращать место аллокатору можно только у размещённой модели.
    GeometryRange vertex_range;
    GeometryRange index_range;
    bool          placed = false;

    // Авторские данные (для редактора/сериализации): ресурс самоописываем. ПУСТЫ у процедурных
    // моделей (сгенерированы кодом) — из файла не пересоздаются, редактором не трогаются.
    std::string model_path;
    std::string index_path;
    // Не писать в models.json при SaveScene (движковые/кодовые дефолты — sphere/quad/cubes).
    // Процедурные и так скипались бы по пустым путям, но флаг — явный маркер намерения (как
    // TextureHandle::dont_save). Пересоздание/UI выставляют false → «тронул = сохраняемый».
    bool dont_save = false;
};