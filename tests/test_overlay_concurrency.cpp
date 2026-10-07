// SPDX-License-Identifier: GPL-2.0-or-later
// Include the implementation so this test needs no renderer or ImGui initialization.
#include "../gpu/shim/bbport_overlay.cpp"

#include <cassert>
#include <thread>
#include <vector>

namespace BbSettings {
Values& Get() {
    static Values values;
    return values;
}
} // namespace BbSettings

int main() {
    BbOverlay::initialized = true;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> done{false};
    std::atomic<unsigned> reads{0};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 3; ++i) {
        readers.emplace_back([&] {
            ready.fetch_add(1);
            while (!done.load()) {
                (void)BbOverlay::Visible();
                (void)BbOverlay::CapturesInput();
                reads.fetch_add(1);
            }
        });
    }
    while (ready.load() != readers.size() || reads.load() == 0) {
        std::this_thread::yield();
    }
    for (unsigned i = 0; i < 10000; ++i) {
        BbOverlay::SetTextEntry(true, "name", "hunter");
        BbOverlay::SetTextEntry(false, "", "");
    }
    done.store(true);
    for (auto& reader : readers) {
        reader.join();
    }
    assert(reads.load() > 0);
    assert(!BbOverlay::Visible() && !BbOverlay::CapturesInput());
    BbOverlay::SetTextEntry(true, "name", "hunter");
    assert(BbOverlay::Visible() && BbOverlay::CapturesInput());
    {
        std::scoped_lock lock{BbOverlay::imgui_mutex};
        assert(BbOverlay::text_entry_prompt == "name");
        assert(BbOverlay::text_entry_text == "hunter");
    }
    BbOverlay::SetTextEntry(false, "", "");
    assert(!BbOverlay::Visible() && !BbOverlay::CapturesInput());
    std::puts("PASS: concurrent overlay visibility and text-entry updates");
}
