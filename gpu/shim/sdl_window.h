// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;
union SDL_Event;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

/// The mouse for keyboard-and-mouse play. A left click in the window captures it (relative mode;
/// that click is not passed on; never in a BB_WINDOW_BACKGROUND window); Cmd, focus loss, the settings menu or the text dialog release it.
/// The pad (runtime_pad.c, through bbgpu_mouse_take) takes the motion and buttons.
class MouseCapture {
public:
    /// Button bits; the Shift variants mean Shift was held when the button went down.
    enum : u32 { Left = 1, Right = 2, Middle = 4, LeftShift = 8, RightShift = 16 };
    enum class Change { None, Capture, Release };
    /// Records one window event. `shift`: Shift is down; `blocked`: the settings menu or the text
    /// dialog has the input. The caller applies a returned change (relative mode, SetCaptured).
    Change OnEvent(const SDL_Event& event, bool shift, bool blocked);
    void SetCaptured(bool value);
    bool Captured() const;
    /// Never captures again (a background window: scripted input only).
    void Disable();
    /// Motion (points) and buttons since the previous take: a button pressed and released in
    /// between is reported once. False, with nothing, while released.
    bool Take(float& dx, float& dy, u32& buttons);

private:
    mutable std::mutex mutex;
    bool captured{}, disabled{};
    float dx{}, dy{};
    u32 held{}, pressed{};
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);
    /// See MouseCapture::Take.
    bool TakeMouse(float& dx, float& dy, u32& buttons) { return mouse.Take(dx, dy, buttons); }

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    void SetMouseCaptured(bool value);
    MouseCapture mouse;
    SDL_Window* window{};
#ifdef __APPLE__
    void* metal_view{};
#endif
    WindowSystemInfo window_info{};
};

} // namespace Frontend
