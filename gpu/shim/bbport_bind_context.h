// bbport (macOS): the shader resource binding being resolved on this thread, for stop messages
// raised deep in the texture cache (an ImageView that cannot be created names the shader, the
// resource and its raw sharp). The binder installs a Scope around the lookup; describing happens
// only when a stop prints, so the binding path pays for two thread-local stores.
#pragma once
#ifdef __APPLE__
#include <string>

namespace BbBindContext {

using Describe = std::string (*)(const void* context);

struct Current {
    const void* context = nullptr;
    Describe describe = nullptr;
};
inline thread_local Current current;

/// While alive, stops on this thread describe `context` with `describe`.
class Scope {
public:
    Scope(const void* context, Describe describe) : previous{current} {
        current = {context, describe};
    }
    ~Scope() {
        current = previous;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Current previous;
};

/// The binding being resolved, or why there is none.
inline std::string Text() {
    return current.describe ? current.describe(current.context)
                            : std::string{"not inside a shader resource binding"};
}

} // namespace BbBindContext
#endif
