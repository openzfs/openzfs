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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 * Copyright (c) 2012, 2017 by Delphix. All rights reserved.
 * Copyright (c) 2012, Joyent, Inc. All rights reserved.
 */

#ifndef	_UTASKQ_H
#define	_UTASKQ_H

#include <stdint.h>
#include <umem.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct utaskq utaskq_t;
typedef uintptr_t utaskqid_t;
typedef void (utask_func_t)(void *);

typedef struct utaskq_ent {
	struct utaskq_ent	*utqent_next;
	struct utaskq_ent	*utqent_prev;
	utask_func_t		*utqent_func;
	void			*utqent_arg;
	uintptr_t		utqent_flags;
} utaskq_ent_t;

#define	UTQENT_FLAG_PREALLOC	0x1	/* taskq_dispatch_ent used */

#define	UTASKQ_PREPOPULATE	0x0001
#define	UTASKQ_CPR_SAFE		0x0002	/* Use CPR safe protocol */
#define	UTASKQ_DYNAMIC		0x0004	/* Use dynamic thread scheduling */
#define	UTASKQ_THREADS_CPU_PCT	0x0008	/* Scale # threads by # cpus */
#define	UTASKQ_DC_BATCH		0x0010	/* Mark threads as batch */

#define	UTQ_SLEEP	UMEM_NOFAIL	/* Can block for memory */
#define	UTQ_NOSLEEP	UMEM_DEFAULT	/* cannot block for memory; may fail */
#define	UTQ_NOQUEUE	0x02		/* Do not enqueue if can't dispatch */
#define	UTQ_FRONT	0x08		/* Queue in front */

extern utaskq_t *system_utaskq;

extern utaskq_t	*utaskq_create(const char *, int, pri_t, int, int, uint_t);
extern utaskqid_t utaskq_dispatch(utaskq_t *, utask_func_t, void *, uint_t);
extern void	utaskq_dispatch_ent(utaskq_t *, utask_func_t, void *, uint_t,
    utaskq_ent_t *);
extern void	utaskq_destroy(utaskq_t *);
extern void	utaskq_wait(utaskq_t *);
extern int	utaskq_member(utaskq_t *, void *);
extern void	system_utaskq_init(void);
extern void	system_utaskq_fini(void);

#ifdef	__cplusplus
}
#endif

#endif	/* _UTASKQ_H */
