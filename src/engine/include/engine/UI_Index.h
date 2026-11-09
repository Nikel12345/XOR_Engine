#pragma once
#include <cstdint>

template <class Tag>
struct UIIndex {
    uint32_t value = 0xFFFFFFFFu;
    explicit operator bool() const { return value != 0xFFFFFFFFu; }
    bool operator==(const UIIndex&) const = default;
};

struct UINodeIndexTag;
struct UIElementIndexTag;
struct UILoopIndexTag;
struct UIBindingIndexTag;

using UINodeIndex    = UIIndex<UINodeIndexTag>;
using UIElementIndex = UIIndex<UIElementIndexTag>;
using UILoopIndex    = UIIndex<UILoopIndexTag>;
using UIBindingIndex = UIIndex<UIBindingIndexTag>;
