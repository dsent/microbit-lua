// -*- mode: c++; indent-tabs-mode: nil; -*-
// Small stand-ins for parts of the C and C++ runtime the firmware links but
// never uses. Each one keeps a large library member out of the image.

extern "C" {

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
