// SPDX-License-Identifier: GPL-2.0-or-later
// macOS GPU user directory: resolved on first use from BB_GPU_USER_DIR (bbgpu_init sets it from
// --user), never from the working directory or shadPS4's Application Support folder.
#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common/path_util.h"

namespace fs = std::filesystem;
using Common::FS::GetUserPath;
using Common::FS::PathType;

// Runs `body` in a child that must die with SIGABRT, and checks it created nothing under `home`.
template <typename Body>
static void ExpectAbort(const fs::path& home, Body&& body) {
    std::fflush(nullptr);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        const struct rlimit limit{0, 0};
        setrlimit(RLIMIT_CORE, &limit);
        setenv("HOME", home.c_str(), 1);
        body();
        (void)GetUserPath(PathType::UserDir);
        _exit(99);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(fs::is_empty(home));
}

int main() {
    char temp_template[] = "/tmp/bbport-gpu-user-XXXXXX";
    assert(mkdtemp(temp_template));
    const fs::path temp = fs::canonical(temp_template);
    const fs::path home = temp / "home";
    fs::create_directories(home);
    assert(chdir(temp.c_str()) == 0);
    // A portable "user" folder in the working directory must not be picked up either.
    fs::create_directories(temp / "user");

    ExpectAbort(home, [] { unsetenv("BB_GPU_USER_DIR"); });
    ExpectAbort(home, [] { setenv("BB_GPU_USER_DIR", "", 1); });
    const fs::path file = temp / "not-a-directory";
    std::fclose(std::fopen(file.c_str(), "w"));
    ExpectAbort(home, [&] { setenv("BB_GPU_USER_DIR", file.c_str(), 1); });
    assert(fs::is_empty(temp / "user"));

    // Set after the library loaded (as bbgpu_init does): the first use resolves it, relative to
    // the working directory, and creates the cache/log/shader folders inside it.
    setenv("BB_GPU_USER_DIR", "configured/user", 1);
    const fs::path expected = temp / "configured" / "user";
    assert(GetUserPath(PathType::UserDir) == expected);
    assert(GetUserPath(PathType::CacheDir) == expected / "cache");
    for (PathType type : {PathType::UserDir, PathType::LogDir, PathType::ShaderDir, PathType::CacheDir})
        assert(fs::is_directory(GetUserPath(type)));
    // Resolved once: later environment changes do not move the caches mid-run.
    setenv("BB_GPU_USER_DIR", (temp / "other").c_str(), 1);
    assert(GetUserPath(PathType::UserDir) == expected);
    assert(!fs::exists(temp / "other") && fs::is_empty(temp / "user") && fs::is_empty(home));

    fs::remove_all(temp);
    std::puts("macOS GPU user directory: lazy --user resolution, no cwd/shadPS4 fallback, loud errors PASS");
}
