/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2010 Sun Microsystems, Inc. All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Copyright (c) 2012 by Delphix. All rights reserved.
 */

/* Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T */
/* All Rights Reserved */


/*
 * Available Solaris debug functions.  All of the ASSERT() macros will be
 * compiled out when NDEBUG is defined, this is the default behavior for
 * the SPL.  To enable assertions use the --enable-debug with configure.
 * The VERIFY() functions are never compiled out and cannot be disabled.
 *
 * PANIC()	- Panic the node and print message.
 * ASSERT()	- Assert X is true, if not panic.
 * ASSERTF()	- Assert X is true, if not panic and print message.
 * ASSERTV()	- Wraps a variable declaration which is only used by ASSERT().
 * ASSERT3S()	- Assert signed X OP Y is true, if not panic.
 * ASSERT3U()	- Assert unsigned X OP Y is true, if not panic.
 * ASSERT3P()	- Assert pointer X OP Y is true, if not panic.
 * ASSERT0()	- Assert value is zero, if not panic.
 * VERIFY()	- Verify X is true, if not panic.
 * VERIFY3S()	- Verify signed X OP Y is true, if not panic.
 * VERIFY3U()	- Verify unsigned X OP Y is true, if not panic.
 * VERIFY3P()	- Verify pointer X OP Y is true, if not panic.
 * VERIFY0()	- Verify value is zero, if not panic.
 */

#ifndef _SPL_DEBUG_H
#define	_SPL_DEBUG_H

#include <spl-debug.h>
#include <stdio.h>
#include <Trace.h>


#if defined(_MSC_VER) && !defined(__clang__)

#define	unlikely(X) X
#define	likely(X) X
#define	__maybe_unused [[maybe_unused]]
#define	__printflike(X, Y)
#define	__unused [[maybe_unused]]
#define	always_inline __forceinline
#define	_Noreturn __declspec(noreturn)

#else

#define	try __try
#define	except __except

#ifndef expect
#define	expect(expr, value) (__builtin_expect((expr), (value)))
#endif
#define	likely(x)		__builtin_expect(!!(x), 1)
#define	unlikely(x)		__builtin_expect(!!(x), 0)

#ifndef __maybe_unused
#define	__maybe_unused  __attribute__((unused))
#endif
#define	__printflike(a, b) __attribute__((__format__(__printf__, a, b)))

#define	__unused  __attribute__((unused))
#define	_Noreturn	__attribute__((__noreturn__))

#endif

// cdefs.h
#ifndef	__DECONST
#define	__DECONST(type, var) ((type)(uintptr_t)(const void *)(var))
#endif

extern void _Noreturn panic(const char *fmt, ...);



extern void printBuffer(const char *fmt, ...);

#define	LUDICROUS_SPEED // use circular buffer
// xprintf is always printed
// dprintf is printed in DEBUG builds
// IOLog is printed in DEBUG builds (legacy from osx)
//
#ifdef DBG /* Debugging Disabled */
#ifdef LUDICROUS_SPEED
#define	dprintf(...) printBuffer(__VA_ARGS__)
#define	IOLog(...) printBuffer(__VA_ARGS__)
#define	xprintf(...) KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
    __VA_ARGS__))
#define	TraceEvent(x, ...)

#else // LUDICROUS_SPEED

#undef KdPrintEx
#define	KdPrintEx(_x_) DbgPrintEx _x_
#define	dprintf(...) KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
    __VA_ARGS__))
#define	IOLog(...) KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
    __VA_ARGS__))
#define	xprintf(...) KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
    __VA_ARGS__))
#define	TraceEvent(level, ...) KdPrintEx((DPFLTR_IHVDRIVER_ID, level, \
    __VA_ARGS__))

#endif // LUDICROUS_SPEED

#define	PANIC(fmt, ...)				\
	do {					\
		xprintf(fmt, __VA_ARGS__);	\
		DbgBreakPoint();		\
	} while (0)

#define	SPL_DEBUG_STR	" (DEBUG mode)"

#else // DBG

#define	SPL_DEBUG_STR	""

#define	TraceEvent(x, ...)
#define	xprintf(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
    __VA_ARGS__)
#define	dprintf(...)
#define	IOLog(...)
#define	PANIC(fmt, ...)				\
	do {					\
		xprintf(fmt, __VA_ARGS__);	\
	} while (0)
#endif

#define	zfs_fallthrough __attribute__((__fallthrough__))

/* From here is a copy of FreeBSD's debug.h */

/*
 * Without this, we see warnings from objtool during normal Linux builds when
 * the kernel is built with CONFIG_STACK_VALIDATION=y:
 *
 * warning: objtool: tsd_create() falls through to next function __list_add()
 * warning: objtool: .text: unexpected end of section
 *
 * Until the toolchain stops doing this, we must only define this attribute on
 * spl_panic() when doing static analysis.
 */
#if defined(__COVERITY__) || defined(__clang_analyzer__)
__attribute__((__noreturn__))
#endif
extern void spl_panic(const char *file, const char *func, int line,
    const char *fmt, ...);
extern void spl_dumpstack(void);

static inline int
spl_assert(const char *buf, const char *file, const char *func, int line)
{
	spl_panic(file, func, line, "%s", buf);
	return (0);
}

#define	VERIFY(cond)							\
	(void) (unlikely(!(cond)) &&					\
	    spl_assert("VERIFY(" #cond ") failed\n",			\
	    __FILE__, __FUNCTION__, __LINE__))

#define	VERIFYF(cond, str, ...)		do {				\
		if (unlikely(!cond))					\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY(" #cond ") failed " str "\n", __VA_ARGS__);\
	} while (0)

#define	VERIFY3B(LEFT, OP, RIGHT)	do {				\
		const boolean_t _verify3_left = (boolean_t)(LEFT);	\
		const boolean_t _verify3_right = (boolean_t)(RIGHT);	\
		if (unlikely(!(_verify3_left OP _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY3(" #LEFT " "  #OP " "  #RIGHT ") "		\
		    "failed (%d " #OP " %d)\n",				\
		    (boolean_t)_verify3_left,				\
		    (boolean_t)_verify3_right);				\
	} while (0)

#define	VERIFY3S(LEFT, OP, RIGHT)	do {				\
		const int64_t _verify3_left = (int64_t)(LEFT);		\
		const int64_t _verify3_right = (int64_t)(RIGHT);	\
		if (unlikely(!(_verify3_left OP _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY3(" #LEFT " "  #OP " "  #RIGHT ") "		\
		    "failed (%lld " #OP " %lld)\n",			\
		    (long long)_verify3_left,				\
		    (long long)_verify3_right);				\
	} while (0)

#define	VERIFY3U(LEFT, OP, RIGHT)	do {				\
		const uint64_t _verify3_left = (uint64_t)(LEFT);	\
		const uint64_t _verify3_right = (uint64_t)(RIGHT);	\
		if (unlikely(!(_verify3_left OP _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY3(" #LEFT " "  #OP " "  #RIGHT ") "		\
		    "failed (%llu " #OP " %llu)\n",			\
		    (unsigned long long)_verify3_left,			\
		    (unsigned long long)_verify3_right);		\
	} while (0)

#define	VERIFY3P(LEFT, OP, RIGHT)	do {				\
		const uintptr_t _verify3_left = (uintptr_t)(LEFT);	\
		const uintptr_t _verify3_right = (uintptr_t)(RIGHT);	\
		if (unlikely(!(_verify3_left OP _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY3(" #LEFT " "  #OP " "  #RIGHT ") "		\
		    "failed (%px " #OP " %px)\n",			\
		    (void *)_verify3_left,				\
		    (void *)_verify3_right);				\
	} while (0)

#define	VERIFY0(RIGHT)	do {						\
		const int64_t _verify0_right = (int64_t)(RIGHT);	\
		if (unlikely(!(0 == _verify0_right)))			\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY0(" #RIGHT ") "				\
		    "failed (0 == %lld)\n",				\
		    (long long)_verify0_right);				\
	} while (0)

#define	VERIFY0P(RIGHT)	do {						\
		const uintptr_t _verify0_right = (uintptr_t)(RIGHT);	\
		if (unlikely(!(0 == _verify0_right)))			\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY0P(" #RIGHT ") "				\
		    "failed (NULL == %px)\n",				\
		    (void *)_verify0_right);				\
	} while (0)

/*
 * Note that you should not put any operations you want to always happen
 * in the print section for ASSERTs unless you only want them to run on
 * debug builds!
 * e.g. ASSERT3UF(2, <, 3, "%s", foo(x)), foo(x) won't run on non-debug
 * builds.
 */

#define	VERIFY3BF(LEFT, OP, RIGHT, STR, ...)	do {			\
		const boolean_t _verify3_left = (boolean_t)(LEFT);	\
		const boolean_t _verify3_right = (boolean_t)(RIGHT);	\
		if (unlikely(!(_verify3_left OP _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY3(" #LEFT " "  #OP " "  #RIGHT ") "		\
		    "failed (%d " #OP " %d) " STR "\n",			\
		    (boolean_t)(_verify3_left),				\
		    (boolean_t)(_verify3_right),			\
		    __VA_ARGS__);					\
	} while (0)

#define	VERIFY3SF(LEFT, OP, RIGHT, STR, ...)	do {			\
		const int64_t _verify3_left = (int64_t)(LEFT);		\
		const int64_t _verify3_right = (int64_t)(RIGHT);	\
		if (unlikely(!(_verify3_left OP _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY3(" #LEFT " "  #OP " "  #RIGHT ") "		\
		    "failed (%lld " #OP " %lld) " STR "\n",		\
		    (long long)(_verify3_left),				\
		    (long long)(_verify3_right),			\
		    __VA_ARGS);						\
	} while (0)

#define	VERIFY3UF(LEFT, OP, RIGHT, STR, ...)	do {			\
		const uint64_t _verify3_left = (uint64_t)(LEFT);	\
		const uint64_t _verify3_right = (uint64_t)(RIGHT);	\
		if (unlikely(!(_verify3_left OP _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY3(" #LEFT " "  #OP " "  #RIGHT ") "		\
		    "failed (%llu " #OP " %llu) " STR "\n",		\
		    (unsigned long long)(_verify3_left),		\
		    (unsigned long long)(_verify3_right),		\
		    __VA_ARGS);						\
	} while (0)

#define	VERIFY3PF(LEFT, OP, RIGHT, STR, ...)	do {			\
		const uintptr_t _verify3_left = (uintptr_t)(LEFT);	\
		const uintptr_t _verify3_right = (uintptr_t)(RIGHT);	\
		if (unlikely(!(_verify3_left OP _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY3(" #LEFT " "  #OP " "  #RIGHT ") "		\
		    "failed (%px " #OP " %px) " STR "\n",		\
		    (void *) (_verify3_left),				\
		    (void *) (_verify3_right),				\
		    __VA_ARGS__);					\
	} while (0)

#define	VERIFY0PF(RIGHT, STR, ...)	do {				\
		const uintptr_t _verify3_left = (uintptr_t)(0);		\
		const uintptr_t _verify3_right = (uintptr_t)(RIGHT);	\
		if (unlikely(!(_verify3_left == _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY0(0 == " #RIGHT ") "				\
		    "failed (0 == %px) " STR "\n",			\
		    (long long) (_verify3_right),			\
		    __VA_ARGS__);					\
	} while (0)

#define	VERIFY0F(RIGHT, STR, ...)	do {				\
		const int64_t _verify3_left = (int64_t)(0);		\
		const int64_t _verify3_right = (int64_t)(RIGHT);	\
		if (unlikely(!(_verify3_left == _verify3_right)))	\
		    spl_panic(__FILE__, __FUNCTION__, __LINE__,		\
		    "VERIFY0(0 == " #RIGHT ") "				\
		    "failed (0 == %lld) " STR "\n",			\
		    (long long) (_verify3_right),			\
		    __VA_ARGS__);					\
	} while (0)

#define	VERIFY_IMPLY(A, B) \
	((void)(likely((!(A)) || (B)) ||				\
	    spl_assert("(" #A ") implies (" #B ")",			\
	    __FILE__, __FUNCTION__, __LINE__)))

#define	VERIFY_EQUIV(A, B) \
	((void)(likely(!!(A) == !!(B)) || 				\
	    spl_assert("(" #A ") is equivalent to (" #B ")",		\
	    __FILE__, __FUNCTION__, __LINE__)))

/*
 * Debugging disabled (--disable-debug)
 */
#ifdef _WIN32
#undef ASSERT
#endif

#if !defined(DBG)

#define	ASSERT(x)		((void) sizeof ((uintptr_t)(x)))
#define	ASSERTV(x)
#define	ASSERT3B(x, y, z)						\
	((void) sizeof ((uintptr_t)(x)), (void) sizeof ((uintptr_t)(z)))
#define	ASSERT3S(x, y, z)						\
	((void) sizeof ((uintptr_t)(x)), (void) sizeof ((uintptr_t)(z)))
#define	ASSERT3U(x, y, z)						\
	((void) sizeof ((uintptr_t)(x)), (void) sizeof ((uintptr_t)(z)))
#define	ASSERT3P(x, y, z)						\
	((void) sizeof ((uintptr_t)(x)), (void) sizeof ((uintptr_t)(z)))
#define	ASSERT0(x)		((void) sizeof ((uintptr_t)(x)))
#define	ASSERT0P(x)		((void) sizeof ((uintptr_t)(x)))
#define	ASSERT3BF(x, y, z, str, ...)	ASSERT3B(x, y, z)
#define	ASSERT3SF(x, y, z, str, ...)	ASSERT3S(x, y, z)
#define	ASSERT3UF(x, y, z, str, ...)	ASSERT3U(x, y, z)
#define	ASSERT3PF(x, y, z, str, ...)	ASSERT3P(x, y, z)
#define	ASSERT0PF(x, str, ...)		ASSERT0P(x)
#define	ASSERT0F(x, str, ...)		ASSERT0(x)
#define	ASSERTF(x, str, ...)		ASSERT(x)
#define	IMPLY(A, B)							\
	((void) sizeof ((uintptr_t)(A)), (void) sizeof ((uintptr_t)(B)))
#define	EQUIV(A, B)		\
	((void) sizeof ((uintptr_t)(A)), (void) sizeof ((uintptr_t)(B)))

/*
 * Debugging enabled (--enable-debug)
 */
#else

#define	ASSERT3B	VERIFY3B
#define	ASSERT3S	VERIFY3S
#define	ASSERT3U	VERIFY3U
#define	ASSERT3P	VERIFY3P
#define	ASSERT0		VERIFY0
#define	ASSERT0P	VERIFY0P
#define	ASSERT3BF	VERIFY3BF
#define	ASSERT3SF	VERIFY3SF
#define	ASSERT3UF	VERIFY3UF
#define	ASSERT3PF	VERIFY3PF
#define	ASSERT0PF	VERIFY0PF
#define	ASSERT0F	VERIFY0F
#define	ASSERTF		VERIFYF
#define	ASSERTV(x)	x
#define	ASSERT		VERIFY
#define	IMPLY		VERIFY_IMPLY
#define	EQUIV		VERIFY_EQUIV

#endif /* NDEBUG */

#endif /* SPL_DEBUG_H */
