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
 * Copyright (c) 2014 by Delphix. All rights reserved.
 */

#include <thread.h>
#include <synch.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/debug.h>
#include <sys/sysmacros.h>

#include "taskq.h"

/* Maximum percentage allowed for TASKQ_THREADS_CPU_PCT */
static int taskq_cpupct_max_percent = 1000;

int taskq_now;
taskq_t *system_taskq;

#define	TASKQ_ACTIVE	0x00010000
#define	TASKQ_NAMELEN	31

struct taskq {
	char		tq_name[TASKQ_NAMELEN + 1];
	mutex_t		tq_lock;
	rwlock_t	tq_threadlock;
	cond_t		tq_dispatch_cv;
	cond_t		tq_wait_cv;
	thread_t	*tq_threadlist;
	int		tq_flags;
	int		tq_active;
	int		tq_nthreads;
	int		tq_nalloc;
	int		tq_minalloc;
	int		tq_maxalloc;
	cond_t		tq_maxalloc_cv;
	int		tq_maxalloc_wait;
	taskq_ent_t	*tq_freelist;
	taskq_ent_t	tq_task;
};

static taskq_ent_t *
task_alloc(taskq_t *tq, int tqflags)
{
	taskq_ent_t *t;
	timestruc_t ts;
	int err;

again:	if ((t = tq->tq_freelist) != NULL && tq->tq_nalloc >= tq->tq_minalloc) {
		tq->tq_freelist = t->tqent_next;
	} else {
		if (tq->tq_nalloc >= tq->tq_maxalloc) {
			if (!(tqflags & UMEM_NOFAIL))
				return (NULL);

			/*
			 * We don't want to exceed tq_maxalloc, but we can't
			 * wait for other tasks to complete (and thus free up
			 * task structures) without risking deadlock with
			 * the caller.  So, we just delay for one second
			 * to throttle the allocation rate. If we have tasks
			 * complete before one second timeout expires then
			 * taskq_ent_free will signal us and we will
			 * immediately retry the allocation.
			 */
			tq->tq_maxalloc_wait++;

			ts.tv_sec = 1;
			ts.tv_nsec = 0;
			err = cond_reltimedwait(&tq->tq_maxalloc_cv,
			    &tq->tq_lock, &ts);

			tq->tq_maxalloc_wait--;
			if (err == 0)
				goto again;		/* signaled */
		}
		VERIFY0(mutex_unlock(&tq->tq_lock));

		t = umem_alloc(sizeof (taskq_ent_t), tqflags);

		VERIFY0(mutex_lock(&tq->tq_lock));
		if (t != NULL)
			tq->tq_nalloc++;
	}
	return (t);
}

static void
task_free(taskq_t *tq, taskq_ent_t *t)
{
	if (tq->tq_nalloc <= tq->tq_minalloc) {
		t->tqent_next = tq->tq_freelist;
		tq->tq_freelist = t;
	} else {
		tq->tq_nalloc--;
		VERIFY0(mutex_unlock(&tq->tq_lock));
		umem_free(t, sizeof (taskq_ent_t));
		VERIFY0(mutex_lock(&tq->tq_lock));
	}

	if (tq->tq_maxalloc_wait)
		VERIFY0(cond_signal(&tq->tq_maxalloc_cv));
}

taskqid_t
taskq_dispatch(taskq_t *tq, task_func_t func, void *arg, uint_t tqflags)
{
	taskq_ent_t *t;

	if (taskq_now) {
		func(arg);
		return (1);
	}

	VERIFY0(mutex_lock(&tq->tq_lock));
	ASSERT(tq->tq_flags & TASKQ_ACTIVE);
	if ((t = task_alloc(tq, tqflags)) == NULL) {
		VERIFY0(mutex_unlock(&tq->tq_lock));
		return (0);
	}
	if (tqflags & TQ_FRONT) {
		t->tqent_next = tq->tq_task.tqent_next;
		t->tqent_prev = &tq->tq_task;
	} else {
		t->tqent_next = &tq->tq_task;
		t->tqent_prev = tq->tq_task.tqent_prev;
	}
	t->tqent_next->tqent_prev = t;
	t->tqent_prev->tqent_next = t;
	t->tqent_func = func;
	t->tqent_arg = arg;
	t->tqent_flags = 0;
	VERIFY0(cond_signal(&tq->tq_dispatch_cv));
	VERIFY0(mutex_unlock(&tq->tq_lock));
	return (1);
}

void
taskq_dispatch_ent(taskq_t *tq, task_func_t func, void *arg, uint_t flags,
    taskq_ent_t *t)
{
	ASSERT(func != NULL);
	ASSERT(!(tq->tq_flags & TASKQ_DYNAMIC));

	/*
	 * Mark it as a prealloc'd task.  This is important
	 * to ensure that we don't free it later.
	 */
	t->tqent_flags |= TQENT_FLAG_PREALLOC;
	/*
	 * Enqueue the task to the underlying queue.
	 */
	VERIFY0(mutex_lock(&tq->tq_lock));

	if (flags & TQ_FRONT) {
		t->tqent_next = tq->tq_task.tqent_next;
		t->tqent_prev = &tq->tq_task;
	} else {
		t->tqent_next = &tq->tq_task;
		t->tqent_prev = tq->tq_task.tqent_prev;
	}
	t->tqent_next->tqent_prev = t;
	t->tqent_prev->tqent_next = t;
	t->tqent_func = func;
	t->tqent_arg = arg;
	VERIFY0(cond_signal(&tq->tq_dispatch_cv));
	VERIFY0(mutex_unlock(&tq->tq_lock));
}

void
taskq_wait(taskq_t *tq)
{
	VERIFY0(mutex_lock(&tq->tq_lock));
	while (tq->tq_task.tqent_next != &tq->tq_task || tq->tq_active != 0) {
		int ret = cond_wait(&tq->tq_wait_cv, &tq->tq_lock);
		VERIFY(ret == 0 || ret == EINTR);
	}
	VERIFY0(mutex_unlock(&tq->tq_lock));
}

static void *
taskq_thread(void *arg)
{
	taskq_t *tq = arg;
	taskq_ent_t *t;
	boolean_t prealloc;

	VERIFY0(mutex_lock(&tq->tq_lock));
	while (tq->tq_flags & TASKQ_ACTIVE) {
		if ((t = tq->tq_task.tqent_next) == &tq->tq_task) {
			int ret;
			if (--tq->tq_active == 0)
				VERIFY0(cond_broadcast(&tq->tq_wait_cv));
			ret = cond_wait(&tq->tq_dispatch_cv, &tq->tq_lock);
			VERIFY(ret == 0 || ret == EINTR);
			tq->tq_active++;
			continue;
		}
		t->tqent_prev->tqent_next = t->tqent_next;
		t->tqent_next->tqent_prev = t->tqent_prev;
		t->tqent_next = NULL;
		t->tqent_prev = NULL;
		prealloc = t->tqent_flags & TQENT_FLAG_PREALLOC;
		VERIFY0(mutex_unlock(&tq->tq_lock));

		VERIFY0(rw_rdlock(&tq->tq_threadlock));
		t->tqent_func(t->tqent_arg);
		VERIFY0(rw_unlock(&tq->tq_threadlock));

		VERIFY0(mutex_lock(&tq->tq_lock));
		if (!prealloc)
			task_free(tq, t);
	}
	tq->tq_nthreads--;
	VERIFY0(cond_broadcast(&tq->tq_wait_cv));
	VERIFY0(mutex_unlock(&tq->tq_lock));
	return (NULL);
}

/*ARGSUSED*/
taskq_t *
taskq_create(const char *name, int nthreads, pri_t pri,
	int minalloc, int maxalloc, uint_t flags)
{
	taskq_t *tq = umem_zalloc(sizeof (taskq_t), UMEM_NOFAIL);
	int t;

	if (flags & TASKQ_THREADS_CPU_PCT) {
		int pct;
		ASSERT3S(nthreads, >=, 0);
		ASSERT3S(nthreads, <=, taskq_cpupct_max_percent);
		pct = MIN(nthreads, taskq_cpupct_max_percent);
		pct = MAX(pct, 0);

		nthreads = (sysconf(_SC_NPROCESSORS_ONLN) * pct) / 100;
		nthreads = MAX(nthreads, 1);	/* need at least 1 thread */
	} else {
		ASSERT3S(nthreads, >=, 1);
	}

	VERIFY0(rwlock_init(&tq->tq_threadlock, USYNC_THREAD, NULL));
	VERIFY0(mutex_init(&tq->tq_lock, USYNC_THREAD, NULL));
	VERIFY0(cond_init(&tq->tq_dispatch_cv, USYNC_THREAD, NULL));
	VERIFY0(cond_init(&tq->tq_wait_cv, USYNC_THREAD, NULL));
	VERIFY0(cond_init(&tq->tq_maxalloc_cv, USYNC_THREAD, NULL));
	(void) strncpy(tq->tq_name, name, TASKQ_NAMELEN + 1);
	tq->tq_flags = flags | TASKQ_ACTIVE;
	tq->tq_active = nthreads;
	tq->tq_nthreads = nthreads;
	tq->tq_minalloc = minalloc;
	tq->tq_maxalloc = maxalloc;
	tq->tq_task.tqent_next = &tq->tq_task;
	tq->tq_task.tqent_prev = &tq->tq_task;
	tq->tq_threadlist =
	    umem_alloc(nthreads * sizeof (thread_t), UMEM_NOFAIL);

	if (flags & TASKQ_PREPOPULATE) {
		VERIFY0(mutex_lock(&tq->tq_lock));
		while (minalloc-- > 0)
			task_free(tq, task_alloc(tq, UMEM_NOFAIL));
		VERIFY0(mutex_unlock(&tq->tq_lock));
	}

	for (t = 0; t < nthreads; t++)
		(void) thr_create(0, 0, taskq_thread,
		    tq, THR_BOUND, &tq->tq_threadlist[t]);

	return (tq);
}

void
taskq_destroy(taskq_t *tq)
{
	int t;
	int nthreads = tq->tq_nthreads;

	taskq_wait(tq);

	VERIFY0(mutex_lock(&tq->tq_lock));

	tq->tq_flags &= ~TASKQ_ACTIVE;
	VERIFY0(cond_broadcast(&tq->tq_dispatch_cv));

	while (tq->tq_nthreads != 0) {
		int ret = cond_wait(&tq->tq_wait_cv, &tq->tq_lock);
		VERIFY(ret == 0 || ret == EINTR);
	}

	tq->tq_minalloc = 0;
	while (tq->tq_nalloc != 0) {
		ASSERT(tq->tq_freelist != NULL);
		task_free(tq, task_alloc(tq, UMEM_NOFAIL));
	}

	VERIFY0(mutex_unlock(&tq->tq_lock));

	for (t = 0; t < nthreads; t++)
		(void) thr_join(tq->tq_threadlist[t], NULL, NULL);

	umem_free(tq->tq_threadlist, nthreads * sizeof (thread_t));

	VERIFY0(rwlock_destroy(&tq->tq_threadlock));
	VERIFY0(mutex_destroy(&tq->tq_lock));
	VERIFY0(cond_destroy(&tq->tq_dispatch_cv));
	VERIFY0(cond_destroy(&tq->tq_wait_cv));
	VERIFY0(cond_destroy(&tq->tq_maxalloc_cv));

	umem_free(tq, sizeof (taskq_t));
}

int
taskq_member(taskq_t *tq, void *t)
{
	int i;

	if (taskq_now)
		return (1);

	for (i = 0; i < tq->tq_nthreads; i++)
		if (tq->tq_threadlist[i] == (thread_t)(uintptr_t)t)
			return (1);

	return (0);
}

void
system_taskq_init(void)
{
	system_taskq = taskq_create("system_taskq", 64, 60, 4, 512,
	    TASKQ_DYNAMIC | TASKQ_PREPOPULATE);
}

void
system_taskq_fini(void)
{
	taskq_destroy(system_taskq);
	system_taskq = NULL; /* defensive */
}
