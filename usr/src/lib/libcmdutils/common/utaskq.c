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
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 * Copyright 2012 Garrett D'Amore <garrett@damore.org>.  All rights reserved.
 * Copyright (c) 2014, 2017 by Delphix. All rights reserved.
 */

#include <thread.h>
#include <synch.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/debug.h>
#include <sys/sysmacros.h>

#include "utaskq.h"

/* Maximum percentage allowed for UTASKQ_THREADS_CPU_PCT */
static int utaskq_cpupct_max_percent = 1000;

int utaskq_now;
utaskq_t *system_utaskq;

#define	UTASKQ_ACTIVE	0x00010000
#define	UTASKQ_NAMELEN	31

struct utaskq {
	char		utq_name[UTASKQ_NAMELEN + 1];
	mutex_t		utq_lock;
	rwlock_t	utq_threadlock;
	cond_t		utq_dispatch_cv;
	cond_t		utq_wait_cv;
	thread_t	*utq_threadlist;
	int		utq_flags;
	int		utq_active;
	int		utq_nthreads;
	int		utq_nalloc;
	int		utq_minalloc;
	int		utq_maxalloc;
	cond_t		utq_maxalloc_cv;
	int		utq_maxalloc_wait;
	utaskq_ent_t	*utq_freelist;
	utaskq_ent_t	utq_task;
};

static utaskq_ent_t *
utask_alloc(utaskq_t *utq, int utqflags)
{
	utaskq_ent_t *t;
	timestruc_t ts;
	int err;

again:	if ((t = utq->utq_freelist) != NULL &&
	    utq->utq_nalloc >= utq->utq_minalloc) {
		utq->utq_freelist = t->utqent_next;
	} else {
		if (utq->utq_nalloc >= utq->utq_maxalloc) {
			if (!(utqflags & UMEM_NOFAIL))
				return (NULL);

			/*
			 * We don't want to exceed utq_maxalloc, but we can't
			 * wait for other tasks to complete (and thus free up
			 * task structures) without risking deadlock with
			 * the caller.  So, we just delay for one second
			 * to throttle the allocation rate. If we have tasks
			 * complete before one second timeout expires then
			 * utaskq_ent_free will signal us and we will
			 * immediately retry the allocation.
			 */
			utq->utq_maxalloc_wait++;

			ts.tv_sec = 1;
			ts.tv_nsec = 0;
			err = cond_reltimedwait(&utq->utq_maxalloc_cv,
			    &utq->utq_lock, &ts);

			utq->utq_maxalloc_wait--;
			if (err == 0)
				goto again;		/* signaled */
		}
		VERIFY0(mutex_unlock(&utq->utq_lock));

		t = umem_alloc(sizeof (utaskq_ent_t), utqflags);

		VERIFY0(mutex_lock(&utq->utq_lock));
		if (t != NULL)
			utq->utq_nalloc++;
	}
	return (t);
}

static void
utask_free(utaskq_t *utq, utaskq_ent_t *t)
{
	if (utq->utq_nalloc <= utq->utq_minalloc) {
		t->utqent_next = utq->utq_freelist;
		utq->utq_freelist = t;
	} else {
		utq->utq_nalloc--;
		VERIFY0(mutex_unlock(&utq->utq_lock));
		umem_free(t, sizeof (utaskq_ent_t));
		VERIFY0(mutex_lock(&utq->utq_lock));
	}

	if (utq->utq_maxalloc_wait)
		VERIFY0(cond_signal(&utq->utq_maxalloc_cv));
}

utaskqid_t
utaskq_dispatch(utaskq_t *utq, utask_func_t func, void *arg, uint_t utqflags)
{
	utaskq_ent_t *t;

	if (utaskq_now) {
		func(arg);
		return (1);
	}

	VERIFY0(mutex_lock(&utq->utq_lock));
	ASSERT(utq->utq_flags & UTASKQ_ACTIVE);
	if ((t = utask_alloc(utq, utqflags)) == NULL) {
		VERIFY0(mutex_unlock(&utq->utq_lock));
		return (0);
	}
	if (utqflags & UTQ_FRONT) {
		t->utqent_next = utq->utq_task.utqent_next;
		t->utqent_prev = &utq->utq_task;
	} else {
		t->utqent_next = &utq->utq_task;
		t->utqent_prev = utq->utq_task.utqent_prev;
	}
	t->utqent_next->utqent_prev = t;
	t->utqent_prev->utqent_next = t;
	t->utqent_func = func;
	t->utqent_arg = arg;
	t->utqent_flags = 0;
	VERIFY0(cond_signal(&utq->utq_dispatch_cv));
	VERIFY0(mutex_unlock(&utq->utq_lock));
	return (1);
}

void
utaskq_dispatch_ent(utaskq_t *utq, utask_func_t func, void *arg, uint_t flags,
    utaskq_ent_t *t)
{
	ASSERT(func != NULL);
	ASSERT(!(utq->utq_flags & UTASKQ_DYNAMIC));

	/*
	 * Mark it as a prealloc'd task.  This is important
	 * to ensure that we don't free it later.
	 */
	t->utqent_flags |= UTQENT_FLAG_PREALLOC;
	/*
	 * Enqueue the task to the underlying queue.
	 */
	VERIFY0(mutex_lock(&utq->utq_lock));

	if (flags & UTQ_FRONT) {
		t->utqent_next = utq->utq_task.utqent_next;
		t->utqent_prev = &utq->utq_task;
	} else {
		t->utqent_next = &utq->utq_task;
		t->utqent_prev = utq->utq_task.utqent_prev;
	}
	t->utqent_next->utqent_prev = t;
	t->utqent_prev->utqent_next = t;
	t->utqent_func = func;
	t->utqent_arg = arg;
	VERIFY0(cond_signal(&utq->utq_dispatch_cv));
	VERIFY0(mutex_unlock(&utq->utq_lock));
}

void
utaskq_wait(utaskq_t *utq)
{
	VERIFY0(mutex_lock(&utq->utq_lock));
	while (utq->utq_task.utqent_next != &utq->utq_task ||
	    utq->utq_active != 0) {
		int ret = cond_wait(&utq->utq_wait_cv, &utq->utq_lock);
		VERIFY(ret == 0 || ret == EINTR);
	}
	VERIFY0(mutex_unlock(&utq->utq_lock));
}

static void *
utaskq_thread(void *arg)
{
	utaskq_t *utq = arg;
	utaskq_ent_t *t;
	boolean_t prealloc;

	VERIFY0(mutex_lock(&utq->utq_lock));
	while (utq->utq_flags & UTASKQ_ACTIVE) {
		if ((t = utq->utq_task.utqent_next) == &utq->utq_task) {
			int ret;
			if (--utq->utq_active == 0)
				VERIFY0(cond_broadcast(&utq->utq_wait_cv));
			ret = cond_wait(&utq->utq_dispatch_cv, &utq->utq_lock);
			VERIFY(ret == 0 || ret == EINTR);
			utq->utq_active++;
			continue;
		}
		t->utqent_prev->utqent_next = t->utqent_next;
		t->utqent_next->utqent_prev = t->utqent_prev;
		t->utqent_next = NULL;
		t->utqent_prev = NULL;
		prealloc = t->utqent_flags & UTQENT_FLAG_PREALLOC;
		VERIFY0(mutex_unlock(&utq->utq_lock));

		VERIFY0(rw_rdlock(&utq->utq_threadlock));
		t->utqent_func(t->utqent_arg);
		VERIFY0(rw_unlock(&utq->utq_threadlock));

		VERIFY0(mutex_lock(&utq->utq_lock));
		if (!prealloc)
			utask_free(utq, t);
	}
	utq->utq_nthreads--;
	VERIFY0(cond_broadcast(&utq->utq_wait_cv));
	VERIFY0(mutex_unlock(&utq->utq_lock));
	return (NULL);
}

/*ARGSUSED*/
utaskq_t *
utaskq_create(const char *name, int nthreads, pri_t pri, int minalloc,
    int maxalloc, uint_t flags)
{
	utaskq_t *utq = umem_zalloc(sizeof (utaskq_t), UMEM_NOFAIL);
	int t;

	if (flags & UTASKQ_THREADS_CPU_PCT) {
		int pct;
		ASSERT3S(nthreads, >=, 0);
		ASSERT3S(nthreads, <=, utaskq_cpupct_max_percent);
		pct = MIN(nthreads, utaskq_cpupct_max_percent);
		pct = MAX(pct, 0);

		nthreads = (sysconf(_SC_NPROCESSORS_ONLN) * pct) / 100;
		nthreads = MAX(nthreads, 1);	/* need at least 1 thread */
	} else {
		ASSERT3S(nthreads, >=, 1);
	}

	VERIFY0(rwlock_init(&utq->utq_threadlock, USYNC_THREAD, NULL));
	VERIFY0(mutex_init(&utq->utq_lock, USYNC_THREAD, NULL));
	VERIFY0(cond_init(&utq->utq_dispatch_cv, USYNC_THREAD, NULL));
	VERIFY0(cond_init(&utq->utq_wait_cv, USYNC_THREAD, NULL));
	VERIFY0(cond_init(&utq->utq_maxalloc_cv, USYNC_THREAD, NULL));
	(void) strncpy(utq->utq_name, name, UTASKQ_NAMELEN + 1);
	utq->utq_flags = flags | UTASKQ_ACTIVE;
	utq->utq_active = nthreads;
	utq->utq_nthreads = nthreads;
	utq->utq_minalloc = minalloc;
	utq->utq_maxalloc = maxalloc;
	utq->utq_task.utqent_next = &utq->utq_task;
	utq->utq_task.utqent_prev = &utq->utq_task;
	utq->utq_threadlist =
	    umem_alloc(nthreads * sizeof (thread_t), UMEM_NOFAIL);

	if (flags & UTASKQ_PREPOPULATE) {
		VERIFY0(mutex_lock(&utq->utq_lock));
		while (minalloc-- > 0)
			utask_free(utq, utask_alloc(utq, UMEM_NOFAIL));
		VERIFY0(mutex_unlock(&utq->utq_lock));
	}

	for (t = 0; t < nthreads; t++) {
		(void) thr_create(0, 0, utaskq_thread,
		    utq, THR_BOUND, &utq->utq_threadlist[t]);
	}

	return (utq);
}

void
utaskq_destroy(utaskq_t *utq)
{
	int t;
	int nthreads = utq->utq_nthreads;

	utaskq_wait(utq);

	VERIFY0(mutex_lock(&utq->utq_lock));

	utq->utq_flags &= ~UTASKQ_ACTIVE;
	VERIFY0(cond_broadcast(&utq->utq_dispatch_cv));

	while (utq->utq_nthreads != 0) {
		int ret = cond_wait(&utq->utq_wait_cv, &utq->utq_lock);
		VERIFY(ret == 0 || ret == EINTR);
	}

	utq->utq_minalloc = 0;
	while (utq->utq_nalloc != 0) {
		ASSERT(utq->utq_freelist != NULL);
		utask_free(utq, utask_alloc(utq, UMEM_NOFAIL));
	}

	VERIFY0(mutex_unlock(&utq->utq_lock));

	for (t = 0; t < nthreads; t++)
		(void) thr_join(utq->utq_threadlist[t], NULL, NULL);

	umem_free(utq->utq_threadlist, nthreads * sizeof (thread_t));

	VERIFY0(rwlock_destroy(&utq->utq_threadlock));
	VERIFY0(mutex_destroy(&utq->utq_lock));
	VERIFY0(cond_destroy(&utq->utq_dispatch_cv));
	VERIFY0(cond_destroy(&utq->utq_wait_cv));
	VERIFY0(cond_destroy(&utq->utq_maxalloc_cv));

	umem_free(utq, sizeof (utaskq_t));
}

int
utaskq_member(utaskq_t *utq, void *t)
{
	int i;

	if (utaskq_now)
		return (1);

	for (i = 0; i < utq->utq_nthreads; i++)
		if (utq->utq_threadlist[i] == (thread_t)(uintptr_t)t)
			return (1);

	return (0);
}

void
system_utaskq_init(void)
{
	system_utaskq = utaskq_create("system_utaskq", 64, 60, 4, 512,
	    UTASKQ_DYNAMIC | UTASKQ_PREPOPULATE);
}

void
system_utaskq_fini(void)
{
	utaskq_destroy(system_utaskq);
	system_utaskq = NULL; /* defensive */
}
