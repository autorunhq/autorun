#include "Interface/Core/LookupCache.h"
#include "Interface/Core/SharedCodeBufferManager.h"
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static constexpr size_t MiB = 1024 * 1024;
static size_t limit = 64 * MiB;
static bool null_failure;
static std::vector<size_t> requests;
static std::map<void*, size_t> mappings;

static void check(bool condition, const char* message) {
  if (!condition) { fprintf(stderr, "FEX code buffer: %s\n", message); std::exit(1); }
}

static void* map_memory(void* base, size_t size, int prot, int flags, int fd, off_t offset) {
  if (prot & PROT_EXEC) {
    requests.push_back(size);
    if (size > limit) return null_failure ? nullptr : MAP_FAILED;
  }
  void* result = ::mmap(base, size, prot, flags, fd, offset);
  check(result != MAP_FAILED, "host mmap failed");
  if (prot & PROT_EXEC) check(mappings.emplace(result, size).second, "duplicate allocation");
  return result;
}

static int unmap_memory(void* base, size_t size) {
  if (auto it = mappings.find(base); it != mappings.end()) {
    check(it->second == size, "release used the requested size instead of the allocated size");
    mappings.erase(it);
  }
  return ::munmap(base, size);
}

static void check_buffer(FEXCore::CPU::CodeBuffer& buffer, size_t size) {
  check(buffer.TotalAllocationSize() == size, "wrong allocation size");
  check(buffer.UsableSize() == size - 4096, "wrong usable size");
  auto allocation = buffer.AtomicAllocateBuffer(buffer.UsableSize());
  check(allocation.BufferAllocationOffset == buffer.GetBufferBase(), "cannot fill available space");
  allocation.BufferAllocationOffset[0] = 0x12;
  allocation.BufferAllocationOffset[buffer.UsableSize() - 1] = 0x34;
  check(buffer.AllocatedSpaceUsed() == buffer.UsableSize(), "incorrect used size");
  check(!buffer.AtomicAllocateBuffer(1).BufferAllocationOffset, "allocation exceeded buffer end");
  buffer.Reset();
  check(buffer.AtomicAllocateBuffer(1).BufferAllocationOffset == buffer.GetBufferBase(), "reset failed");
  check(buffer.AllocatedSpaceUsed() == 16, "allocation is not aligned");
}

int main() {
  LogMan::Throw::InstallHandler([](const char* message) { fprintf(stderr, "%s\n", message); std::abort(); });
  FEXCore::Allocator::mmap = map_memory;
  FEXCore::Allocator::munmap = unmap_memory;
  for (bool fail_with_null : {false, true}) {
    null_failure = fail_with_null;
    for (size_t available : {16 * MiB, 32 * MiB, 64 * MiB}) {
      limit = available;
      requests.clear();
      {
        FEXCore::CPU::CodeBuffer buffer(64 * MiB);
        std::vector<size_t> expected;
        for (size_t size = 64 * MiB; size >= available; size /= 2) expected.push_back(size);
        check(requests == expected, "wrong retry sequence");
        check_buffer(buffer, available);
      }
      check(mappings.empty(), "buffer leaked");
    }
    limit = 16 * MiB;
    {
      FEXCore::CPU::SharedCodeBufferManager manager;
      auto old = manager.GetLatest();
      auto* old_base = old->GetBufferBase();
      old_base[0] = 0x5a;
      requests.clear();
      auto next = manager.StartLargerCodeBuffer();
      check(requests == std::vector<size_t> {32 * MiB, 16 * MiB}, "rollover did not retry smaller buffer");
      check(next != old && manager.GetLatest() == next, "wrong active buffer");
      check(mappings.size() == 2 && old_base[0] == 0x5a, "rollover destroyed old code");
      check_buffer(*next, 16 * MiB);
      old.reset();
      check(!mappings.contains(old_base) && mappings.size() == 1, "old code was not released");
      requests.clear();
      auto maximal = manager.StartMaximalCodeBuffer();
      check(requests == std::vector<size_t> {64 * MiB, 32 * MiB, 16 * MiB}, "maximum retry sequence is wrong");
      check_buffer(*maximal, 16 * MiB);
    }
    check(mappings.empty(), "manager leaked code buffers");
    const auto child = fork();
    check(child >= 0, "fork failed");
    if (!child) {
      limit = 0;
      requests.clear();
      LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char*) {
        if (level == LogMan::ASSERT)
          _exit(requests == std::vector<size_t> {64 * MiB, 32 * MiB, 16 * MiB} ? 0 : 2);
      });
      FEXCore::CPU::CodeBuffer buffer(64 * MiB);
      _exit(3);
    }
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "allocation failure was not bounded at 16 MiB");
  }
  FEXCore::Allocator::mmap = ::mmap;
  FEXCore::Allocator::munmap = ::munmap;
  puts("FEX code buffer: growth retries, bounds, rollover, retained code, release and exhaustion passed");
}
