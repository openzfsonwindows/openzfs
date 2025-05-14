
#ifdef _MSC_VER
#include <intrin.h>
#include <stdint.h>

// we can use the MSVC-specific intrinsic __readcr8() to read the value of the CR8
// register directly. This intrinsic is part of MSVC's built-in functions,
// which allows us to access hardware-level registers without writing assembly code.

// Clang does not support the __readcr8 intrinsic, as it is specific to MSVC. 
// Clang does not have a direct equivalent 
// for accessing the CR8 register via a built-in function. Therefore, if we are using 
// Clang, we must either use inline assembly or a different method to access the register.
// https://learn.microsoft.com/en-us/cpp/intrinsics/readcr8?view=msvc-170

__declspec(dllexport) uint64_t read_cr8_msvc(void) {
    return __readcr8();
}
#else
#error "_MSC_VER not defined"
#endif
