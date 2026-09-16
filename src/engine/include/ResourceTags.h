#pragma once
#include <cstdint>

enum class ResourceTag : uint32_t {
    None      = 0,
    // Владелец — код, а не сцена. Отсюда оба следствия: в манифест не пишется и снос сценовых
    // ресурсов при загрузке его не трогает — восстанавливать его было бы нечем.
    CodeOwned = 1u << 0,
    // Служебный: в списках выбора редактора не показывается (галочка «Show internal»).
    System    = 1u << 1,
    // Движковый дефолт: создаётся кодом на старте.
    Default   = 1u << 2,
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
