#include "PCH.h"
#include "InputManager.h"

namespace {
    // Бит маски для SDL-кнопки мыши (SDL_BUTTON_LEFT == 1 и т.д.).
    inline uint32_t MouseBit(uint8_t button) {
        return button ? (1u << (button - 1)) : 0u;
    }
}

InputManager::InputManager() = default;

void InputManager::HandleEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    {
        SDL_Scancode sc = event.key.scancode;
        if (static_cast<size_t>(sc) < held_.size())
            held_[static_cast<size_t>(sc)].store(true, std::memory_order_relaxed);
        // repeat не плодим в дискретной очереди — это удержание, не новое ребро.
        if (!event.key.repeat) {
            std::lock_guard<std::mutex> lock(key_mutex_);
            key_events_.push_back({ sc, true });
        }
        break;
    }
    case SDL_EVENT_KEY_UP:
    {
        SDL_Scancode sc = event.key.scancode;
        if (static_cast<size_t>(sc) < held_.size())
            held_[static_cast<size_t>(sc)].store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(key_mutex_);
        key_events_.push_back({ sc, false });
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        mouse_x_.store(event.motion.x, std::memory_order_relaxed);
        mouse_y_.store(event.motion.y, std::memory_order_relaxed);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        mouse_buttons_.fetch_or(MouseBit(event.button.button), std::memory_order_relaxed);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        mouse_buttons_.fetch_and(~MouseBit(event.button.button), std::memory_order_relaxed);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
    {
        float dir = (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0f : 1.0f;
        wheel_accum_.fetch_add(dir * event.wheel.y, std::memory_order_relaxed);
        break;
    }
    default:
        break;
    }
}

void InputManager::DrainKeyEvents(std::vector<KeyEvent>& out)
{
    out.clear();
    std::lock_guard<std::mutex> lock(key_mutex_);
    out.swap(key_events_);
}

void InputManager::SnapshotHeldKeys(std::vector<SDL_Scancode>& out) const
{
    out.clear();
    for (size_t i = 0; i < held_.size(); ++i) {
        if (held_[i].load(std::memory_order_relaxed))
            out.push_back(static_cast<SDL_Scancode>(i));
    }
}

bool InputManager::IsHeld(SDL_Scancode sc) const
{
    if (static_cast<size_t>(sc) >= held_.size()) return false;
    return held_[static_cast<size_t>(sc)].load(std::memory_order_relaxed);
}

bool InputManager::IsMouseButtonDown(uint8_t sdl_button) const
{
    return (mouse_buttons_.load(std::memory_order_relaxed) & MouseBit(sdl_button)) != 0u;
}

void InputManager::PushCommand(CommandId id, const void* data)
{
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    commands_.push_back({ id, data });
}

void InputManager::ExecuteCommands(EngineContext* ctx)
{
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        commands_scratch_.swap(commands_);
    }
    for (const InterfaceCommand& cmd : commands_scratch_) {
        const CommandFn& fn = registry_[static_cast<size_t>(cmd.id)];
        if (fn) fn(ctx, cmd.data);
    }
    commands_scratch_.clear();
}
