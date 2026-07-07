#pragma once
// Реестр сериалайзеров компонентов. Мост «строка из файла ↔ конкретный C++ тип»:
// у архетипа компонент стёрт до IComponentArray (виден только type_index), а в файле
// он — строка-имя. Поэтому на каждый сериализуемый компонент один раз регистрируется
// пара функций {save, load}, замкнутых на конкретный T. Открытая регистрация: движок
// регистрирует свои компоненты здесь, верхние слои (физика/игра) могут добавить свои.
#include <string>
#include <string_view>
#include <vector>
#include <typeindex>
#include <unordered_map>
#include "BaseComponents.h"   // Archetype, ComponentArray, компоненты

struct ComponentSerializer {
    std::string     name;       // имя в файле, напр. "Model"
    std::type_index sig_type;   // type_index, идущий в сигнатуру архетипа (для SoA — тип хранилища)

    // Память → файл: прочитать строку i архетипа, дописать токены полезной нагрузки в out.
    void (*save)(Archetype& arch, size_t i, std::string& out);
    // Файл → память: распарсить токены, ensure_component<T> на архетипе и дописать значение.
    // Токены — string_view В ИСХОДНЫЙ буфер сцены (жив на всё время загрузки): без копий строк.
    void (*load)(Archetype& arch, const std::vector<std::string_view>& tokens);
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
