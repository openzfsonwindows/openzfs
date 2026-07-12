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
 *
 * Copyright (C) 2017 Jorgen Lundman <lundman@lundman.net>
 *
 * Following the guide at http://www.cs.wustl.edu/~schmidt/win32-cv-1.html
 * and implementing the second-to-last suggestion, albeit in kernel mode,
 * and replacing CriticalSection with Atomics. At some point, we should
 * perhaps look at the final "SignalObjectAndWait" solution, presumably
 * by using the Wait argument to Mutex, and call WaitForObject.
 */

#include <sys/atomic.h>
#include <sys/condvar.h>
#include <spl-debug.h>
#include <sys/callb.h>

#ifdef SPL_DEBUG_MUTEX
void spl_wdlist_settime(void *mpleak, uint64_t value);
#endif

#define	CONDVAR_INIT 0x12345678

void
spl_cv_init(kcondvar_t *cvp, char *name, kcv_type_t type, void *arg)
{
	(void) cvp;	(void) name; (void) type; (void) arg;

	KeInitializeEvent(&cvp->cv_kevent[CV_SIGNAL], SynchronizationEvent,
	    FALSE);
	KeInitializeEvent(&cvp->cv_kevent[CV_BROADCAST], NotificationEvent,
	    FALSE);

	cvp->cv_waiters_count = 0;
	cvp->cv_initialised = CONDVAR_INIT;
}

void
spl_cv_destroy(kcondvar_t *cvp)
{
	if (cvp->cv_initialised != CONDVAR_INIT)
		panic("%s: not cv_initialised", __func__);
	// We have probably already signalled the waiters, but we need to
	// kick around long enough for them to wake.
	while (cvp->cv_waiters_count > 0)
		cv_broadcast(cvp);
	ASSERT0(cvp->cv_waiters_count);
	cvp->cv_initialised = 0;
}

void
spl_cv_signal(kcondvar_t *cvp)
{
	if (cvp->cv_initialised != CONDVAR_INIT)
		panic("%s: not cv_initialised", __func__);

	/*
	 * Always set the event even when cv_waiters_count == 0.  A thread may
	 * be about to call spl_cv_wait() but hasn't incremented the counter
	 * yet (it increments before releasing the mutex).  Because the caller
	 * holds the same mutex, the signal cannot arrive between the counter
	 * increment and the KeWaitForMultipleObjects call — but it CAN arrive
	 * before the counter increment if the signaller holds the mutex while
	 * calling us and the waiter hasn't acquired it yet.  Skipping
	 * KeSetEvent in that window causes a missed wakeup.
	 *
	 * CV_SIGNAL is a SynchronizationEvent (auto-reset): it auto-clears
	 * after waking one waiter, so there is no spurious-wakeup accumulation
	 * risk from always setting it.
	 */
	KeSetEvent(&cvp->cv_kevent[CV_SIGNAL], 0, FALSE);
}

/* WakeConditionVariable or WakeAllConditionVariable function. */

void
spl_cv_broadcast(kcondvar_t *cvp)
{
	if (cvp->cv_initialised != CONDVAR_INIT)
		panic("%s: not cv_initialised", __func__);

	/*
	 * Only set the broadcast event when there are registered waiters.
	 *
	 * CV_BROADCAST is a NotificationEvent (manual-reset): once set it
	 * stays set until KeClearEvent is called by the last exiting waiter.
	 * If we set it with zero waiters it remains set indefinitely, causing
	 * the next unrelated cv_wait call to return immediately as a spurious
	 * wakeup.  Callers such as dnode_special_close use "if" rather than
	 * "while" before cv_wait and will ASSERT if cv_wait returns while the
	 * condition is still false.
	 *
	 * This is safe against missed wakeups: cv_waiters_count is incremented
	 * inside cv_wait *before* the caller's mutex is released, and the
	 * broadcaster always holds that same mutex.  Therefore cv_waiters_count
	 * == 0 at broadcast time means no thread is yet registered as a waiter.
	 * Any thread that will later call cv_wait must first acquire the mutex,
	 * at which point it will observe the already-updated condition (set by
	 * the broadcaster under the same lock) and will skip the wait.
	 */
	if (cvp->cv_waiters_count > 0)
		KeSetEvent(&cvp->cv_kevent[CV_BROADCAST], 0, FALSE);
}

/*
 * Block on the indicated condition variable and
 * release the associated mutex while blocked.
 */
int
spl_cv_wait(kcondvar_t *cvp, kmutex_t *mp, int flags, const char *msg)
{
	int result;
	if (cvp->cv_initialised != CONDVAR_INIT)
		panic("%s: not cv_initialised", __func__);

	if (msg != NULL && msg[0] == '&')
		++msg;  /* skip over '&' prefixes */
#ifdef SPL_DEBUG_MUTEX
	spl_wdlist_settime(mp->leak, 0);
#endif

	atomic_inc_32(&cvp->cv_waiters_count);
	mutex_exit(mp);

	void *locks[CV_MAX_EVENTS] =
		{ &cvp->cv_kevent[CV_SIGNAL], &cvp->cv_kevent[CV_BROADCAST] };

	LARGE_INTEGER timeout;
	timeout.QuadPart = -10000000LL;  // 1s

	/*
	 * This variant blocks forever, unless PCATCH is supplied
	 * (the cv_wait_sig() variant, then we need to periodically
	 * surface to see if the process has been told to terminate
	 */
	do {

		result = KeWaitForMultipleObjects(CV_MAX_EVENTS,
		    locks, WaitAny, Executive, KernelMode, FALSE,
		    (flags & PCATCH) ? &timeout : NULL, NULL);

		if (PsIsThreadTerminating(PsGetCurrentThread()))
			result = STATUS_ALERTED;

	} while (result == STATUS_TIMEOUT);

	mutex_enter(mp);

	atomic_dec_32(&cvp->cv_waiters_count);

	/*
	 * If we were the last waiter and we woke on the broadcast event,
	 * clear it so stale signals don't cause spurious wakeups for the
	 * next unrelated wait cycle.  We do this after decrementing the
	 * counter (and while still holding mp) so that a concurrent
	 * cv_broadcast() — which also runs under mp — cannot have its
	 * KeSetEvent silently undone by our KeClearEvent.
	 */
	if (result == STATUS_WAIT_0 + CV_BROADCAST &&
	    cvp->cv_waiters_count == 0)
		KeClearEvent(&cvp->cv_kevent[CV_BROADCAST]);

#ifdef SPL_DEBUG_MUTEX
	spl_wdlist_settime(mp->leak, gethrestime_sec());
#endif
	/*
	 * 1 - condvar got cv_signal()/cv_broadcast()
	 * 0 - received signal (kill -signal)
	 */
	return (result == STATUS_ALERTED ? 0 : 1);
}

/*
 * Same as cv_wait except the thread will unblock at 'tim'
 * (an absolute time) if it hasn't already unblocked.
 *
 * Returns the amount of time left from the original 'tim' value
 * when it was unblocked.
 */
int
spl_cv_timedwait(kcondvar_t *cvp, kmutex_t *mp, clock_t tim, int flags,
    const char *msg)
{
	int result;
	clock_t timenow;
	LARGE_INTEGER timeout;
	(void) cvp;	(void) flags;

	if (cvp->cv_initialised != CONDVAR_INIT)
		panic("%s: not cv_initialised", __func__);

	if (msg != NULL && msg[0] == '&')
		++msg;  /* skip over '&' prefixes */

	timenow = zfs_lbolt();

	// Check for events already in the past
	if (tim < timenow)
		tim = timenow;

	/*
	 * Pointer to a time-out value that specifies the absolute or
	 * relative time, in 100-nanosecond units, at which the wait is to
	 * be completed.  A positive value specifies an absolute time,
	 * relative to January 1, 1601. A negative value specifies an
	 * interval relative to the current time.
	 */
	timeout.QuadPart = -100000 * MAX(1, (tim - timenow) / hz);

#ifdef SPL_DEBUG_MUTEX
	spl_wdlist_settime(mp->leak, 0);
#endif

	atomic_inc_32(&cvp->cv_waiters_count);
	mutex_exit(mp);

	void *locks[CV_MAX_EVENTS] =
		{ &cvp->cv_kevent[CV_SIGNAL], &cvp->cv_kevent[CV_BROADCAST] };

	result = KeWaitForMultipleObjects(CV_MAX_EVENTS, locks, WaitAny,
	    Executive, KernelMode, FALSE, &timeout, NULL);

	atomic_dec_32(&cvp->cv_waiters_count);

	mutex_enter(mp);

	/*
	 * Clear the broadcast event only after reacquiring the mutex so that
	 * a concurrent cv_broadcast() (which also runs under that mutex) cannot
	 * race with KeClearEvent and have its KeSetEvent silently undone.
	 */
	if (result == STATUS_WAIT_0 + CV_BROADCAST &&
	    cvp->cv_waiters_count == 0)
		KeClearEvent(&cvp->cv_kevent[CV_BROADCAST]);

#ifdef SPL_DEBUG_MUTEX
	spl_wdlist_settime(mp->leak, gethrestime_sec());
#endif

	switch (result) {

		case STATUS_ALERTED: /* Signal */
		case ERESTART:
			return (0);

		case STATUS_TIMEOUT: /* Timeout */
			return (-1);
	}

	return (1);
}


/*
 * Compatibility wrapper for the cv_timedwait_hires() Illumos interface.
 */
int
cv_timedwait_hires(kcondvar_t *cvp, kmutex_t *mp, hrtime_t tim,
    hrtime_t res, int flag)
{
	int result;
	LARGE_INTEGER timeout;

	if (cvp->cv_initialised != CONDVAR_INIT)
		panic("%s: not cv_initialised", __func__);
	ASSERT(cvp->cv_initialised == CONDVAR_INIT);

	if (res > 1) {
		/*
		 * Align expiration to the specified resolution.
		 */
		if (flag & CALLOUT_FLAG_ROUNDUP)
			tim += res - 1;
		tim = (tim / res) * res;
	}

	if (flag & CALLOUT_FLAG_ABSOLUTE) {
		// 'tim' here is absolute UNIX time (from gethrtime()) so
		// convert it to absolute Windows time
		hrtime_t now = gethrtime();

		tim -= now; // Remove the ticks, what remains is "sleep" amount.
	}
	timeout.QuadPart = -tim / 100;

#ifdef SPL_DEBUG_MUTEX
	spl_wdlist_settime(mp->leak, 0);
#endif

	atomic_inc_32(&cvp->cv_waiters_count);
	mutex_exit(mp);

	void *locks[CV_MAX_EVENTS] =
	    { &cvp->cv_kevent[CV_SIGNAL], &cvp->cv_kevent[CV_BROADCAST] };

	result = KeWaitForMultipleObjects(CV_MAX_EVENTS, locks, WaitAny,
	    Executive, KernelMode, FALSE, &timeout, NULL);

	atomic_dec_32(&cvp->cv_waiters_count);

	mutex_enter(mp);

	/*
	 * Clear the broadcast event only after reacquiring the mutex so that
	 * a concurrent cv_broadcast() (which also runs under that mutex) cannot
	 * race with KeClearEvent and have its KeSetEvent silently undone.
	 */
	if (result == STATUS_WAIT_0 + CV_BROADCAST &&
	    cvp->cv_waiters_count == 0)
		KeClearEvent(&cvp->cv_kevent[CV_BROADCAST]);

#ifdef SPL_DEBUG_MUTEX
	spl_wdlist_settime(mp->leak, gethrestime_sec());
#endif

	switch (result) {

		case STATUS_ALERTED: /* Signal */
		case ERESTART:
			return (0);

		case STATUS_TIMEOUT: /* Timeout */
			return (-1);
	}

	return (1);
}
