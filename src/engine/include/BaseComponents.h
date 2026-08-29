#pragma once
// КОМПОНЕНТЫ движка — только данные. Машинерия хранилища (Archetype, ComponentArray,
// SoAProxyAddable, трейты) вынесена в ComponentStorage.h: правка поля компонента не должна
// пересобирать всё, что работает с ECS вообще, а не с этим компонентом.
//
// Игровые компоненты объявляются в файлах игры и регистрируются там же
// (ComponentSpecRegistry::Register) — движок для этого не правится, см. GameComponents.h в game.
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

// Маркер «скрыть из редактора»: движковое понятие видимости в UI. Энтити с этим
// тегом не выводятся в списке объектов (вспомогательная визуализация — debug-рамки
// коллайдеров и т.п.). Верхние либы (Physics/игра) вешают его сами; движок при этом
// не знает про их типы-теги — он фильтрует по своему.
struct EditorHiddenComponent {};

// Маркер «сущность сгенерирована кодом при настройке сцены, а не авторская». Такие
// НЕ сериализуются (SaveScene их пропускает) — при загрузке их заново создаёт
// зарегистрированный генератор (EngineContext::RegisterGenerator/RunGenerators).
// Пример: debug-рамки коллайдеров, выводимые из ColliderComponent. Единственный смысл —
// «не в файл, пересоздаётся»; в отличие от EditorHiddenComponent (только фильтр UI).
struct GeneratedComponent {};

// Локальная (относительно родителя) матрица 4×4 в column-major раскладке glm
// (m[0..3] = столбец 0 = образ X-оси, m[12..14] = трансляция). Каждый кадр
// TransformDataModule::UpdateLocalTransforms пишет в Positions = матрица_родителя ×
// эта_локальная — полная иерархия (поворот+масштаб+сдвиг), в отличие от LocalOffsets.
// SoA-локальная матрица (по аналогии с Positions): 16 параллельных колонок, column-major
// glm (m0..m3 = столбец 0, m12..m14 = трансляция). Заменила AoS LocalMatrixComponent ради
// единообразия с Positions и cache-friendly доступа в TransformDataModule. В CreateEntity
// передаётся как LocalMatrixProxy16 (как PositionProxy16 для Positions).
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

// Ссылка на ассет — ТОЛЬКО имя (то же правило, что внутри Material: текстуры/sp по имени).
// Резолв в ModelData*/Material* происходит у потребителя, на месте использования, и резолвер
// приходит туда параметром — ECS про ModelManager/MaterialManager по-прежнему не знает.
// Имя ПЕРЕЖИВАЕТ delete+recreate ассета под тем же именем и не требует фиксапа после загрузки
// сцены: сохранение и рантайм держат одно и то же представление, чинить нечего.
struct ModelComponent {
    std::string name;
};

// Роль текстурного слота объявлена НЕПРОЗРАЧНО, а не через ShaderTypes.h: scoped enum и без
// определения — полный тип (подлежащий int), а тянуть сюда ShaderTypes.h нельзя — он приводит
// за собой glm и SDL_gpu, которых у EngineEcs нет в PUBLIC (эту цель линкует и модуль физики).
// ECS хранит номер роли как непрозрачный тег: сравнивать и сохранять его умеет, разыменовывать
// в конкретный слот — дело потребителя (BatchBuilder, инспектор).
enum class TextureSlotRole;

// Ссылка сущности на материал + ЕЁ СОБСТВЕННОЕ состояние вариантов. Имя — как было (name-based
// ссылка, резолв на сборке батча). Ячейка, а не две параллельные коллекции, — тот же приём, что
// SpBinding в MaterialData.h: у списка и данных один владелец, разъехаться им негде.
//
// states РАЗРЕЖЕННЫЕ и ПО РОЛИ, а не по номеру ячейки: номер зависит от набора вариативных ролей
// материала и сдвигается при его правке, роль — нет. Пусто = все слоты показывают дефолт.
// Состояние живёт ЗДЕСЬ, а не в материале: материал у объектов общий, а выбор — per-object
// (два куба с одним материалом показывают разные варианты и остаются в одном инстанс-батче).
// Имя MaterialData занято (MaterialData.h → struct Material), отсюда MaterialRef.
struct MaterialRef {
    std::string                                       name;
    std::vector<std::pair<TextureSlotRole, uint32_t>> states;   // роль -> номер варианта
};

// Порядок расположения материалов должен соответствовать порядку сабмешей в модели, поскольку индекс материала в сабмеше используется для доступа к материалу
// Order of materials must correspond to the order of submeshes in the model, as the material index in the submesh is used to access the material
struct MaterialComponent {
    std::vector<MaterialRef> materials;
};

// Тег «эта сущность переключает варианты текстур». Данных не несёт — они остаются в
// MaterialRef::states; тег нужен ФИЛЬТРОМ.
//
// Зачем. Без него вопрос «переключает ли» — предикат ПО ЗНАЧЕНИЮ: чтобы ответить, надо на
// каждой строке домена шагнуть в MaterialComponent (вектор в куче на строку). Тегом тот же
// вопрос становится фактом об АРХЕТИПЕ, и обход буфера состояний фильтруется на входе.
//
// И, что важнее, он делает префиксный буфер независимым от значений: место сущности в буфере
// состояний определяет наличие тега и число её материалов — обе величины меняются только
// структурно. Поэтому префикс гейтится обычной ревизией батчей, а не отдельным счётчиком
// правок states (см. TextureStateDataModule).
//
// Тег ставится при СОЗДАНИИ сущности и не снимается при возврате на дефолтный вариант: миграции
// архетипов в ECS нет, а переключение варианта не должно быть структурной правкой — в этом вся
// идея фичи. «Тег есть, а все слоты дефолтные» — законное состояние (элемент из нулей).
struct TextureStateComponent {};


enum class LightTypes {   // scoped: SPOT/SPHERE/DIRECT слишком общие для глобала
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

        // Статичные ВЛОЖЕННЫЕ ortho-боксы теней (камера НЕ едет за игроком):
        //  center      — общий центр всех каскадов в мире;
        //  half_extent — половина ширины/высоты каскада 0 (самого резкого, поперёк dir);
        //  half_depth  — половина протяжённости вдоль dir (общая), far = 2*half_depth;
        //  cascade_ratio — во сколько раз каждый следующий каскад шире предыдущего.
        // Экстент каскада c = half_extent * cascade_ratio^c. Каскад 0 — самый мелкий/резкий.
        float center_x = 0, center_y = 0, center_z = 0;
        float half_extent = 20.0f;
        float half_depth = 20.0f;

        // Число каскадов = число ortho-камер у источника. Ограничено MAX_CASCADES,
        // т.к. все каскады всех светов делят 8-слойную теневую карту.
        static constexpr int MAX_CASCADES = 4;
        int   cascade_count = 3;
        float cascade_ratio = 3.0f;

        // Латеральный полупролёт каскада c (вложенные концентрические боксы).
        float CascadeExtent(int c) const {
            float e = half_extent;
            for (int k = 0; k < c; ++k) e *= cascade_ratio;
            return e;
        }

        // Полуглубина каскада c вдоль dir. Масштабируется тем же ratio, что и латераль —
        // иначе пол покрывается только в боковом направлении (рост «только по X»), а по
        // глубине света остаётся размером с каскад 0.
        float CascadeDepth(int c) const {
            float e = half_depth;
            for (int k = 0; k < c; ++k) e *= cascade_ratio;
            return e;
        }

        // far каскада c: им нормируется глубина в его теневой карте (per-cascade).
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

// Маркер «энтити участвует в отрисовке». Сборщик батчей и трансформ-модуль отбирают
// по нему (+Positions), а ModelComponent/MaterialComponent тянут через GetComponent.
// Это развязывает «рисуемость» от жёсткого триплета и позволяет голый шейдер
// (материал без текстур). Добавляется ЯВНО — без неявных приписок в CreateEntity.
//
// visible — источник истины «рисуется ли сейчас». Менять ТОЛЬКО через
// EngineContext::HideEntity: тот пишет флаг И ставит инкрементальную дельту в батч-дерево
// (QueueCreate/QueueDelete). Прямая запись поля батчи не перестроит. Полная пересборка
// (BuildRenderBatches) и инкремент (ApplyIncremental) этот флаг уважают, поэтому он
// переживает реактивацию сцены. Скрытие НЕ трогает ECS — трансформ-строка остаётся,
// индексы соседей не едут (в отличие от DeleteEntity).
struct DrawComponent {
	bool     visible = true;
	float    alpha   = 1.0f;   // per-instance прозрачность (× текстура × материал)
	uint32_t flags   = 0;      // задел под per-instance биты (tint/dissolve/gpu-visible/...)
};

// Маркер «эта энтити — элемент игрового интерфейса». По нему UI-проход и UI_DataModule
// отбирают энтити ОТДЕЛЬНО от мировой геометрии (аналог DrawComponent, но для UI). Пока
// чистый маркер без полей — якоря/слой/интерактивность добавятся позже. Полноценно в
// сериализатор (ComponentSerializer) НЕ регистрируется — это заготовка под UI-подсистему.
struct UIComponent {};

// Текст UI-элемента (концепт). Хранит ПОСЛЕДОВАТЕЛЬНОСТЬ кодов глифов — «текст-буфер» на
// элемент. Отдельного пути «строка = одна готовая текстура» нет: всё идёт через коды,
// которые шейдер разворачивает в UVL глифов. Строка→коды резолвится ВЫШЕ (шрифт+раскладка),
// модуль потребляет уже готовые коды. font — выбор шрифта/атласа (на будущее; в разреженный
// индекс не входит — шрифт обычно определяется батчем).
struct UITextComponent {
	std::vector<uint32_t> glyphs;   // коды глифов (в TextBuffer лягут подряд, count на элемент)
	uint32_t              font = 0;
};

struct TestComponent {};
