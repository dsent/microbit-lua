// -*- mode: c++; indent-tabs-mode: nil; -*-
// Small stand-ins for parts of the C and C++ runtime the firmware links but
// never uses. Each one keeps a large library member out of the image.

#include <cstddef>
#include <cstdlib>

// The panic code abort shows. CODAL's own codes leave 80 free: 20 to 21,
// 30, 40, 45, 50 to 52, 70 and 90 are taken.
#define RUNTIME_ABORT_PANIC 80

extern "C" void target_panic(int statusCode);

// operator new and delete on CODAL's heap. libstdc++'s new links its new
// handler and abort for a failure that cannot come back: CODAL's malloc
// stops the board with panic 020 when the heap runs out. A request for no
// bytes still gets a block of its own, as C++ requires.
static void *runtime_new(std::size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p)
        target_panic(20);
    return p;
}

void *operator new(std::size_t size) { return runtime_new(size); }
void *operator new[](std::size_t size) { return runtime_new(size); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, std::size_t) noexcept { free(p); }
void operator delete[](void *p, std::size_t) noexcept { free(p); }

extern "C" {

// newlib's abort raises a signal and ends in _exit's endless loop, with
// nothing on the display; this one stops the board with a panic code.
void abort(void)
{
    target_panic(RUNTIME_ABORT_PANIC);
    while (true) {
    }
}

// A failed assert in newlib, such as rand's check of its state, stops the
// board as abort does. newlib's __assert_func printed with fiprintf, which
// kept stdio's whole write path in the image, to a stderr no _write leads
// anywhere.
void __assert_func(const char *file, int line, const char *func,
                   const char *expr)
{
    (void)file; (void)line; (void)func; (void)expr;
    abort();
}

// The firmware never returns from main, so a global object's destructor
// never runs. Registering none keeps newlib's atexit list out.
int __aeabi_atexit(void *object, void (*destructor)(void *), void *dso)
{
    (void)object; (void)destructor; (void)dso;
    return 0;
}

// newlib's setjmp carries an ARM unwind table entry, and the entry names
// __aeabi_unwind_cpp_pr0. That one name links libgcc's whole C++ unwinder,
// about 4 KB. The firmware throws no C++ exception, since everything builds
// with -fno-exceptions, so nothing ever unwinds a frame. These three take
// the personality routines' names and answer _URC_FAILURE.
int __aeabi_unwind_cpp_pr0(int state, void *ucbp, void *context)
{
    (void)state; (void)ucbp; (void)context;
    return 9;
}

int __aeabi_unwind_cpp_pr1(int state, void *ucbp, void *context)
{
    (void)state; (void)ucbp; (void)context;
    return 9;
}

int __aeabi_unwind_cpp_pr2(int state, void *ucbp, void *context)
{
    (void)state; (void)ucbp; (void)context;
    return 9;
}

}
