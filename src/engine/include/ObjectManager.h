#pragma once
#include <map>
#include <set>
#include <string>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include "BaseComponents.h"
#include "SceneData.h"
#include "Aliases.h"

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

    template<typename ...Ts, typename Fn>
    void ForEachArchetype(SceneData* scene, Fn&& fn);

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
    SceneData* GetActiveScene();
    SceneName GetActiveSceneName();
    SceneData* GetScene(const SceneName& name);

    void DeleteEntity(const SceneName& name, Entity e);
    void DeleteEntity(SceneData* scene, Entity e);


    bool CheckNewObjects() { return dirty_entity; };
    void NewObjectsCommit() { dirty_entity = false; };

    SceneData* operator[](const std::string& name);

private:
    template<typename... Components>
    void add_components(Archetype& arch, Components&&... comps);

    std::unordered_map<SceneName, std::unique_ptr<SceneData>> scenes_data;
	bool dirty_entity = false;
};

#include "ObjectManager.inl"