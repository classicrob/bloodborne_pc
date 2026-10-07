#include "bbport_guest_alloc.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

extern "C" std::uintptr_t runtime_lookup(const void*, std::size_t, const char*) { return 0; }
struct alignas(64) GuestObject : BbPort::GuestAllocated {
    explicit GuestObject(bool& destroyed) : destroyed(destroyed) {}
    ~GuestObject() { destroyed = true; }
    std::uint64_t labels[16]{};
    bool& destroyed;
};
int main() {
    bool destroyed = false;
    {
        auto object = std::make_unique<GuestObject>(destroyed);
        auto start = reinterpret_cast<std::uintptr_t>(object.get());
        assert(start < (UINT64_C(1) << 40));
        assert(start + sizeof(*object) < (UINT64_C(1) << 40));
        assert(start % alignof(GuestObject) == 0);
        assert(reinterpret_cast<std::uintptr_t>(object->labels) < (UINT64_C(1) << 40));
        object->labels[15] = 42;
    }
    assert(destroyed);
    const char filename[] = "/app0/movie/sample.mp4";
    BbPort::GuestCopy low_filename(filename, sizeof(filename));
    assert(reinterpret_cast<std::uintptr_t>(low_filename.Data()) < (UINT64_C(1) << 40));
    assert(!std::strcmp(static_cast<const char*>(low_filename.Data()), filename));
    unsigned char data[256];
    std::memset(data, 0x55, sizeof(data));
    BbPort::GuestCopy low_buffer(data, sizeof(data));
    std::memset(low_buffer.Data(), 0x17, 37);
    low_buffer.CopyBack(data);
    for (unsigned i = 0; i < sizeof(data); ++i) assert(data[i] == (i < 37 ? 0x17 : 0x55));
    std::int32_t error = -5;
    BbPort::GuestCopy low_error(&error, sizeof(error));
    assert(*static_cast<std::int32_t*>(low_error.Data()) == error);
    *static_cast<std::int32_t*>(low_error.Data()) = -7;
    low_error.CopyBack(&error);
    assert(error == -7);
}
