#pragma once
// Компоненты движка — только данные; машинерия хранилища в ComponentStorage.h (там же описан
// путь SoA-прокси, по которому собраны Positions/Velocities/...). Игровые компоненты объявляются
// в файлах игры и регистрируются там же (ComponentSpecRegistry::Register) — движок не правится.
#include "ComponentStorage.h"
#include <cmath>
#include <SDL3/SDL.h>

struct Accelerations : SoAProxyAddable<Accelerations> {
    using soa_tag = void;
    std::vector<float> x, y, z;
    size_t size() const { return x.size(); }
    auto columns() { return std::tie(x, y, z); }
};

struct AccelerationProxy {
    float x = 0, y = 0, z = 0;
    using related_soa = Accelerations;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.x.push_back(x);  soa.y.push_back(y);  soa.z.push_back(z);
    }
};

struct Velocities3 {
    float x = 0, y = 0, z = 0;
};
struct Velocities : SoAProxyAddable<Velocities> {
    using soa_tag = void;

    std::vector<float> x, y, z;
    size_t size() const { return x.size(); }
    auto columns() { return std::tie(x, y, z); }

    void MoveByAccelerations(const std::vector<float>& ax, const std::vector<float>& ay, const std::vector<float>& az);

};
struct VelocityProxy {
    float x = 0, y = 0, z = 0;
    using related_soa = Velocities;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.x.push_back(x);  soa.y.push_back(y);  soa.z.push_back(z);
    }
};

// Мировая матрица 4x4, разложенная на 16 колонок. Буквы идут ПО СТРОКАМ (x,y,z,w — первая
// строка матрицы), а GPU читает column-major, поэтому трансляция лежит в w,d,h — не в i,j,k,
// а x,y,z — это m00,m01,m02, а не позиция. Перекладку колонок в матрицу делает
// TransformDataModule::LoadPositionMatrix, она же — определение этого соответствия.
struct Positions : SoAProxyAddable<Positions> {
    using soa_tag = void;

    std::vector<float> x, y, z, w, a, b, c, d, e, f, g, h, i, j, k, l;
    size_t size() const { return x.size(); }
    auto columns() { return std::tie(x, y, z, w, a, b, c, d, e, f, g, h, i, j, k, l); }

    void MoveByVelocities(const std::vector<float>& vx, const std::vector<float>& vy, const std::vector<float>& vz);

};
struct PositionProxy16 {
    float x = 1, y = 0, z = 0, w = 0,
        a = 0, b = 1, c = 0, d = 0,
        e = 0, f = 0, g = 1, h = 0,
        i = 0, j = 0, k = 0, l = 1;
    using related_soa = Positions;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.x.push_back(x);  soa.y.push_back(y);  soa.z.push_back(z);  soa.w.push_back(w);
        soa.a.push_back(a);  soa.b.push_back(b);  soa.c.push_back(c);  soa.d.push_back(d);
        soa.e.push_back(e);  soa.f.push_back(f);  soa.g.push_back(g);  soa.h.push_back(h);
        soa.i.push_back(i);  soa.j.push_back(j);  soa.k.push_back(k);  soa.l.push_back(l);
    }

};

struct Positions16 {
    float x = 1, y = 0, z = 0, w = 0,
        a = 0, b = 1, c = 0, d = 0,
        e = 0, f = 0, g = 1, h = 0,
        i = 0, j = 0, k = 0, l = 1;
};

struct Parents : SoAProxyAddable<Parents> {
    using soa_tag = void;
    std::vector<Entity> parent;
    size_t size() const { return parent.size(); }
    auto columns() { return std::tie(parent); }
};

struct ParentProxy {
    Entity parent;
    using related_soa = Parents;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.parent.push_back(parent);
    }
};

struct ParentComponent {
    Entity parent;
};

// Не показывать в списке объектов редактора. Вешают и верхние либы (Physics/игра) — движок
// фильтрует по своему тегу, про их типы не зная.
struct EditorHiddenComponent {};

// Сущность выведена кодом из авторских данных, поэтому SaveScene её ПРОПУСКАЕТ: на загрузке
// её заново делает генератор (EngineContext::RegisterGenerator/RunGenerators). С фильтром UI
// (EditorHiddenComponent) не связан — смысл ровно один, «не в файл, пересоздаётся».
struct GeneratedComponent {};

// Матрица относительно родителя, 16 колонок — но, в отличие от Positions, разложенных
// column-major как есть (m0..m3 = столбец 0, m12..m14 = трансляция). Каждый кадр
// TransformDataModule::UpdateLocalTransforms пишет Positions = матрица_родителя x эта:
// полная иерархия с поворотом и масштабом, в отличие от LocalOffsets.
struct LocalMatrices : SoAProxyAddable<LocalMatrices> {
    using soa_tag = void;
    std::vector<float> m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15;
    size_t size() const { return m0.size(); }
    auto columns() { return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15); }
};
struct LocalMatrixProxy16 {
    float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    using related_soa = LocalMatrices;
    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.m0.push_back(m[0]);   soa.m1.push_back(m[1]);   soa.m2.push_back(m[2]);   soa.m3.push_back(m[3]);
        soa.m4.push_back(m[4]);   soa.m5.push_back(m[5]);   soa.m6.push_back(m[6]);   soa.m7.push_back(m[7]);
        soa.m8.push_back(m[8]);   soa.m9.push_back(m[9]);   soa.m10.push_back(m[10]); soa.m11.push_back(m[11]);
        soa.m12.push_back(m[12]); soa.m13.push_back(m[13]); soa.m14.push_back(m[14]); soa.m15.push_back(m[15]);
    }
};

struct LocalOffsets : SoAProxyAddable<LocalOffsets> {
    using soa_tag = void;
    std::vector<float> ox, oy, oz;
    size_t size() const { return ox.size(); }
    auto columns() { return std::tie(ox, oy, oz); }
};

struct LocalOffsetProxy {
    float ox, oy, oz;
    using related_soa = LocalOffsets;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.ox.push_back(ox);
        soa.oy.push_back(oy);
        soa.oz.push_back(oz);
    }
};

// Ссылка на ассет — только имя: резолв в ModelData* делает потребитель, получив ModelManager
// параметром. ECS про менеджеры ресурсов не знает, и фиксапа после загрузки сцены нет — в файле
// и в рантайме лежит одно и то же.
struct ModelComponent {
    std::string name;
};

// Тот же enum, что в ShaderTypes.h, но объявленный без определения намеренно: scoped enum и так
// полный тип (подлежащий int), а ShaderTypes.h привёл бы за собой glm и SDL_gpu, которых у
// EngineEcs нет в PUBLIC (эту цель линкует физика). ECS хранит номер роли непрозрачно —
// сравнивает и сохраняет, а разворачивает его в слот потребитель.
enum class TextureSlotRole;

// Ссылка сущности на материал + ЕЁ СОБСТВЕННОЕ состояние вариантов: материал у объектов общий,
// а выбор варианта per-object (два куба с одним материалом показывают разное и остаются в одном
// инстанс-батче). states РАЗРЕЖЕННЫЕ и по РОЛИ, а не по номеру слота: номер зависит от набора
// вариативных ролей материала и едет при его правке, роль — нет. Пусто = всюду дефолт.
struct MaterialRef {
    std::string                                       name;
    std::vector<std::pair<TextureSlotRole, uint32_t>> states;   // роль -> номер варианта
};

// Порядок расположения материалов должен соответствовать порядку сабмешей в модели, поскольку индекс материала в сабмеше используется для доступа к материалу
// Order of materials must correspond to the order of submeshes in the model, as the material index in the submesh is used to access the material
struct MaterialComponent {
    std::vector<MaterialRef> materials;
};

// «Эта сущность переключает варианты текстур» — фильтр, данных нет (они в MaterialRef::states).
// Тегом вопрос становится фактом об АРХЕТИПЕ, а место сущности в префиксном буфере состояний
// зависит только от наличия тега и числа её материалов — обе величины структурные. Поэтому
// буфер гейтится обычной ревизией батчей, а не счётчиком правок states (TextureStateDataModule).
// Ставится при СОЗДАНИИ и не снимается на возврате к дефолту: миграции архетипов в ECS нет, а
// переключение варианта не должно быть структурной правкой. «Тег есть, всё дефолтное» — законно.
struct TextureStateComponent {};


enum class LightTypes {
    SPOT,
    SPHERE,
    DIRECT
};

struct SpotLightComponent {
    struct SpotLightData {
        float source_radius = 0;
        float dir_x = 0, dir_y = 0, dir_z = 1;
        float source_angle = 0.3f;
        float r = 1, g = 1, b = 1;
        float power = 1;
        float attenuation = 1.0f;

        SpotLightData(
            float source_radius = 0,
            float dir_x = 0, float dir_y = 0, float dir_z = 0,
            float source_angle = 0.3f,
            float r = 1, float g = 1, float b = 1,
            float power = 1,
            float attenuation = 1.0f
        )
            : source_radius(source_radius),
            dir_x(dir_x), dir_y(dir_y), dir_z(dir_z),
            source_angle(source_angle),
            r(r), g(g), b(b),
            power(power),
            attenuation(attenuation) {
        }

        void ResolveDistance() {
            if (cached_attenuation != attenuation
                || cached_power != power
                || cached_source_angle != source_angle) {
                max_distance = std::sqrt(power * attenuation) / std::tan(source_angle);
                cached_attenuation = attenuation;
                cached_power = power;
                cached_source_angle = source_angle;
            }
        }

        float GetMaxDistance() const { return max_distance; }

    private:
        float max_distance = 0.0f;
        float cached_attenuation = -1.0f;
        float cached_power = -1.0f;
        float cached_source_angle = -1.0f;
    } light_data;
    bool needsUpdate = true;
};

struct SphereLightComponent {
    struct SphereLightData {
        float source_radius = 0;
        float r = 1, g = 1, b = 1;
        float power = 1;
        float attenuation = 1.0f;

        SphereLightData(
            float source_radius = 0,
            float r = 1, float g = 1, float b = 1,
            float power = 1,
            float attenuation = 1.0f
        )
            : source_radius(source_radius),
            r(r), g(g), b(b),
            power(power),
            attenuation(attenuation) {
        }

        void ResolveDistance() {
            if (cached_attenuation != attenuation || cached_power != power) {
                max_distance = std::sqrt(power * attenuation);
                cached_attenuation = attenuation;
                cached_power = power;
            }
        }

        float GetMaxDistance() const { return max_distance; }

    private:
        float max_distance = 0.0f;
        float cached_attenuation = -1.0f;
        float cached_power = -1.0f;
    } light_data;
    bool needsUpdate = true;
};

struct DirectLightComponent {
    struct DirectLightData {
        // Направление лучей (нормализуется при заливке). Позиции у directional нет.
        float dir_x = 0, dir_y = -1, dir_z = 0;
        float r = 1, g = 1, b = 1;
        float power = 1;

        // Статичные ВЛОЖЕННЫЕ ortho-боксы каскадов — камера теней НЕ едет за игроком: center —
        // общий центр всех каскадов, half_extent — поперёк dir у каскада 0 (самого мелкого и
        // резкого), half_depth — вдоль dir, каждый следующий каскад в cascade_ratio раз больше.
        float center_x = 0, center_y = 0, center_z = 0;
        float half_extent = 20.0f;
        float half_depth = 20.0f;

        // Каскад = отдельная ortho-камера. Потолок есть потому, что все каскады всех светов
        // делят одну 8-слойную теневую карту.
        static constexpr int MAX_CASCADES = 4;
        int   cascade_count = 3;
        float cascade_ratio = 3.0f;

        float CascadeExtent(int c) const {
            float e = half_extent;
            for (int k = 0; k < c; ++k) e *= cascade_ratio;
            return e;
        }

        // Глубина растёт тем же ratio, что и латераль: иначе дальний каскад шире, но по глубине
        // остаётся размером с нулевой, и пол уходит из-под теней.
        float CascadeDepth(int c) const {
            float e = half_depth;
            for (int k = 0; k < c; ++k) e *= cascade_ratio;
            return e;
        }

        // far каскада: им нормируется глубина в ЕГО слое теневой карты, у каждого свой.
        float CascadeFar(int c) const { return 2.0f * CascadeDepth(c); }

        DirectLightData(
            float dir_x = 0, float dir_y = -1, float dir_z = 0,
            float r = 1, float g = 1, float b = 1,
            float power = 1,
            float center_x = 0, float center_y = 0, float center_z = 0,
            float half_extent = 20.0f,
            float half_depth = 20.0f,
            int cascade_count = 3,
            float cascade_ratio = 3.0f)
            : dir_x(dir_x), dir_y(dir_y), dir_z(dir_z),
            r(r), g(g), b(b),
            power(power),
            center_x(center_x), center_y(center_y), center_z(center_z),
            half_extent(half_extent), half_depth(half_depth),
            cascade_count(cascade_count), cascade_ratio(cascade_ratio) {
        }
    } light_data;
    bool needsUpdate = true;
};
struct ShadowCasterComponent{};

struct ShadowComponent {};

// «Сущность участвует в отрисовке»: пара Draw+Positions — то, по чему её отбирают сборщик
// батчей и дата-модули (модель и материалы тянутся уже через GetComponent).
//
// visible менять ТОЛЬКО через EngineContext::HideEntity — тот пишет флаг И ставит дельту в
// батч-дерево; прямая запись поля батчи не перестроит. Скрытие не трогает ECS, поэтому
// трансформ-строка остаётся на месте и индексы соседей не едут (в отличие от DeleteEntity).
struct DrawComponent {
	bool     visible = true;
	float    alpha   = 1.0f;   // per-instance прозрачность (× текстура × материал)
	uint32_t flags   = 0;      // задел под per-instance биты (tint/dissolve/gpu-visible/...)
};

// «Элемент игрового интерфейса»: по нему UI-проход и UI_DataModule отбирают энтити ОТДЕЛЬНО от
// мировой геометрии. В ComponentSerializer не регистрируется — заготовка, из файла не приходит.
struct UIComponent {};

// Текст UI-элемента — ПОСЛЕДОВАТЕЛЬНОСТЬ кодов глифов. Пути «строка = готовая текстура» нет
// вообще: коды разворачивает в UVL глифов шейдер, а строку в коды переводит слой выше
// (шрифт+раскладка). font пока не участвует в отборе — шрифт определяется батчем.
struct UITextComponent {
	std::vector<uint32_t> glyphs;   // коды глифов (в TextBuffer лягут подряд, count на элемент)
	uint32_t              font = 0;
};

struct TestComponent {};
