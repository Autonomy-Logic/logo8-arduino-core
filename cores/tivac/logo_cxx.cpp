// C++14/17 sized deallocation operators — Energia's new.cpp only provides the
// unsized forms, but -std=gnu++17 emits calls to the sized ones. Provided here
// minimal C++ runtime support hooks for this core.
#include <cstddef>
void operator delete(void* p, unsigned int) noexcept  { operator delete(p); }
void operator delete(void* p, unsigned long) noexcept { operator delete(p); }
void operator delete[](void* p, unsigned int) noexcept  { operator delete[](p); }
void operator delete[](void* p, unsigned long) noexcept { operator delete[](p); }
