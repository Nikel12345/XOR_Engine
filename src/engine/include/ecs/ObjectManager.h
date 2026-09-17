#pragma once
#include <map>
#include <set>
#include <string>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include "ComponentStorage.h"
#include "SceneData.h"
#include "Aliases.h"

// Только по имени: CreateEntity различает его через is_same_v, а поле parent читает
// уже в контексте вызывающего — тот полный тип, конечно, включает.
struct ParentComponent;

class PassManager;
class BufferManager;
class PipeManager;
struct RenderBatchData;
struct ComponentSpec;   // реестр спецификаций компонентов (ComponentSerializer.h)

template<typename T, typename... Ts>
constexpr bool contains_type_v = (std::is_same_v<T, std::decay_t<Ts>> || ...);

// Один ряд SoA-хранилища. Ссылки на элемент не существует (элемента в памяти нет), поэтому
// ForEach и GetComponent отдают пару «хранилище + индекс». Приведение к SoA& неявное, так что
// лямбда, объявившая параметр как Positions&, тоже соберётся — но получит ВСЁ хранилище и
// потеряет индекс, молча. В поэлементном обходе параметр пишут SoAElement.
template<typename SoA>
struct SoAElement {
    SoA* soa = nullptr;
    size_t index = 0;

    SoA& container() { return *soa; }
    const SoA& container() const { return *soa; }
    size_t i() const { return index; }

    operator SoA& () { return *soa; }
    operator const SoA& () const { return *soa; }
};

template<typename T>
using foreach_arg_t = std::conditional_t<is_soa<T>::value, SoAElement<T>, T&>;

class ObjectManager {
public:
    template<typename... Components>
    Entity CreateEntity(const std::string& scene_name, Components&&... comps);

    // Раз на архетип, колонками целиком; цикл по сущностям пишет вызывающий. Форма для ГОРЯЧИХ
    // проходов: только так тело выходит векторизуемым — указатели колонок поднимаются в локальные
    // float* __restrict над циклом (замеры: sandbox/GravityVecProbe.cpp). Сигнатуры лямбды:
    //   (ComponentArray<Ts>*...)                                колонки
    //   (ComponentArray<Ts>*..., const std::vector<Entity>&)    колонки + entities архетипа
    // Индекс в entities — тот же, что в колонках: CreateEntity кладёт сущность и её поля одним
    // шагом, swap_remove снимает их вместе.
    template<typename ...Ts, typename Fn>
    void ForEachArchetype(SceneData* scene, Fn&& fn);

    // Три формы, и первую выбирает НЕ сигнатура лямбды, а сам набор Ts:
    //   (Ts&...)                        все Ts — SoA: хранилища целиком, раз на архетип;
    //   (foreach_arg_t<Ts>...)          иначе поэлементно, SoA приходит как SoAElement;
    //   (Entity, foreach_arg_t<Ts>...)  то же поэлементно, плюс id сущности.
    // То есть под набор из одних SoA поэлементную лямбду не написать — она не соберётся;
    // поэлементный обход по такому набору возвращают, добавив Entity первым параметром.
    // Векторизуется только форма с хранилищами: поэлементной расширяться нечем, тело получает
    // объект на каждую сущность. Горячему проходу нужен ForEachArchetype.
    template<typename ...Ts, typename Fn>
    void ForEach(SceneData* scene, Fn&& fn);

    template<typename T>
    foreach_arg_t<T> GetComponent(SceneData* scene, Entity e);

    template<typename Component>
    bool Has(SceneData* scene, Entity e) const;

    SceneData* CreateScene(const SceneName& name);

    // Сериализация сцены; формат и реестр компонентов — ComponentSerializer. Прохода-фиксапа
    // после загрузки нет: ассеты и в файле, и в рантайме — имена, резолв живёт в BatchBuilder.
    std::string SaveScene(SceneData* scene);
    // Возвращает сущности, созданные ЭТОЙ загрузкой (в порядке появления в файле) — по ним
    // вызывающий и работает, не задевая то, что уже было в сцене: LoadScene ДОБАВЛЯЕТ к сцене,
    // а не заменяет её (сносит содержимое, если нужно, слой выше).
    std::vector<Entity> LoadScene(const SceneName& scene_name, const std::string& text);

    // Сущность по набору спецификаций, известному только в рантайме (форма создания в UI): тот же
    // путь, что проход 1 LoadScene, с дефолтным рядом каждого компонента. EntityRevision НЕ
    // двигает — это решает вызывающий: черновику формы, живущему в своей сцене, ревизия не нужна.
    Entity CreateEntityFromSpecs(SceneData* scene, const std::vector<const ComponentSpec*>& specs);

	void SetSceneState(const SceneName& scene_name, bool is_active);
    // ИСКЛЮЧИТЕЛЬНАЯ активация: названная сцена зажигается, все прочие гасятся. Переключение сцен
    // ходит сюда, а не в SetSceneState(name, true): у SceneData is_active=true по умолчанию, то
    // есть новая сцена рождается активной, и две активные превращают GetActiveScene в лотерею.
    void SetActiveScene(const SceneName& scene_name);
    SceneData* GetActiveScene();
    SceneName GetActiveSceneName();
    SceneData* GetScene(const SceneName& name);

    void DeleteEntity(const SceneName& name, Entity e);
    void DeleteEntity(SceneData* scene, Entity e);


    // Монотонная ревизия СОСТАВА сущностей и их геометрических ссылок: ++ на создание/удаление,
    // загрузку, смену активной сцены и смену модели. Не флаг: её сравнивают со своей копией
    // сколько угодно потребителей и послотно, никто её не «потребляет».
    uint64_t EntityRevision() const { return entity_revision; }
    // Для правки СОДЕРЖИМОГО, меняющей геометрию сущности без структурного изменения — сейчас
    // это только смена модели (EngineContext::ChangeModel).
    void BumpEntityRevision() { ++entity_revision; }

    SceneData* operator[](const std::string& name);

private:
    template<typename... Components>
    void add_components(Archetype& arch, Components&&... comps);

    std::unordered_map<SceneName, std::unique_ptr<SceneData>> scenes_data;
	uint64_t entity_revision = 0;
    // «Про отсутствие активной сцены уже сообщено»: геттеры сцены зовут десятки раз за кадр, и
    // без флага состояние топит себя в логе — сотни строк в секунду роняют ещё и fps, то есть
    // ровно тот пустой кадр, который смотрят. Логируется каждый ВХОД в состояние, ровно раз.
    bool no_active_scene_reported = false;
};

#include "ObjectManager.inl"