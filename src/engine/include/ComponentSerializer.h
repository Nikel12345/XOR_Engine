#pragma once
// Реестр сериалайзеров компонентов. Мост «строка из файла ↔ конкретный C++ тип»:
// у архетипа компонент стёрт до IComponentArray (виден только type_index), а в файле
// он — строка-имя. Поэтому на каждый сериализуемый компонент один раз регистрируется
// пара функций {save, load}, замкнутых на конкретный T. Открытая регистрация: движок
// регистрирует свои компоненты здесь, верхние слои (физика/игра) могут добавить свои.
#include <string>
#include <vector>
#include <typeindex>
#include <unordered_map>
#include "BaseComponents.h"   // Archetype, ComponentArray, компоненты
#include "yyjson.h"           // scene.json колоночный: save/load пишут/читают колонки полей

struct ComponentSerializer {
    std::string     name;       // имя в файле = ключ объекта компонента, напр. "Model"
    std::type_index sig_type;   // type_index, идущий в сигнатуру архетипа (для SoA — тип хранилища)

    // Память → json: записать ВЕСЬ компонент архетипа (count строк) КОЛОНКАМИ по полям в comp
    // (объект компонента). Для SoA это прямой сброс массива поля; для AoS — колонка на поле.
    void (*save)(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp);
    // json → память: ensure_component<T> и добавить РОВНО count строк, читая поля-колонки из comp
    // (nullptr, если компонент без данных-тег). Недостающие/короткие колонки → дефолт (count-гвард).
    void (*load)(Archetype& arch, yyjson_val* comp, size_t count);
};

class ComponentSerializerRegistry {
public:
    static ComponentSerializerRegistry& Get();

    void Register(ComponentSerializer s);
    const ComponentSerializer* ByName(const std::string& name) const;   // для загрузки
    const ComponentSerializer* ByType(std::type_index t) const;         // для сохранения

private:
    std::vector<ComponentSerializer>              serializers_;
    std::unordered_map<std::string, size_t>       by_name_;
    std::unordered_map<std::type_index, size_t>   by_type_;
};

// Регистрирует сериалайзеры встроенных (движковых) компонентов. Идемпотентна —
// повторный вызов ничего не дублирует. Звать один раз на старте движка.
void RegisterBuiltinComponentSerializers();
