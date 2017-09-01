/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright (c) 2017 by Delphix. All rights reserved.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <thread.h>
#include <synch.h>
#include <utaskq.h>
#include <errno.h>

/*
 * utaskq_test.c --
 *
 *      This file implements some simple tests to verify the behavior of the
 *      utaskq API implemented in the libcmdutils library.
 */

static boolean_t debug;

static void
test_debug(const char *format, ...)
{
	va_list args;

	if (!debug) {
		return;
	}

	(void) printf("DEBUG: ");

	va_start(args, format);
	(void) vprintf(format, args);
	va_end(args);
}


static void
test_start(const char *testName, const char *format, ...)
{
	va_list	args;

	(void) printf("TEST STARTING %s: ", testName);

	va_start(args, format);
	(void) vprintf(format, args);
	va_end(args);
	(void) fflush(stdout);
}

static void
test_failed(const char *testName, const char *format, ...)
{
	va_list	args;

	(void) printf("TEST FAILED %s: ", testName);

	va_start(args, format);
	(void) vprintf(format, args);
	va_end(args);

	(void) exit(-1);
}

static void
test_passed(const char *testName)
{
	(void) printf("TEST PASS: %s\n", testName);
	(void) fflush(stdout);
}

static mutex_t	accounting_lock;	/* exclusivity for below counters */
static uint_t	max_concurrency;	/* maximum concurrency achieved */
static uint_t	executed_tasks;		/* total number of tasks executed */

static void
utq_task(void *arg)
{
	int taskid = (int)arg;

	test_debug("executing task %d\n", taskid);

	(void) mutex_lock(&accounting_lock);
	max_concurrency++;
	(void) mutex_unlock(&accounting_lock);

	(void) sleep(1);

	(void) mutex_lock(&accounting_lock);
	max_concurrency--;
	executed_tasks++;
	(void) mutex_unlock(&accounting_lock);
}

/*
 * Basic utaskq sanity test that dispatches tasks on a utaskq. The test simply
 * verifies that all tasks execute with the proper amount of concurrency.
 */
static void
utaskq_sanity_test(void)
{
	const char *test_name = __func__;
	const int nthreads = 2;
	const int ntasks = 20;
	utaskq_t *utq;
	int i;

	test_start(test_name, "basic utaskq sanity test.\n");

	utq = utaskq_create("sanity test", nthreads, 0, nthreads, INT_MAX, 0);
	if (utq == NULL)
		test_failed(test_name, "utaskq_create() failed: %d", errno);

	for (i = 0; i < ntasks; i++) {
		test_debug("dispatching task %d\n", i);
		(void) utaskq_dispatch(utq, utq_task, (void *)i, UTQ_SLEEP);
	}

	utaskq_wait(utq);

	if (executed_tasks != ntasks) {
		test_failed(test_name, "Expected %d tasks to run, but %d "
		    "actually ran.", ntasks, executed_tasks);
	}

	if (max_concurrency > nthreads) {
		test_failed(test_name, "Expected maximum concurrency of %d, "
		    "but achieved %d.", nthreads, max_concurrency);
	}

	utaskq_destroy(utq);

	test_passed(test_name);
}

int
main(int argc, char * const argv[])
{
	int c;

	while ((c = getopt(argc, argv, "d")) != -1) {
		switch (c) {
		case 'd':
			debug = B_TRUE;
			test_debug("debugging on.\n");
			break;
		default:
			break;
		}
	}

	utaskq_sanity_test();
	exit(0);
}
