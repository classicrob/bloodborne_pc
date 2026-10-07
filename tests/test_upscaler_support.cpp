// SPDX-License-Identifier: GPL-2.0-or-later
// Validate saved choices and the platform policy before the first rendered frame.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unistd.h>
#ifdef __APPLE__
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#endif
#include "bbport_settings.h"

int main() {
    using namespace BbSettings;
    char path[] = "/tmp/bbport-upscaler-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    auto& s = Get();
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);
    for (bool fsr4 : {false, true}) {
        for (bool fsr411 : {false, true}) {
            for (int requested = 0; requested < UpscalerCount; ++requested) {
                FILE* config = std::fopen(path, "w");
                assert(config);
                std::fprintf(config, "upscaler=%s\npreset=3\noutput_res=2560x1440\n",
                             UpscalerName(requested));
                std::fclose(config);
                s.fsr4_problem = nullptr;
                Load();
                assert(s.upscaler == requested);
#ifdef __APPLE__
                if (requested == UpscalerFsr4 || requested == UpscalerFsr411) {
                    std::fflush(nullptr);
                    const pid_t child = fork();
                    assert(child >= 0);
                    if (child == 0) {
                        const struct rlimit limit{0, 0};
                        setrlimit(RLIMIT_CORE, &limit);
                        ConfigureUpscalerSupport(fsr4, fsr411);
                        _exit(99);
                    }
                    int status = 0;
                    assert(waitpid(child, &status, 0) == child);
                    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
                    continue;
                }
                ConfigureUpscalerSupport(fsr4, fsr411);
                assert(s.upscaler == requested);
                assert(!s.fsr4_supported && !s.fsr411_supported);
                assert(!s.fsr4_problem.load());
                assert(s.preset == Performance && s.output_res == 2);
                assert(s.startup_upscaler == requested);
#else
                ConfigureUpscalerSupport(fsr4, fsr411);
                const bool unsupported = (requested == UpscalerFsr4 && !fsr4) ||
                    (requested == UpscalerFsr411 && !(fsr4 && fsr411));
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
                assert(s.fsr4_supported == fsr4);
                assert(s.fsr411_supported == (fsr4 && fsr411));
                assert(bool(s.fsr4_problem.load()) == unsupported);
                assert(s.preset == Performance && s.output_res == 2);
                // Startup patch settings still describe the already applied guest patches.
                assert(s.startup_upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411);
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
#endif
            }
        }
    }
    s.startup_preset = Quality;
    s.startup_upscaler = UpscalerFsr3;
    s.upscaler = UpscalerFsr3;
    s.preset = Performance;
    assert(RenderPreset() == Performance && !ResolutionNeedsRestart());
    setenv("BB_RENDER_RES", "1706x960", 1);
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart()); // provider change at the same patched size is live
    s.upscaler = UpscalerTaa;          // TAA needs native guest targets
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const int output = s.startup_output_res;
    s.output_res = (output + 1) % OutputCount;
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    setenv("BB_RENDER_RES", "", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    unsetenv("BB_RENDER_RES");
#ifdef __APPLE__
    static_assert(OutputCount == 5);
    static_assert(OutputWidths[3] == 3840 && OutputHeights[3] == 2160);
    static_assert(OutputWidths[4] == 3456 && OutputHeights[4] == 1944);
    s.upscaler = UpscalerFsr3;
    s.preset = Quality;
    s.output_res = 4;
    Save();
    s.output_res = OutputDefault;
    Load();
    assert(s.output_res == 4 && s.startup_output_res == 4);
    assert(s.upscaler == UpscalerFsr3 && RenderPreset() == Quality);
    FILE* config = std::fopen(path, "r");
    assert(config);
    char saved[4096]{};
    assert(std::fread(saved, 1, sizeof(saved) - 1, config) > 0);
    std::fclose(config);
    assert(std::strstr(saved, "output_res=3456x1944"));
#else
    static_assert(OutputCount == 4);
#endif
    std::remove(path);
#ifdef __APPLE__
    std::puts("Upscaler support: saved choices and explicit FSR 4/4.1.1 rejection PASS");
#else
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
#endif
}
