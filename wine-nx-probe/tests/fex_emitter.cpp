#include <CodeEmitter/Emitter.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
extern "C" {
#include "../source/fex_jit.h"
}

static uint8_t *rx, *rw;
static constexpr size_t size = 8192;

namespace FEXCore::Allocator {
void *HorizonWritableAddress(const void *pointer)
{
    auto address = reinterpret_cast<uintptr_t>(pointer), base = reinterpret_cast<uintptr_t>(rx);
    return address >= base && address - base < size ? rw + address - base : const_cast<void *>(pointer);
}
void HorizonFlushICache(void *pointer, size_t length) { assert(!wine_nx_fex_jit_flush(pointer, length)); }
void *aligned_alloc(size_t alignment, size_t length)
{
    void *pointer = nullptr;
    assert(!::posix_memalign(&pointer, std::max(alignment, sizeof(void *)), length));
    return pointer;
}
void aligned_free(void *pointer) { std::free(pointer); }
}

int main()
{
    void *exec, *write;
    assert(!wine_nx_fex_jit_create(size, &exec, &write));
    rx = static_cast<uint8_t *>(exec);
    rw = static_cast<uint8_t *>(write);
    using namespace ARMEmitter;
    for (unsigned kind = 0; kind < 6; kind++)
    {
        std::memset(rw, 0, size);
        Emitter emitter(rx, size);
        ForwardLabel label;
        switch (kind)
        {
        case 0: assert(emitter.b(&label) == BranchEncodeSucceeded::Success); break;
        case 1: assert(emitter.adr(Reg::r0, &label) == BranchEncodeSucceeded::Success); emitter.ret(); break;
        case 2: assert(emitter.adrp(Reg::r0, &label) == BranchEncodeSucceeded::Success); emitter.ret(); emitter.Align(4096); break;
        case 3: assert(emitter.tbz(Reg::r0, 0, &label) == BranchEncodeSucceeded::Success); break;
        case 4: assert(emitter.cbz(Size::i64Bit, Reg::r0, &label) == BranchEncodeSucceeded::Success); break;
        case 5: emitter.ldr(XReg::x0, &label); emitter.ret(); break;
        }
        if (kind == 0 || kind == 3 || kind == 4) { emitter.movz(WReg::w0, 1); emitter.ret(); }
        const auto target = emitter.GetCursorAddress<uintptr_t>();
        assert(emitter.Bind(&label));
        if (kind == 5) emitter.dc64(42);
        else { emitter.movz(WReg::w0, 42); emitter.ret(); }
        emitter.ClearICache(rx, emitter.GetCursorOffset());
        auto result = reinterpret_cast<uintptr_t (*)(uintptr_t)>(rx)(0);
        assert(result == ((kind == 1 || kind == 2) ? target : 42));
        assert(emitter.GetBufferBase() == rx);
    }
    uint8_t plain[32] {};
    Buffer buffer(plain, sizeof(plain));
    buffer.EmitString("abc");
    buffer.Align(8);
    buffer.dc32(0x12345678);
    assert(buffer.GetCursorOffset() == 12 && !std::memcmp(plain, "abc", 3));
    assert(!wine_nx_fex_jit_close(rx));
    std::puts("FEX emitter: executable/write aliases and all forward-label encodings passed");
}
