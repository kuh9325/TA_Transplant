// C++ allocation counts in the offline observer only; C malloc is not counted.
#include <cstdlib>
#include <cstdint>
#include <new>
bool phase8cCountAllocations=false;
std::uint64_t phase8cAllocations=0, phase8cAllocatedBytes=0;
void* operator new(std::size_t n) {
    if (phase8cCountAllocations) { ++phase8cAllocations; phase8cAllocatedBytes+=n; }
    if (auto p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
