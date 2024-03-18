
#ifndef	_SPL_PROCESSOR_H
#define	_SPL_PROCESSOR_H

#include <sys/types.h>
#include <intrin.h>

extern uint32_t getcpuid();

typedef int	processorid_t;

#if defined(__amd64__) || defined(__x86_64__)

extern int __cpuid_count(unsigned int __level, unsigned int __sublevel,
	unsigned int __eax, unsigned int __ebx,
	unsigned int __ecx, unsigned int __edx);

static inline unsigned int
__get_cpuid_max(unsigned int __ext, unsigned int *__sig)
{
#ifdef _WIN32
	int32_t r[4];
	__cpuid(r, __ext);
	if (__sig)
		*__sig = (uint32_t)r[1];
	return ((uint32_t)r[0]);
#else
	unsigned int __eax, __ebx, __ecx, __edx;
	__cpuid(__ext, __eax, __ebx, __ecx, __edx);
	if (__sig)
		*__sig = __ebx;
	return (__eax);
#endif
}

#endif // x86

typedef int	processorid_t;
extern int spl_processor_init(void);

#endif /* _SPL_PROCESSOR_H */
