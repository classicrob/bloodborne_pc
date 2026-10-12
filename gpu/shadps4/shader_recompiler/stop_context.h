// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Apple): what the shader being compiled on this thread is compiled for (the draw or
// dispatch, set by the pipeline cache), for loud stops whose cause is game state the recompiler
// only sees through its user data (e.g. the bound V# of an access it cannot bound).
#pragma once

#include <string>

namespace Shader {

inline thread_local std::string compile_stop_context;

/// Sets compile_stop_context for one compile.
struct CompileStopContext {
    explicit CompileStopContext(std::string context) {
        compile_stop_context = std::move(context);
    }
    ~CompileStopContext() {
        compile_stop_context.clear();
    }
    CompileStopContext(const CompileStopContext&) = delete;
    CompileStopContext& operator=(const CompileStopContext&) = delete;
};

} // namespace Shader
