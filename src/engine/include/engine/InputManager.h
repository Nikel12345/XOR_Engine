#pragma once
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "CommandId.h"
#include "UI_Interaction.h"

class EngineContext;

class InputManager {
public:
    struct KeyEvent {
        SDL_Scancode scancode;
        bool         down;
    };

    struct MouseButtonEvent {
        uint8_t button;
        bool    down;
        float   x;
        float   y;
    };

    InputManager();

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    void HandleEvent(const SDL_Event& event);

    void DrainKeyEvents(std::vector<KeyEvent>& out);
    void DrainMouseButtonEvents(std::vector<MouseButtonEvent>& out);
    void DrainUIInputEvents(std::vector<UIInputEvent>& out);

    void SetTextInputWanted(bool wanted) { text_input_wanted.store(wanted, std::memory_order_relaxed); }
    bool TextInputWanted() const { return text_input_wanted.load(std::memory_order_relaxed); }

    void  SnapshotHeldKeys(std::vector<SDL_Scancode>& out) const;

    bool  IsHeld(SDL_Scancode sc) const;
    bool  IsMouseButtonDown(uint8_t sdl_button) const;
    float MouseX() const { return mouse_x_.load(std::memory_order_relaxed); }
    float MouseY() const { return mouse_y_.load(std::memory_order_relaxed); }
    float ConsumeWheelDelta() { return wheel_accum_.exchange(0.0f, std::memory_order_relaxed); }

    using CommandFn = std::function<void(EngineContext*, const void* data)>;

    void RegisterCommand(CommandId id, CommandFn fn) {
        registry_[static_cast<size_t>(id)] = std::move(fn);
    }

    void PushCommand(CommandId id, const void* data = nullptr);

    void ExecuteCommands(EngineContext* ctx);

private:
    void PushMouseButtonEvent(const SDL_MouseButtonEvent& event);
    void PushUIInputEvent(UIInputEvent event);

    struct InterfaceCommand {
        CommandId   id;
        const void* data;
    };

    std::array<std::atomic<bool>, SDL_SCANCODE_COUNT> held_{};
    std::atomic<float>    mouse_x_{ 0.0f };
    std::atomic<float>    mouse_y_{ 0.0f };
    std::atomic<uint32_t> mouse_buttons_{ 0 };
    std::atomic<float>    wheel_accum_{ 0.0f };

    std::mutex            key_mutex_;
    std::vector<KeyEvent> key_events_;

    std::mutex                    mouse_button_mutex;
    std::vector<MouseButtonEvent> mouse_button_events;

    std::mutex                ui_input_mutex;
    std::vector<UIInputEvent> ui_input_events;
    std::atomic<bool>         text_input_wanted{ false };

    std::mutex                    cmd_mutex_;
    std::vector<InterfaceCommand> commands_;
    std::vector<InterfaceCommand> commands_scratch_;
    std::array<CommandFn, static_cast<size_t>(CommandId::COUNT)> registry_{};
};
