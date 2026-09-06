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

    // Колонки архетипа целиком, раз на архетип; цикл по сущностям пишет вызывающий.
    // Форма для ГОРЯЧИХ проходов: только так тело можно сделать векторизуемым (поднять
    // указатели колонок в локальные float* __restrict над циклом, обойтись без ветвлений).
    // Две сигнатуры лямбды:
    //   (ComponentArray<Ts>*...)                                колонки
    //   (ComponentArray<Ts>*..., const std::vector<Entity>&)    колонки + entities архетипа
    // Индекс в entities совпадает с индексом в колонках.
    template<typename ...Ts, typename Fn>
    void ForEachArchetype(SceneData* scene, Fn&& fn);

    // Формы, выбор по сигнатуре лямбды:
    //   (foreach_arg_t<Ts>...)          поэлементно; для SoA приходит SoAElement
    //   (Entity, foreach_arg_t<Ts>...)  то же + id сущности
    //   (Ts&...)                        все компоненты SoA -> колонки целиком, раз на архетип
    // Поэлементные формы НЕ векторизуются: тело получает объект на каждую сущность, и
    // расширять его нечем. Горячим проходам нужен ForEachArchetype - там цикл пишет сам
    // вызывающий (см. комментарий у него).
    template<typename ...Ts, typename Fn>
    void ForEach(SceneData* scene, Fn&& fn);

    template<typename T>
    foreach_arg_t<T> GetComponent(SceneData* scene, Entity e);

    template<typename Component>
    bool Has(SceneData* scene, Entity e) const;

    SceneData* CreateScene(const SceneName& name);

    // Сериализация сцены (формат и реестр компонентов — ComponentSerializer).
    // SaveScene: обходит сущности активного набора, на каждый компонент с
    // зарегистрированным сериалайзером пишет строку. LoadScene: парсит текст и строит
    // сущности тем же путём, что CreateEntity (собрать сигнатуру → архетип → дописать
    // в массивы), но набор берётся из файла, а не из шаблона. Указатели на ассеты
    // (Model/Material) НЕ восстанавливаются здесь — только имена; их чинит верхний слой
    // (EngineContext::LoadScene), т.к. ECS-ядро не знает про менеджеры ресурсов.
    std::string SaveScene(SceneData* scene);
    // Возвращает сущности, созданные ЭТОЙ загрузкой (в порядке появления в файле).
    // Верхний слой чинит указатели на ассеты только по ним, не трогая то, что уже
    // было в сцене (напр. сгенерированные сущности с живыми указателями).
    std::vector<Entity> LoadScene(const SceneName& scene_name, const std::string& text);

    // Рантайм-создание сущности по НАБОРУ спецификаций (форма создания в UI): тот же путь,
    // что проход 1 LoadScene — сигнатура из sig_type → архетип → Load(nullptr, 1) = дефолтный
    // ряд каждого компонента. В отличие от шаблонного CreateEntity набор задаётся в рантайме.
    // dirty_entity НЕ взводит — решает вызывающий (staging-сцене формы это не нужно).
    Entity CreateEntityFromSpecs(SceneData* scene, const std::vector<const ComponentSpec*>& specs);

	void SetSceneState(const SceneName& scene_name, bool is_active);
    // ИСКЛЮЧИТЕЛЬНАЯ активация: названная сцена становится активной, все прочие гасятся.
    // Активная ровно одна по построению — на неё смотрят GetActiveScene, дата-модули и сборка
    // батчей. SetSceneState(name, true) этого НЕ гарантирует (у SceneData is_active=true по
    // умолчанию, т.е. новая сцена рождается активной): две активные — и GetActiveScene отдаёт
    // произвольную из них. Переключение сцен ходит сюда.
    void SetActiveScene(const SceneName& scene_name);
    SceneData* GetActiveScene();
    SceneName GetActiveSceneName();
    SceneData* GetScene(const SceneName& name);

    void DeleteEntity(const SceneName& name, Entity e);
    void DeleteEntity(SceneData* scene, Entity e);


    bool CheckNewObjects() { return dirty_entity; };
    void NewObjectsCommit() { dirty_entity = false; };

    // Монотонная ревизия СОСТАВА сущностей и их геометрических ссылок: ++ на создание/удаление
    // сущности, загрузку и смену активной сцены, смену модели у сущности (BumpEntityRevision).
    //
    // Зачем отдельно от dirty_entity: тот флаг ПОТРЕБЛЯЕТСЯ (NewObjectsCommit в
    // TransformDataModule), то есть обслуживает ровно одного читателя. Ревизию же сравнивают со
    // своей запомненной копией сколько угодно потребителей и послотно — та же дисциплина, что у
    // BatchBuilder::BatchesRevision.
    uint64_t EntityRevision() const { return entity_revision; }
    // Для правки СОДЕРЖИМОГО, которая меняет геометрию сущности без структурного изменения —
    // сейчас это смена модели (EngineContext::ChangeModel).
    void BumpEntityRevision() { ++entity_revision; }

    SceneData* operator[](const std::string& name);

private:
    template<typename... Components>
    void add_components(Archetype& arch, Components&&... comps);

    std::unordered_map<SceneName, std::unique_ptr<SceneData>> scenes_data;
	bool dirty_entity = false;
	uint64_t entity_revision = 0;   // см. EntityRevision()
    // «Про отсутствие активной сцены уже сообщено». Оба геттера зовутся ДЕСЯТКИ раз за кадр
    // (один DefaultUpdateSet — около десяти), поэтому без этого состояние «сцены нет» топит себя
    // в собственном логе: сотни строк в секунду через SDL_Log на консоль ещё и роняют fps, и
    // ровно тот пустой кадр, который хотели посмотреть, разглядеть уже нельзя. Флаг снимается
    // при появлении активной сцены — каждый ВХОД в состояние логируется ровно раз.
    // Гонка между потоками безобидна: худшее — лишняя или пропущенная строка лога.
    bool no_active_scene_reported = false;
};

#include "ObjectManager.inl"