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
    const int result = bb_macos_ui_run(worker, nullptr);
    assert(result == 0 && !window);
    std::puts("Darwin main-thread SDL Metal surface test passed");
    return result;
}
#else
int main() { return 0; }
#endif
