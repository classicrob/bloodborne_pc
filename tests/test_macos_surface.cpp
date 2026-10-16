#ifdef __APPLE__
#include <SDL3/SDL.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <pthread.h>
#include "macos_ui.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "video_core/renderer_vulkan/vk_platform.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace BbOverlay {
bool HandleEvent(const SDL_Event&) { assert(pthread_main_np()); return false; }
void UpdateTextInput(SDL_Window*) { assert(pthread_main_np()); }
void SetTextEntry(bool, const std::string&, const std::string&) { assert(pthread_main_np()); }
bool CapturesInput() { return false; }
}

// Keyboard-and-mouse capture: a left click captures (and is not the game's); motion and clicks
// count only while captured; Cmd, focus loss, hiding or the menu/text dialog release everything.
static void mouse_capture() {
    using Frontend::MouseCapture;
    MouseCapture mouse;
    const auto event = [](SDL_EventType type, u8 button = 0) {
        SDL_Event e{};
        e.type = type;
        e.button.button = button;
        return e;
    };
    const auto motion = [](float x, float y) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.xrel = x;
        e.motion.yrel = y;
        return e;
    };
    float dx = 0, dy = 0;
    u32 buttons = 0;
    const auto take = [&] { return mouse.Take(dx, dy, buttons); };
    using C = MouseCapture::Change;
    assert(mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT), false, false) == C::None);
    assert(mouse.OnEvent(motion(5, 5), false, false) == C::None);
    assert(mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT), false, true) == C::None);
    assert(!take() && dx == 0 && dy == 0 && buttons == 0);
    assert(mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT), false, false) == C::Capture);
    mouse.SetCaptured(true);
    assert(mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT), false, false) == C::None);
    assert(take() && buttons == 0);
    mouse.OnEvent(motion(3, -2), false, false);
    mouse.OnEvent(motion(3, -2), false, false);
    assert(take() && dx == 6 && dy == -4 && buttons == 0);
    assert(take() && dx == 0 && dy == 0);
    // Shift at the press picks the variant for the whole hold.
    mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT), true, false);
    mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT), false, false);
    assert(take() && buttons == (MouseCapture::LeftShift | MouseCapture::Right));
    mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT), false, false);
    assert(take() && buttons == MouseCapture::Right);
    mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_RIGHT), true, false);
    assert(take() && buttons == 0);
    // A click between two takes is reported once.
    mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_MIDDLE), false, false);
    mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_MIDDLE), false, false);
    assert(take() && buttons == MouseCapture::Middle);
    assert(take() && buttons == 0);
    // Every release path, with a button held and motion pending.
    SDL_Event cmd{};
    cmd.type = SDL_EVENT_KEY_DOWN;
    cmd.key.key = SDLK_LGUI;
    SDL_Event other_key = cmd;
    other_key.key.key = SDLK_W;
    const SDL_Event releases[] = {cmd, event(SDL_EVENT_WINDOW_FOCUS_LOST),
                                  event(SDL_EVENT_WINDOW_HIDDEN), event(SDL_EVENT_WINDOW_MINIMIZED)};
    for (int blocked = 0; blocked < 2; ++blocked) {
        for (const SDL_Event& release : releases) {
            mouse.SetCaptured(true);
            mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT), false, false);
            mouse.OnEvent(motion(1, 1), false, false);
            assert(mouse.OnEvent(other_key, false, false) == C::None);
            assert(mouse.OnEvent(blocked ? other_key : release, false, blocked) == C::Release);
            mouse.SetCaptured(false);
            assert(!mouse.Captured() && !take() && dx == 0 && dy == 0 && buttons == 0);
        }
    }
    // A background (scripted) window never captures, whatever is clicked.
    mouse.SetCaptured(true);
    mouse.Disable();
    assert(!mouse.Captured() && !take() && buttons == 0);
    assert(mouse.OnEvent(event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT), false, false) == C::None);
    assert(!take() && buttons == 0);
    std::puts("PASS: mouse capture, Shift-latched clicks, short clicks, release on Cmd/focus/hide/menu, "
              "never in background windows");
}

static Frontend::WindowSDL* window;
static std::atomic<int> polls;

static void poll_window(void*) {
    assert(pthread_main_np());
    assert(window->PollEvents());
    ++polls;
}

static void create_window(void*) {
    assert(pthread_main_np());
    window = new Frontend::WindowSDL(320, 240, "bbport Metal surface test");
    assert(window->GetWindowInfo().type == Frontend::WindowSystemType::Metal);
    assert(window->GetWindowInfo().render_surface);
    assert(window->GetWidth() > 0 && window->GetHeight() > 0);
    assert(bb_macos_ui_set_poll(poll_window, nullptr) == 0);
}

static void destroy_window(void*) {
    assert(pthread_main_np());
    assert(bb_macos_ui_set_poll(nullptr, nullptr) == 0);
    delete window;
    window = nullptr;
}

static void accept_text(void*) {
    assert(pthread_main_np());
    assert(window->PollEvents());
    assert(SDL_TextInputActive(window->GetSDLWindow()));
    SDL_Event text{};
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.text = " entry";
    assert(SDL_PushEvent(&text));
    SDL_Event enter{};
    enter.type = SDL_EVENT_KEY_DOWN;
    enter.key.key = SDLK_RETURN;
    assert(SDL_PushEvent(&enter));
    assert(window->PollEvents());
    assert(!SDL_TextInputActive(window->GetSDLWindow()));
}

static int worker(void*) {
    assert(!pthread_main_np());
    assert(bb_macos_ui_call(create_window, nullptr) == 0);
    auto instance = Vulkan::CreateInstance(Frontend::WindowSystemType::Metal, false, false);
    auto surface = Vulkan::CreateSurface(*instance, *window);
    const auto [result, devices] = instance->enumeratePhysicalDevices();
    assert(result == vk::Result::eSuccess && !devices.empty());
    const auto [caps_result, caps] = devices.front().getSurfaceCapabilitiesKHR(surface);
    assert(caps_result == vk::Result::eSuccess && caps.maxImageCount > 0);
    for (int i = 0; i < 1000 && polls.load() == 0; ++i) {
        SDL_Delay(1);
    }
    assert(polls.load() > 0);
    window->BeginTextInput("Test", "Prompt");
    assert(bb_macos_ui_call(accept_text, nullptr) == 0);
    std::string text;
    assert(window->PollTextInput(text) == 1 && text == "Test entry");
    instance->destroySurfaceKHR(surface);
    instance.reset();
    assert(bb_macos_ui_call(destroy_window, nullptr) == 0);
    return 0;
}

int main() {
    mouse_capture();
    const int result = bb_macos_ui_run(worker, nullptr);
    assert(result == 0 && !window);
    std::puts("Darwin main-thread SDL Metal surface test passed");
    return result;
}
#else
int main() { return 0; }
#endif
