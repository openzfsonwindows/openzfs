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
 * Copyright (C) 2018 Jorgen Lundman <lundman@lundman.net>
 *
 */

#include <sys/rwlock.h>
#include <sys/atomic.h>
#include <sys/mutex.h>

uint64_t zfs_active_rwlock = 0;

/*
 * We run rwlock with DEBUG on for now, as it protects against
 * uninitialised access etc, and almost no cost.
 */
#ifndef DEBUG
#define	DEBUG
#endif

#ifdef DEBUG
int
rw_isinit(krwlock_t *rwlp)
{
	if (rwlp->rw_pad != 0x012345678)
		return (0);
	return (1);
}
#endif

static __inline void
rw_wake_waiters_locked(krwlock_t *rwlp)
{
	/*
	 * Writer-prefer policy:
	 * - If a writer can run (no active writer, no readers), wake ONE writer
	 * - Else, if no writers are waiting and no active writer, allow readers
	 */
	if (rwlp->rw_owner == NULL) {
		if (rwlp->writers_waiting > 0) {
			if (rwlp->readers == 0) {
				/* keep readers blocked while writers wait */
				KeClearEvent(&rwlp->read_event);
				KeReleaseSemaphore(&rwlp->write_sem, 0, 1,
				    FALSE);
			} else {
				KeClearEvent(&rwlp->read_event);
			}
		} else {
			/* no writers waiting, let readers flow */
			KeSetEvent(&rwlp->read_event, IO_NO_INCREMENT, FALSE);
		}
	} else {
		KeClearEvent(&rwlp->read_event);
	}
}


void
rw_init(krwlock_t *rwlp, char *name, krw_type_t type, __unused void *arg)
{
	ASSERT(type != RW_DRIVER);

#ifdef DEBUG
	VERIFY3U(rwlp->rw_pad, !=, 0x012345678);
#endif
	KeInitializeSpinLock(&rwlp->spin);

	/* Readers allowed initially */
	KeInitializeEvent(&rwlp->read_event, NotificationEvent, TRUE);

	/* No writers runnable initially */
	KeInitializeSemaphore(&rwlp->write_sem, 0, MAXLONG);

	rwlp->readers = 0;
	rwlp->writers_waiting = 0;
	rwlp->rw_owner = NULL;

#ifdef DEBUG
	rwlp->rw_pad = 0x012345678;
#endif

	atomic_inc_64(&zfs_active_rwlock);
}

void
rw_destroy(krwlock_t *rwlp)
{
	// Confirm it was initialised, and is unlocked,
	// and not already destroyed.
#ifdef DEBUG
	VERIFY3U(rwlp->rw_pad, ==, 0x012345678);
#endif
	VERIFY3U(rwlp->rw_owner, ==, NULL);
	VERIFY3U(rwlp->readers, ==, 0);
	VERIFY3U(rwlp->writers_waiting, ==, 0);

#ifdef DEBUG
	rwlp->rw_pad = 0x99;
#endif
	atomic_dec_64(&zfs_active_rwlock);
}


void
rw_enter(krwlock_t *rwlp, krw_t rw)
{
#ifdef DEBUG
	if (rwlp->rw_pad != 0x012345678)
		panic("rwlock %p not initialised\n", rwlp);
#endif

	if (rw == RW_READER) {
		for (;;) {
			KIRQL oldIrql;
			KeAcquireSpinLock(&rwlp->spin, &oldIrql);

			/*
			 * Writer preference:
			 * - readers may enter only if no active writer AND
			 * no writers waiting.
			 */
			if (rwlp->rw_owner == NULL &&
			    rwlp->writers_waiting == 0) {
				rwlp->readers++;
				KeReleaseSpinLock(&rwlp->spin, oldIrql);
				return;
			}

			/*
			 * Readers are blocked either because a writer
			 * is active, or because there are waiting writers.
			 */
			KeClearEvent(&rwlp->read_event);
			KeReleaseSpinLock(&rwlp->spin, oldIrql);

			KeWaitForSingleObject(&rwlp->read_event, Executive,
			    KernelMode, FALSE, NULL);
		}
	}

	/* RW_WRITER */
	if (rwlp->rw_owner == current_thread())
		panic("rw_enter: locking against myself!");

	/*
	 * Make sure we count ourselves as a waiting writer only once, not on
	 * every loop iteration.
	 */
	BOOLEAN counted_waiter = FALSE;

	for (;;) {
		KIRQL oldIrql;
		KeAcquireSpinLock(&rwlp->spin, &oldIrql);

		if (!counted_waiter) {
			rwlp->writers_waiting++;
			counted_waiter = TRUE;
		}

		if (rwlp->rw_owner == NULL && rwlp->readers == 0) {
			/*
			 * We are no longer waiting; we are becoming the
			 * active writer.
			 */
			rwlp->writers_waiting--;
			counted_waiter = FALSE;

			rwlp->rw_owner = current_thread();

			/* Block readers while writer is active */
			KeClearEvent(&rwlp->read_event);

			KeReleaseSpinLock(&rwlp->spin, oldIrql);
			return;
		}

		/* Block new readers while writers are waiting */
		KeClearEvent(&rwlp->read_event);

		KeReleaseSpinLock(&rwlp->spin, oldIrql);

		KeWaitForSingleObject(&rwlp->write_sem, Executive,
		    KernelMode, FALSE, NULL);

		/* Loop and re-check under spin */
	}
}

int
rw_tryenter(krwlock_t *rwlp, krw_t rw)
{
#ifdef DEBUG
	if (rwlp->rw_pad != 0x012345678)
		panic("rwlock %p not initialised\n", rwlp);
#endif

	if (rw == RW_READER) {
		KIRQL oldIrql;
		KeAcquireSpinLock(&rwlp->spin, &oldIrql);

		if (rwlp->rw_owner == NULL && rwlp->writers_waiting == 0) {
			rwlp->readers++;
			KeReleaseSpinLock(&rwlp->spin, oldIrql);
			return (1);
		}

		KeReleaseSpinLock(&rwlp->spin, oldIrql);
		return (0);
	}

	/* RW_WRITER */
	if (rwlp->rw_owner == current_thread())
		panic("rw_tryenter: locking against myself!");

	KIRQL oldIrql;
	KeAcquireSpinLock(&rwlp->spin, &oldIrql);

	if (rwlp->rw_owner == NULL && rwlp->readers == 0) {
		rwlp->rw_owner = current_thread();

		/* Block readers while writer is active */
		KeClearEvent(&rwlp->read_event);

		KeReleaseSpinLock(&rwlp->spin, oldIrql);
		return (1);
	}

	KeReleaseSpinLock(&rwlp->spin, oldIrql);
	return (0);
}

void
rw_exit(krwlock_t *rwlp)
{
#ifdef DEBUG
	if (rwlp->rw_pad != 0x012345678)
		panic("rwlock %p not initialised\n", rwlp);
#endif

	KIRQL oldIrql;

	KeAcquireSpinLock(&rwlp->spin, &oldIrql);

	if (rwlp->rw_owner == current_thread()) {
		/* Writer exit */
		rwlp->rw_owner = NULL;
	} else {
		/* Reader exit */
		ASSERT(rwlp->rw_owner == NULL);
		ASSERT(rwlp->readers > 0);
		rwlp->readers--;
	}

	/*
	 * Wake policy (writer preference):
	 * - If a writer is waiting, only wake one writer once readers
	 * drop to 0.
	 * - Otherwise allow readers.
	 */
	if (rwlp->rw_owner == NULL) {
		if (rwlp->writers_waiting > 0) {
			KeClearEvent(&rwlp->read_event);

			if (rwlp->readers == 0) {
				KeReleaseSemaphore(&rwlp->write_sem,
				    0, 1, FALSE);
			}
		} else {
			/* No writers waiting: let readers flow */
			KeSetEvent(&rwlp->read_event, IO_NO_INCREMENT, FALSE);
		}
	} else {
		KeClearEvent(&rwlp->read_event);
	}

	KeReleaseSpinLock(&rwlp->spin, oldIrql);
}

/*
 * Illumos semantics: try to upgrade while holding read.
 * On failure, return 0 and *still hold read*.
 *
 * This implementation is atomic: it only succeeds if the caller is the
 * sole reader and no writer is active. It does not drop the read lock to
 * "race" for write.
 */
int
rw_tryupgrade(krwlock_t *rwlp)
{
#ifdef DEBUG
	if (rwlp->rw_pad != 0x012345678)
		panic("rwlock %p not initialised\n", rwlp);
#endif

	if (rwlp->rw_owner == current_thread())
		panic("rw_tryupgrade: already writer");

	KIRQL oldIrql;
	KeAcquireSpinLock(&rwlp->spin, &oldIrql);

	/*
	 * Must be exactly one reader (us) and no active writer.
	 * Note: writers_waiting may be > 0; in that case we still do not
	 * allow an upgrade unless we are the last reader and no writer is
	 * active. This preserves writer preference for NEW readers, but
	 * allows the current reader to upgrade safely when it is alone.
	 */
	if (rwlp->rw_owner == NULL && rwlp->readers == 1) {
		rwlp->readers = 0;
		rwlp->rw_owner = current_thread();

		/* Block readers while writer is active */
		KeClearEvent(&rwlp->read_event);

		KeReleaseSpinLock(&rwlp->spin, oldIrql);
		return (1);
	}

	/* Failure: keep the read lock held */
	KeReleaseSpinLock(&rwlp->spin, oldIrql);
	return (0);
}

int
rw_read_held(krwlock_t *rwlp)
{
	return (rw_lock_held(rwlp) && rwlp->rw_owner == NULL);
}

int
rw_lock_held(krwlock_t *rwlp)
{
	return (rwlp->rw_owner == current_thread() || rwlp->readers > 0);
}

int
rw_write_held(krwlock_t *rwlp)
{
	return (rwlp->rw_owner == current_thread());
}

/*
 * Downgrade from writer to reader.
 * On return, caller holds read lock.
 */
void
rw_downgrade(krwlock_t *rwlp)
{
#ifdef DEBUG
	if (rwlp->rw_pad != 0x012345678)
		panic("rwlock %p not initialised\n", rwlp);
#endif

	KIRQL oldIrql;
	KeAcquireSpinLock(&rwlp->spin, &oldIrql);

	if (rwlp->rw_owner != current_thread())
		panic("SPL: rw_downgrade not WRITE lock held\n");

	/*
	 * Convert writer -> one reader (this thread).
	 */
	rwlp->rw_owner = NULL;
	rwlp->readers = 1;

	/*
	 * Writer preference:
	 * - If writers are waiting, keep read_event cleared so *new* readers
	 *   will block. Current thread still holds read, so it's fine.
	 * - If no writers are waiting, allow readers to flow.
	 */
	if (rwlp->writers_waiting > 0) {
		KeClearEvent(&rwlp->read_event);
	} else {
		KeSetEvent(&rwlp->read_event, IO_NO_INCREMENT, FALSE);
	}

	KeReleaseSpinLock(&rwlp->spin, oldIrql);
}

int
spl_rwlock_init(void)
{
	return (0);
}

void
spl_rwlock_fini(void)
{
	ASSERT(zfs_active_rwlock == 0);
}
