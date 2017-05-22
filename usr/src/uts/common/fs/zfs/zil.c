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
 * Copyright (c) 2011, 2017 by Delphix. All rights reserved.
 * Copyright (c) 2014 Integros [integros.com]
 */

/* Portions Copyright 2010 Robert Milkowski */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/dmu.h>
#include <sys/zap.h>
#include <sys/arc.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/zil.h>
#include <sys/zil_impl.h>
#include <sys/dsl_dataset.h>
#include <sys/vdev_impl.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/abd.h>

/*
 * The ZFS Intent Log (ZIL) saves "transaction records" (itxs) of
 * system calls that change the file system with enough information to
 * be able to replay them after a system crash, power loss, or
 * equivalent failure mode. These are stored in memory until either:
 *
 *   1. they are committed to the pool by the DMU transaction group
 *      (txg), at which point they can be discarded; or
 *   2. they are committed to the on-disk ZIL for the dataset being
 *      modified (e.g. due to an fsync, O_DSYNC, or other synchronous
 *      requirement).
 *
 * In the event of a crash or power loss, the itxs contained by each
 * dataset's on-disk ZIL will be replayed when that dataset is first
 * instantianted (e.g. if the dataset is a normal fileystem, when it is
 * first mounted).
 *
 * As hinted at above, there is one ZIL per dataset (both the in-memory
 * representation, and the on-disk representation). The on-disk format
 * consists of 3 parts:
 *
 * 	- a single, per-dataset, ZIL header
 * 	- zero or more ZIL blocks
 * 	- zero or more ZIL records
 *
 * A ZIL record holds the information necessary to replay a single
 * system call transaction. A ZIL block can hold many ZIL records, and
 * the blocks are chained together, similarly to a singly linked list.
 *
 * Each ZIL block contains a block pointer (blkptr_t) to the next ZIL
 * block in the chain, and the ZIL header points to the first block in
 * the chain.
 *
 * Note, there is not a fixed place in the pool to hold these ZIL
 * blocks; they are dynamically allocated and freed as needed from the
 * blocks available on the pool.
 */

/*
 * Disable intent logging replay.  This global ZIL switch affects all pools.
 */
int zil_replay_disable = 0;

/*
 * Tunable parameter for debugging or performance analysis.  Setting
 * zfs_nocacheflush will cause corruption on power loss if a volatile
 * out-of-order write cache is enabled.
 */
boolean_t zfs_nocacheflush = B_FALSE;

static kmem_cache_t *zil_lwb_cache;
static kmem_cache_t *zil_zcw_cache;

static void zil_async_to_sync(zilog_t *zilog, uint64_t foid);

#define	LWB_EMPTY(lwb) ((BP_GET_LSIZE(&lwb->lwb_blk) - \
    sizeof (zil_chain_t)) == (lwb->lwb_sz - lwb->lwb_nused))

static int
zil_bp_compare(const void *x1, const void *x2)
{
	const dva_t *dva1 = &((zil_bp_node_t *)x1)->zn_dva;
	const dva_t *dva2 = &((zil_bp_node_t *)x2)->zn_dva;

	if (DVA_GET_VDEV(dva1) < DVA_GET_VDEV(dva2))
		return (-1);
	if (DVA_GET_VDEV(dva1) > DVA_GET_VDEV(dva2))
		return (1);

	if (DVA_GET_OFFSET(dva1) < DVA_GET_OFFSET(dva2))
		return (-1);
	if (DVA_GET_OFFSET(dva1) > DVA_GET_OFFSET(dva2))
		return (1);

	return (0);
}

static void
zil_bp_tree_init(zilog_t *zilog)
{
	avl_create(&zilog->zl_bp_tree, zil_bp_compare,
	    sizeof (zil_bp_node_t), offsetof(zil_bp_node_t, zn_node));
}

static void
zil_bp_tree_fini(zilog_t *zilog)
{
	avl_tree_t *t = &zilog->zl_bp_tree;
	zil_bp_node_t *zn;
	void *cookie = NULL;

	while ((zn = avl_destroy_nodes(t, &cookie)) != NULL)
		kmem_free(zn, sizeof (zil_bp_node_t));

	avl_destroy(t);
}

int
zil_bp_tree_add(zilog_t *zilog, const blkptr_t *bp)
{
	avl_tree_t *t = &zilog->zl_bp_tree;
	const dva_t *dva;
	zil_bp_node_t *zn;
	avl_index_t where;

	if (BP_IS_EMBEDDED(bp))
		return (0);

	dva = BP_IDENTITY(bp);

	if (avl_find(t, dva, &where) != NULL)
		return (SET_ERROR(EEXIST));

	zn = kmem_alloc(sizeof (zil_bp_node_t), KM_SLEEP);
	zn->zn_dva = *dva;
	avl_insert(t, zn, where);

	return (0);
}

static zil_header_t *
zil_header_in_syncing_context(zilog_t *zilog)
{
	return ((zil_header_t *)zilog->zl_header);
}

static void
zil_init_log_chain(zilog_t *zilog, blkptr_t *bp)
{
	zio_cksum_t *zc = &bp->blk_cksum;

	zc->zc_word[ZIL_ZC_GUID_0] = spa_get_random(-1ULL);
	zc->zc_word[ZIL_ZC_GUID_1] = spa_get_random(-1ULL);
	zc->zc_word[ZIL_ZC_OBJSET] = dmu_objset_id(zilog->zl_os);
	zc->zc_word[ZIL_ZC_SEQ] = 1ULL;
}

/*
 * Read a log block and make sure it's valid.
 */
static int
zil_read_log_block(zilog_t *zilog, const blkptr_t *bp, blkptr_t *nbp, void *dst,
    char **end)
{
	enum zio_flag zio_flags = ZIO_FLAG_CANFAIL;
	arc_flags_t aflags = ARC_FLAG_WAIT;
	arc_buf_t *abuf = NULL;
	zbookmark_phys_t zb;
	int error;

	if (zilog->zl_header->zh_claim_txg == 0)
		zio_flags |= ZIO_FLAG_SPECULATIVE | ZIO_FLAG_SCRUB;

	if (!(zilog->zl_header->zh_flags & ZIL_CLAIM_LR_SEQ_VALID))
		zio_flags |= ZIO_FLAG_SPECULATIVE;

	SET_BOOKMARK(&zb, bp->blk_cksum.zc_word[ZIL_ZC_OBJSET],
	    ZB_ZIL_OBJECT, ZB_ZIL_LEVEL, bp->blk_cksum.zc_word[ZIL_ZC_SEQ]);

	error = arc_read(NULL, zilog->zl_spa, bp, arc_getbuf_func, &abuf,
	    ZIO_PRIORITY_SYNC_READ, zio_flags, &aflags, &zb);

	if (error == 0) {
		zio_cksum_t cksum = bp->blk_cksum;

		/*
		 * Validate the checksummed log block.
		 *
		 * Sequence numbers should be... sequential.  The checksum
		 * verifier for the next block should be bp's checksum plus 1.
		 *
		 * Also check the log chain linkage and size used.
		 */
		cksum.zc_word[ZIL_ZC_SEQ]++;

		if (BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_ZILOG2) {
			zil_chain_t *zilc = abuf->b_data;
			char *lr = (char *)(zilc + 1);
			uint64_t len = zilc->zc_nused - sizeof (zil_chain_t);

			if (bcmp(&cksum, &zilc->zc_next_blk.blk_cksum,
			    sizeof (cksum)) || BP_IS_HOLE(&zilc->zc_next_blk)) {
				error = SET_ERROR(ECKSUM);
			} else {
				ASSERT3U(len, <=, SPA_OLD_MAXBLOCKSIZE);
				bcopy(lr, dst, len);
				*end = (char *)dst + len;
				*nbp = zilc->zc_next_blk;
			}
		} else {
			char *lr = abuf->b_data;
			uint64_t size = BP_GET_LSIZE(bp);
			zil_chain_t *zilc = (zil_chain_t *)(lr + size) - 1;

			if (bcmp(&cksum, &zilc->zc_next_blk.blk_cksum,
			    sizeof (cksum)) || BP_IS_HOLE(&zilc->zc_next_blk) ||
			    (zilc->zc_nused > (size - sizeof (*zilc)))) {
				error = SET_ERROR(ECKSUM);
			} else {
				ASSERT3U(zilc->zc_nused, <=,
				    SPA_OLD_MAXBLOCKSIZE);
				bcopy(lr, dst, zilc->zc_nused);
				*end = (char *)dst + zilc->zc_nused;
				*nbp = zilc->zc_next_blk;
			}
		}

		arc_buf_destroy(abuf, &abuf);
	}

	return (error);
}

/*
 * Read a TX_WRITE log data block.
 */
static int
zil_read_log_data(zilog_t *zilog, const lr_write_t *lr, void *wbuf)
{
	enum zio_flag zio_flags = ZIO_FLAG_CANFAIL;
	const blkptr_t *bp = &lr->lr_blkptr;
	arc_flags_t aflags = ARC_FLAG_WAIT;
	arc_buf_t *abuf = NULL;
	zbookmark_phys_t zb;
	int error;

	if (BP_IS_HOLE(bp)) {
		if (wbuf != NULL)
			bzero(wbuf, MAX(BP_GET_LSIZE(bp), lr->lr_length));
		return (0);
	}

	if (zilog->zl_header->zh_claim_txg == 0)
		zio_flags |= ZIO_FLAG_SPECULATIVE | ZIO_FLAG_SCRUB;

	SET_BOOKMARK(&zb, dmu_objset_id(zilog->zl_os), lr->lr_foid,
	    ZB_ZIL_LEVEL, lr->lr_offset / BP_GET_LSIZE(bp));

	error = arc_read(NULL, zilog->zl_spa, bp, arc_getbuf_func, &abuf,
	    ZIO_PRIORITY_SYNC_READ, zio_flags, &aflags, &zb);

	if (error == 0) {
		if (wbuf != NULL)
			bcopy(abuf->b_data, wbuf, arc_buf_size(abuf));
		arc_buf_destroy(abuf, &abuf);
	}

	return (error);
}

/*
 * Parse the intent log, and call parse_func for each valid record within.
 */
int
zil_parse(zilog_t *zilog, zil_parse_blk_func_t *parse_blk_func,
    zil_parse_lr_func_t *parse_lr_func, void *arg, uint64_t txg)
{
	const zil_header_t *zh = zilog->zl_header;
	boolean_t claimed = !!zh->zh_claim_txg;
	uint64_t claim_blk_seq = claimed ? zh->zh_claim_blk_seq : UINT64_MAX;
	uint64_t claim_lr_seq = claimed ? zh->zh_claim_lr_seq : UINT64_MAX;
	uint64_t max_blk_seq = 0;
	uint64_t max_lr_seq = 0;
	uint64_t blk_count = 0;
	uint64_t lr_count = 0;
	blkptr_t blk, next_blk;
	char *lrbuf, *lrp;
	int error = 0;

	/*
	 * Old logs didn't record the maximum zh_claim_lr_seq.
	 */
	if (!(zh->zh_flags & ZIL_CLAIM_LR_SEQ_VALID))
		claim_lr_seq = UINT64_MAX;

	/*
	 * Starting at the block pointed to by zh_log we read the log chain.
	 * For each block in the chain we strongly check that block to
	 * ensure its validity.  We stop when an invalid block is found.
	 * For each block pointer in the chain we call parse_blk_func().
	 * For each record in each valid block we call parse_lr_func().
	 * If the log has been claimed, stop if we encounter a sequence
	 * number greater than the highest claimed sequence number.
	 */
	lrbuf = zio_buf_alloc(SPA_OLD_MAXBLOCKSIZE);
	zil_bp_tree_init(zilog);

	for (blk = zh->zh_log; !BP_IS_HOLE(&blk); blk = next_blk) {
		uint64_t blk_seq = blk.blk_cksum.zc_word[ZIL_ZC_SEQ];
		int reclen;
		char *end;

		if (blk_seq > claim_blk_seq)
			break;
		if ((error = parse_blk_func(zilog, &blk, arg, txg)) != 0)
			break;
		ASSERT3U(max_blk_seq, <, blk_seq);
		max_blk_seq = blk_seq;
		blk_count++;

		if (max_lr_seq == claim_lr_seq && max_blk_seq == claim_blk_seq)
			break;

		error = zil_read_log_block(zilog, &blk, &next_blk, lrbuf, &end);
		if (error != 0)
			break;

		for (lrp = lrbuf; lrp < end; lrp += reclen) {
			lr_t *lr = (lr_t *)lrp;
			reclen = lr->lrc_reclen;
			ASSERT3U(reclen, >=, sizeof (lr_t));
			if (lr->lrc_seq > claim_lr_seq)
				goto done;
			if ((error = parse_lr_func(zilog, lr, arg, txg)) != 0)
				goto done;
			ASSERT3U(max_lr_seq, <, lr->lrc_seq);
			max_lr_seq = lr->lrc_seq;
			lr_count++;
		}
	}
done:
	zilog->zl_parse_error = error;
	zilog->zl_parse_blk_seq = max_blk_seq;
	zilog->zl_parse_lr_seq = max_lr_seq;
	zilog->zl_parse_blk_count = blk_count;
	zilog->zl_parse_lr_count = lr_count;

	ASSERT(!claimed || !(zh->zh_flags & ZIL_CLAIM_LR_SEQ_VALID) ||
	    (max_blk_seq == claim_blk_seq && max_lr_seq == claim_lr_seq));

	zil_bp_tree_fini(zilog);
	zio_buf_free(lrbuf, SPA_OLD_MAXBLOCKSIZE);

	return (error);
}

static int
zil_claim_log_block(zilog_t *zilog, blkptr_t *bp, void *tx, uint64_t first_txg)
{
	/*
	 * Claim log block if not already committed and not already claimed.
	 * If tx == NULL, just verify that the block is claimable.
	 */
	if (BP_IS_HOLE(bp) || bp->blk_birth < first_txg ||
	    zil_bp_tree_add(zilog, bp) != 0)
		return (0);

	return (zio_wait(zio_claim(NULL, zilog->zl_spa,
	    tx == NULL ? 0 : first_txg, bp, spa_claim_notify, NULL,
	    ZIO_FLAG_CANFAIL | ZIO_FLAG_SPECULATIVE | ZIO_FLAG_SCRUB)));
}

static int
zil_claim_log_record(zilog_t *zilog, lr_t *lrc, void *tx, uint64_t first_txg)
{
	lr_write_t *lr = (lr_write_t *)lrc;
	int error;

	if (lrc->lrc_txtype != TX_WRITE)
		return (0);

	/*
	 * If the block is not readable, don't claim it.  This can happen
	 * in normal operation when a log block is written to disk before
	 * some of the dmu_sync() blocks it points to.  In this case, the
	 * transaction cannot have been committed to anyone (we would have
	 * waited for all writes to be stable first), so it is semantically
	 * correct to declare this the end of the log.
	 */
	if (lr->lr_blkptr.blk_birth >= first_txg &&
	    (error = zil_read_log_data(zilog, lr, NULL)) != 0)
		return (error);
	return (zil_claim_log_block(zilog, &lr->lr_blkptr, tx, first_txg));
}

/* ARGSUSED */
static int
zil_free_log_block(zilog_t *zilog, blkptr_t *bp, void *tx, uint64_t claim_txg)
{
	zio_free_zil(zilog->zl_spa, dmu_tx_get_txg(tx), bp);

	return (0);
}

static int
zil_free_log_record(zilog_t *zilog, lr_t *lrc, void *tx, uint64_t claim_txg)
{
	lr_write_t *lr = (lr_write_t *)lrc;
	blkptr_t *bp = &lr->lr_blkptr;

	/*
	 * If we previously claimed it, we need to free it.
	 */
	if (claim_txg != 0 && lrc->lrc_txtype == TX_WRITE &&
	    bp->blk_birth >= claim_txg && zil_bp_tree_add(zilog, bp) == 0 &&
	    !BP_IS_HOLE(bp))
		zio_free(zilog->zl_spa, dmu_tx_get_txg(tx), bp);

	return (0);
}

static int
zil_lwb_vdev_compare(const void *x1, const void *x2)
{
	const uint64_t v1 = ((zil_vdev_node_t *)x1)->zv_vdev;
	const uint64_t v2 = ((zil_vdev_node_t *)x2)->zv_vdev;

	if (v1 < v2)
		return (-1);
	if (v1 > v2)
		return (1);

	return (0);
}

static lwb_t *
zil_alloc_lwb(zilog_t *zilog, blkptr_t *bp, uint64_t txg)
{
	lwb_t *lwb;

	lwb = kmem_cache_alloc(zil_lwb_cache, KM_SLEEP);
	lwb->lwb_zilog = zilog;
	lwb->lwb_blk = *bp;
	lwb->lwb_buf = zio_buf_alloc(BP_GET_LSIZE(bp));
	lwb->lwb_max_txg = txg;
	lwb->lwb_zio = NULL;
	lwb->lwb_tx = NULL;
	if (BP_GET_CHECKSUM(bp) == ZIO_CHECKSUM_ZILOG2) {
		lwb->lwb_nused = sizeof (zil_chain_t);
		lwb->lwb_sz = BP_GET_LSIZE(bp);
	} else {
		lwb->lwb_nused = 0;
		lwb->lwb_sz = BP_GET_LSIZE(bp) - sizeof (zil_chain_t);
	}

	list_create(&lwb->lwb_waiters, sizeof (zil_commit_waiter_t),
	    offsetof(zil_commit_waiter_t, zcw_node));

	avl_create(&lwb->lwb_vdev_tree, zil_lwb_vdev_compare,
	    sizeof (zil_vdev_node_t), offsetof(zil_vdev_node_t, zv_node));

	mutex_init(&lwb->lwb_vdev_lock, NULL, MUTEX_DEFAULT, NULL);

	mutex_enter(&zilog->zl_lock);
	list_insert_tail(&zilog->zl_lwb_list, lwb);
	mutex_exit(&zilog->zl_lock);

	return (lwb);
}

static void
zil_free_lwb(lwb_t *lwb)
{
	mutex_destroy(&lwb->lwb_vdev_lock);
	avl_destroy(&lwb->lwb_vdev_tree);

	ASSERT(list_is_empty(&lwb->lwb_waiters));
	list_destroy(&lwb->lwb_waiters);

	kmem_cache_free(zil_lwb_cache, lwb);
}

/*
 * Called when we create in-memory log transactions so that we know
 * to cleanup the itxs at the end of spa_sync().
 */
void
zilog_dirty(zilog_t *zilog, uint64_t txg)
{
	dsl_pool_t *dp = zilog->zl_dmu_pool;
	dsl_dataset_t *ds = dmu_objset_ds(zilog->zl_os);

	ASSERT(spa_writeable(zilog->zl_spa));

	if (ds->ds_is_snapshot)
		panic("dirtying snapshot!");

	if (txg_list_add(&dp->dp_dirty_zilogs, zilog, txg)) {
		/* up the hold count until we can be written out */
		dmu_buf_add_ref(ds->ds_dbuf, zilog);

		zilog->zl_dirty_max_txg = MAX(txg, zilog->zl_dirty_max_txg);
	}
}

/*
 * Determine if the zil is dirty in the specified txg. Callers wanting to
 * ensure that the dirty state does not change must hold the itxg_lock for
 * the specified txg. Holding the lock will ensure that the zil cannot be
 * dirtied (zil_itx_assign) or cleaned (zil_clean) while we check its current
 * state.
 */
boolean_t
zilog_is_dirty_in_txg(zilog_t *zilog, uint64_t txg)
{
	dsl_pool_t *dp = zilog->zl_dmu_pool;

	if (txg_list_member(&dp->dp_dirty_zilogs, zilog, txg & TXG_MASK))
		return (B_TRUE);
	return (B_FALSE);
}

/*
 * Determine if the zil is dirty. The zil is considered dirty if it has
 * any pending itx records that have not been cleaned by zil_clean().
 */
boolean_t
zilog_is_dirty(zilog_t *zilog)
{
	dsl_pool_t *dp = zilog->zl_dmu_pool;

	for (int t = 0; t < TXG_SIZE; t++) {
		if (txg_list_member(&dp->dp_dirty_zilogs, zilog, t))
			return (B_TRUE);
	}
	return (B_FALSE);
}

/*
 * Create an on-disk intent log.
 */
static lwb_t *
zil_alloc_first_lwb(zilog_t *zilog)
{
	const zil_header_t *zh = zilog->zl_header;
	uint64_t txg = 0;
	dmu_tx_t *tx = NULL;
	blkptr_t blk;

	/*
	 * Wait for any previous destroy to complete.
	 */
	txg_wait_synced(zilog->zl_dmu_pool, zilog->zl_destroy_txg);

	ASSERT(zh->zh_claim_txg == 0);
	ASSERT(zh->zh_replay_seq == 0);

	blk = zh->zh_log;

	/*
	 * Allocate an initial log block if:
	 *    - there isn't one already
	 *    - the existing block is the wrong endianess
	 */
	if (BP_IS_HOLE(&blk) || BP_SHOULD_BYTESWAP(&blk)) {
		tx = dmu_tx_create(zilog->zl_os);
		VERIFY0(dmu_tx_assign(tx, TXG_WAIT));
		dsl_dataset_dirty(dmu_objset_ds(zilog->zl_os), tx);
		txg = dmu_tx_get_txg(tx);

		if (!BP_IS_HOLE(&blk)) {
			zio_free_zil(zilog->zl_spa, txg, &blk);
			BP_ZERO(&blk);
		}

		VERIFY0(zio_alloc_zil(zilog->zl_spa, txg, &blk, NULL,
		    ZIL_MIN_BLKSZ, zilog->zl_logbias == ZFS_LOGBIAS_LATENCY));
		zil_init_log_chain(zilog, &blk);
	}

	/*
	 * Allocate a log write block (lwb) for the first log block.
	 */
	lwb_t *lwb = zil_alloc_lwb(zilog, &blk, txg);

	/*
	 * If we just allocated the first log block, commit our transaction
	 * and wait for zil_sync() to stuff the block poiner into zh_log.
	 * (zh is part of the MOS, so we cannot modify it in open context.)
	 */
	if (tx != NULL) {
		dmu_tx_commit(tx);
		txg_wait_synced(zilog->zl_dmu_pool, txg);
	}

	ASSERT(bcmp(&blk, &zh->zh_log, sizeof (blk)) == 0);

	ASSERT3P(lwb, !=, NULL);
	return (lwb);
}

/*
 * In one tx, free all log blocks and clear the log header. If keep_first
 * keep_first is set, then we're replaying a log with no content.  We want
 * to keep the first block, however, so that the first synchronous
 * transaction doesn't require a txg_wait_synced() in zil_alloc_first_lwb().
 * We don't need to txg_wait_synced() here either when keep_first is set,
 * because both zil_alloc_first_lwb() and zil_destroy() will wait for any
 * in-progress destroys to complete.
 */
void
zil_destroy(zilog_t *zilog, boolean_t keep_first)
{
	const zil_header_t *zh = zilog->zl_header;
	lwb_t *lwb;
	dmu_tx_t *tx;
	uint64_t txg;

	/*
	 * Wait for any previous destroy to complete.
	 */
	txg_wait_synced(zilog->zl_dmu_pool, zilog->zl_destroy_txg);

	zilog->zl_old_header = *zh;		/* debugging aid */

	if (BP_IS_HOLE(&zh->zh_log))
		return;

	tx = dmu_tx_create(zilog->zl_os);
	VERIFY0(dmu_tx_assign(tx, TXG_WAIT));
	dsl_dataset_dirty(dmu_objset_ds(zilog->zl_os), tx);
	txg = dmu_tx_get_txg(tx);

	mutex_enter(&zilog->zl_lock);

	ASSERT3U(zilog->zl_destroy_txg, <, txg);
	zilog->zl_destroy_txg = txg;
	zilog->zl_keep_first = keep_first;

	if (!list_is_empty(&zilog->zl_lwb_list)) {
		ASSERT(zh->zh_claim_txg == 0);
		VERIFY(!keep_first);
		while ((lwb = list_head(&zilog->zl_lwb_list)) != NULL) {
			list_remove(&zilog->zl_lwb_list, lwb);
			if (lwb->lwb_buf != NULL)
				zio_buf_free(lwb->lwb_buf, lwb->lwb_sz);
			zio_free_zil(zilog->zl_spa, txg, &lwb->lwb_blk);
			zil_free_lwb(lwb);
		}
	} else if (!keep_first) {
		zil_destroy_sync(zilog, tx);
	}
	mutex_exit(&zilog->zl_lock);

	dmu_tx_commit(tx);
}

void
zil_destroy_sync(zilog_t *zilog, dmu_tx_t *tx)
{
	ASSERT(list_is_empty(&zilog->zl_lwb_list));
	(void) zil_parse(zilog, zil_free_log_block,
	    zil_free_log_record, tx, zilog->zl_header->zh_claim_txg);
}

int
zil_claim(dsl_pool_t *dp, dsl_dataset_t *ds, void *txarg)
{
	dmu_tx_t *tx = txarg;
	uint64_t first_txg = dmu_tx_get_txg(tx);
	zilog_t *zilog;
	zil_header_t *zh;
	objset_t *os;
	int error;

	error = dmu_objset_own_obj(dp, ds->ds_object,
	    DMU_OST_ANY, B_FALSE, FTAG, &os);
	if (error != 0) {
		/*
		 * EBUSY indicates that the objset is inconsistent, in which
		 * case it can not have a ZIL.
		 */
		if (error != EBUSY) {
			cmn_err(CE_WARN, "can't open objset for %llu, error %u",
			    (unsigned long long)ds->ds_object, error);
		}
		return (0);
	}

	zilog = dmu_objset_zil(os);
	zh = zil_header_in_syncing_context(zilog);

	if (spa_get_log_state(zilog->zl_spa) == SPA_LOG_CLEAR) {
		if (!BP_IS_HOLE(&zh->zh_log))
			zio_free_zil(zilog->zl_spa, first_txg, &zh->zh_log);
		BP_ZERO(&zh->zh_log);
		dsl_dataset_dirty(dmu_objset_ds(os), tx);
		dmu_objset_disown(os, FTAG);
		return (0);
	}

	/*
	 * Claim all log blocks if we haven't already done so, and remember
	 * the highest claimed sequence number.  This ensures that if we can
	 * read only part of the log now (e.g. due to a missing device),
	 * but we can read the entire log later, we will not try to replay
	 * or destroy beyond the last block we successfully claimed.
	 */
	ASSERT3U(zh->zh_claim_txg, <=, first_txg);
	if (zh->zh_claim_txg == 0 && !BP_IS_HOLE(&zh->zh_log)) {
		(void) zil_parse(zilog, zil_claim_log_block,
		    zil_claim_log_record, tx, first_txg);
		zh->zh_claim_txg = first_txg;
		zh->zh_claim_blk_seq = zilog->zl_parse_blk_seq;
		zh->zh_claim_lr_seq = zilog->zl_parse_lr_seq;
		if (zilog->zl_parse_lr_count || zilog->zl_parse_blk_count > 1)
			zh->zh_flags |= ZIL_REPLAY_NEEDED;
		zh->zh_flags |= ZIL_CLAIM_LR_SEQ_VALID;
		dsl_dataset_dirty(dmu_objset_ds(os), tx);
	}

	ASSERT3U(first_txg, ==, (spa_last_synced_txg(zilog->zl_spa) + 1));
	dmu_objset_disown(os, FTAG);
	return (0);
}

/*
 * Check the log by walking the log chain.
 * Checksum errors are ok as they indicate the end of the chain.
 * Any other error (no device or read failure) returns an error.
 */
/* ARGSUSED */
int
zil_check_log_chain(dsl_pool_t *dp, dsl_dataset_t *ds, void *tx)
{
	zilog_t *zilog;
	objset_t *os;
	blkptr_t *bp;
	int error;

	ASSERT(tx == NULL);

	error = dmu_objset_from_ds(ds, &os);
	if (error != 0) {
		cmn_err(CE_WARN, "can't open objset %llu, error %d",
		    (unsigned long long)ds->ds_object, error);
		return (0);
	}

	zilog = dmu_objset_zil(os);
	bp = (blkptr_t *)&zilog->zl_header->zh_log;

	/*
	 * Check the first block and determine if it's on a log device
	 * which may have been removed or faulted prior to loading this
	 * pool.  If so, there's no point in checking the rest of the log
	 * as its content should have already been synced to the pool.
	 */
	if (!BP_IS_HOLE(bp)) {
		vdev_t *vd;
		boolean_t valid = B_TRUE;

		spa_config_enter(os->os_spa, SCL_STATE, FTAG, RW_READER);
		vd = vdev_lookup_top(os->os_spa, DVA_GET_VDEV(&bp->blk_dva[0]));
		if (vd->vdev_islog && vdev_is_dead(vd))
			valid = vdev_log_state_valid(vd);
		spa_config_exit(os->os_spa, SCL_STATE, FTAG);

		if (!valid)
			return (0);
	}

	/*
	 * Because tx == NULL, zil_claim_log_block() will not actually claim
	 * any blocks, but just determine whether it is possible to do so.
	 * In addition to checking the log chain, zil_claim_log_block()
	 * will invoke zio_claim() with a done func of spa_claim_notify(),
	 * which will update spa_max_claim_txg.  See spa_load() for details.
	 */
	error = zil_parse(zilog, zil_claim_log_block, zil_claim_log_record, tx,
	    zilog->zl_header->zh_claim_txg ? -1ULL : spa_first_txg(os->os_spa));

	return ((error == ECKSUM || error == ENOENT) ? 0 : error);
}

/*
 * When an lwb write is considered "stable" this function is used to
 * iterate over all lwb waiters, marking each as "done" and signalling
 * any threads waiting on them.
 */
static void
zil_commit_waiter_done(zil_commit_waiter_t *zcw, lwb_t *lwb, int zio_error)
{
	ASSERT(!MUTEX_HELD(&zcw->zcw_lock));

	mutex_enter(&zcw->zcw_lock);

	ASSERT(list_link_active(&zcw->zcw_node));
	list_remove(&lwb->lwb_waiters, zcw);

	zcw->zcw_zio_error = zio_error;

	ASSERT3B(zcw->zcw_done, ==, B_FALSE);
	zcw->zcw_done = B_TRUE;
	cv_broadcast(&zcw->zcw_cv);

	mutex_exit(&zcw->zcw_lock);
}

/*
 * When an itx is "skipped", this function is used to properly mark the
 * waiter as "done, and signal any thread(s) waiting on it. An itx can
 * be skipped (and not committed to an lwb) for a variety of reasons,
 * one of them being that the itx was committed via spa_sync(), prior to
 * it being committed to an lwb; this can happen if a thread calling
 * zil_commit() is racing with spa_sync().
 */
static void
zil_commit_waiter_skip(zil_commit_waiter_t *zcw)
{
	mutex_enter(&zcw->zcw_lock);
	ASSERT3B(zcw->zcw_done, ==, B_FALSE);
	zcw->zcw_done = B_TRUE;
	cv_broadcast(&zcw->zcw_cv);
	mutex_exit(&zcw->zcw_lock);
}

/*
 * This function is used when the given waiter is to be linked into an
 * lwb's "lwb_waiter" list; i.e. when the itx is committed to the lwb.
 * At this point, the waiter will no longer be referenced by the itx,
 * and instead, will be referenced by the lwb.
 */
static void
zil_commit_waiter_link(zil_commit_waiter_t *zcw, lwb_t *lwb)
{
	mutex_enter(&zcw->zcw_lock);
	ASSERT(!list_link_active(&zcw->zcw_node));

	/*
	 * Once the lwb's zio is submitted, the "lwb_waiters" list is
	 * protected by the ZIL's zl_lock.
	 *
	 * Prior to the lwb's zio being submitted, this field should
	 * only be accessed by the zil_commit_writer() thread (which is
	 * single threaded), so the lock isn't necessary. Once the lwb's
	 * zio is submitted, the "lwb_waiters" can be concurrently
	 * accessed by the zil_commit_writer() thread, and by the lwb
	 * zio's completion callback.
	 *
	 * Since this lock isn't _always_ needed, we can't assert that
	 * the lock is held each time this function is called.
	 */
	list_insert_tail(&lwb->lwb_waiters, zcw);
	mutex_exit(&zcw->zcw_lock);
}

void
zil_lwb_add_block(lwb_t *lwb, const blkptr_t *bp)
{
	avl_tree_t *t = &lwb->lwb_vdev_tree;
	avl_index_t where;
	zil_vdev_node_t *zv, zvsearch;
	int ndvas = BP_GET_NDVAS(bp);
	int i;

	if (zfs_nocacheflush)
		return;

	mutex_enter(&lwb->lwb_vdev_lock);
	for (i = 0; i < ndvas; i++) {
		zvsearch.zv_vdev = DVA_GET_VDEV(&bp->blk_dva[i]);
		if (avl_find(t, &zvsearch, &where) == NULL) {
			zv = kmem_alloc(sizeof (*zv), KM_SLEEP);
			zv->zv_vdev = zvsearch.zv_vdev;
			avl_insert(t, zv, where);
		}
	}
	mutex_exit(&lwb->lwb_vdev_lock);
}

/*
 * This function is a called after all VDEVs associated with a given lwb
 * write have completed their DKIOCFLUSHWRITECACHE command; or as soon
 * as the lwb write completes, if "zfs_nocacheflush" is set.
 *
 * The intention is for this function to be called as soon as the
 * contents of an lwb are considered "stable" on disk, and will survive
 * any sudden loss of power. At this point, any threads waiting for the
 * lwb to reach this state are signalled, and the "waiter" structures
 * are marked "done".
 */
static void
zil_lwb_flush_vdevs_done(zio_t *zio)
{
	lwb_t *lwb = zio->io_private;
	zilog_t *zilog = lwb->lwb_zilog;
	dmu_tx_t *tx = lwb->lwb_tx;
	zil_commit_waiter_t *zcw;

	/*
	 * Ensure the lwb buffer pointer is cleared before releasing
	 * the txg. If we have had an allocation failure and
	 * the txg is waiting to sync then we want want zil_sync()
	 * to remove the lwb so that it's not picked up as the next new
	 * one in zil_commit_writer(). zil_sync() will only remove
	 * the lwb if lwb_buf is null.
	 */
	zio_buf_free(lwb->lwb_buf, lwb->lwb_sz);

	mutex_enter(&zilog->zl_lock);

	lwb->lwb_buf = NULL;
	lwb->lwb_tx = NULL;

	if (zilog->zl_last_lwb == lwb) {
		/*
		 * The zl_last_lwb field is used to build the lwb zio
		 * dependency chain when creating the lwb zio's. When a
		 * new lwb zio is created, the zl_last_lwb's zio will
		 * become a
		 * child of that new lwb zio, such that the new lwb zio
		 * cannot complete until the zl_last_lwb's zio completes.
		 *
		 * If the zl_last_lwb's zio has already completed, though, we
		 * cannot add it as a child, as the zio structure might
		 * have already been freed. In that case, we don't want
		 * the new lwb zio to have any children.
		 *
		 * We set zl_last_lwb to fulfull these two requirements.
		 */
		zilog->zl_last_lwb = NULL;

		/*
		 * Remember the highest committed log sequence number
		 * for ztest. We only update this value when all the log
		 * writes succeeded, because ztest wants to ASSERT that
		 * it got the whole log chain.
		 */
		zilog->zl_commit_lr_seq = zilog->zl_lr_seq;
	}

	while ((zcw = list_head(&lwb->lwb_waiters)) != NULL) {
		/*
		 * This function will remove "zcw" from the lwb's
		 * "lwb_waiter" list, preventing an infinite loop.
		 *
		 * TODO: The prior code would ignore errors when the
		 * flush command failed, and would justify that being OK
		 * because "not all devices support the ioctl". With the
		 * current code, we'll notice the error, and then cause
		 * the zil_commit() thread to call txg_wait_synced().
		 * Maybe we should ignore the error too?
		 */
		zil_commit_waiter_done(zcw, lwb, zio->io_error);
	}

	mutex_exit(&zilog->zl_lock);

	/*
	 * Now that we've written this log block, we have a stable pointer
	 * to the next block in the chain, so it's OK to let the txg in
	 * which we allocated the next block sync.
	 */
	dmu_tx_commit(tx);
}

/*
 * The intention of this function is to issue a DKIOCFLUSHWRITECACHE
 * comamnd for all VDEVs involved with writing out a give lwb. Thus,
 * after an lwb write completes, this function should be used to issue
 * the DKIOCFLUSHWRITECACHE commands to all necessary VDEVs (this
 * includes VDEVs used by dmu_sync).
 */
static void
zil_lwb_flush_vdevs(zio_t *zio)
{
	lwb_t *lwb = zio->io_private;
	spa_t *spa = zio->io_spa;
	avl_tree_t *t = &lwb->lwb_vdev_tree;
	void *cookie = NULL;
	zil_vdev_node_t *zv;

	if (avl_numnodes(t) == 0) {
		zil_lwb_flush_vdevs_done(zio);
		return;
	}

	/*
	 * If there was an IO error, we're not going to call zio_flush()
	 * on these vdevs, so we simply empty the tree and free the
	 * nodes. We avoid calling zio_flush() since there isn't any
	 * good reason for doing so, after the lwb block failed to be
	 * written out.
	 */
	if (zio->io_error != 0) {
		while ((zv = avl_destroy_nodes(t, &cookie)) != NULL)
			kmem_free(zv, sizeof (*zv));
		zil_lwb_flush_vdevs_done(zio);
		return;
	}

	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);

	zio_t *root = zio_root(zio->io_spa,
	    zil_lwb_flush_vdevs_done, lwb, ZIO_FLAG_CANFAIL);

	/*
	 * We add the passed in zio (the lwb zio) as a child to prevent
	 * zil_lwb_flush_vdevs_done() racing with zil_lwb_write_done().
	 */
	zio_add_child(root, zio);

	while ((zv = avl_destroy_nodes(t, &cookie)) != NULL) {
		vdev_t *vd = vdev_lookup_top(spa, zv->zv_vdev);
		if (vd != NULL)
			zio_flush(root, vd);
		kmem_free(zv, sizeof (*zv));
	}

	zio_nowait(root);

	spa_config_exit(spa, SCL_STATE, FTAG);
}

/*
 * This is called when an lwb write completes. This means, this specific
 * lwb was written to disk, and all dependent lwb have also been
 * written to disk. At this point, a DKIOCFLUSHWRITECACHE command hasn't
 * been issued to the VDEVs involved in writing out this specific lwb.
 */
static void
zil_lwb_write_done(zio_t *zio)
{
	ASSERT(BP_GET_COMPRESS(zio->io_bp) == ZIO_COMPRESS_OFF);
	ASSERT(BP_GET_TYPE(zio->io_bp) == DMU_OT_INTENT_LOG);
	ASSERT(BP_GET_LEVEL(zio->io_bp) == 0);
	ASSERT(BP_GET_BYTEORDER(zio->io_bp) == ZFS_HOST_BYTEORDER);
	ASSERT(!BP_IS_GANG(zio->io_bp));
	ASSERT(!BP_IS_HOLE(zio->io_bp));
	ASSERT(BP_GET_FILL(zio->io_bp) == 0);

	abd_put(zio->io_abd);

	lwb_t *lwb = zio->io_private;
	zilog_t *zilog = lwb->lwb_zilog;

	/*
	 * After the lwb's zio completes, we set it's lwb_zio field to
	 * NULL as a way to communicate to zil_lwb_write_init(), that
	 * this zio is no longer safe to be used as a child of any
	 * future lwb zio.
	 */
	mutex_enter(&zilog->zl_lock);
	lwb->lwb_zio = NULL;
	mutex_exit(&zilog->zl_lock);

	zil_lwb_flush_vdevs(zio);
}

/*
 * Initialize the io for a log block.
 */
static void
zil_lwb_write_init(zilog_t *zilog, lwb_t *lwb)
{
	zbookmark_phys_t zb;

	SET_BOOKMARK(&zb, lwb->lwb_blk.blk_cksum.zc_word[ZIL_ZC_OBJSET],
	    ZB_ZIL_OBJECT, ZB_ZIL_LEVEL,
	    lwb->lwb_blk.blk_cksum.zc_word[ZIL_ZC_SEQ]);

	if (lwb->lwb_zio == NULL) {
		abd_t *lwb_abd = abd_get_from_buf(lwb->lwb_buf,
		    BP_GET_LSIZE(&lwb->lwb_blk));
		lwb->lwb_zio = zio_rewrite(NULL, zilog->zl_spa, 0,
		    &lwb->lwb_blk, lwb_abd, BP_GET_LSIZE(&lwb->lwb_blk),
		    zil_lwb_write_done, lwb, ZIO_PRIORITY_SYNC_WRITE,
		    ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_PROPAGATE, &zb);

		mutex_enter(&zilog->zl_lock);
		lwb_t *last_lwb = zilog->zl_last_lwb;
		if (last_lwb != NULL && last_lwb->lwb_zio != NULL)
			zio_add_child(lwb->lwb_zio, last_lwb->lwb_zio);
		zilog->zl_last_lwb = lwb;
		mutex_exit(&zilog->zl_lock);
	}
}

/*
 * Define a limited set of intent log block sizes.
 *
 * These must be a multiple of 4KB. Note only the amount used (again
 * aligned to 4KB) actually gets written. However, we can't always just
 * allocate SPA_OLD_MAXBLOCKSIZE as the slog space could be exhausted.
 */
uint64_t zil_block_buckets[] = {
    4096,		/* non TX_WRITE */
    8192+4096,		/* data base */
    32*1024 + 4096, 	/* NFS writes */
    UINT64_MAX
};

/*
 * Use the slog as long as the logbias is 'latency' and the current commit size
 * is less than the limit or the total list size is less than 2X the limit.
 * Limit checking is disabled by setting zil_slog_limit to UINT64_MAX.
 */
uint64_t zil_slog_limit = 1024 * 1024;
#define	USE_SLOG(zilog) (((zilog)->zl_logbias == ZFS_LOGBIAS_LATENCY) && \
	(((zilog)->zl_cur_used < zil_slog_limit) || \
	((zilog)->zl_itx_list_sz < (zil_slog_limit << 1))))

/*
 * Start a log block write and advance to the next log block.
 * Calls are serialized.
 */
static lwb_t *
zil_lwb_write_start(zilog_t *zilog, lwb_t *lwb)
{
	zil_chain_t *zilc;
	spa_t *spa = zilog->zl_spa;
	blkptr_t *bp;
	dmu_tx_t *tx;
	uint64_t txg;
	uint64_t zil_blksz, wsz;
	int i;

	if (BP_GET_CHECKSUM(&lwb->lwb_blk) == ZIO_CHECKSUM_ZILOG2) {
		zilc = (zil_chain_t *)lwb->lwb_buf;
		bp = &zilc->zc_next_blk;
	} else {
		zilc = (zil_chain_t *)(lwb->lwb_buf + lwb->lwb_sz);
		bp = &zilc->zc_next_blk;
	}

	ASSERT(lwb->lwb_nused <= lwb->lwb_sz);

	/*
	 * Allocate the next block and save its address in this block
	 * before writing it in order to establish the log chain.
	 * Note that if the allocation of nlwb synced before we wrote
	 * the block that points at it (lwb), we'd leak it if we crashed.
	 * Therefore, we don't do dmu_tx_commit() until zil_lwb_write_done().
	 * We dirty the dataset to ensure that zil_sync() will be called
	 * to clean up in the event of allocation failure or I/O failure.
	 */

	tx = dmu_tx_create(zilog->zl_os);
	VERIFY0(dmu_tx_assign(tx, TXG_WAIT));
	dsl_dataset_dirty(dmu_objset_ds(zilog->zl_os), tx);
	txg = dmu_tx_get_txg(tx);

	lwb->lwb_tx = tx;

	/*
	 * Log blocks are pre-allocated. Here we select the size of the next
	 * block, based on size used in the last block.
	 * - first find the smallest bucket that will fit the block from a
	 *   limited set of block sizes. This is because it's faster to write
	 *   blocks allocated from the same metaslab as they are adjacent or
	 *   close.
	 * - next find the maximum from the new suggested size and an array of
	 *   previous sizes. This lessens a picket fence effect of wrongly
	 *   guesssing the size if we have a stream of say 2k, 64k, 2k, 64k
	 *   requests.
	 *
	 * Note we only write what is used, but we can't just allocate
	 * the maximum block size because we can exhaust the available
	 * pool log space.
	 */
	zil_blksz = zilog->zl_cur_used + sizeof (zil_chain_t);
	for (i = 0; zil_blksz > zil_block_buckets[i]; i++)
		continue;
	zil_blksz = zil_block_buckets[i];
	if (zil_blksz == UINT64_MAX)
		zil_blksz = SPA_OLD_MAXBLOCKSIZE;
	zilog->zl_prev_blks[zilog->zl_prev_rotor] = zil_blksz;
	for (i = 0; i < ZIL_PREV_BLKS; i++)
		zil_blksz = MAX(zil_blksz, zilog->zl_prev_blks[i]);
	zilog->zl_prev_rotor = (zilog->zl_prev_rotor + 1) & (ZIL_PREV_BLKS - 1);

	BP_ZERO(bp);

	/* pass the old blkptr in order to spread log blocks across devs */
	VERIFY0(zio_alloc_zil(spa, txg, bp, &lwb->lwb_blk, zil_blksz,
	    USE_SLOG(zilog)));

	ASSERT3U(bp->blk_birth, ==, txg);
	bp->blk_cksum = lwb->lwb_blk.blk_cksum;
	bp->blk_cksum.zc_word[ZIL_ZC_SEQ]++;

	/*
	 * Allocate a new log write block (lwb).
	 */
	lwb_t *nlwb = zil_alloc_lwb(zilog, bp, txg);

	if (BP_GET_CHECKSUM(&lwb->lwb_blk) == ZIO_CHECKSUM_ZILOG2) {
		/* For Slim ZIL only write what is used. */
		wsz = P2ROUNDUP_TYPED(lwb->lwb_nused, ZIL_MIN_BLKSZ, uint64_t);
		ASSERT3U(wsz, <=, lwb->lwb_sz);
		zio_shrink(lwb->lwb_zio, wsz);

	} else {
		wsz = lwb->lwb_sz;
	}

	zilc->zc_pad = 0;
	zilc->zc_nused = lwb->lwb_nused;
	zilc->zc_eck.zec_cksum = lwb->lwb_blk.blk_cksum;

	/*
	 * clear unused data for security
	 */
	bzero(lwb->lwb_buf + lwb->lwb_nused, wsz - lwb->lwb_nused);

	zio_nowait(lwb->lwb_zio); /* Kick off the write for the old log block */

	ASSERT3P(nlwb, !=, NULL);
	return (nlwb);
}

static lwb_t *
zil_lwb_commit(zilog_t *zilog, itx_t *itx, lwb_t *lwb)
{
	lr_t *lrc = &itx->itx_lr; /* common log record */
	lr_write_t *lrw = (lr_write_t *)lrc;
	char *lr_buf;
	uint64_t txg = lrc->lrc_txg;
	uint64_t reclen = lrc->lrc_reclen;
	uint64_t dlen = 0;

	ASSERT3P(lwb, !=, NULL);
	ASSERT3P(lwb->lwb_buf, !=, NULL);
	IMPLY(lwb->lwb_zio != NULL, lwb->lwb_zio->io_executor == NULL);

	zil_lwb_write_init(zilog, lwb);

	/*
	 * A commit itx doesn't represent any on-disk state; instead
	 * it's simply used as a place holder on the commit list, and
	 * provides a mechanism for attaching a "commit waiter" onto the
	 * correct lwb (such that the waiter can be signalled upon
	 * completion of that lwb). Thus, we don't process this itx's
	 * log record if it's a commit itx (these itx's don't have log
	 * records), and instead link the itx's waiter onto the lwb's
	 * list of waiters.
	 *
	 * For more details, see the comment above zil_commit().
	 */
	if (lrc->lrc_txtype == TX_COMMIT) {
		zil_commit_waiter_link(itx->itx_private, lwb);
		itx->itx_private = NULL;
		return (lwb);
	}

	if (lrc->lrc_txtype == TX_WRITE && itx->itx_wr_state == WR_NEED_COPY)
		dlen = P2ROUNDUP_TYPED(
		    lrw->lr_length, sizeof (uint64_t), uint64_t);

	zilog->zl_cur_used += (reclen + dlen);

	/*
	 * If this record won't fit in the current log block, start a new one.
	 */
	if (lwb->lwb_nused + reclen + dlen > lwb->lwb_sz) {
		lwb = zil_lwb_write_start(zilog, lwb);
		zil_lwb_write_init(zilog, lwb);
		ASSERT(LWB_EMPTY(lwb));
		if (lwb->lwb_nused + reclen + dlen > lwb->lwb_sz) {
			txg_wait_synced(zilog->zl_dmu_pool, txg);
			return (lwb);
		}
	}

	lr_buf = lwb->lwb_buf + lwb->lwb_nused;
	bcopy(lrc, lr_buf, reclen);
	lrc = (lr_t *)lr_buf;
	lrw = (lr_write_t *)lrc;

	/*
	 * If it's a write, fetch the data or get its blkptr as appropriate.
	 */
	if (lrc->lrc_txtype == TX_WRITE) {
		if (txg > spa_freeze_txg(zilog->zl_spa))
			txg_wait_synced(zilog->zl_dmu_pool, txg);
		if (itx->itx_wr_state != WR_COPIED) {
			char *dbuf;
			int error;

			if (dlen) {
				ASSERT(itx->itx_wr_state == WR_NEED_COPY);
				dbuf = lr_buf + reclen;
				lrw->lr_common.lrc_reclen += dlen;
			} else {
				ASSERT(itx->itx_wr_state == WR_INDIRECT);
				dbuf = NULL;
			}
			error = zilog->zl_get_data(
			    itx->itx_private, lrw, dbuf, lwb);
			if (error == EIO) {
				txg_wait_synced(zilog->zl_dmu_pool, txg);
				return (lwb);
			}
			if (error != 0) {
				ASSERT(error == ENOENT || error == EEXIST ||
				    error == EALREADY);
				return (lwb);
			}
		}
	}

	/*
	 * We're actually making an entry, so update lrc_seq to be the
	 * log record sequence number.  Note that this is generally not
	 * equal to the itx sequence number because not all transactions
	 * are synchronous, and sometimes spa_sync() gets there first.
	 */
	lrc->lrc_seq = ++zilog->zl_lr_seq; /* we are single threaded */
	lwb->lwb_nused += reclen + dlen;
	lwb->lwb_max_txg = MAX(lwb->lwb_max_txg, txg);
	ASSERT3U(lwb->lwb_nused, <=, lwb->lwb_sz);
	ASSERT0(P2PHASE(lwb->lwb_nused, sizeof (uint64_t)));

	return (lwb);
}

itx_t *
zil_itx_create(uint64_t txtype, size_t lrsize)
{
	itx_t *itx;

	lrsize = P2ROUNDUP_TYPED(lrsize, sizeof (uint64_t), size_t);

	itx = kmem_alloc(offsetof(itx_t, itx_lr) + lrsize, KM_SLEEP);
	itx->itx_lr.lrc_txtype = txtype;
	itx->itx_lr.lrc_reclen = lrsize;

	if (txtype != TX_COMMIT) {
		/* if write & WR_NEED_COPY will be increased */
		itx->itx_sod = lrsize;
	} else {
		itx->itx_sod = 0;
	}

	itx->itx_lr.lrc_seq = 0;	/* defensive */
	itx->itx_sync = B_TRUE;		/* default is synchronous */

	return (itx);
}

void
zil_itx_destroy(itx_t *itx)
{
	kmem_free(itx, offsetof(itx_t, itx_lr) + itx->itx_lr.lrc_reclen);
}

/*
 * Free up the sync and async itxs. The itxs_t has already been detached
 * so no locks are needed.
 */
static void
zil_itxg_clean(itxs_t *itxs)
{
	itx_t *itx;
	list_t *list;
	avl_tree_t *t;
	void *cookie;
	itx_async_node_t *ian;

	list = &itxs->i_sync_list;
	while ((itx = list_head(list)) != NULL) {
		/*
		 * In the general case, commit itxs will not be found
		 * here, as they'll be committed to an lwb via
		 * zil_lwb_commit(), and free'd in that function. Having
		 * said that, it is still possible for commit itxs to be
		 * found here, due to the following race:
		 *
		 * 	- a thread calls zil_commit() which assisgns the
		 * 	  commit itx to a per-txg i_sync_list
		 * 	- zil_itxg_clean() is called (e.g. via spa_sync())
		 * 	  while the waiter is still on the i_sync_list
		 *
		 * There's nothing to prevent syncing the txg while the
		 * waiter is on the i_sync_list. This normally doesn't
		 * happen because spa_sync() is slower than zil_commit(),
		 * but if zil_commit() calls txg_wait_synced() (e.g.
		 * because the zilog_t hasn't been created yet) we will
		 * hit this case.
		 */
		if (itx->itx_lr.lrc_txtype == TX_COMMIT)
			zil_commit_waiter_skip(itx->itx_private);

		list_remove(list, itx);
		zil_itx_destroy(itx);
	}

	cookie = NULL;
	t = &itxs->i_async_tree;
	while ((ian = avl_destroy_nodes(t, &cookie)) != NULL) {
		list = &ian->ia_list;
		while ((itx = list_head(list)) != NULL) {
			list_remove(list, itx);
			/* commit itxs should never be on the async lists. */
			ASSERT3U(itx->itx_lr.lrc_txtype, !=, TX_COMMIT);
			zil_itx_destroy(itx);
		}
		list_destroy(list);
		kmem_free(ian, sizeof (itx_async_node_t));
	}
	avl_destroy(t);

	kmem_free(itxs, sizeof (itxs_t));
}

static int
zil_aitx_compare(const void *x1, const void *x2)
{
	const uint64_t o1 = ((itx_async_node_t *)x1)->ia_foid;
	const uint64_t o2 = ((itx_async_node_t *)x2)->ia_foid;

	if (o1 < o2)
		return (-1);
	if (o1 > o2)
		return (1);

	return (0);
}

/*
 * Remove all async itx with the given oid.
 */
static void
zil_remove_async(zilog_t *zilog, uint64_t oid)
{
	uint64_t otxg, txg;
	itx_async_node_t *ian;
	avl_tree_t *t;
	avl_index_t where;
	list_t clean_list;
	itx_t *itx;

	ASSERT(oid != 0);
	list_create(&clean_list, sizeof (itx_t), offsetof(itx_t, itx_node));

	if (spa_freeze_txg(zilog->zl_spa) != UINT64_MAX) /* ziltest support */
		otxg = ZILTEST_TXG;
	else
		otxg = spa_last_synced_txg(zilog->zl_spa) + 1;

	for (txg = otxg; txg < (otxg + TXG_CONCURRENT_STATES); txg++) {
		itxg_t *itxg = &zilog->zl_itxg[txg & TXG_MASK];

		mutex_enter(&itxg->itxg_lock);
		if (itxg->itxg_txg != txg) {
			mutex_exit(&itxg->itxg_lock);
			continue;
		}

		/*
		 * Locate the object node and append its list.
		 */
		t = &itxg->itxg_itxs->i_async_tree;
		ian = avl_find(t, &oid, &where);
		if (ian != NULL)
			list_move_tail(&clean_list, &ian->ia_list);
		mutex_exit(&itxg->itxg_lock);
	}
	while ((itx = list_head(&clean_list)) != NULL) {
		list_remove(&clean_list, itx);
		/* commit itxs should never be on the async lists. */
		ASSERT3U(itx->itx_lr.lrc_txtype, !=, TX_COMMIT);
		zil_itx_destroy(itx);
	}
	list_destroy(&clean_list);
}

void
zil_itx_assign(zilog_t *zilog, itx_t *itx, dmu_tx_t *tx)
{
	uint64_t txg;
	itxg_t *itxg;
	itxs_t *itxs, *clean = NULL;

	/*
	 * Object ids can be re-instantiated in the next txg so
	 * remove any async transactions to avoid future leaks.
	 * This can happen if a fsync occurs on the re-instantiated
	 * object for a WR_INDIRECT or WR_NEED_COPY write, which gets
	 * the new file data and flushes a write record for the old object.
	 */
	if ((itx->itx_lr.lrc_txtype & ~TX_CI) == TX_REMOVE)
		zil_remove_async(zilog, itx->itx_oid);

	/*
	 * Ensure the data of a renamed file is committed before the rename.
	 */
	if ((itx->itx_lr.lrc_txtype & ~TX_CI) == TX_RENAME)
		zil_async_to_sync(zilog, itx->itx_oid);

	if (spa_freeze_txg(zilog->zl_spa) != UINT64_MAX)
		txg = ZILTEST_TXG;
	else
		txg = dmu_tx_get_txg(tx);

	itxg = &zilog->zl_itxg[txg & TXG_MASK];
	mutex_enter(&itxg->itxg_lock);
	itxs = itxg->itxg_itxs;
	if (itxg->itxg_txg != txg) {
		if (itxs != NULL) {
			/*
			 * The zil_clean callback hasn't got around to cleaning
			 * this itxg. Save the itxs for release below.
			 * This should be rare.
			 */
			zfs_dbgmsg("zil_itx_assign: missed itx cleanup for "
			    "txg %llu", itxg->itxg_txg);
			atomic_add_64(&zilog->zl_itx_list_sz, -itxg->itxg_sod);
			itxg->itxg_sod = 0;
			clean = itxg->itxg_itxs;
		}
		ASSERT(itxg->itxg_sod == 0);
		itxg->itxg_txg = txg;
		itxs = itxg->itxg_itxs = kmem_zalloc(sizeof (itxs_t), KM_SLEEP);

		list_create(&itxs->i_sync_list, sizeof (itx_t),
		    offsetof(itx_t, itx_node));
		avl_create(&itxs->i_async_tree, zil_aitx_compare,
		    sizeof (itx_async_node_t),
		    offsetof(itx_async_node_t, ia_node));
	}
	if (itx->itx_sync) {
		list_insert_tail(&itxs->i_sync_list, itx);
		atomic_add_64(&zilog->zl_itx_list_sz, itx->itx_sod);
		itxg->itxg_sod += itx->itx_sod;
	} else {
		avl_tree_t *t = &itxs->i_async_tree;
		uint64_t foid = ((lr_ooo_t *)&itx->itx_lr)->lr_foid;
		itx_async_node_t *ian;
		avl_index_t where;

		ian = avl_find(t, &foid, &where);
		if (ian == NULL) {
			ian = kmem_alloc(sizeof (itx_async_node_t), KM_SLEEP);
			list_create(&ian->ia_list, sizeof (itx_t),
			    offsetof(itx_t, itx_node));
			ian->ia_foid = foid;
			avl_insert(t, ian, where);
		}
		list_insert_tail(&ian->ia_list, itx);
	}

	itx->itx_lr.lrc_txg = dmu_tx_get_txg(tx);

	/*
	 * We don't want to dirty the ZIL using ZILTEST_TXG, because
	 * zil_clean() will never be called using ZILTEST_TXG. Thus, we
	 * need to be careful to always dirty the ZIL using the "real"
	 * TXG even when the SPA is frozen.
	 */
	zilog_dirty(zilog, dmu_tx_get_txg(tx));
	mutex_exit(&itxg->itxg_lock);

	/* Release the old itxs now we've dropped the lock */
	if (clean != NULL)
		zil_itxg_clean(clean);
}

/*
 * If there are any in-memory intent log transactions which have now been
 * synced then start up a taskq to free them. We should only do this after we
 * have written out the uberblocks (i.e. txg has been comitted) so that
 * don't inadvertently clean out in-memory log records that would be required
 * by zil_commit().
 */
void
zil_clean(zilog_t *zilog, uint64_t synced_txg)
{
	itxg_t *itxg = &zilog->zl_itxg[synced_txg & TXG_MASK];
	itxs_t *clean_me;

	ASSERT3U(synced_txg, <, ZILTEST_TXG);

	mutex_enter(&itxg->itxg_lock);
	if (itxg->itxg_itxs == NULL || itxg->itxg_txg == ZILTEST_TXG) {
		mutex_exit(&itxg->itxg_lock);
		return;
	}
	ASSERT3U(itxg->itxg_txg, <=, synced_txg);
	ASSERT(itxg->itxg_txg != 0);
	ASSERT(zilog->zl_clean_taskq != NULL);
	atomic_add_64(&zilog->zl_itx_list_sz, -itxg->itxg_sod);
	itxg->itxg_sod = 0;
	clean_me = itxg->itxg_itxs;
	itxg->itxg_itxs = NULL;
	itxg->itxg_txg = 0;
	mutex_exit(&itxg->itxg_lock);
	/*
	 * Preferably start a task queue to free up the old itxs but
	 * if taskq_dispatch can't allocate resources to do that then
	 * free it in-line. This should be rare. Note, using TQ_SLEEP
	 * created a bad performance problem.
	 */
	if (taskq_dispatch(zilog->zl_clean_taskq,
	    (void (*)(void *))zil_itxg_clean, clean_me, TQ_NOSLEEP) == NULL)
		zil_itxg_clean(clean_me);
}

/*
 * Move the async itxs for a specified object to commit into sync lists.
 */
static void
zil_async_to_sync(zilog_t *zilog, uint64_t foid)
{
	uint64_t otxg, txg;
	itx_async_node_t *ian;
	avl_tree_t *t;
	avl_index_t where;

	if (spa_freeze_txg(zilog->zl_spa) != UINT64_MAX) /* ziltest support */
		otxg = ZILTEST_TXG;
	else
		otxg = spa_last_synced_txg(zilog->zl_spa) + 1;

	/*
	 * This is inherently racy, since there is nothing to prevent
	 * the last synced txg from changing.
	 */
	for (txg = otxg; txg < (otxg + TXG_CONCURRENT_STATES); txg++) {
		itxg_t *itxg = &zilog->zl_itxg[txg & TXG_MASK];

		mutex_enter(&itxg->itxg_lock);
		if (itxg->itxg_txg != txg) {
			mutex_exit(&itxg->itxg_lock);
			continue;
		}

		/*
		 * If a foid is specified then find that node and append its
		 * list. Otherwise walk the tree appending all the lists
		 * to the sync list. We add to the end rather than the
		 * beginning to ensure the create has happened.
		 */
		t = &itxg->itxg_itxs->i_async_tree;
		if (foid != 0) {
			ian = avl_find(t, &foid, &where);
			if (ian != NULL) {
				list_move_tail(&itxg->itxg_itxs->i_sync_list,
				    &ian->ia_list);
			}
		} else {
			void *cookie = NULL;

			while ((ian = avl_destroy_nodes(t, &cookie)) != NULL) {
				list_move_tail(&itxg->itxg_itxs->i_sync_list,
				    &ian->ia_list);
				list_destroy(&ian->ia_list);
				kmem_free(ian, sizeof (itx_async_node_t));
			}
		}
		mutex_exit(&itxg->itxg_lock);
	}
}

/*
 * This function will traverse the queue of itxs that need to be
 * committed, and move them onto the ZIL's zl_itx_commit_list.
 */
static void
zil_get_commit_list(zilog_t *zilog)
{
	uint64_t otxg, txg;
	list_t *commit_list = &zilog->zl_itx_commit_list;
	uint64_t push_sod = 0;

	if (spa_freeze_txg(zilog->zl_spa) != UINT64_MAX) /* ziltest support */
		otxg = ZILTEST_TXG;
	else
		otxg = spa_last_synced_txg(zilog->zl_spa) + 1;

	/*
	 * This is inherently racy, since there is nothing to prevent
	 * the last synced txg from changing. That's okay since we'll
	 * only commit things in the future.
	 */
	for (txg = otxg; txg < (otxg + TXG_CONCURRENT_STATES); txg++) {
		itxg_t *itxg = &zilog->zl_itxg[txg & TXG_MASK];

		mutex_enter(&itxg->itxg_lock);
		if (itxg->itxg_txg != txg) {
			mutex_exit(&itxg->itxg_lock);
			continue;
		}

		/*
		 * If we're adding itx records to the zl_itx_commit_list,
		 * then the zil better be dirty in this "txg". We can assert
		 * that here since we're holding the itxg_lock which will
		 * prevent spa_sync from cleaning it. Once we add the itxs
		 * to the zl_itx_commit_list we must commit it to disk even
		 * if it's unnecessary (i.e. the txg was synced).
		 */
		ASSERT(zilog_is_dirty_in_txg(zilog, txg) ||
		    spa_freeze_txg(zilog->zl_spa) != UINT64_MAX);
		list_move_tail(commit_list, &itxg->itxg_itxs->i_sync_list);
		push_sod += itxg->itxg_sod;
		itxg->itxg_sod = 0;

		mutex_exit(&itxg->itxg_lock);
	}
	atomic_add_64(&zilog->zl_itx_list_sz, -push_sod);
}


/*
 * This function will prune all commit itxs from the head of the commit
 * list, and either: a) attach them to the last lwb that's still pending
 * completion, or b) skip them altogether.
 *
 * This is used as a performance optimization, called from
 * zil_commit_writer(), and prevents commit itxs from generating new
 * lwbs when it's unnecessary to do so.
 */
static void
zil_prune_commit_list(zilog_t *zilog)
{
	itx_t *itx;

	while (itx = list_head(&zilog->zl_itx_commit_list)) {
		lr_t *lrc = &itx->itx_lr;
		if (lrc->lrc_txtype != TX_COMMIT)
			break;

		mutex_enter(&zilog->zl_lock);

		/*
		 * We rely on the fact that zil_lwb_flush_vdevs_done
		 * will set zl_last_lwb to NULL, and clear the lwb's
		 * list of waiters, all while holding the zl_lock. As
		 * long as we also hold that lock here, it's safe to
		 * check zl_last_lwb, and insert this itx's waiter onto
		 * zl_last_lwb's list of waiters (when it's non-NULL).
		 */

		if (zilog->zl_last_lwb == NULL) {
			/*
			 * If there isn't a "last" lwb that's
			 * outstanding, then all of the itxs this waiter
			 * was waiting on must have already completed (or
			 * there weren't any for it to wait on to begin
			 * with), so it's safe to skip this itx and
			 * immediately mark it as done.
			 */
			zil_commit_waiter_skip(itx->itx_private);
		} else {
			/*
			 * If there's still a pending "last" lwb that
			 * hasn't yet completed, we link this waiter
			 * onto that last lwb's list of waiters. That
			 * way, as soon as that lwb completes, it will
			 * mark this waiter as done.
			 */
			zil_commit_waiter_link(
			    itx->itx_private, zilog->zl_last_lwb);
			itx->itx_private = NULL;
		}

		mutex_exit(&zilog->zl_lock);

		list_remove(&zilog->zl_itx_commit_list, itx);
		zil_itx_destroy(itx);
	}
}

/*
 * This function will traverse the commit list, creating new lwbs as
 * needed, and committing the itxs from the commit list to these newly
 * created lwbs. Additionally, as a new lwb is created, the previous
 * lwb will be issued to the zio layer to be written to disk.
 */
static void
zil_process_commit_list(zilog_t *zilog)
{
	spa_t *spa = zilog->zl_spa;
	lwb_t *lwb;
	itx_t *itx;

	/*
	 * Return if there's nothing to commit before we dirty the fs by
	 * calling zil_alloc_first_lwb().
	 */
	if (list_head(&zilog->zl_itx_commit_list) == NULL)
		return;

	lwb = list_tail(&zilog->zl_lwb_list);
	if (lwb == NULL)
		lwb = zil_alloc_first_lwb(zilog);

	while (itx = list_head(&zilog->zl_itx_commit_list)) {
		lr_t *lrc = &itx->itx_lr;
		uint64_t txg = lrc->lrc_txg;

		ASSERT3U(txg, !=, 0);
		ASSERT3P(lwb, !=, NULL);

		/*
		 * This is inherently racy and may result in us writing
		 * out a log block for a txg that was just synced. This
		 * is ok since we'll end cleaning up that log block the
		 * next time we call zil_sync().
		 */
		boolean_t synced = txg <= spa_last_synced_txg(spa);
		boolean_t frozen = txg > spa_freeze_txg(spa);

		if (!synced || frozen) {
			lwb = zil_lwb_commit(zilog, itx, lwb);
			ASSERT3P(lwb, !=, NULL);
		} else if (lrc->lrc_txtype == TX_COMMIT) {
			ASSERT3B(synced, ==, B_TRUE);
			ASSERT3B(frozen, ==, B_FALSE);

			/*
			 * If this is a commit itx, then there will be a
			 * thread that is either: already waiting for
			 * it, or soon will be waiting.
			 *
			 * This itx has already been committed to disk
			 * via spa_sync() so we don't bother committing
			 * it to an lwb. As a result, we cannot use the
			 * lwb zio callback to signal the waiter and
			 * mark it as done, so we must do that here.
			 */
			zil_commit_waiter_skip(itx->itx_private);
		}

		list_remove(&zilog->zl_itx_commit_list, itx);
		zil_itx_destroy(itx);
	}
	DTRACE_PROBE1(zil__cw2, zilog_t *, zilog);

	/* write the last block out */
	if (lwb != NULL && lwb->lwb_zio != NULL)
		lwb = zil_lwb_write_start(zilog, lwb);

	zilog->zl_cur_used = 0;
	ASSERT3P(lwb, !=, NULL);
}

/*
 * This function is responsible for issuing the IO for all itxs waiting
 * to be committed at the time the function was called. When this
 * function completes, any itxs that were previously in the queue of
 * itxs to be committed will have been committed to an lwb, and the
 * lwb's zio will have been issued to disk.
 *
 * When this function completes, the itxs are not gauranteed to be
 * committed to stable storage yet, but the zio(s) responsible for
 * committing the itxs to stable storage is gauranteed to have been
 * issued.
 *
 * In order to be notified when these itxs have been committed to stable
 * storage (or failed to be committed), one must use a "commit waiter"
 * and a "commit itx"; see how this is done in zil_commit() for details.
 */
static void
zil_commit_writer(zilog_t *zilog)
{
	ASSERT(!MUTEX_HELD(&zilog->zl_lock));
	ASSERT(spa_writeable(zilog->zl_spa));
	ASSERT0(zilog->zl_suspend);

	mutex_enter(&zilog->zl_writer_lock);
	zil_get_commit_list(zilog);
	zil_prune_commit_list(zilog);
	zil_process_commit_list(zilog);
	mutex_exit(&zilog->zl_writer_lock);
}

static zil_commit_waiter_t *
zil_alloc_commit_waiter()
{
	zil_commit_waiter_t *zcw = kmem_cache_alloc(zil_zcw_cache, KM_SLEEP);

	cv_init(&zcw->zcw_cv, NULL, CV_DEFAULT, NULL);
	mutex_init(&zcw->zcw_lock, NULL, MUTEX_DEFAULT, NULL);
	list_link_init(&zcw->zcw_node);
	zcw->zcw_done = B_FALSE;
	zcw->zcw_zio_error = 0;

	return (zcw);
}

static void
zil_free_commit_waiter(zil_commit_waiter_t *zcw)
{
	ASSERT(!list_link_active(&zcw->zcw_node));
	ASSERT3B(zcw->zcw_done, ==, B_TRUE);
	mutex_destroy(&zcw->zcw_lock);
	cv_destroy(&zcw->zcw_cv);
	kmem_cache_free(zil_zcw_cache, zcw);
}

/*
 * This function is used to create a TX_COMMIT itx and assign it. This
 * way, it will be linked into the ZIL's list of synchronous itxs, and
 * then later committed to an lwb (or skipped) when zil_commit_writer()
 * is called.
 */
static void
zil_commit_itx_assign(zilog_t *zilog, zil_commit_waiter_t *zcw)
{
	dmu_tx_t *tx = dmu_tx_create(zilog->zl_os);
	VERIFY0(dmu_tx_assign(tx, TXG_WAIT));

	itx_t *itx = zil_itx_create(TX_COMMIT, sizeof (lr_t));
	itx->itx_sync = B_TRUE;
	itx->itx_private = zcw;

	zil_itx_assign(zilog, itx, tx);

	dmu_tx_commit(tx);
}

/*
 * Commit ZFS Intent Log transactions (itxs) to stable storage.
 *
 * When writing ZIL transactions to the on-disk representation of the
 * ZIL, the itxs are committed to a Log Write Block (lwb). Multiple
 * itxs can be committed to a single lwb. Once a lwb is written and
 * committed to stable storage (i.e. the lwb is written, and vdevs have
 * been flushed), each itx that was committed to that lwb is also
 * considered to be committed to stable storage.
 *
 * When an itx is committed to an lwb, the log record (lr_t) contained
 * by the itx is copied into the lwb's zio buffer, and once this buffer
 * is written to disk, it becomes an on-disk ZIL block.
 *
 * As itxs are generated, they're inserted into the ZIL's queue of
 * uncommitted itxs. The semantics of zil_commit() are such it will
 * block until all itxs that were in the queue when it was called, will
 * be committed to stable storage prior to zil_commit() returning.
 *
 * If "foid" is zero, this means all "synchronous" and "asynchronous"
 * itxs, for all objects in the dataset, will be committed to stable
 * storage prior to zil_commit() returning. If "foid" is non-zero, all
 * "synchronous" itxs for all objects, but only "asynchronous" itxs
 * that correspond to the foid passed in, will be committed to stable
 * storage prior to zil_commit() returning.
 *
 * Generally speaking, when zil_commit() is called, the consumer doesn't
 * actually care about _all_ of the uncommitted itxs. Instead, they're
 * simply trying to waiting for a specific itx to be committed to disk,
 * but the interface(s) for interacting with the ZIL don't allow such
 * fine-grained communication. A better interface would allow a consumer
 * to create and assign an itx, and then pass a reference to this itx to
 * zil_commit(); such that zil_commit() would return as soon as that
 * specific itx was committed to disk (instead of waiting for _all_
 * itxs to be committed).
 *
 * When a thread calls zil_commit() a special "commit itx" will be
 * generated, along with a corresponding "waiter" for this commit itx.
 * zil_commit() will wait on this waiter's CV, such that when the waiter
 * is marked done, and signalled, zil_commit() will return.
 *
 * This commit itx is inserted into the queue of uncommitted itxs. This
 * provides an easy mechanism for determining which itxs were in the
 * queue prior to zil_commit() having been called, and which itxs were
 * added after zil_commit() was called.
 *
 * The commit itx is special, such that it doesn't equate to any on-disk
 * representation. Instead, when it is "committed" to an lwb, the waiter
 * associated with it is linked onto the lwb's list of waiters. Then,
 * when that lwb completes, each waiter on the lwb's list is marked done
 * and signalled -- allowing the thread waiting on the waiter to return
 * from zil_commit().
 *
 * It's important to point out a few critical factors that allow us
 * to make use of the commit itxs, commit waiters, per-lwb lists of
 * commit waiters, and zio completion callbacks like we're doing:
 *
 *   1. The list of waiters for each lwb is traversed, and each commit
 *      waiter is marked "done" and signalled, in the zio completion
 *      callback of the lwb's zio[*].
 *
 *      * Actually, the waiters are signalled in the zio completion
 *        callback of the root zio for the DKIOCFLUSHWRITECACHE commands
 *        that are sent to the vdevs upon completion of the lwb zio.
 *
 *   2. When the itxs are inserted into the ZIL's queue of uncommitted
 *      itxs, the order in which they are inserted is preserved[*]; as
 *      itxs are added to the queue, they are added to the tail of
 *      in-memory linked lists.
 *
 *      When committing the itxs to lwbs (to be written to disk), they
 *      are committed in the same order in which the itxs were added to
 *      the uncommitted queue's linked list(s); i.e. the linked list of
 *      itxs to commit is traversed from head to tail, and each itx is
 *      committed to an lwb in that order.
 *
 *      * To clarify:
 *
 *        - the order of "sync" itxs is preserved w.r.t. other
 *          "sync" itxs, regardless of the corresponding objects.
 *        - the order of "async" itxs is preserved w.r.t. other
 *          "async" itxs corresponding to the same object.
 *        - the order of "async" itxs is *not* preserved w.r.t. other
 *          "async" itxs corresponding to different objects.
 *        - the order of "sync" itxs w.r.t. "async" itxs (or vice
 *          versa) is *not* preserved, even for itxs that correspond
 *          to the same object.
 *
 *      For more details, see: zil_itx_assign(), zil_async_to_sync(),
 *      zil_get_commit_list(), and zil_commit_writer().
 *
 *   3. The lwbs represent a linked list of blocks on disk. Thus, any
 *      lwb cannot be considered committed to stable storage, until its
 *      "previous" lwb is also committed to stable storage. This fact,
 *      coupled with the fact described above, means that itxs are
 *      committed in (roughly) the order in which they were generated.
 *      This is essential because itxs are dependent on prior itxs.
 *      Thus, we *must not* deem an itx as being committed to stable
 *      storage, until *all* prior itxs have also been committed to
 *      stable storage.
 *
 *      To enforce this ordering of lwb zio's, while still leveraging as
 *      much of the underlying storage performance as possible, we rely
 *      on two fundamental concepts:
 *
 *          1. Creation of lwb zio's is single threaded
 *          2. The "previous" lwb is a child of the "current" lwb
 *             (leveraging the zio parent-child depenency graph)
 *
 *      By relying on this parent-child zio relationship, we can have
 *      many lwb zio's concurrently issued to the underlying storage,
 *      but the order in which they complete will be the same order in
 *      which they were created.
 *
 */
void
zil_commit(zilog_t *zilog, uint64_t foid)
{
	if (zilog->zl_sync == ZFS_SYNC_DISABLED)
		return;

	if (!spa_writeable(zilog->zl_spa)) {
		/*
		 * If the SPA is not writable, there should never be any
		 * pending itxs waiting to be committed to disk. If that
		 * weren't true, we'd skip writing those itxs out, and
		 * would break the sematics of zil_commit(); thus, we're
		 * verifying that truth before we return to the caller.
		 */
		ASSERT(list_is_empty(&zilog->zl_lwb_list));
		ASSERT3P(zilog->zl_last_lwb, ==, NULL);
		for (int i = 0; i < TXG_SIZE; i++)
			ASSERT3P(zilog->zl_itxg[i].itxg_itxs, ==, NULL);
		return;
	}

	/*
	 * If the ZIL is suspended, we don't want to dirty it by calling
	 * zil_commit_itx_assign() below, nor can we write out
	 * lwbs like would be done in zil_commit_write(). Thus, we
	 * simply rely on txg_wait_synced() to maintain the necessary
	 * semantics, and avoid calling those functions altogether.
	 */
	if (zilog->zl_suspend > 0) {
		txg_wait_synced(zilog->zl_dmu_pool, 0);
		return;
	}

	/*
	 * Move the "async" itxs for the specified foid to the "sync"
	 * queues, such that they will be later committed (or skipped)
	 * to an lwb when zil_commit_writer is called.
	 *
	 * Since these "async" itxs must be committed prior to this
	 * call to zil_commit returning, we must perform this operation
	 * before we call zil_commit_itx_assign().
	 */
	zil_async_to_sync(zilog, foid);

	/*
	 * We allocate a new "waiter" structure which will initially be
	 * linked to the commit itx using the itx's "itx_private" field.
	 * Since the commit itx doesn't represent any on-disk state,
	 * when it's committed to an lwb, rather than copying the its
	 * lr_t into the lwb's buffer, the commit itx's "waiter" will be
	 * added to the lwb's list of waiters. Then, when the lwb is
	 * committed to stable storage, each waiter in the lwb's list of
	 * waiters will be marked "done", and signalled.
	 *
	 * We *must* assign create the waiter and assing the commit itx
	 * prior to calling zil_commit_writer(), or else our specific
	 * commit itx is not gauranteed to be committed to an lwb. If
	 * the commit itx doesn't get committed to an lwb, we can end up
	 * waiting "forever" when we call cv_wait() on the waiter's CV.
	 */
	zil_commit_waiter_t *zcw = zil_alloc_commit_waiter();
	zil_commit_itx_assign(zilog, zcw);

	zil_commit_writer(zilog);

	mutex_enter(&zcw->zcw_lock);
	while (!zcw->zcw_done) {
		cv_wait(&zcw->zcw_cv, &zcw->zcw_lock);
	}
	mutex_exit(&zcw->zcw_lock);

	if (zcw->zcw_zio_error != 0) {
		/*
		 * If there was an error writing out the ZIL blocks that
		 * this thread is waiting on, then we fallback to
		 * relying on spa_sync() to write out the data this
		 * thread is waiting on. Obviously this has performance
		 * implications, but the expectation is for this to be
		 * an exceptional case, and shouldn't occur often.
		 */

		/*
		 * TODO: Maybe this should be a kstat? DTRACE_PROBE is
		 * easier to add, using this for now.
		 */
		DTRACE_PROBE2(zil__commit__io__error,
		    zilog_t *, zilog, zil_commit_waiter_t *, zcw);
		txg_wait_synced(zilog->zl_dmu_pool, 0);
	}

	zil_free_commit_waiter(zcw);
}

/*
 * Called in syncing context to free committed log blocks and update log header.
 */
void
zil_sync(zilog_t *zilog, dmu_tx_t *tx)
{
	zil_header_t *zh = zil_header_in_syncing_context(zilog);
	uint64_t txg = dmu_tx_get_txg(tx);
	spa_t *spa = zilog->zl_spa;
	uint64_t *replayed_seq = &zilog->zl_replayed_seq[txg & TXG_MASK];
	lwb_t *lwb;

	/*
	 * We don't zero out zl_destroy_txg, so make sure we don't try
	 * to destroy it twice.
	 */
	if (spa_sync_pass(spa) != 1)
		return;

	mutex_enter(&zilog->zl_lock);

	ASSERT(zilog->zl_stop_sync == 0);

	if (*replayed_seq != 0) {
		ASSERT(zh->zh_replay_seq < *replayed_seq);
		zh->zh_replay_seq = *replayed_seq;
		*replayed_seq = 0;
	}

	if (zilog->zl_destroy_txg == txg) {
		blkptr_t blk = zh->zh_log;

		ASSERT(list_head(&zilog->zl_lwb_list) == NULL);

		bzero(zh, sizeof (zil_header_t));
		bzero(zilog->zl_replayed_seq, sizeof (zilog->zl_replayed_seq));

		if (zilog->zl_keep_first) {
			/*
			 * If this block was part of log chain that couldn't
			 * be claimed because a device was missing during
			 * zil_claim(), but that device later returns,
			 * then this block could erroneously appear valid.
			 * To guard against this, assign a new GUID to the new
			 * log chain so it doesn't matter what blk points to.
			 */
			zil_init_log_chain(zilog, &blk);
			zh->zh_log = blk;
		}
	}

	while ((lwb = list_head(&zilog->zl_lwb_list)) != NULL) {
		zh->zh_log = lwb->lwb_blk;
		if (lwb->lwb_buf != NULL || lwb->lwb_max_txg > txg)
			break;
		list_remove(&zilog->zl_lwb_list, lwb);
		zio_free_zil(spa, txg, &lwb->lwb_blk);
		zil_free_lwb(lwb);

		/*
		 * If we don't have anything left in the lwb list then
		 * we've had an allocation failure and we need to zero
		 * out the zil_header blkptr so that we don't end
		 * up freeing the same block twice.
		 */
		if (list_head(&zilog->zl_lwb_list) == NULL)
			BP_ZERO(&zh->zh_log);
	}
	mutex_exit(&zilog->zl_lock);
}

void
zil_init(void)
{
	zil_lwb_cache = kmem_cache_create("zil_lwb_cache",
	    sizeof (lwb_t), 0, NULL, NULL, NULL, NULL, NULL, 0);

	zil_zcw_cache = kmem_cache_create("zil_zcw_cache",
	    sizeof (zil_commit_waiter_t), 0, NULL, NULL, NULL, NULL, NULL, 0);
}

void
zil_fini(void)
{
	kmem_cache_destroy(zil_zcw_cache);
	kmem_cache_destroy(zil_lwb_cache);
}

void
zil_set_sync(zilog_t *zilog, uint64_t sync)
{
	zilog->zl_sync = sync;
}

void
zil_set_logbias(zilog_t *zilog, uint64_t logbias)
{
	zilog->zl_logbias = logbias;
}

zilog_t *
zil_alloc(objset_t *os, zil_header_t *zh_phys)
{
	zilog_t *zilog;

	zilog = kmem_zalloc(sizeof (zilog_t), KM_SLEEP);

	zilog->zl_header = zh_phys;
	zilog->zl_os = os;
	zilog->zl_spa = dmu_objset_spa(os);
	zilog->zl_dmu_pool = dmu_objset_pool(os);
	zilog->zl_destroy_txg = TXG_INITIAL - 1;
	zilog->zl_logbias = dmu_objset_logbias(os);
	zilog->zl_sync = dmu_objset_syncprop(os);
	zilog->zl_dirty_max_txg = 0;

	mutex_init(&zilog->zl_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zilog->zl_writer_lock, NULL, MUTEX_DEFAULT, NULL);

	for (int i = 0; i < TXG_SIZE; i++) {
		mutex_init(&zilog->zl_itxg[i].itxg_lock, NULL,
		    MUTEX_DEFAULT, NULL);
	}

	list_create(&zilog->zl_lwb_list, sizeof (lwb_t),
	    offsetof(lwb_t, lwb_node));

	list_create(&zilog->zl_itx_commit_list, sizeof (itx_t),
	    offsetof(itx_t, itx_node));

	cv_init(&zilog->zl_cv_suspend, NULL, CV_DEFAULT, NULL);

	return (zilog);
}

void
zil_free(zilog_t *zilog)
{
	zilog->zl_stop_sync = 1;

	ASSERT0(zilog->zl_suspend);
	ASSERT0(zilog->zl_suspending);

	ASSERT(list_is_empty(&zilog->zl_lwb_list));
	list_destroy(&zilog->zl_lwb_list);

	ASSERT(list_is_empty(&zilog->zl_itx_commit_list));
	list_destroy(&zilog->zl_itx_commit_list);

	for (int i = 0; i < TXG_SIZE; i++) {
		/*
		 * It's possible for an itx to be generated that doesn't dirty
		 * a txg (e.g. ztest TX_TRUNCATE). So there's no zil_clean()
		 * callback to remove the entry. We remove those here.
		 *
		 * Also free up the ziltest itxs.
		 */
		if (zilog->zl_itxg[i].itxg_itxs)
			zil_itxg_clean(zilog->zl_itxg[i].itxg_itxs);
		mutex_destroy(&zilog->zl_itxg[i].itxg_lock);
	}

	mutex_destroy(&zilog->zl_writer_lock);
	mutex_destroy(&zilog->zl_lock);

	cv_destroy(&zilog->zl_cv_suspend);

	kmem_free(zilog, sizeof (zilog_t));
}

/*
 * Open an intent log.
 */
zilog_t *
zil_open(objset_t *os, zil_get_data_t *get_data)
{
	zilog_t *zilog = dmu_objset_zil(os);

	ASSERT(zilog->zl_clean_taskq == NULL);
	ASSERT(zilog->zl_get_data == NULL);
	ASSERT(list_is_empty(&zilog->zl_lwb_list));

	zilog->zl_get_data = get_data;
	zilog->zl_clean_taskq = taskq_create("zil_clean", 1, minclsyspri,
	    2, 2, TASKQ_PREPOPULATE);

	return (zilog);
}

/*
 * Close an intent log.
 */
void
zil_close(zilog_t *zilog)
{
	lwb_t *lwb;
	uint64_t txg;

	if (zilog_is_dirty(zilog))
		zil_commit(zilog, 0); /* commit all itx */

	mutex_enter(&zilog->zl_lock);
	lwb = list_tail(&zilog->zl_lwb_list);
	if (lwb == NULL)
		txg = zilog->zl_dirty_max_txg;
	else
		txg = MAX(zilog->zl_dirty_max_txg, lwb->lwb_max_txg);
	mutex_exit(&zilog->zl_lock);

	/*
	 * We need to use txg_wait_synced() to wait long enough for the
	 * ZIL to be clean, and to wait for all pending lwbs to be
	 * written out.
	 */
	if (txg != 0)
		txg_wait_synced(zilog->zl_dmu_pool, txg);

	if (zilog_is_dirty(zilog))
		zfs_dbgmsg("zil (%p) is dirty, txg %llu", zilog, txg);
	VERIFY(!zilog_is_dirty(zilog));

	taskq_destroy(zilog->zl_clean_taskq);
	zilog->zl_clean_taskq = NULL;
	zilog->zl_get_data = NULL;

	/*
	 * We should have only one lwb left on the list; remove it now.
	 */
	mutex_enter(&zilog->zl_lock);
	lwb = list_head(&zilog->zl_lwb_list);
	if (lwb != NULL) {
		ASSERT(lwb == list_tail(&zilog->zl_lwb_list));
		list_remove(&zilog->zl_lwb_list, lwb);
		zio_buf_free(lwb->lwb_buf, lwb->lwb_sz);
		zil_free_lwb(lwb);
	}
	mutex_exit(&zilog->zl_lock);
}

static char *suspend_tag = "zil suspending";

/*
 * Suspend an intent log.  While in suspended mode, we still honor
 * synchronous semantics, but we rely on txg_wait_synced() to do it.
 * On old version pools, we suspend the log briefly when taking a
 * snapshot so that it will have an empty intent log.
 *
 * Long holds are not really intended to be used the way we do here --
 * held for such a short time.  A concurrent caller of dsl_dataset_long_held()
 * could fail.  Therefore we take pains to only put a long hold if it is
 * actually necessary.  Fortunately, it will only be necessary if the
 * objset is currently mounted (or the ZVOL equivalent).  In that case it
 * will already have a long hold, so we are not really making things any worse.
 *
 * Ideally, we would locate the existing long-holder (i.e. the zfsvfs_t or
 * zvol_state_t), and use their mechanism to prevent their hold from being
 * dropped (e.g. VFS_HOLD()).  However, that would be even more pain for
 * very little gain.
 *
 * if cookiep == NULL, this does both the suspend & resume.
 * Otherwise, it returns with the dataset "long held", and the cookie
 * should be passed into zil_resume().
 */
int
zil_suspend(const char *osname, void **cookiep)
{
	objset_t *os;
	zilog_t *zilog;
	const zil_header_t *zh;
	int error;

	error = dmu_objset_hold(osname, suspend_tag, &os);
	if (error != 0)
		return (error);
	zilog = dmu_objset_zil(os);

	mutex_enter(&zilog->zl_lock);
	zh = zilog->zl_header;

	if (zh->zh_flags & ZIL_REPLAY_NEEDED) {		/* unplayed log */
		mutex_exit(&zilog->zl_lock);
		dmu_objset_rele(os, suspend_tag);
		return (SET_ERROR(EBUSY));
	}

	/*
	 * Don't put a long hold in the cases where we can avoid it.  This
	 * is when there is no cookie so we are doing a suspend & resume
	 * (i.e. called from zil_vdev_offline()), and there's nothing to do
	 * for the suspend because it's already suspended, or there's no ZIL.
	 */
	if (cookiep == NULL && !zilog->zl_suspending &&
	    (zilog->zl_suspend > 0 || BP_IS_HOLE(&zh->zh_log))) {
		mutex_exit(&zilog->zl_lock);
		dmu_objset_rele(os, suspend_tag);
		return (0);
	}

	dsl_dataset_long_hold(dmu_objset_ds(os), suspend_tag);
	dsl_pool_rele(dmu_objset_pool(os), suspend_tag);

	zilog->zl_suspend++;

	if (zilog->zl_suspend > 1) {
		/*
		 * Someone else is already suspending it.
		 * Just wait for them to finish.
		 */

		while (zilog->zl_suspending)
			cv_wait(&zilog->zl_cv_suspend, &zilog->zl_lock);
		mutex_exit(&zilog->zl_lock);

		if (cookiep == NULL)
			zil_resume(os);
		else
			*cookiep = os;
		return (0);
	}

	/*
	 * If there is no pointer to an on-disk block, this ZIL must not
	 * be active (e.g. filesystem not mounted), so there's nothing
	 * to clean up.
	 */
	if (BP_IS_HOLE(&zh->zh_log)) {
		ASSERT(cookiep != NULL); /* fast path already handled */

		*cookiep = os;
		mutex_exit(&zilog->zl_lock);
		return (0);
	}

	zilog->zl_suspending = B_TRUE;
	mutex_exit(&zilog->zl_lock);

	zil_commit(zilog, 0);

	zil_destroy(zilog, B_FALSE);

	mutex_enter(&zilog->zl_lock);
	zilog->zl_suspending = B_FALSE;
	cv_broadcast(&zilog->zl_cv_suspend);
	mutex_exit(&zilog->zl_lock);

	if (cookiep == NULL)
		zil_resume(os);
	else
		*cookiep = os;
	return (0);
}

void
zil_resume(void *cookie)
{
	objset_t *os = cookie;
	zilog_t *zilog = dmu_objset_zil(os);

	mutex_enter(&zilog->zl_lock);
	ASSERT(zilog->zl_suspend != 0);
	zilog->zl_suspend--;
	mutex_exit(&zilog->zl_lock);
	dsl_dataset_long_rele(dmu_objset_ds(os), suspend_tag);
	dsl_dataset_rele(dmu_objset_ds(os), suspend_tag);
}

typedef struct zil_replay_arg {
	zil_replay_func_t **zr_replay;
	void		*zr_arg;
	boolean_t	zr_byteswap;
	char		*zr_lr;
} zil_replay_arg_t;

static int
zil_replay_error(zilog_t *zilog, lr_t *lr, int error)
{
	char name[ZFS_MAX_DATASET_NAME_LEN];

	zilog->zl_replaying_seq--;	/* didn't actually replay this one */

	dmu_objset_name(zilog->zl_os, name);

	cmn_err(CE_WARN, "ZFS replay transaction error %d, "
	    "dataset %s, seq 0x%llx, txtype %llu %s\n", error, name,
	    (u_longlong_t)lr->lrc_seq,
	    (u_longlong_t)(lr->lrc_txtype & ~TX_CI),
	    (lr->lrc_txtype & TX_CI) ? "CI" : "");

	return (error);
}

static int
zil_replay_log_record(zilog_t *zilog, lr_t *lr, void *zra, uint64_t claim_txg)
{
	zil_replay_arg_t *zr = zra;
	const zil_header_t *zh = zilog->zl_header;
	uint64_t reclen = lr->lrc_reclen;
	uint64_t txtype = lr->lrc_txtype;
	int error = 0;

	zilog->zl_replaying_seq = lr->lrc_seq;

	if (lr->lrc_seq <= zh->zh_replay_seq)	/* already replayed */
		return (0);

	if (lr->lrc_txg < claim_txg)		/* already committed */
		return (0);

	/* Strip case-insensitive bit, still present in log record */
	txtype &= ~TX_CI;

	if (txtype == 0 || txtype >= TX_MAX_TYPE)
		return (zil_replay_error(zilog, lr, EINVAL));

	/*
	 * If this record type can be logged out of order, the object
	 * (lr_foid) may no longer exist.  That's legitimate, not an error.
	 */
	if (TX_OOO(txtype)) {
		error = dmu_object_info(zilog->zl_os,
		    ((lr_ooo_t *)lr)->lr_foid, NULL);
		if (error == ENOENT || error == EEXIST)
			return (0);
	}

	/*
	 * Make a copy of the data so we can revise and extend it.
	 */
	bcopy(lr, zr->zr_lr, reclen);

	/*
	 * If this is a TX_WRITE with a blkptr, suck in the data.
	 */
	if (txtype == TX_WRITE && reclen == sizeof (lr_write_t)) {
		error = zil_read_log_data(zilog, (lr_write_t *)lr,
		    zr->zr_lr + reclen);
		if (error != 0)
			return (zil_replay_error(zilog, lr, error));
	}

	/*
	 * The log block containing this lr may have been byteswapped
	 * so that we can easily examine common fields like lrc_txtype.
	 * However, the log is a mix of different record types, and only the
	 * replay vectors know how to byteswap their records.  Therefore, if
	 * the lr was byteswapped, undo it before invoking the replay vector.
	 */
	if (zr->zr_byteswap)
		byteswap_uint64_array(zr->zr_lr, reclen);

	/*
	 * We must now do two things atomically: replay this log record,
	 * and update the log header sequence number to reflect the fact that
	 * we did so. At the end of each replay function the sequence number
	 * is updated if we are in replay mode.
	 */
	error = zr->zr_replay[txtype](zr->zr_arg, zr->zr_lr, zr->zr_byteswap);
	if (error != 0) {
		/*
		 * The DMU's dnode layer doesn't see removes until the txg
		 * commits, so a subsequent claim can spuriously fail with
		 * EEXIST. So if we receive any error we try syncing out
		 * any removes then retry the transaction.  Note that we
		 * specify B_FALSE for byteswap now, so we don't do it twice.
		 */
		txg_wait_synced(spa_get_dsl(zilog->zl_spa), 0);
		error = zr->zr_replay[txtype](zr->zr_arg, zr->zr_lr, B_FALSE);
		if (error != 0)
			return (zil_replay_error(zilog, lr, error));
	}
	return (0);
}

/* ARGSUSED */
static int
zil_incr_blks(zilog_t *zilog, blkptr_t *bp, void *arg, uint64_t claim_txg)
{
	zilog->zl_replay_blks++;

	return (0);
}

/*
 * If this dataset has a non-empty intent log, replay it and destroy it.
 */
void
zil_replay(objset_t *os, void *arg, zil_replay_func_t *replay_func[TX_MAX_TYPE])
{
	zilog_t *zilog = dmu_objset_zil(os);
	const zil_header_t *zh = zilog->zl_header;
	zil_replay_arg_t zr;

	if ((zh->zh_flags & ZIL_REPLAY_NEEDED) == 0) {
		zil_destroy(zilog, B_TRUE);
		return;
	}

	zr.zr_replay = replay_func;
	zr.zr_arg = arg;
	zr.zr_byteswap = BP_SHOULD_BYTESWAP(&zh->zh_log);
	zr.zr_lr = kmem_alloc(2 * SPA_MAXBLOCKSIZE, KM_SLEEP);

	/*
	 * Wait for in-progress removes to sync before starting replay.
	 */
	txg_wait_synced(zilog->zl_dmu_pool, 0);

	zilog->zl_replay = B_TRUE;
	zilog->zl_replay_time = ddi_get_lbolt();
	ASSERT(zilog->zl_replay_blks == 0);
	(void) zil_parse(zilog, zil_incr_blks, zil_replay_log_record, &zr,
	    zh->zh_claim_txg);
	kmem_free(zr.zr_lr, 2 * SPA_MAXBLOCKSIZE);

	zil_destroy(zilog, B_FALSE);
	txg_wait_synced(zilog->zl_dmu_pool, zilog->zl_destroy_txg);
	zilog->zl_replay = B_FALSE;
}

boolean_t
zil_replaying(zilog_t *zilog, dmu_tx_t *tx)
{
	if (zilog->zl_sync == ZFS_SYNC_DISABLED)
		return (B_TRUE);

	if (zilog->zl_replay) {
		dsl_dataset_dirty(dmu_objset_ds(zilog->zl_os), tx);
		zilog->zl_replayed_seq[dmu_tx_get_txg(tx) & TXG_MASK] =
		    zilog->zl_replaying_seq;
		return (B_TRUE);
	}

	return (B_FALSE);
}

/* ARGSUSED */
int
zil_vdev_offline(const char *osname, void *arg)
{
	int error;

	error = zil_suspend(osname, NULL);
	if (error != 0)
		return (SET_ERROR(EEXIST));
	return (0);
}
