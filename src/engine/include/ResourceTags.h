#pragma once
#include <cstdint>

enum class ResourceTag : uint32_t {
    None     = 0,
    // В манифест сцены не пишется. Ресурс, который сцена не возит и потому не может потерять.
    DontSave = 1u << 0,
    // Служебный: в списках выбора редактора не показывается (галочка «Show internal»). Раньше это
    // выражалось именем на «_» и проверялось ui::IsInternalName — то есть логика висела на имени.
    System   = 1u << 1,
    // Движковый дефолт: создаётся кодом на старте и обязан пережить загрузку сцены. По нему
    // отличают «наше, восстановится само» от «пришло из сцены» при сносе сценовых ресурсов.
    Default  = 1u << 2,
};

constexpr ResourceTag operator|(ResourceTag a, ResourceTag b)
{
    return static_cast<ResourceTag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr ResourceTag& operator|=(ResourceTag& a, ResourceTag b) { a = a | b; return a; }

constexpr bool HasTag(ResourceTag tags, ResourceTag t)
{
    return (static_cast<uint32_t>(tags) & static_cast<uint32_t>(t)) != 0;
}
