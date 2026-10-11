// bbport: SDL3 window for the Vulkan swapchain.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <utility>
#include <cstring>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#include <pthread.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"

namespace Frontend {

#ifdef __APPLE__
namespace {
/// "AxB" (separator `sep`) as two ints, or a stop: a malformed or out-of-range value is a setup
/// error.
std::pair<int, int> ParsePair(const char* name, const char* value, char sep) {
    const auto parse = [&](const char* start, char terminator, const char** next) {
        char* end = nullptr;
        errno = 0;
        const long number = std::strtol(start, &end, 10);
        ASSERT_MSG(end != start && *end == terminator && errno != ERANGE &&
                       number >= std::numeric_limits<int>::min() &&
                       number <= std::numeric_limits<int>::max(),
                   "{}={} is not of the form A{}B with A and B integers", name, value, sep);
        *next = end;
        return int(number);
    };
    const char* end = nullptr;
    const int first = parse(value, sep, &end);
    const int second = parse(end + 1, '\0', &end);
    return {first, second};
}

/// "1" or "0" (or unset/empty: off); anything else stops.
bool ParseFlag(const char* name, const char* value) {
    if (!value || !*value || !std::strcmp(value, "0")) {
        return false;
    }
    ASSERT_MSG(!std::strcmp(value, "1"), "{}={} must be 0 or 1", name, value);
    return true;
}
} // namespace
#endif

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
#ifdef __APPLE__
    ASSERT_MSG(pthread_main_np(), "SDL video must be initialized on the process main thread");
#endif
#ifdef __APPLE__
    // bbport: test runs on a Mac someone is using (play_route). BB_WINDOW_SIZE=WxH (points; the
    // render size is unchanged, the swapchain follows the window), BB_WINDOW_POS=X,Y (instead of
    // centred) and BB_WINDOW_BACKGROUND=1 (an accessory app whose window is never activated and
    // takes no focus: input comes from BB_PAD_FILE). Off by default; malformed values stop.
    const char* size_env = std::getenv("BB_WINDOW_SIZE");
    const char* pos_env = std::getenv("BB_WINDOW_POS");
    const bool background = ParseFlag("BB_WINDOW_BACKGROUND", std::getenv("BB_WINDOW_BACKGROUND"));
    if (background) {
        // Before the video subsystem registers the app with Cocoa. Override priority, and checked:
        // a hint set elsewhere (environment) must not quietly bring activation back.
        for (const auto& [hint, value] : {std::pair{SDL_HINT_MAC_BACKGROUND_APP, "1"},
                                          std::pair{SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0"}}) {
            ASSERT_MSG(SDL_SetHintWithPriority(hint, value, SDL_HINT_OVERRIDE),
                       "BB_WINDOW_BACKGROUND: SDL refused hint {}={}: {}", hint, value,
                       SDL_GetError());
            const char* effective = SDL_GetHint(hint);
            ASSERT_MSG(effective && !std::strcmp(effective, value),
                       "BB_WINDOW_BACKGROUND: hint {} is {}, not {}", hint,
                       effective ? effective : "(unset)", value);
        }
    }
    if (size_env && *size_env) {
        const auto [w, h] = ParsePair("BB_WINDOW_SIZE", size_env, 'x');
        ASSERT_MSG(w > 0 && h > 0, "BB_WINDOW_SIZE={} must be positive", size_env);
        width_ = w;
        height_ = h;
    }
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
    if (pos_env && *pos_env) {
        std::tie(x, y) = ParsePair("BB_WINDOW_POS", pos_env, ',');
    }
#else
    const int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
#endif
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
#ifdef __APPLE__
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN, true);
#else
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
#endif
#ifdef __APPLE__
    if (background) {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FOCUSABLE_BOOLEAN, false);
    }
#endif
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
#ifdef __APPLE__
    if (driver && !std::strcmp(driver, "cocoa")) {
        metal_view = SDL_Metal_CreateView(window);
        ASSERT_MSG(metal_view, "Failed to create Metal view: {}", SDL_GetError());
        window_info.type = WindowSystemType::Metal;
        window_info.render_surface = SDL_Metal_GetLayer(metal_view);
        ASSERT_MSG(window_info.render_surface, "Failed to get Metal layer: {}", SDL_GetError());
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
#else
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
#endif
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
#ifdef __APPLE__
    if ((size_env && *size_env) || (pos_env && *pos_env) || background) {
        int at_x = 0, at_y = 0;
        SDL_GetWindowPosition(window, &at_x, &at_y);
        std::printf("Window: %dx%d points (%dx%d pixels) at %d,%d%s\n", width_, height_, w, h,
                    at_x, at_y, background ? ", background (no activation, not focusable)" : "");
        std::fflush(stdout);
    }
#endif
}

WindowSDL::~WindowSDL() {
#ifdef __APPLE__
    ASSERT_MSG(pthread_main_np(), "Metal view must be destroyed on the process main thread");
    SDL_Metal_DestroyView(metal_view);
#endif
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextEntry(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
#ifdef __APPLE__
    ASSERT_MSG(pthread_main_np(), "SDL events must be pumped on the process main thread");
#endif
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    return is_open;
}

} // namespace Frontend
