
#ifndef	_SPL_PROCESSOR_H
#define	_SPL_PROCESSOR_H

#include <sys/types.h>

extern uint32_t getcpuid();

typedef int	processorid_t;

#if defined(__amd64__) || defined(__x86_64__)

extern int __cpuid_count(unsigned int __level, unsigned int __sublevel,
	unsigned int __eax, unsigned int __ebx,
	unsigned int __ecx, unsigned int __edx);

#define	__cpuid_count(level, count, a, b, c, d) \
	__asm__("xchg{l}\t{%%}ebx, %1\n\t" \
		"cpuid\n\t" \
		"xchg{l}\t{%%}ebx, %1\n\t" \
		: "=a" (a), "=r" (b), "=c" (c), "=d" (d) \
		: "0" (level), "2" (count))

#define	__cpuid(level, a, b, c, d) \
	__asm__("xchg{l}\t{%%}ebx, %1\n\t" \
		"cpuid\n\t" \
		"xchg{l}\t{%%}ebx, %1\n\t" \
		: "=a" (a), "=r" (b), "=c" (c), "=d" (d) \
		: "0" (level))

static inline unsigned int
__get_cpuid_max(unsigned int __ext, unsigned int *__sig)
{
	unsigned int __eax, __ebx, __ecx, __edx;
	__cpuid(__ext, __eax, __ebx, __ecx, __edx);
	if (__sig)
		*__sig = __ebx;
	return (__eax);
}

/* macOS does have do_cpuid() macro */
static inline int
__get_cpuid(unsigned int __level,
    unsigned int *__eax, unsigned int *__ebx,
    unsigned int *__ecx, unsigned int *__edx)
{
	unsigned int __ext = __level & 0x80000000;
	if (__get_cpuid_max(__ext, 0) < __level)
		return (0);
	__cpuid(__level, *__eax, *__ebx, *__ecx, *__edx);
	return (1);
}

#endif // x86

typedef int	processorid_t;
extern int spl_processor_init(void);

#endif /* _SPL_PROCESSOR_H */
