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
 * Copyright 2011 Nexenta Systems, Inc. All rights reserved.
 * Copyright (c) 2011, 2017 by Delphix. All rights reserved.
 * Copyright (c) 2014, Joyent, Inc. All rights reserved.
 * Copyright 2014 HybridCluster. All rights reserved.
 * Copyright 2016 RackTop Systems.
 * Copyright (c) 2014 Integros [integros.com]
 */

#include <sys/dmu.h>
#include <sys/dmu_impl.h>
#include <sys/dmu_tx.h>
#include <sys/dbuf.h>
#include <sys/dnode.h>
#include <sys/zfs_context.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/zfs_ioctl.h>
#include <sys/zap.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_znode.h>
#include <zfs_fletcher.h>
#include <sys/avl.h>
#include <sys/ddt.h>
#include <sys/zfs_onexit.h>
#include <sys/dmu_send.h>
#include <sys/dsl_destroy.h>
#include <sys/blkptr.h>
#include <sys/dsl_bookmark.h>
#include <sys/zfeature.h>
#include <sys/bqueue.h>
#include <sys/objlist.h>
#ifdef _KERNEL
#include <sys/zfs_vfsops.h>
#endif

/* Set this tunable to TRUE to replace corrupt data with 0x2f5baddb10c */
int zfs_send_corrupt_data = B_FALSE;
/*
 * This tunable controls the amount of data (measured in bytes) that will be
 * prefetched by zfs send.  If the main thread is blocking on reads that haven't
 * completed, this variable might need to be increased.  If instead the main
 * thread is issuing new reads because the prefetches have fallen out of the
 * cache, this may need to be decreased.
 */
int zfs_send_queue_length = 16 * 1024 * 1024;
/*
 * This tunable controls the length of the queues that zfs send worker threads
 * use to communicate.  If the send_main_thread is blocking on these queues,
 * this variable may need to be increased.  If there is a significant slowdown
 * at the start of a send as these threads consume all the available IO
 * resources, this variable may need to be decreased.
 */
int zfs_send_no_prefetch_queue_length = 1024 * 1024;
/*
 * These tunables control the fill fraction of the queues by zfs send.  The fill
 * fraction controls the frequency with which threads have to be cv_signaled.
 * If a lot of cpu time is being spent on cv_signal, then these should be tuned
 * down.  If the queues empty before the signalled thread can catch up, then
 * these should be tuned up.
 */
uint64_t zfs_send_queue_ff = 20;
uint64_t zfs_send_no_prefetch_queue_ff = 20;

/*
 * Use this to override the recordsize calculation for fast zfs send estimates.
 */
uint64_t zfs_override_estimate_recordsize = 0;

/* Set this tunable to FALSE to disable setting of DRR_FLAG_FREERECORDS */
int zfs_send_set_freerecords_bit = B_TRUE;

static inline boolean_t
overflow_multiply(uint64_t a, uint64_t b, uint64_t *c)
{
	uint64_t temp = a * b;
	if (b != 0 && temp / b != a)
		return (B_FALSE);
	*c = temp;
	return (B_TRUE);
}

/*
 * Return B_TRUE and modifies *out to the span if the span is less than 2^64,
 * returns B_FALSE otherwise.
 */
static inline boolean_t
bp_span(uint32_t datablksz, uint8_t indblkshift, uint64_t level, uint64_t *out)
{
	uint64_t spanb = bp_span_in_blocks(indblkshift, level);
	return (overflow_multiply(spanb, datablksz, out));
}

struct send_thread_arg {
	bqueue_t	q;
	dsl_dataset_t	*ds;		/* Dataset to traverse */
	redaction_list_t *redaction_list;
	struct send_redact_record *current_record;
	uint64_t	fromtxg;	/* Traverse from this txg */
	int		flags;		/* flags to pass to traverse_dataset */
	int		error_code;
	boolean_t	cancel;
	zbookmark_phys_t resume;
	objlist_t	*deleted_objs;
	uint64_t	*num_blocks_visited;
};

struct redact_list_thread_arg {
	boolean_t		cancel;
	bqueue_t		q;
	zbookmark_phys_t	resume;
	redaction_list_t	*rl;
	boolean_t		mark_redact;
	int			error_code;
	uint64_t		*num_blocks_visited;
};

/*
 * A wrapper around struct redact_block so it can be stored in a list_t.
 */
struct redact_block_list_node {
	redact_block_phys_t	block;
	list_node_t		node;
};

struct redact_bookmark_info {
	redact_block_phys_t	rbi_furthest[TXG_SIZE];
	/* Lists of struct redact_block_list_node. */
	list_t			rbi_blocks[TXG_SIZE];
	boolean_t		rbi_synctasc_txg[TXG_SIZE];
	uint64_t		rbi_latest_synctask_txg;
	redaction_list_t	*rbi_redaction_list;
};

struct send_merge_thread_arg {
	bqueue_t			q;
	objset_t			*os;
	struct redact_list_thread_arg	*from_arg;
	struct send_thread_arg		*to_arg;
	struct redact_list_thread_arg	*redact_arg;
	int				error;
	boolean_t			cancel;
	struct redact_bookmark_info	rbi;
	/*
	 * If we're resuming a redacted send, then the object/offset from the
	 * resume token may be different from the object/offset that we have
	 * updated the bookmark to.  resume_redact_zb will store the earlier of
	 * the two object/offset pairs, and bookmark_before will be B_TRUE if
	 * resume_redact_zb has the object/offset for resuming the redaction
	 * bookmark, and B_FALSE if resume_redact_zb is storing the
	 * object/offset from the resume token.
	 */
	zbookmark_phys_t		resume_redact_zb;
	boolean_t			bookmark_before;
};

struct send_range {
	boolean_t		eos_marker; /* Marks the end of the stream */
	uint64_t		object;
	uint64_t		start_blkid;
	uint64_t		end_blkid;
	bqueue_node_t		ln;
	enum type {DATA, HOLE, OBJECT, REDACT, PREVIOUSLY_REDACTED} type;
	union {
		struct srd {
			dmu_object_type_t	obj_type;
			uint32_t		datablksz;
			blkptr_t		bp;
		} data;
		struct srh {
			uint32_t		datablksz;
		} hole;
		struct sro {
			/*
			 * This is a pointer because embedding it in the struct
			 * causes these structures to be massively larger for
			 * all range types; this makes the code much less memory
			 * efficient.
			 */
			dnode_phys_t		*dnp;
		} object;
		struct srr {
			uint32_t		datablksz;
		} redact;
	} sru;
};

/*
 * The list of data whose inclusion in a send stream can be pending from
 * one call to backup_cb to another.  Multiple calls to dump_free(),
 * dump_freeobjects(), and dump_redact() can be aggregated into a single
 * DRR_FREE, DRR_FREEOBJECTS, or DRR_REDACT replay record.
 */
typedef enum {
	PENDING_NONE,
	PENDING_FREE,
	PENDING_FREEOBJECTS,
	PENDING_REDACT
} dmu_pendop_t;

typedef struct dmu_send_cookie {
	dmu_replay_record_t *dsc_drr;
	dmu_send_outparams_t *dsc_dso;
	offset_t *dsc_off;
	objset_t *dsc_os;
	zio_cksum_t dsc_zc;
	uint64_t dsc_toguid;
	int dsc_err;
	dmu_pendop_t dsc_pending_op;
	uint64_t dsc_featureflags;
	uint64_t dsc_last_data_object;
	uint64_t dsc_last_data_offset;
	uint64_t dsc_resume_object;
	uint64_t dsc_resume_offset;
	boolean_t dsc_sent_begin;
	boolean_t dsc_sent_end;
} dmu_send_cookie_t;

static void
range_free(struct send_range *range)
{
	if (range->type == OBJECT) {
		kmem_free(range->sru.object.dnp,
		    sizeof (*range->sru.object.dnp));
	}
	kmem_free(range, sizeof (*range));
}

/*
 * For all record types except BEGIN, fill in the checksum (overlaid in
 * drr_u.drr_checksum.drr_checksum).  The checksum verifies everything
 * up to the start of the checksum itself.
 */
static int
dump_record(dmu_send_cookie_t *dscp, void *payload, int payload_len)
{
	dmu_send_outparams_t *dso = dscp->dsc_dso;
	ASSERT3U(offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
	    ==, sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	(void) fletcher_4_incremental_native(dscp->dsc_drr,
	    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
	    &dscp->dsc_zc);
	if (dscp->dsc_drr->drr_type == DRR_BEGIN) {
		dscp->dsc_sent_begin = B_TRUE;
	} else {
		ASSERT(ZIO_CHECKSUM_IS_ZERO(&dscp->dsc_drr->drr_u.
		    drr_checksum.drr_checksum));
		dscp->dsc_drr->drr_u.drr_checksum.drr_checksum = dscp->dsc_zc;
	}
	if (dscp->dsc_drr->drr_type == DRR_END) {
		dscp->dsc_sent_end = B_TRUE;
	}
	(void) fletcher_4_incremental_native(&dscp->dsc_drr->
	    drr_u.drr_checksum.drr_checksum,
	    sizeof (zio_cksum_t), &dscp->dsc_zc);
	*dscp->dsc_off += sizeof (dmu_replay_record_t);
	dscp->dsc_err = dso->dso_outfunc(dscp->dsc_drr,
	    sizeof (dmu_replay_record_t), dso->dso_arg);
	if (dscp->dsc_err != 0)
		return (SET_ERROR(EINTR));
	if (payload_len != 0) {
		*dscp->dsc_off += payload_len;
		/*
		 * payload is null when dso->ryrun == B_TRUE (i.e. when we're
		 * doing a send size calculation)
		 */
		if (payload != NULL) {
			(void) fletcher_4_incremental_native(
			    payload, payload_len, &dscp->dsc_zc);
		}
		dscp->dsc_err = dso->dso_outfunc(payload, payload_len,
		    dso->dso_arg);
		if (dscp->dsc_err != 0)
			return (SET_ERROR(EINTR));
	}
	return (0);
}

/*
 * Fill in the drr_free struct, or perform aggregation if the previous record is
 * also a free record, and the two are adjacent.
 *
 * Note that we send free records even for a full send, because we want to be
 * able to receive a full send as a clone, which requires a list of all the free
 * and freeobject records that were generated on the source.
 */
static int
dump_free(dmu_send_cookie_t *dscp, uint64_t object, uint64_t offset,
    uint64_t length)
{
	struct drr_free *drrf = &(dscp->dsc_drr->drr_u.drr_free);

	/*
	 * When we receive a free record, dbuf_free_range() assumes
	 * that the receiving system doesn't have any dbufs in the range
	 * being freed.  This is always true because there is a one-record
	 * constraint: we only send one WRITE record for any given
	 * object,offset.  We know that the one-record constraint is
	 * true because we always send data in increasing order by
	 * object,offset.
	 *
	 * If the increasing-order constraint ever changes, we should find
	 * another way to assert that the one-record constraint is still
	 * satisfied.
	 */
	ASSERT(object > dscp->dsc_last_data_object ||
	    (object == dscp->dsc_last_data_object &&
	    offset > dscp->dsc_last_data_offset));

	if (length != -1ULL && offset + length < offset)
		length = -1ULL;

	/*
	 * If there is a pending op, but it's not PENDING_FREE, push it out,
	 * since free block aggregation can only be done for blocks of the
	 * same type (i.e., DRR_FREE records can only be aggregated with
	 * other DRR_FREE records.  DRR_FREEOBJECTS records can only be
	 * aggregated with other DRR_FREEOBJECTS records).
	 */
	if (dscp->dsc_pending_op != PENDING_NONE &&
	    dscp->dsc_pending_op != PENDING_FREE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	if (dscp->dsc_pending_op == PENDING_FREE) {
		/*
		 * Check to see whether this free block can be aggregated
		 * with pending one.
		 */
		if (drrf->drr_object == object && drrf->drr_offset +
		    drrf->drr_length == offset) {
			if (length == UINT64_MAX)
				drrf->drr_length = UINT64_MAX;
			else
				drrf->drr_length += length;
			return (0);
		} else {
			/* not a continuation.  Push out pending record */
			if (dump_record(dscp, NULL, 0) != 0)
				return (SET_ERROR(EINTR));
			dscp->dsc_pending_op = PENDING_NONE;
		}
	}
	/* create a FREE record and make it pending */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_FREE;
	drrf->drr_object = object;
	drrf->drr_offset = offset;
	drrf->drr_length = length;
	drrf->drr_toguid = dscp->dsc_toguid;
	if (length == -1ULL) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
	} else {
		dscp->dsc_pending_op = PENDING_FREE;
	}

	return (0);
}

/*
 * Fill in the drr_redact struct, or perform aggregation if the previous record
 * is also a redaction record, and the two are adjacent.
 */
static int
dump_redact(dmu_send_cookie_t *dscp, uint64_t object, uint64_t offset,
    uint64_t length)
{
	struct drr_redact *drrr = &dscp->dsc_drr->drr_u.drr_redact;

	/*
	 * If there is a pending op, but it's not PENDING_REDACT, push it out,
	 * since free block aggregation can only be done for blocks of the
	 * same type (i.e., DRR_REDACT records can only be aggregated with
	 * other DRR_REDACT records).
	 */
	if (dscp->dsc_pending_op != PENDING_NONE &&
	    dscp->dsc_pending_op != PENDING_REDACT) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	if (dscp->dsc_pending_op == PENDING_REDACT) {
		/*
		 * Check to see whether this redacted block can be aggregated
		 * with pending one.
		 */
		if (drrr->drr_object == object && drrr->drr_offset +
		    drrr->drr_length == offset) {
			drrr->drr_length += length;
			return (0);
		} else {
			/* not a continuation.  Push out pending record */
			if (dump_record(dscp, NULL, 0) != 0)
				return (SET_ERROR(EINTR));
			dscp->dsc_pending_op = PENDING_NONE;
		}
	}
	/* create a REDACT record and make it pending */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_REDACT;
	drrr->drr_object = object;
	drrr->drr_offset = offset;
	drrr->drr_length = length;
	drrr->drr_toguid = dscp->dsc_toguid;
	dscp->dsc_pending_op = PENDING_REDACT;

	return (0);
}

static int
dump_write(dmu_sendarg_t *dsp, dmu_object_type_t type, uint64_t object,
    uint64_t offset, int lsize, int psize, const blkptr_t *bp, void *data)
{
	uint64_t payload_size;
	boolean_t raw = (dsp->dsa_featureflags & DMU_BACKUP_FEATURE_RAW);
	struct drr_write *drrw = &(dscp->dsc_drr->drr_u.drr_write);

	/*
	 * We send data in increasing object, offset order.
	 * See comment in dump_free() for details.
	 */
	ASSERT(object > dscp->dsc_last_data_object ||
	    (object == dscp->dsc_last_data_object &&
	    offset > dscp->dsc_last_data_offset));
	dscp->dsc_last_data_object = object;
	dscp->dsc_last_data_offset = offset + lsize - 1;

	/*
	 * If there is any kind of pending aggregation (currently either
	 * a grouping of free objects or free blocks), push it out to
	 * the stream, since aggregation can't be done across operations
	 * of different types.
	 */
	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}
	/* write a WRITE record */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_WRITE;
	drrw->drr_object = object;
	drrw->drr_type = type;
	drrw->drr_offset = offset;
	drrw->drr_toguid = dscp->dsc_toguid;
	drrw->drr_logical_size = lsize;

	/* only set the compression fields if the buf is compressed or raw */
	if (raw || lsize != psize) {
		ASSERT(raw || dsp->dsa_featureflags & DMU_BACKUP_FEATURE_COMPRESSED);
		ASSERT(!BP_IS_EMBEDDED(bp));
		ASSERT3S(psize, >, 0);

		if (raw) {
			ASSERT(BP_IS_PROTECTED(bp));

			/*
			 * This is a raw protected block so we need to pass
			 * along everything the receiving side will need to
			 * interpret this block, including the byteswap, salt,
			 * IV, and MAC.
			 */
			if (BP_SHOULD_BYTESWAP(bp))
				drrw->drr_flags |= DRR_RAW_BYTESWAP;
			zio_crypt_decode_params_bp(bp, drrw->drr_salt,
			    drrw->drr_iv);
			zio_crypt_decode_mac_bp(bp, drrw->drr_mac);
		} else {
			/* this is a compressed block */
			ASSERT(dsp->dsa_featureflags &
			    DMU_BACKUP_FEATURE_COMPRESSED);
			ASSERT(!BP_SHOULD_BYTESWAP(bp));
			ASSERT(!DMU_OT_IS_METADATA(BP_GET_TYPE(bp)));
			ASSERT3U(BP_GET_COMPRESS(bp), !=, ZIO_COMPRESS_OFF);
			ASSERT3S(lsize, >=, psize);
		}

		/* set fields common to compressed and raw sends */
		drrw->drr_compressiontype = BP_GET_COMPRESS(bp);
		drrw->drr_compressed_size = psize;
		payload_size = drrw->drr_compressed_size;
	} else {
		payload_size = drrw->drr_logical_size;
	}

	if (bp == NULL || BP_IS_EMBEDDED(bp) || (BP_IS_PROTECTED(bp) && !raw)) {
		/*
		 * There's no pre-computed checksum for partial-block writes,
		 * embedded BP's, or encrypted BP's that are being sent as
		 * plaintext, so (like fletcher4-checkummed blocks) userland
		 * will have to compute a dedup-capable checksum itself.
		 */
		drrw->drr_checksumtype = ZIO_CHECKSUM_OFF;
	} else {
		drrw->drr_checksumtype = BP_GET_CHECKSUM(bp);
		if (zio_checksum_table[drrw->drr_checksumtype].ci_flags &
		    ZCHECKSUM_FLAG_DEDUP)
			drrw->drr_flags |= DRR_CHECKSUM_DEDUP;
		DDK_SET_LSIZE(&drrw->drr_key, BP_GET_LSIZE(bp));
		DDK_SET_PSIZE(&drrw->drr_key, BP_GET_PSIZE(bp));
		DDK_SET_COMPRESS(&drrw->drr_key, BP_GET_COMPRESS(bp));
		DDK_SET_CRYPT(&drrw->drr_key, BP_IS_PROTECTED(bp));
		drrw->drr_key.ddk_cksum = bp->blk_cksum;
	}

	if (dump_record(dscp, data, payload_size) != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static int
dump_write_embedded(dmu_send_cookie_t *dscp, uint64_t object, uint64_t offset,
    int blksz, const blkptr_t *bp)
{
	char buf[BPE_PAYLOAD_SIZE];
	struct drr_write_embedded *drrw =
	    &(dscp->dsc_drr->drr_u.drr_write_embedded);

	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (EINTR);
		dscp->dsc_pending_op = PENDING_NONE;
	}

	ASSERT(BP_IS_EMBEDDED(bp));

	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_WRITE_EMBEDDED;
	drrw->drr_object = object;
	drrw->drr_offset = offset;
	drrw->drr_length = blksz;
	drrw->drr_toguid = dscp->dsc_toguid;
	drrw->drr_compression = BP_GET_COMPRESS(bp);
	drrw->drr_etype = BPE_GET_ETYPE(bp);
	drrw->drr_lsize = BPE_GET_LSIZE(bp);
	drrw->drr_psize = BPE_GET_PSIZE(bp);

	decode_embedded_bp_compressed(bp, buf);

	if (dump_record(dscp, buf, P2ROUNDUP(drrw->drr_psize, 8)) != 0)
		return (EINTR);
	return (0);
}

static int
dump_spill(dmu_send_cookie_t *dscp, const blkptr_t *bp, uint64_t object,
    int blksz, void *data)
{
	struct drr_spill *drrs = &(dscp->dsc_drr->drr_u.drr_spill);
	uint64_t blksz = BP_GET_LSIZE(bp);

	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	/* write a SPILL record */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_SPILL;
	drrs->drr_object = object;
	drrs->drr_length = blksz;
	drrs->drr_toguid = dscp->dsc_toguid;

	/* handle raw send fields */
	if (dscp->dsc_featureflags & DMU_BACKUP_FEATURE_RAW) {
		ASSERT(BP_IS_PROTECTED(bp));

		if (BP_SHOULD_BYTESWAP(bp))
			drrs->drr_flags |= DRR_RAW_BYTESWAP;
		drrs->drr_compressiontype = BP_GET_COMPRESS(bp);
		drrs->drr_compressed_size = BP_GET_PSIZE(bp);
		zio_crypt_decode_params_bp(bp, drrs->drr_salt, drrs->drr_iv);
		zio_crypt_decode_mac_bp(bp, drrs->drr_mac);
	}

	if (dump_record(dscp, data, blksz) != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static int
dump_freeobjects(dmu_send_cookie_t *dscp, uint64_t firstobj, uint64_t numobjs)
{
	struct drr_freeobjects *drrfo = &(dscp->dsc_drr->drr_u.drr_freeobjects);

	/*
	 * If there is a pending op, but it's not PENDING_FREEOBJECTS,
	 * push it out, since free block aggregation can only be done for
	 * blocks of the same type (i.e., DRR_FREE records can only be
	 * aggregated with other DRR_FREE records.  DRR_FREEOBJECTS records
	 * can only be aggregated with other DRR_FREEOBJECTS records).
	 */
	if (dscp->dsc_pending_op != PENDING_NONE &&
	    dscp->dsc_pending_op != PENDING_FREEOBJECTS) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}
	if (numobjs == 0)
		numobjs = UINT64_MAX - firstobj;
	
	if (dscp->dsc_pending_op == PENDING_FREEOBJECTS) {
		/*
		 * See whether this free object array can be aggregated
		 * with pending one
		 */
		if (drrfo->drr_firstobj + drrfo->drr_numobjs == firstobj) {
			drrfo->drr_numobjs += numobjs;
			return (0);
		} else {
			/* can't be aggregated.  Push out pending record */
			if (dump_record(dscp, NULL, 0) != 0)
				return (SET_ERROR(EINTR));
			dscp->dsc_pending_op = PENDING_NONE;
		}
	}

	/* write a FREEOBJECTS record */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_FREEOBJECTS;
	drrfo->drr_firstobj = firstobj;
	drrfo->drr_numobjs = numobjs;
	drrfo->drr_toguid = dscp->dsc_toguid;

	dscp->dsc_pending_op = PENDING_FREEOBJECTS;

	return (0);
}

static int
dump_dnode(dmu_send_cookie_t *dscp, const blkptr_t *bp, uint64_t object,
    dnode_phys_t *dnp)
{
	struct drr_object *drro = &(dscp->dsc_drr->drr_u.drr_object);
	int bonuslen;

	if (object < dscp->dsc_resume_object) {
		/*
		 * Note: when resuming, we will visit all the dnodes in
		 * the block of dnodes that we are resuming from.  In
		 * this case it's unnecessary to send the dnodes prior to
		 * the one we are resuming from.  We should be at most one
		 * block's worth of dnodes behind the resume point.
		 */
		ASSERT3U(dscp->dsc_resume_object - object, <,
		    1 << (DNODE_BLOCK_SHIFT - DNODE_SHIFT));
		return (0);
	}

	if (dnp == NULL || dnp->dn_type == DMU_OT_NONE)
		return (dump_freeobjects(dscp, object, 1));

	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	/* write an OBJECT record */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_OBJECT;
	drro->drr_object = object;
	drro->drr_type = dnp->dn_type;
	drro->drr_bonustype = dnp->dn_bonustype;
	drro->drr_blksz = dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	drro->drr_bonuslen = dnp->dn_bonuslen;
	drro->drr_checksumtype = dnp->dn_checksum;
	drro->drr_compress = dnp->dn_compress;
	drro->drr_toguid = dscp->dsc_toguid;

	if (!(dscp->dsc_featureflags & DMU_BACKUP_FEATURE_LARGE_BLOCKS) &&
	    drro->drr_blksz > SPA_OLD_MAXBLOCKSIZE)
		drro->drr_blksz = SPA_OLD_MAXBLOCKSIZE;

	bonuslen = P2ROUNDUP(dnp->dn_bonuslen, 8);

	if ((dsp->dsa_featureflags & DMU_BACKUP_FEATURE_RAW)) {
		ASSERT(BP_IS_ENCRYPTED(bp));

		if (BP_SHOULD_BYTESWAP(bp))
			drro->drr_flags |= DRR_RAW_BYTESWAP;

		/* needed for reconstructing dnp on recv side */
		drro->drr_maxblkid = dnp->dn_maxblkid;
		drro->drr_indblkshift = dnp->dn_indblkshift;
		drro->drr_nlevels = dnp->dn_nlevels;
		drro->drr_nblkptr = dnp->dn_nblkptr;

		/*
		 * Since we encrypt the entire bonus area, the (raw) part
		 * beyond the bonuslen is actually nonzero, so we need
		 * to send it.
		 */
		if (bonuslen != 0) {
			drro->drr_raw_bonuslen = DN_MAX_BONUS_LEN(dnp);
			bonuslen = drro->drr_raw_bonuslen;
		}
	}

	if (dump_record(dscp, DN_BONUS(dnp), bonuslen) != 0)
		return (SET_ERROR(EINTR));

	/* Free anything past the end of the file. */
	if (dump_free(dscp, object, (dnp->dn_maxblkid + 1) *
	    (dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT), -1ULL) != 0)
		return (SET_ERROR(EINTR));
	if (dscp->dsc_err != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static int
dump_object_range(dmu_sendarg_t *dsp, const blkptr_t *bp, uint64_t firstobj,
    uint64_t numslots)
{
	struct drr_object_range *drror =
	    &(dsp->dsa_drr->drr_u.drr_object_range);

	/* we only use this record type for raw sends */
	ASSERT(BP_IS_PROTECTED(bp));
	ASSERT(dsp->dsa_featureflags & DMU_BACKUP_FEATURE_RAW);
	ASSERT3U(BP_GET_COMPRESS(bp), ==, ZIO_COMPRESS_OFF);
	ASSERT3U(BP_GET_TYPE(bp), ==, DMU_OT_DNODE);
	ASSERT0(BP_GET_LEVEL(bp));

	if (dsp->dsa_pending_op != PENDING_NONE) {
		if (dump_record(dsp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dsp->dsa_pending_op = PENDING_NONE;
	}

	bzero(dsp->dsa_drr, sizeof (dmu_replay_record_t));
	dsp->dsa_drr->drr_type = DRR_OBJECT_RANGE;
	drror->drr_firstobj = firstobj;
	drror->drr_numslots = numslots;
	drror->drr_toguid = dsp->dsa_toguid;
	if (BP_SHOULD_BYTESWAP(bp))
		drror->drr_flags |= DRR_RAW_BYTESWAP;
	zio_crypt_decode_params_bp(bp, drror->drr_salt, drror->drr_iv);
	zio_crypt_decode_mac_bp(bp, drror->drr_mac);

	if (dump_record(dsp, NULL, 0) != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static boolean_t
send_do_embed(dmu_send_cookie_t *dscp, const blkptr_t *bp)
{
	if (!BP_IS_EMBEDDED(bp))
		return (B_FALSE);

	/*
	 * Compression function must be legacy, or explicitly enabled.
	 */
	if ((BP_GET_COMPRESS(bp) >= ZIO_COMPRESS_LEGACY_FUNCTIONS &&
	    !(dscp->dsc_featureflags & DMU_BACKUP_FEATURE_LZ4)))
		return (B_FALSE);

	/*
	 * Embed type must be explicitly enabled.
	 */
	switch (BPE_GET_ETYPE(bp)) {
	case BP_EMBEDDED_TYPE_DATA:
		if (dscp->dsc_featureflags & DMU_BACKUP_FEATURE_EMBED_DATA)
			return (B_TRUE);
		break;
	default:
		return (B_FALSE);
	}
	return (B_FALSE);
}

/*
 * This function actually handles figuring out what kind of record needs to be
 * dumped, reading the data (which has hopefully been prefetched), and calling
 * the appropriate helper function.
 */
static int
do_dump(dmu_send_cookie_t *dscp, struct send_range *range)
{
	int err = 0;
	switch (range->type) {
	case OBJECT:
		err = dump_dnode(dscp, range->object, range->sru.object.dnp);
		return (err);
	case REDACT: {
		struct srr *srrp = &range->sru.redact;
		err = dump_redact(dscp, range->object, range->start_blkid *
		    srrp->datablksz, (range->end_blkid - range->start_blkid) *
		    srrp->datablksz);
		return (err);
	}
	case DATA: {
		struct srd *srdp = &range->sru.data;
		blkptr_t *bp = &srdp->bp;
		spa_t *spa =
		    dmu_objset_spa(dscp->dsc_os);
		ASSERT3U(srdp->datablksz, ==, BP_GET_LSIZE(bp));
		ASSERT3U(range->start_blkid + 1, ==, range->end_blkid);
		if (BP_GET_TYPE(bp) == DMU_OT_SA) {
			arc_flags_t aflags = ARC_FLAG_WAIT;
			enum zio_flag zioflags = ZIO_FLAG_CANFAIL;

			if (dsa->dsa_featureflags & DMU_BACKUP_FEATURE_RAW) {
				ASSERT(BP_IS_PROTECTED(bp));
				zioflags |= ZIO_FLAG_RAW;
			}

			arc_buf_t *abuf;
			zbookmark_phys_t zb;
			ASSERT3U(range->start_blkid, ==, DMU_SPILL_BLKID);
			zb.zb_objset = dmu_objset_id(dscp->dsc_os);
			zb.zb_object = range->object;
			zb.zb_level = 0;
			zb.zb_blkid = range->start_blkid;

			if (!dscp->dsc_dso->dso_dryrun && arc_read(NULL, spa,
			    bp, arc_getbuf_func, &abuf, ZIO_PRIORITY_ASYNC_READ,
		    ZIO_PRIORITY_ASYNC_READ, zioflags, &aflags, zb) != 0)
				return (SET_ERROR(EIO));

		err = dump_spill(dsa, bp, zb->zb_object, abuf->b_data);
			arc_buf_destroy(abuf, &abuf);
			return (err);
		}
		if (send_do_embed(dscp, bp)) {
			err = dump_write_embedded(dscp, range->object,
			    range->start_blkid * srdp->datablksz,
			    srdp->datablksz, bp);
			return (err);
		}
		ASSERT(range->object > dscp->dsc_resume_object ||
		    (range->object == dscp->dsc_resume_object &&
		    range->start_blkid * srdp->datablksz >=
		    dscp->dsc_resume_offset));
		/* it's a level-0 block of a regular object */
		arc_flags_t aflags = ARC_FLAG_WAIT;
		arc_buf_t *abuf = NULL;
		uint64_t offset;

		/*
		 * If we have large blocks stored on disk but the send flags
		 * don't allow us to send large blocks, we split the data from
		 * the arc buf into chunks.
		 */
		boolean_t split_large_blocks =
		    srdp->datablksz > SPA_OLD_MAXBLOCKSIZE &&
		    !(dscp->dsc_featureflags & DMU_BACKUP_FEATURE_LARGE_BLOCKS);

		/*
		 * Raw sends require that we always get raw data as it exists
		 * on disk, so we assert that we are not splitting blocks here.
		 */
		boolean_t request_raw =
		    (dsa->dsa_featureflags & DMU_BACKUP_FEATURE_RAW) != 0;

		/*
		 * We should only request compressed data from the ARC if all
		 * the following are true:
		 *  - stream compression was requested
		 *  - we aren't splitting large blocks into smaller chunks
		 *  - the data won't need to be byteswapped before sending
		 *  - this isn't an embedded block
		 *  - this isn't metadata (if receiving on a different endian
		 *    system it can be byteswapped more easily)
		 */
		boolean_t request_compressed =
		    (dscp->dsc_featureflags & DMU_BACKUP_FEATURE_COMPRESSED) &&
		    !split_large_blocks && !BP_SHOULD_BYTESWAP(bp) &&
		    !BP_IS_EMBEDDED(bp) && !DMU_OT_IS_METADATA(BP_GET_TYPE(bp));

		IMPLY(request_raw, !split_large_blocks);
		IMPLY(request_raw, BP_IS_PROTECTED(bp));
		if (!dscp->dsc_dso->dso_dryrun) {
			enum zio_flag zioflags = ZIO_FLAG_CANFAIL;

			ASSERT3U(srdp->datablksz, ==, BP_GET_LSIZE(bp));

			if (request_raw)
				zioflags |= ZIO_FLAG_RAW;
			else if (request_compressed)
				zioflags |= ZIO_FLAG_RAW_COMPRESS;
			zb.zb_object = range->object;
			zb.zb_level = 0;
			zb.zb_blkid = range->start_blkid;

			err = arc_read(NULL, spa, bp, arc_getbuf_func, &abuf,
			    ZIO_PRIORITY_ASYNC_READ, zioflags, &aflags, &zb);
		}

		if (err != 0) {
			if (zfs_send_corrupt_data &&
			    !dscp->dsc_dso->dso_dryrun) {
				/* Send a block filled with 0x"zfs badd bloc" */
				abuf = arc_alloc_buf(spa, &abuf, ARC_BUFC_DATA,
				    srdp->datablksz);
				uint64_t *ptr;
				for (ptr = abuf->b_data;
				    (char *)ptr < (char *)abuf->b_data +
				    srdp->datablksz; ptr++)
					*ptr = 0x2f5baddb10cULL;
			} else {
				return (SET_ERROR(EIO));
			}
		}

		offset = range->start_blkid * srdp->datablksz;

		if (split_large_blocks) {
			ASSERT0(arc_is_encrypted(abuf));
			ASSERT3U(arc_get_compression(abuf), ==,
			    ZIO_COMPRESS_OFF);
			char *buf = abuf->b_data;
			while (srdp->datablksz > 0 && err == 0) {
				int n = MIN(srdp->datablksz,
				    SPA_OLD_MAXBLOCKSIZE);
				err = dump_write(dscp, srdp->obj_type,
				    range->object, offset, n, n, NULL, buf);
				offset += n;
				buf += n;
				srdp->datablksz -= n;
			}
		} else {
			int psize;
			if (abuf != NULL) {
				psize = arc_buf_size(abuf);
				if (arc_get_compression(abuf) !=
				    ZIO_COMPRESS_OFF) {
					ASSERT3S(psize, ==, BP_GET_PSIZE(bp));
				}
			} else if (!request_compressed) {
				psize = srdp->datablksz;
			} else {
				psize = BP_GET_PSIZE(bp);
			}
			err = dump_write(dscp, srdp->obj_type, range->object,
			    offset, srdp->datablksz, psize, bp,
			    (abuf == NULL ? NULL : abuf->b_data));
		}
		if (abuf != NULL)
			arc_buf_destroy(abuf, &abuf);
		return (err);
	}
	case HOLE: {
		struct srh *srhp = &range->sru.hole;
		if (range->object == DMU_META_DNODE_OBJECT) {
			uint32_t span = srhp->datablksz >> DNODE_SHIFT;
			uint64_t first_obj = range->start_blkid * span;
			uint64_t numobj = range->end_blkid * span - first_obj;
			return (dump_freeobjects(dscp, first_obj, numobj));
		}
		uint64_t offset = 0;

		/*
		 * If this multiply overflows, we don't need to send this block.
		 * Even if it has a birth time, it can never not be a hole, so
		 * we don't need to send records for it.
		 */
		if (!overflow_multiply(range->start_blkid, srhp->datablksz,
		    &offset)) {
			return (0);
		}
		uint64_t len = 0;

		if (!overflow_multiply(range->end_blkid, srhp->datablksz, &len))
			len = UINT64_MAX;
		len = len - offset;
		return (dump_free(dscp, range->object, offset, len));
	}
	default:
		panic("Invalid range type in do_dump: %d", range->type);
	}
	return (err);
}

struct send_range *
range_alloc(enum type type, uint64_t object, uint64_t start_blkid,
    uint64_t end_blkid, boolean_t eos)
{
	struct send_range *range = kmem_alloc(sizeof (*range), KM_SLEEP);
	range->type = type;
	range->object = object;
	range->start_blkid = start_blkid;
	range->end_blkid = end_blkid;
	range->eos_marker = eos;
	return (range);
}

/*
 * This is the callback function to traverse_dataset that acts as a worker
 * thread for dmu_send_impl.
 */
/*ARGSUSED*/
static int
send_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const struct dnode_phys *dnp, void *arg)
{
	struct send_thread_arg *sta = arg;
	struct send_range *record;

	ASSERT(zb->zb_object == DMU_META_DNODE_OBJECT ||
	    zb->zb_object >= sta->resume.zb_object);
	ASSERT3P(sta->ds, !=, NULL);

	/*
	 * All bps of an encrypted os should have the encryption bit set.
	 * If this is not true it indicates tampering and we report an error.
	 */
	if (dsa->dsa_os->os_encrypted &&
	    !BP_IS_HOLE(bp) && !BP_USES_CRYPT(bp)) {
		spa_log_error(spa, zb);
		zfs_panic_recover("unencrypted block in encrypted "
		    "object set %llu", ds->ds_object);
		return (SET_ERROR(EIO));
	}

	if (sta->cancel)
		return (SET_ERROR(EINTR));
	if (zb->zb_object != DMU_META_DNODE_OBJECT &&
	    DMU_OBJECT_IS_SPECIAL(zb->zb_object))
		return (0);
	atomic_inc_64(sta->num_blocks_visited);

	if (bp == NULL) {
		if (zb->zb_object == DMU_META_DNODE_OBJECT)
			return (0);
		record = range_alloc(OBJECT, zb->zb_object, 0, 0, B_FALSE);
		record->sru.object.dnp = kmem_alloc(sizeof (*dnp), KM_SLEEP);
		*record->sru.object.dnp = *dnp;
		bqueue_enqueue(&sta->q, record, sizeof (*record));
		return (0);
	}
	if (zb->zb_level < 0 || (zb->zb_level > 0 && !BP_IS_HOLE(bp)))
		return (0);
	if (zb->zb_object == DMU_META_DNODE_OBJECT && !BP_IS_HOLE(bp))
		return (0);

	uint64_t span = bp_span_in_blocks(dnp->dn_indblkshift, zb->zb_level);
	uint64_t start;

	/*
	 * If this multiply overflows, we don't need to send this block.
	 * Even if it has a birth time, it can never not be a hole, so
	 * we don't need to send records for it.
	 */
	if (!overflow_multiply(span, zb->zb_blkid, &start) ||
	    (!DMU_OT_IS_METADATA(dnp->dn_type) &&
	    span * zb->zb_blkid > dnp->dn_maxblkid)) {
		ASSERT(BP_IS_HOLE(bp));
		return (0);
	}

	if (zb->zb_blkid == DMU_SPILL_BLKID)
		ASSERT3U(BP_GET_TYPE(bp), ==, DMU_OT_SA);

	record = range_alloc(DATA, zb->zb_object, start, (start + span < start ?
	    0 : start + span), B_FALSE);

	uint64_t datablksz = dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	if (BP_IS_HOLE(bp)) {
		record->type = HOLE;
		record->sru.hole.datablksz = datablksz;
	} else if (BP_IS_REDACTED(bp)) {
		record->type = REDACT;
		record->sru.redact.datablksz = datablksz;
	} else {
		record->type = DATA;
		record->sru.data.datablksz = datablksz;
		record->sru.data.obj_type = dnp->dn_type;
		record->sru.data.bp = *bp;
	}
	bqueue_enqueue(&sta->q, record, sizeof (*record));
	return (0);
}

struct redact_list_cb_arg {
	uint64_t *num_blocks_visited;
	bqueue_t *q;
	boolean_t *cancel;
	boolean_t mark_redact;
};

static int
redact_list_cb(redact_block_phys_t *rb, void *arg)
{
	struct redact_list_cb_arg *rlcap = arg;

	atomic_inc_64(rlcap->num_blocks_visited);
	if (*rlcap->cancel)
		return (-1);

	struct send_range *data = range_alloc(REDACT, rb->rbp_object,
	    rb->rbp_blkid, rb->rbp_blkid + redact_block_get_count(rb), B_FALSE);
	ASSERT3U(data->end_blkid, >, rb->rbp_blkid);
	if (rlcap->mark_redact) {
		data->type = REDACT;
		data->sru.redact.datablksz = redact_block_get_size(rb);
	} else {
		data->type = PREVIOUSLY_REDACTED;
	}
	bqueue_enqueue(rlcap->q, data, sizeof (*data));

	return (0);
}

/*
 * This function kicks off the traverse_dataset.  It also handles setting the
 * error code of the thread in case something goes wrong, and pushes the End of
 * Stream record when the traverse_dataset call has finished.  If there is no
 * dataset to traverse, then we traverse the redaction list provided and enqueue
 * records for that.  If neither is provided, the thread immediately pushes an
 * End of Stream marker.
 */
static void
send_traverse_thread(void *arg)
{
	struct send_thread_arg *st_arg = arg;
	int err = 0;
	struct send_range *data;

	if (st_arg->ds != NULL) {
		ASSERT3P(st_arg->redaction_list, ==, NULL);
		err = traverse_dataset_resume(st_arg->ds,
		    st_arg->fromtxg, &st_arg->resume,
		    st_arg->flags, send_cb, st_arg);
	} else if (st_arg->redaction_list != NULL) {
		struct redact_list_cb_arg rlcba = {0};
		rlcba.cancel = &st_arg->cancel;
		rlcba.num_blocks_visited = st_arg->num_blocks_visited;
		rlcba.q = &st_arg->q;
		rlcba.mark_redact = B_FALSE;
		err = dsl_redaction_list_traverse(st_arg->redaction_list,
		    &st_arg->resume, redact_list_cb, &rlcba);
	}

	if (err != EINTR)
		st_arg->error_code = err;
	data = range_alloc(DATA, 0, 0, 0, B_TRUE);
	bqueue_enqueue_flush(&st_arg->q, data, sizeof (*data));
	thread_exit();
}

/*
 * Utility function that causes End of Stream records to compare after of all
 * others, so that other threads' comparison logic can stay simple.
 */
static int
send_range_after(const struct send_range *from, const struct send_range *to)
{
	if (from->eos_marker == B_TRUE)
		return (1);
	if (to->eos_marker == B_TRUE)
		return (-1);

	uint64_t from_obj = from->object;
	uint64_t from_end_obj = from->object + 1;
	uint64_t to_obj = to->object;
	uint64_t to_end_obj = to->object + 1;
	if (from_obj == 0) {
		ASSERT3U(from->type, ==, HOLE);
		from_obj = from->start_blkid << DNODES_PER_BLOCK_SHIFT;
		from_end_obj = from->end_blkid << DNODES_PER_BLOCK_SHIFT;
	}
	if (to_obj == 0) {
		ASSERT3U(to->type, ==, HOLE);
		to_obj = to->start_blkid << DNODES_PER_BLOCK_SHIFT;
		to_end_obj = to->end_blkid << DNODES_PER_BLOCK_SHIFT;
	}

	if (from_end_obj <= to_obj)
		return (-1);
	if (from_obj >= to_end_obj)
		return (1);
	if (from->type == OBJECT && to->type != OBJECT)
		return (-1);
	if (from->type != OBJECT && to->type == OBJECT)
		return (1);
	if (from->end_blkid <= to->start_blkid)
		return (-1);
	if (from->start_blkid >= to->end_blkid)
		return (1);
	return (0);
}

/*
 * Pop the new data off the queue, check that the records we receive are in
 * the right order, but do not free the old data.  This is used so that the
 * records can be sent on to the main thread without copying the data.
 */
static struct send_range *
get_next_range_nofree(bqueue_t *bq, struct send_range *prev)
{
	struct send_range *next = bqueue_dequeue(bq);
	ASSERT3S(send_range_after(prev, next), ==, -1);
	return (next);
}
    int outfd, uint64_t resumeobj, uint64_t resumeoff,
/*
 * Pop the new data off the queue, check that the records we receive are in
 * the right order, and free the old data.
 */
static struct send_range *
get_next_range(bqueue_t *bq, struct send_range *prev)
{
	struct send_range *next = get_next_range_nofree(bq, prev);
	range_free(prev);
	return (next);
}

static void
redact_list_thread(void *arg)
{
	struct redact_list_thread_arg *rlt_arg = arg;
	struct send_range *record;
	if (rlt_arg->rl != NULL) {
		struct redact_list_cb_arg rlcba = {0};
		rlcba.cancel = &rlt_arg->cancel;
		rlcba.q = &rlt_arg->q;
		rlcba.num_blocks_visited = rlt_arg->num_blocks_visited;
		rlcba.mark_redact = rlt_arg->mark_redact;
		int err = dsl_redaction_list_traverse(rlt_arg->rl,
		    &rlt_arg->resume, redact_list_cb, &rlcba);
		if (err != EINTR)
			rlt_arg->error_code = err;
	}
	record = range_alloc(DATA, 0, 0, 0, B_TRUE);
	bqueue_enqueue_flush(&rlt_arg->q, record, sizeof (*record));
}

/*
 * Compare the start point of the two provided ranges. End of stream ranges
 * compare last, objects compare before any data or hole inside that object and
 * multi-object holes that start at the same object.
 */
static int
send_range_start_compare(struct send_range *r1, struct send_range *r2)
{
	uint64_t r1_objequiv = r1->object;
	uint64_t r1_l0equiv = r1->start_blkid;
	uint64_t r2_objequiv = r2->object;
	uint64_t r2_l0equiv = r2->start_blkid;
	if (r1->eos_marker)
		return (1);
	if (r2->eos_marker)
		return (-1);
	if (r1->object == 0) {
		r1_objequiv = r1->start_blkid * DNODES_PER_BLOCK;
		r1_l0equiv = 0;
	}
	if (r2->object == 0) {
		r2_objequiv = r2->start_blkid * DNODES_PER_BLOCK;
		r2_l0equiv = 0;
	}

	if (r1_objequiv < r2_objequiv)
		return (-1);
	if (r1_objequiv > r2_objequiv)
		return (1);
	if (r1->type == OBJECT && r2->type != OBJECT)
		return (-1);
	if (r1->type != OBJECT && r2->type == OBJECT)
		return (1);
	if (r1_l0equiv < r2_l0equiv)
		return (-1);
	if (r1_l0equiv > r2_l0equiv)
		return (1);
	return (0);
}

enum q_idx {
	REDACT_IDX,
	TO_IDX,
	FROM_IDX,
	NUM_THREADS
};

/*
 * This function returns the next range the send_merge_thread should operate on.
 * The inputs are two arrays; the first one stores the range at the front of the
 * queues stored in the second one.  The ranges are sorted in descending
 * priority order; the metadata from earlier ranges overrules metadata from
 * later ranges.  out_mask is used to return which threads the ranges came from;
 * bit i is set if ranges[i] started at the same place as the returned range.
 *
 * This code is not hardcoded to compare a specific number of threads; it could
 * be used with any number, just by changing the q_idx enum.
 *
 * The "next range" is the one with the earliest start; if two starts are equal,
 * the highest-priority range is the next to operate on.  If a higher-priority
 * range starts in the middle of the first range, then the first range will be
 * truncated to end where the higher-priority range starts, and we will operate
 * on that one next time.   In this way, we make sure that each block covered by
 * some range gets covered by a returned range, and each block covered is
 * returned using the metadata of the highest-priority range it appears in.
 *
 * For example, if the three ranges at the front of the queues were [2,4),
 * [3,5), and [1,3), then the ranges returned would be [1,2) with the metadata
 * from the third range, [2,4) with the metadata from the first range, and then
 * [4,5) with the metadata from the second.
 */
static struct send_range *
find_next_range(struct send_range **ranges, bqueue_t **qs, uint64_t *out_mask)
{
	int idx = 0; // index of the range with the earliest start
	int i;
	uint64_t bmask = 0;
	for (i = 1; i < NUM_THREADS; i++) {
		if (send_range_start_compare(ranges[i], ranges[idx]) < 0)
			idx = i;
	}
	if (ranges[idx]->eos_marker) {
		struct send_range *ret = range_alloc(DATA, 0, 0, 0, B_TRUE);
		*out_mask = 0;
		return (ret);
	}
	/*
	 * Find all the ranges that start at that same point.
	 */
	for (i = 0; i < NUM_THREADS; i++) {
		if (send_range_start_compare(ranges[i], ranges[idx]) == 0)
			bmask |= 1 << i;
	}
	*out_mask = bmask;
	/*
	 * Find the first start or end point after the start of the first range.
	 */
	uint64_t first_change = ranges[idx]->end_blkid;
	for (i = 0; i < NUM_THREADS; i++) {
		if (i == idx || ranges[i]->eos_marker ||
		    ranges[i]->object > ranges[idx]->object ||
		    ranges[i]->object == DMU_META_DNODE_OBJECT)
			continue;
		ASSERT3U(ranges[i]->object, ==, ranges[idx]->object);
		if (first_change > ranges[i]->start_blkid &&
		    (bmask & (1 << i)) == 0)
			first_change = ranges[i]->start_blkid;
		else if (first_change > ranges[i]->end_blkid)
			first_change = ranges[i]->end_blkid;
	}
	/*
	 * Update all ranges to no longer overlap with the range we're
	 * returning. All such ranges must start at the same place as the range
	 * being returned, and end at or after first_change. Thus we update
	 * their start to first_change. If that makes them size 0, then free
	 * them and pull a new range from that thread.
	 */
	for (i = 0; i < NUM_THREADS; i++) {
		if (i == idx || (bmask & (1 << i)) == 0)
			continue;
		ASSERT3U(first_change, >, ranges[i]->start_blkid);
		ranges[i]->start_blkid = first_change;
		ASSERT3U(ranges[i]->start_blkid, <=, ranges[i]->end_blkid);
		if (ranges[i]->start_blkid == ranges[i]->end_blkid)
			ranges[i] = get_next_range(qs[i], ranges[i]);
	}
	/*
	 * Short-circuit the simple case; if the range doesn't overlap with
	 * anything else, or it only overlaps with things that start at the same
	 * place and are longer, send it on.
	 */
	if (first_change == ranges[idx]->end_blkid) {
		struct send_range *ret = ranges[idx];
		ranges[idx] = get_next_range_nofree(qs[idx], ranges[idx]);
		return (ret);
	}

	/*
	 * Otherwise, return a truncated copy of ranges[idx] and move the start
	 * of ranges[idx] back to first_change.
	 */
	struct send_range *ret = kmem_alloc(sizeof (*ret), KM_SLEEP);
	*ret = *ranges[idx];
	ret->end_blkid = first_change;
	ranges[idx]->start_blkid = first_change;
	return (ret);
}

#define	FROM_AND_REDACT_BITS ((1 << REDACT_IDX) | (1 << FROM_IDX))

/*
 * Merge the results from the from thread and the to thread, and then hand the
 * records off to send_prefetch_thread to prefetch them.  If this is not a
 * send from a redaction bookmark, the from thread will push an end of stream
 * record and stop, and we'll just send everything that was changed in the
 * to_ds since the ancestor's creation txg. If it is, then since
 * traverse_dataset has a canonical order, we can compare each change as
 * they're pulled off the queues.  That will give us a stream that is
 * appropriately sorted, and covers all records.  In addition, we pull the
 * data from the redact_list_thread and use that to determine which blocks
 * should be redacted.
 */
static void
send_merge_thread(void *arg)
{
	struct send_merge_thread_arg *smt_arg = arg;
	struct send_range *front_ranges[NUM_THREADS];
	bqueue_t *queues[NUM_THREADS];
	int err = 0;

	if (smt_arg->redact_arg == NULL) {
		front_ranges[REDACT_IDX] =
		    kmem_zalloc(sizeof (struct send_range), KM_SLEEP);
		front_ranges[REDACT_IDX]->eos_marker = B_TRUE;
		front_ranges[REDACT_IDX]->type = REDACT;
		queues[REDACT_IDX] = NULL;
	} else {
		front_ranges[REDACT_IDX] =
		    bqueue_dequeue(&smt_arg->redact_arg->q);
		queues[REDACT_IDX] = &smt_arg->redact_arg->q;
	}
	front_ranges[TO_IDX] = bqueue_dequeue(&smt_arg->to_arg->q);
	queues[TO_IDX] = &smt_arg->to_arg->q;
	front_ranges[FROM_IDX] = bqueue_dequeue(&smt_arg->from_arg->q);
	queues[FROM_IDX] = &smt_arg->from_arg->q;
	uint64_t mask = 0;
	struct send_range *range;
	for (range = find_next_range(front_ranges, queues, &mask);
	    !range->eos_marker && err == 0 && !smt_arg->cancel;
	    range = find_next_range(front_ranges, queues, &mask)) {
		/*
		 * If the range in question was in both the from redact bookmark
		 * and the bookmark we're using to redact, then don't send it.
		 * It's already redacted on the receiving system, so a redaction
		 * record would be redundant.
		 */
		if ((mask & FROM_AND_REDACT_BITS) == FROM_AND_REDACT_BITS) {
			ASSERT3U(range->type, ==, REDACT);
			range_free(range);
			continue;
		}
		bqueue_enqueue(&smt_arg->q, range, sizeof (*range));

		if (smt_arg->to_arg->error_code != 0) {
			err = smt_arg->to_arg->error_code;
		} else if (smt_arg->from_arg->error_code != 0) {
			err = smt_arg->from_arg->error_code;
		} else if (smt_arg->redact_arg != NULL &&
		    smt_arg->redact_arg->error_code != 0) {
			err = smt_arg->redact_arg->error_code;
		}
	}
	if (smt_arg->cancel && err == 0)
		err = SET_ERROR(EINTR);
	smt_arg->error = err;
	if (smt_arg->error != 0) {
		smt_arg->to_arg->cancel = B_TRUE;
		smt_arg->from_arg->cancel = B_TRUE;
		if (smt_arg->redact_arg != NULL)
			smt_arg->redact_arg->cancel = B_TRUE;
	}
	for (int i = 0; i < NUM_THREADS; i++) {
		while (!front_ranges[i]->eos_marker) {
			front_ranges[i] = get_next_range(queues[i],
			    front_ranges[i]);
		}
		range_free(front_ranges[i]);
	}
	if (range == NULL)
		range = kmem_zalloc(sizeof (*range), KM_SLEEP);
	range->eos_marker = B_TRUE;
	bqueue_enqueue_flush(&smt_arg->q, range, 1);
	thread_exit();
}

struct send_prefetch_thread_arg {
	struct send_merge_thread_arg *smta;
	bqueue_t q;
	boolean_t cancel;
	boolean_t issue_prefetches;
	int error;
};

/*
 * Create a new record with the given values.  If the record is of a type that
 * can be coalesced, and if it can be coalesced with the previous record, then
 * coalesce those and don't push anything out.  If either of those are not true,
 * we push out the pending record and create a new one out of the current
 * record.
 */
static void
enqueue_range(struct send_prefetch_thread_arg *spta, bqueue_t *q, dnode_t *dn,
    uint64_t blkid, blkptr_t *bp, uint32_t datablksz, struct send_range **pendp)
{
	struct send_range *pending = *pendp;
	enum type pending_type = (pending == NULL ? PREVIOUSLY_REDACTED :
	    pending->type);
	enum type new_type = (BP_IS_HOLE(bp) ? HOLE :
	    (BP_IS_REDACTED(bp) ? REDACT : DATA));

	if (pending_type == new_type) {
		pending->end_blkid = blkid;
		return;
	}
	if (pending_type != PREVIOUSLY_REDACTED) {
		bqueue_enqueue(q, pending, sizeof (*pending));
		pending = NULL;
	}
	ASSERT3P(pending, ==, NULL);
	pending = range_alloc(new_type, dn->dn_object, blkid, blkid + 1,
	    B_FALSE);

	if (blkid == DMU_SPILL_BLKID)
		ASSERT3U(BP_GET_TYPE(bp), ==, DMU_OT_SA);

	switch (new_type) {
	case HOLE:
		pending->sru.hole.datablksz = datablksz;
		break;
	case DATA:
		pending->sru.data.datablksz = datablksz;
		pending->sru.data.obj_type = dn->dn_type;
		pending->sru.data.bp = *bp;
		if (spta->issue_prefetches) {
			zbookmark_phys_t zb = {0};
			zb.zb_objset = dmu_objset_id(dn->dn_objset);
			zb.zb_object = dn->dn_object;
			zb.zb_level = 0;
			zb.zb_blkid = blkid;
			arc_flags_t aflags = ARC_FLAG_NOWAIT |
			    ARC_FLAG_PREFETCH;
			(void) arc_read(NULL, dn->dn_objset->os_spa, bp, NULL,
			    NULL, ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL |
			    ZIO_FLAG_SPECULATIVE, &aflags, &zb);
		}
		bqueue_enqueue(q, pending, datablksz);
		pending = NULL;
		break;
	case REDACT:
		pending->sru.redact.datablksz = datablksz;
		break;
	}
	*pendp = pending;
}

/*
 * This thread is responsible for two things: First, it retrieves the correct
 * blkptr in the to ds if we need to send the data because of something from
 * the from thread.  As a result of this, we're the first ones to discover that
 * some indirect blocks can be discarded because they're not holes. Second,
 * it issues prefetches for the data we need to send.
 */
static void
send_prefetch_thread(void *arg)
{
	struct send_prefetch_thread_arg *spta = arg;
	struct send_merge_thread_arg *smta = spta->smta;
	bqueue_t *inq = &smta->q;
	bqueue_t *outq = &spta->q;
	objset_t *os = smta->os;
	struct send_range *range = bqueue_dequeue(inq);
	int err = 0;

	/*
	 * If the record we're analyzing is from a redaction bookmark from the
	 * fromds, then we need to know whether or not it exists in the tods so
	 * we know whether to create records for it or not. If it does, we need
	 * the datablksz so we can generate an appropriate record for it.
	 * Finally, if it isn't redacted, we need the blkptr so that we can send
	 * a WRITE record containing the actual data.
	 */
	uint64_t last_obj = UINT64_MAX;
	uint64_t last_obj_exists = B_TRUE;
	while (!range->eos_marker && !spta->cancel && smta->error == 0) {
		switch (range->type) {
		case DATA: {
			zbookmark_phys_t zb;
			zb.zb_objset = dmu_objset_id(os);
			zb.zb_object = range->object;
			zb.zb_level = 0;
			zb.zb_blkid = range->start_blkid;
			ASSERT3U(range->start_blkid + 1, ==, range->end_blkid);
			if (!BP_IS_REDACTED(&range->sru.data.bp) &&
			    spta->issue_prefetches &&
			    !BP_IS_EMBEDDED(&range->sru.data.bp)) {
				arc_flags_t aflags = ARC_FLAG_NOWAIT |
				    ARC_FLAG_PREFETCH;
				(void) arc_read(NULL, os->os_spa,
				    &range->sru.data.bp, NULL, NULL,
				    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL |
				    ZIO_FLAG_SPECULATIVE, &aflags, &zb);
			}
			bqueue_enqueue(outq, range, range->sru.data.datablksz);
			range = get_next_range_nofree(inq, range);
			break;
		}
		case HOLE:
		case OBJECT:
		case REDACT: // Redacted blocks must exist
			bqueue_enqueue(outq, range, sizeof (*range));
			range = get_next_range_nofree(inq, range);
			break;
		case PREVIOUSLY_REDACTED: {
			/*
			 * This entry came from the "from bookmark" when
			 * sending from a bookmark that has a redaction
			 * list.  We need to check if this object/blkid
			 * exists in the target ("to") dataset, and if
			 * not then we drop this entry.  We also need
			 * to fill in the block pointer so that we know
			 * what to prefetch.
			 *
			 * To accomplish the above, we first cache whether or
			 * not the last object we examined exists.  If it
			 * doesn't, we can drop this record. If it does, we hold
			 * the dnode and use it to call dbuf_dnode_findbp. We do
			 * this instead of dbuf_bookmark_findbp because we will
			 * often operate on large ranges, and holding the dnode
			 * once is more efficient.
			 */
			boolean_t object_exists = B_TRUE;
			/*
			 * If the data is redacted, we only care if it exists,
			 * so that we don't send records for objects that have
			 * been deleted.
			 */
			dnode_t *dn;
			if (range->object == last_obj && !last_obj_exists) {
				/*
				 * If we're still examining the same object as
				 * previously, and it doesn't exist, we don't
				 * need to call dbuf_bookmark_findbp.
				 */
				object_exists = B_FALSE;
			} else {
				err = dnode_hold(os, range->object, FTAG, &dn);
				if (err == ENOENT) {
					object_exists = B_FALSE;
					err = 0;
				}
				last_obj = range->object;
				last_obj_exists = object_exists;
			}

			if (err != 0) {
				break;
			} else if (!object_exists) {
				/*
				 * The block was modified, but doesn't
				 * exist in the to dataset; if it was
				 * deleted in the to dataset, then we'll
				 * visit the hole bp for it at some point.
				 */
				range = get_next_range(inq, range);
				continue;
			}
			struct send_range *pending = NULL;
			uint64_t file_max =
			    (dn->dn_maxblkid < range->end_blkid ?
			    dn->dn_maxblkid : range->end_blkid);
			/*
			 * The object exists, so we need to try to find the
			 * blkptr for each block in the range we're processing.
			 */
			rw_enter(&dn->dn_struct_rwlock, RW_READER);
			for (uint64_t blkid = range->start_blkid;
			    blkid < file_max; blkid++) {
				uint16_t datablkszsec;
				blkptr_t bp;
				err = dbuf_dnode_findbp(dn, 0, blkid, &bp,
				    &datablkszsec, NULL);
				if (err != 0)
					break;
				enqueue_range(spta, outq, dn, blkid, &bp,
				    datablkszsec << SPA_MINBLOCKSHIFT,
				    &pending);
			}
			if (pending != NULL) {
				bqueue_enqueue(outq, pending,
				    sizeof (*pending));
			}
			rw_exit(&dn->dn_struct_rwlock);
			dnode_rele(dn, FTAG);
			range = get_next_range(inq, range);
		}
		}
	}
	if (spta->cancel || err != 0) {
		smta->cancel = B_TRUE;
		spta->error = err;
	} else if (smta->error != 0) {
		spta->error = smta->error;
	}
	while (!range->eos_marker)
		range = get_next_range(inq, range);

	bqueue_enqueue_flush(outq, range, 1);
	thread_exit();
}

#define	NUM_SNAPS_NOT_REDACTED UINT64_MAX

struct dmu_send_params {
	/* Pool args */
	void *tag; // Tag that dp was held with, will be used to release dp.
	dsl_pool_t *dp;
	/* To snapshot args */
	const char *tosnap;
	dsl_dataset_t *to_ds;
	/* From snapshot args */
	zfs_bookmark_phys_t ancestor_zb;
	uint64_t *fromredactsnaps;
	/* NUM_SNAPS_NOT_REDACTED if not sending from redaction bookmark */
	uint64_t numfromredactsnaps;
	/* Stream params */
	boolean_t is_clone;
	boolean_t embedok;
	boolean_t large_block_ok;
	boolean_t compressok;
	uint64_t resumeobj;
	uint64_t resumeoff;
	zfs_bookmark_phys_t *redactbook;
	/* Stream output params */
	dmu_send_outparams_t *dso;

	/* Stream progress params */
	offset_t *off;
	int outfd;
	boolean_t rawok;
};

static int
setup_featureflags(struct dmu_send_params *dspp, objset_t *os,
    uint64_t *featureflags)
{
	dsl_dataset_t *to_ds = dspp->to_ds;
	dsl_pool_t *dp = dspp->dp;
#ifdef _KERNEL
	if (dmu_objset_type(os) == DMU_OST_ZFS) {
		uint64_t version;
		if (zfs_get_zplprop(os, ZFS_PROP_VERSION, &version) != 0)
			return (SET_ERROR(EINVAL));

		if (version >= ZPL_VERSION_SA)
			*featureflags |= DMU_BACKUP_FEATURE_SA_SPILL;
	}
#endif

	/* raw sends imply large_block_ok */
	if ((dspp->rawok || dspp->large_block_ok) &&
		dsl_dataset_feature_is_active(to_ds, SPA_FEATURE_LARGE_BLOCKS)) {
		*featureflags |= DMU_BACKUP_FEATURE_LARGE_BLOCKS;
	}

	/* encrypted datasets will not have embedded blocks */
	if ((dspp->embedok || dspp->rawok) && !os->os_encrypted &&
	    spa_feature_is_active(dp->dp_spa, SPA_FEATURE_EMBEDDED_DATA)) {
		*featureflags |= DMU_BACKUP_FEATURE_EMBED_DATA;
	}

	/* raw send implies compressok */
	if (dspp->compressok || dspp->rawok)
		*featureflags |= DMU_BACKUP_FEATURE_COMPRESSED;
	if (dspp->rawok && os->os_encrypted)
		featureflags |= DMU_BACKUP_FEATURE_RAW;

	if ((*featureflags &
	    (DMU_BACKUP_FEATURE_EMBED_DATA | DMU_BACKUP_FEATURE_COMPRESSED |
	    DMU_BACKUP_FEATURE_RAW)) != 0 &&
	    spa_feature_is_active(dp->dp_spa, SPA_FEATURE_LZ4_COMPRESS)) {
		*featureflags |= DMU_BACKUP_FEATURE_LZ4;
	}

	if (dspp->resumeobj != 0 || dspp->resumeoff != 0) {
		*featureflags |= DMU_BACKUP_FEATURE_RESUMING;
	}

	if (dspp->redactbook != NULL) {
		*featureflags |= DMU_BACKUP_FEATURE_REDACTED;
	}
	return (0);
}

static dmu_replay_record_t *
create_begin_record(struct dmu_send_params *dspp, objset_t *os,
    uint64_t featureflags)
{
	dmu_replay_record_t *drr = kmem_zalloc(sizeof (dmu_replay_record_t),
	    KM_SLEEP);
	drr->drr_type = DRR_BEGIN;

	struct drr_begin *drrb = &drr->drr_u.drr_begin;
	dsl_dataset_t *to_ds = dspp->to_ds;

	drrb->drr_magic = DMU_BACKUP_MAGIC;
	drrb->drr_creation_time = dsl_dataset_phys(to_ds)->ds_creation_time;
	drrb->drr_type = dmu_objset_type(os);
	drrb->drr_toguid = dsl_dataset_phys(to_ds)->ds_guid;
	drrb->drr_fromguid = dspp->ancestor_zb.zbm_guid;

	DMU_SET_STREAM_HDRTYPE(drrb->drr_versioninfo, DMU_SUBSTREAM);
	DMU_SET_FEATUREFLAGS(drrb->drr_versioninfo, featureflags);

	if (dspp->is_clone)
		drrb->drr_flags |= DRR_FLAG_CLONE;
	if (dsl_dataset_phys(dspp->to_ds)->ds_flags & DS_FLAG_CI_DATASET)
		drrb->drr_flags |= DRR_FLAG_CI_DATA;
	if (zfs_send_set_freerecords_bit)
		drrb->drr_flags |= DRR_FLAG_FREERECORDS;

	dsl_dataset_name(to_ds, drrb->drr_toname);
	if (!to_ds->ds_is_snapshot) {
		(void) strlcat(drrb->drr_toname, "@--head--",
		    sizeof (drrb->drr_toname));
	}
	return (drr);
}

static void
setup_to_thread(struct send_thread_arg *to_arg, dsl_dataset_t *to_ds,
    dmu_sendstatus_t *dssp, uint64_t fromtxg)
{
	VERIFY0(bqueue_init(&to_arg->q, zfs_send_no_prefetch_queue_ff,
	    zfs_send_no_prefetch_queue_length,
	    offsetof(struct send_range, ln)));
	to_arg->error_code = 0;
	to_arg->cancel = B_FALSE;
	to_arg->ds = to_ds;
	to_arg->fromtxg = fromtxg;
	to_arg->flags = TRAVERSE_PRE | TRAVERSE_PREFETCH_METADATA;
	if (dspp->rawok)
		to_arg->flags |= TRAVERSE_NO_DECRYPT;
	to_arg->redaction_list = NULL;
	to_arg->num_blocks_visited = &dssp->dss_blocks;
	(void) thread_create(NULL, 0, send_traverse_thread, to_arg, 0,
	    curproc, TS_RUN, minclsyspri);
}

static void
setup_from_thread(struct redact_list_thread_arg *from_arg,
    redaction_list_t *from_rl, dmu_sendstatus_t *dssp)
{
	VERIFY0(bqueue_init(&from_arg->q, zfs_send_no_prefetch_queue_ff,
	    zfs_send_no_prefetch_queue_length,
	    offsetof(struct send_range, ln)));
	from_arg->error_code = 0;
	from_arg->cancel = B_FALSE;
	from_arg->rl = from_rl;
	from_arg->mark_redact = B_FALSE;
	from_arg->num_blocks_visited = &dssp->dss_blocks;
	/*
	 * If from_ds is null, send_traverse_thread just returns success and
	 * enqueues an eos marker.
	 */
	(void) thread_create(NULL, 0, redact_list_thread, from_arg, 0,
	    curproc, TS_RUN, minclsyspri);
}

static void
setup_redact_list_thread(struct redact_list_thread_arg *rlt_arg,
    struct dmu_send_params *dspp, redaction_list_t *rl, dmu_sendstatus_t *dssp)
{
	if (dspp->redactbook == NULL)
		return;

	rlt_arg->cancel = B_FALSE;
	VERIFY0(bqueue_init(&rlt_arg->q, zfs_send_no_prefetch_queue_ff,
	    zfs_send_no_prefetch_queue_length,
	    offsetof(struct send_range, ln)));
	rlt_arg->error_code = 0;
	rlt_arg->mark_redact = B_TRUE;
	rlt_arg->rl = rl;
	rlt_arg->num_blocks_visited = &dssp->dss_blocks;

	(void) thread_create(NULL, 0, redact_list_thread, rlt_arg, 0,
	    curproc, TS_RUN, minclsyspri);
}

static void
setup_merge_thread(struct send_merge_thread_arg *smt_arg,
    struct dmu_send_params *dspp, struct redact_list_thread_arg *from_arg,
    struct send_thread_arg *to_arg, struct redact_list_thread_arg *rlt_arg,
    objset_t *os)
{
	VERIFY0(bqueue_init(&smt_arg->q, zfs_send_no_prefetch_queue_ff,
	    zfs_send_no_prefetch_queue_length,
	    offsetof(struct send_range, ln)));
	smt_arg->cancel = B_FALSE;
	smt_arg->error = 0;
	smt_arg->from_arg = from_arg;
	smt_arg->to_arg = to_arg;
	if (dspp->redactbook != NULL)
		smt_arg->redact_arg = rlt_arg;

	smt_arg->os = os;
	(void) thread_create(NULL, 0, send_merge_thread, smt_arg, 0, curproc,
	    TS_RUN, minclsyspri);
}

static void
setup_prefetch_thread(struct send_prefetch_thread_arg *spt_arg,
    struct dmu_send_params *dspp, struct send_merge_thread_arg *smt_arg)
{
	VERIFY0(bqueue_init(&spt_arg->q, zfs_send_queue_ff,
	    zfs_send_queue_length, offsetof(struct send_range, ln)));
	spt_arg->smta = smt_arg;
	spt_arg->issue_prefetches = !dspp->dso->dso_dryrun;
	(void) thread_create(NULL, 0, send_prefetch_thread, spt_arg, 0,
	    curproc, TS_RUN, minclsyspri);
}

static int
setup_resume_points(struct dmu_send_params *dspp,
    struct send_thread_arg *to_arg, struct redact_list_thread_arg *from_arg,
    struct redact_list_thread_arg *rlt_arg,
    struct send_merge_thread_arg *smt_arg, boolean_t resuming, objset_t *os,
    redaction_list_t *redact_rl, nvlist_t *nvl)
{
	dsl_dataset_t *to_ds = dspp->to_ds;
	int err = 0;

	uint64_t obj = 0;
	uint64_t blkid = 0;
	if (resuming) {
		obj = dspp->resumeobj;
		dmu_object_info_t to_doi;
		err = dmu_object_info(os, obj, &to_doi);
		if (err != 0)
			return (err);

		blkid = dspp->resumeoff / to_doi.doi_data_block_size;
	}
	/*
	 * If we're resuming a redacted send, we can skip to the appropriate
	 * point in the redaction bookmark by binary searching through it.
	 */
	smt_arg->bookmark_before = B_FALSE;
	if (redact_rl != NULL) {
		SET_BOOKMARK(&rlt_arg->resume, to_ds->ds_object, obj, 0, blkid);
	}

	SET_BOOKMARK(&to_arg->resume, to_ds->ds_object, obj, 0, blkid);
	if (nvlist_exists(nvl, BEGINNV_REDACT_FROM_SNAPS)) {
		uint64_t objset = dspp->ancestor_zb.zbm_redaction_obj;
		/*
		 * Note: If the resume point is in an object whose
		 * blocksize is different in the from vs to snapshots,
		 * we will have divided by the "wrong" blocksize.
		 * However, in this case fromsnap's send_cb() will
		 * detect that the blocksize has changed and therefore
		 * ignore this object.
		 *
		 * If we're resuming a send from a redaction bookmark,
		 * we still cannot accidentally suggest blocks behind
		 * the to_ds.  In addition, we know that any blocks in
		 * the object in the to_ds will have to be sent, since
		 * the size changed.  Therefore, we can't cause any harm
		 * this way either.
		 */
		SET_BOOKMARK(&from_arg->resume, objset, obj, 0, blkid);
	}
	if (resuming) {
		fnvlist_add_uint64(nvl, BEGINNV_RESUME_OBJECT, dspp->resumeobj);
		fnvlist_add_uint64(nvl, BEGINNV_RESUME_OFFSET, dspp->resumeoff);
	}
	return (0);
}

static dmu_sendstatus_t *
setup_send_progress(struct dmu_send_params *dspp)
{
	dmu_sendstatus_t *dssp = kmem_zalloc(sizeof (*dssp), KM_SLEEP);
	dssp->dss_outfd = dspp->outfd;
	dssp->dss_off = dspp->off;
	dssp->dss_proc = curproc;
	mutex_enter(&dspp->to_ds->ds_sendstream_lock);
	list_insert_head(&dspp->to_ds->ds_sendstreams, dssp);
	mutex_exit(&dspp->to_ds->ds_sendstream_lock);
	return (dssp);
}

/*
 * Actually do the bulk of the work in a zfs send.
 *
 * The idea is that we want to do a send from ancestor_zb to to_ds.  We also
 * want to not send any data that has been modified by all the datasets in
 * redactsnaparr, and store the list of blocks that are redacted in this way in
 * a bookmark named redactbook, created on the to_ds.  We do this by creating
 * several worker threads, whose function is described below.
 *
 * There are three cases.
 * The first case is a redacted zfs send.  In this case there are 5 threads.
 * The first thread is the to_ds traversal thread: it calls dataset_traverse on
 * the to_ds and finds all the blocks that have changed since ancestor_zb (if
 * it's a full send, that's all blocks in the dataset).  It then sends those
 * blocks on to the send merge thread. The redact list thread takes the data
 * from the redaction bookmark and sends those blocks on to the send merge
 * thread.  The send merge thread takes the data from the to_ds traversal
 * thread, and combines it with the redaction records from the redact list
 * thread.  If a block appears in both the to_ds's data and the redaction data,
 * the send merge thread will mark it as redacted and send it on to the prefetch
 * thread.  Otherwise, the send merge thread will send the block on to the
 * prefetch thread unchanged. The prefetch thread will issue prefetch reads for
 * any data that isn't redacted, and then send the data on to the main thread.
 * The main thread behaves the same as in a normal send case, issuing demand
 * reads for data blocks and sending out records over the network
 *
 * The graphic below diagrams the flow of data in the case of a redacted zfs
 * send.  Each box represents a thread, and each line represents the flow of
 * data.
 *
 *             Records from the |
 *           redaction bookmark |
 * +--------------------+       |  +---------------------------+
 * |                    |       v  | Send Merge Thread         |
 * | Redact List Thread +----------> Apply redaction marks to  |
 * |                    |          | records as specified by   |
 * +--------------------+          | redaction ranges          |
 *                                 +----^---------------+------+
 *                                      |               | Merged data
 *                                      |               |
 *                                      |  +------------v--------+
 *                                      |  | Prefetch Thread     |
 * +--------------------+               |  | Issues prefetch     |
 * | to_ds Traversal    |               |  | reads of data blocks|
 * | Thread (finds      +---------------+  +------------+--------+
 * | candidate blocks)  |  Blocks modified              | Prefetched data
 * +--------------------+  by to_ds since               |
 *                         ancestor_zb     +------------v----+
 *                                         | Main Thread     |  File Descriptor
 *                                         | Sends data over +->(to zfs receive)
 *                                         | wire            |
 *                                         +-----------------+
 *
 * The second case is an incremental send from a redaction bookmark.  The to_ds
 * traversal thread and the main thread behave the same as in the redacted
 * send case.  The new thread is the from bookmark traversal thread.  It
 * iterates over the redaction list in the redaction bookmark, and enqueues
 * records for each block that was redacted in the original send.  The send
 * merge thread now has to merge the data from the two threads.  For details
 * about that process, see the header comment of send_merge_thread().  Any data
 * it decides to send on will be prefetched by the prefetch thread.  Note that
 * you can perform a redacted send from a redaction bookmark; in that case,
 * the data flow behaves very similarly to the flow in the redacted send case,
 * except with the addition of the bookmark traversal thread iterating over the
 * redaction bookmark.  The send_merge_thread also has to take on the
 * responsibility of merging the redact list thread's records, the bookmark
 * traversal thread's records, and the to_ds records.
 *
 * +---------------------+
 * |                     |
 * | Redact List Thread  +--------------+
 * |                     |              |
 * +---------------------+              |
 *        Blocks in redaction list      | Ranges modified by every secure snap
 *        of from bookmark              | (or EOS if not readcted)
 *                                      |
 * +---------------------+   |     +----v----------------------+
 * | bookmark Traversal  |   v     | Send Merge Thread         |
 * | Thread (finds       +---------> Merges bookmark, rlt, and |
 * | candidate blocks)   |         | to_ds send records        |
 * +---------------------+         +----^---------------+------+
 *                                      |               | Merged data
 *                                      |  +------------v--------+
 *                                      |  | Prefetch Thread     |
 * +--------------------+               |  | Issues prefetch     |
 * | to_ds Traversal    |               |  | reads of data blocks|
 * | Thread (finds      +---------------+  +------------+--------+
 * | candidate blocks)  |  Blocks modified              | Prefetched data
 * +--------------------+  by to_ds since  +------------v----+
 *                         ancestor_zb     | Main Thread     |  File Descriptor
 *                                         | Sends data over +->(to zfs receive)
 *                                         | wire            |
 *                                         +-----------------+
 *
 * The final case is a simple zfs full or incremental send.  The to_ds traversal
 * thread behaves the same as always. The redact list thread is never started.
 * The send merge thread takes all the blocks that the to_ds traveral thread
 * sends it, prefetches the data, and sends the blocks on to the main thread.
 * The main thread sends the data over the wire.
 *
 * To keep performance acceptable, we want to prefetch the data in the worker
 * threads.  While the to_ds thread could simply use the TRAVERSE_PREFETCH
 * feature built into traverse_dataset, the combining and deletion of records
 * due to redaction and sends from redaction bookmarks mean that we could
 * issue many unnecessary prefetches.  As a result, we only prefetch data
 * after we've determined that the record is not going to be redacted.  To
 * prevent the prefetching from getting too far ahead of the main thread, the
 * blocking queues that are used for communication are capped not by the
 * number of entries in the queue, but by the sum of the size of the
 * prefetches associated with them.  The limit on the amount of data that the
 * thread can prefetch beyond what the main thread has reached is controlled
 * by the global variable zfs_send_queue_length.  In addition, to prevent poor
 * performance in the beginning of a send, we also limit the distance ahead
 * that the traversal threads can be.  That distance is controlled by the
 * zfs_send_no_prefetch_queue_length tunable.
 *
 * Note: Releases dp using the specified tag.
 */
static int
dmu_send_impl(struct dmu_send_params *dspp)
{
	objset_t *os;
	dmu_replay_record_t *drr;
	dmu_sendstatus_t *dssp;
	dmu_send_cookie_t dsc = {0};
	int err;
	uint64_t fromtxg = dspp->ancestor_zb.zbm_creation_txg;
	uint64_t featureflags = 0;
	struct redact_list_thread_arg from_arg = { 0 };
	struct send_thread_arg to_arg = { 0 };
	struct redact_list_thread_arg rlt_arg = { 0 };
	struct send_merge_thread_arg smt_arg = { 0 };
	struct send_prefetch_thread_arg spt_arg = { 0 };
	struct send_range *range;
	redaction_list_t *from_rl = NULL;
	redaction_list_t *redact_rl = NULL;
	boolean_t resuming = (dspp->resumeobj != 0 || dspp->resumeoff != 0);
	boolean_t book_resuming = resuming;

	dsl_dataset_t *to_ds = dspp->to_ds;
	zfs_bookmark_phys_t *ancestor_zb = &dspp->ancestor_zb;
	dsl_pool_t *dp = dspp->dp;
	void *tag = dspp->tag;

	err = dmu_objset_from_ds(to_ds, &os);
	if (err != 0) {
		dsl_pool_rele(dp, tag);
		return (err);
	}
	/*
	 * If this is a non-raw send of an encrypted ds, we can ensure that
	 * the objset_phys_t is authenticated. This is safe because this is
	 * either a snapshot or we have owned the dataset, ensuring that
	 * it can't be modified.
	 */
	if (!rawok && os->os_encrypted &&
	    arc_is_unauthenticated(os->os_phys_buf)) {
		err = arc_untransform(os->os_phys_buf, os->os_spa,
		    to_ds->ds_object, B_FALSE);
		if (err != 0) {
			dsl_pool_rele(dp, tag);
			return (err);
		}

		ASSERT0(arc_is_unauthenticated(os->os_phys_buf));
	}

	if ((err = setup_featureflags(dspp, os, &featureflags)) != 0) {
		dsl_pool_rele(dp, tag);
		return (err);
	}

	/*
	 * If we're doing a redacted send, hold the bookmark's redaction list.
	 */
	if (dspp->redactbook != NULL) {
		err = dsl_redaction_list_hold_obj(dp,
		    dspp->redactbook->zbm_redaction_obj, FTAG,
		    &redact_rl);
		if (err != 0) {
			dsl_pool_rele(dp, tag);
			return (SET_ERROR(EINVAL));
		}
		dsl_redaction_list_long_hold(dp, redact_rl, FTAG);
	}

	/*
	 * If we're sending from a redaction bookmark, hold the redaction list
	 * so that we can consider sending the redacted blocks.
	 */
	if (ancestor_zb->zbm_redaction_obj != 0) {
		err = dsl_redaction_list_hold_obj(dp,
		    ancestor_zb->zbm_redaction_obj, FTAG, &from_rl);
		if (err != 0) {
			if (redact_rl != NULL) {
				dsl_redaction_list_long_rele(redact_rl, FTAG);
				dsl_redaction_list_rele(redact_rl, FTAG);
			}
			dsl_pool_rele(dp, tag);
			return (SET_ERROR(EINVAL));
		}
		dsl_redaction_list_long_hold(dp, from_rl, FTAG);
	}

	dsl_dataset_long_hold(to_ds, FTAG);

	drr = create_begin_record(dspp, os, featureflags);
	dssp = setup_send_progress(dspp);

	dsc.dsc_drr = drr;
	dsc.dsc_dso = dspp->dso;
	dsc.dsc_os = os;
	dsc.dsc_off = dspp->off;
	dsc.dsc_toguid = dsl_dataset_phys(to_ds)->ds_guid;
	dsc.dsc_pending_op = PENDING_NONE;
	dsc.dsc_featureflags = featureflags;
	dsc.dsc_resume_object = dspp->resumeobj;
	dsc.dsc_resume_offset = dspp->resumeoff;

	dsl_pool_rele(dp, tag);

	void *payload = NULL;
	size_t payload_len = 0;
	nvlist_t *nvl = fnvlist_alloc();

	/*
	 * If we're doing a redacted send, we include the snapshots we're
	 * redacted with respect to so that the target system knows what send
	 * streams can be correctly received on top of this dataset. If we're
	 * instead sending a redacted dataset, we include the snapshots that the
	 * dataset was created with respect to.
	 */
	if (dspp->redactbook != NULL) {
		fnvlist_add_uint64_array(nvl, BEGINNV_REDACT_SNAPS,
		    redact_rl->rl_phys->rlp_snaps,
		    redact_rl->rl_phys->rlp_num_snaps);
	} else if (dsl_dataset_feature_is_active(to_ds,
	    SPA_FEATURE_REDACTED_DATASETS)) {
		uint64_t *tods_guids;
		uint64_t length;
		VERIFY(dsl_dataset_get_uint64_array_feature(to_ds,
		    SPA_FEATURE_REDACTED_DATASETS, &length, &tods_guids));
		fnvlist_add_uint64_array(nvl, BEGINNV_REDACT_SNAPS, tods_guids,
		    length);
	}

	/*
	 * If we're sending from a redaction bookmark, then we should retrieve
	 * the guids of that bookmark so we can send them over the wire.
	 */
	if (from_rl != NULL) {
		fnvlist_add_uint64_array(nvl, BEGINNV_REDACT_FROM_SNAPS,
		    from_rl->rl_phys->rlp_snaps,
		    from_rl->rl_phys->rlp_num_snaps);
	}

	/*
	 * If the snapshot we're sending from is redacted, include the redaction
	 * list in the stream.
	 */
	if (dspp->numfromredactsnaps != NUM_SNAPS_NOT_REDACTED) {
		ASSERT3P(from_rl, ==, NULL);
		fnvlist_add_uint64_array(nvl, BEGINNV_REDACT_FROM_SNAPS,
		    dspp->fromredactsnaps, (uint_t)dspp->numfromredactsnaps);
		if (dspp->numfromredactsnaps > 0) {
			kmem_free(dspp->fromredactsnaps,
			    dspp->numfromredactsnaps * sizeof (uint64_t));
			dspp->fromredactsnaps = NULL;
		}
	}

	if (resuming || book_resuming) {
		err = setup_resume_points(dspp, &to_arg, &from_arg,
		    &rlt_arg, &smt_arg, resuming, os, redact_rl, nvl);
		if (err != 0)
			goto out;
	}
	
	if (featureflags & DMU_BACKUP_FEATURE_RAW) {
		nvlist_t *keynvl = NULL;
		ASSERT(os->os_encrypted);

		err = dsl_crypto_populate_key_nvlist(to_ds, &keynvl);
		if (err != 0) {
			fnvlist_free(nvl);
			goto out;
		}

		fnvlist_add_nvlist(nvl, "crypt_keydata", keynvl);
		fnvlist_free(keynvl);
	}

	if (!nvlist_empty(nvl)) {

		payload = fnvlist_pack(nvl, &payload_len);
		drr->drr_payloadlen = payload_len;
	}

	fnvlist_free(nvl);
	err = dump_record(&dsc, payload, payload_len);
	fnvlist_pack_free(payload, payload_len);
	if (err != 0) {
		err = dsc.dsc_err;
		goto out;
	}

	setup_to_thread(&to_arg, to_ds, dssp, fromtxg);
	setup_from_thread(&from_arg, from_rl, dssp);
	setup_redact_list_thread(&rlt_arg, dspp, redact_rl, dssp);
	setup_merge_thread(&smt_arg, dspp, &from_arg, &to_arg, &rlt_arg, os);
	setup_prefetch_thread(&spt_arg, dspp, &smt_arg);

	range = bqueue_dequeue(&spt_arg.q);
	while (err == 0 && !range->eos_marker) {
		err = do_dump(&dsc, range);
		range = get_next_range(&spt_arg.q, range);
		if (issig(JUSTLOOKING) && issig(FORREAL))
			err = EINTR;
	}

	/*
	 * If we hit an error or are interrupted, cancel our worker threads and
	 * clear the queue of any pending records.  The threads will pass the
	 * cancel up the tree of worker threads, and each one will clean up any
	 * pending records before exiting.
	 */
	if (err != 0) {
		spt_arg.cancel = B_TRUE;
		while (!range->eos_marker) {
			range = get_next_range(&spt_arg.q, range);
		}
	}
	range_free(range);

	bqueue_destroy(&spt_arg.q);
	bqueue_destroy(&smt_arg.q);
	if (dspp->redactbook != NULL)
		bqueue_destroy(&rlt_arg.q);
	bqueue_destroy(&to_arg.q);
	bqueue_destroy(&from_arg.q);

	if (err == 0 && spt_arg.error != 0)
		err = spt_arg.error;

	if (err != 0)
		goto out;

	if (dsc.dsc_pending_op != PENDING_NONE)
		if (dump_record(&dsc, NULL, 0) != 0)
			err = SET_ERROR(EINTR);

	if (err != 0) {
		if (err == EINTR && dsc.dsc_err != 0)
			err = dsc.dsc_err;
		goto out;
	}

	bzero(drr, sizeof (dmu_replay_record_t));
	drr->drr_type = DRR_END;
	drr->drr_u.drr_end.drr_checksum = dsc.dsc_zc;
	drr->drr_u.drr_end.drr_toguid = dsc.dsc_toguid;

	if (dump_record(&dsc, NULL, 0) != 0)
		err = dsc.dsc_err;
out:
	mutex_enter(&to_ds->ds_sendstream_lock);
	list_remove(&to_ds->ds_sendstreams, dssp);
	mutex_exit(&to_ds->ds_sendstream_lock);

	VERIFY(err != 0 || (dsc.dsc_sent_begin && dsc.dsc_sent_end));

	kmem_free(drr, sizeof (dmu_replay_record_t));
	kmem_free(dssp, sizeof (dmu_sendstatus_t));

	dsl_dataset_long_rele(to_ds, FTAG);
	if (from_rl != NULL) {
		dsl_redaction_list_long_rele(from_rl, FTAG);
		dsl_redaction_list_rele(from_rl, FTAG);
	}
	if (redact_rl != NULL) {
		dsl_redaction_list_long_rele(redact_rl, FTAG);
		dsl_redaction_list_rele(redact_rl, FTAG);
	}

	return (err);
}

static int
dsl_dataset_walk_origin(dsl_pool_t *dp, dsl_dataset_t **ds, void *tag)
{
	uint64_t origin_obj = dsl_dir_phys((*ds)->ds_dir)->dd_origin_obj;
	dsl_dataset_t *prev;
	int err = dsl_dataset_hold_obj(dp, origin_obj, tag, &prev);
	if (err != 0)
		return (err);
	dsl_dataset_rele(*ds, tag);
	*ds = prev;
	prev = NULL;
	return (err);
}

int
dmu_send_obj(const char *pool, uint64_t tosnap, uint64_t fromsnap,
    boolean_t embedok, boolean_t large_block_ok, boolean_t compressok,
    boolean_t rawok, int outfd, offset_t *off, dmu_send_outparams_t *dsop)
{
	int err;
	dsl_dataset_t *fromds;
	ds_hold_flags_t dsflags = (rawok) ? 0 : DS_HOLD_FLAG_DECRYPT;
	struct dmu_send_params dspp = {0};
	dspp.embedok = embedok;
	dspp.large_block_ok = large_block_ok;
	dspp.compressok = compressok;
	dspp.outfd = outfd;
	dspp.off = off;
	dspp.dso = dsop;
	dspp.tag = FTAG;
	dspp.rawok = rawok;

	err = dsl_pool_hold(pool, FTAG, &dspp.dp);
	if (err != 0)
		return (err);

	err = dsl_dataset_hold_obj(dspp.dp, tosnap, dsflags, FTAG, &dspp.to_ds);
	if (err != 0) {
		dsl_pool_rele(dspp.dp, FTAG);
		return (err);
	}

	if (fromsnap != 0) {
		err = dsl_dataset_hold_obj(dspp.dp, fromsnap, FTAG, &fromds);
		if (err != 0) {
			dsl_dataset_rele(dspp.to_ds, dsflags, FTAG);
			dsl_pool_rele(dspp.dp, FTAG);
			return (err);
		}
		dspp.ancestor_zb.zbm_guid = dsl_dataset_phys(fromds)->ds_guid;
		dspp.ancestor_zb.zbm_creation_txg =
		    dsl_dataset_phys(fromds)->ds_creation_txg;
		dspp.ancestor_zb.zbm_creation_time =
		    dsl_dataset_phys(fromds)->ds_creation_time;
		/* See dmu_send for the reasons behind this. */
		uint64_t *fromredact;

		if (!dsl_dataset_get_uint64_array_feature(fromds,
		    SPA_FEATURE_REDACTED_DATASETS,
		    &dspp.numfromredactsnaps,
		    &fromredact)) {
			dspp.numfromredactsnaps = NUM_SNAPS_NOT_REDACTED;
		} else if (dspp.numfromredactsnaps > 0) {
			uint64_t size = dspp.numfromredactsnaps *
			    sizeof (uint64_t);
			dspp.fromredactsnaps = kmem_zalloc(size, KM_SLEEP);
			bcopy(fromredact, dspp.fromredactsnaps, size);
		}

		if (!dsl_dataset_is_before(dspp.to_ds, fromds, 0)) {
			err = SET_ERROR(EXDEV);
		} else {
			dspp.is_clone = (dspp.to_ds->ds_dir !=
			    fromds->ds_dir);
			dsl_dataset_rele(fromds, FTAG);
			err = dmu_send_impl(&dspp);
		}
	} else {
		dspp.numfromredactsnaps = NUM_SNAPS_NOT_REDACTED;
		err = dmu_send_impl(&dspp);
	}
	dsl_dataset_rele(dspp.to_ds, FTAG);
	return (err);
}

int
dmu_send(const char *tosnap, const char *fromsnap, boolean_t embedok,
    boolean_t large_block_ok, boolean_t compressok, boolean_t rawok,
	uint64_t resumeobj, uint64_t resumeoff, const char *redactbook, int outfd,
	offset_t *off, dmu_send_outparams_t *dsop)
{
	int err = 0;
	ds_hold_flags_t dsflags = (rawok) ? 0 : DS_HOLD_FLAG_DECRYPT;
	boolean_t owned = B_FALSE;
	dsl_dataset_t *fromds = NULL;
	zfs_bookmark_phys_t book = {0};
	struct dmu_send_params dspp = {0};
	dspp.tosnap = tosnap;
	dspp.embedok = embedok;
	dspp.large_block_ok = large_block_ok;
	dspp.compressok = compressok;
	dspp.outfd = outfd;
	dspp.off = off;
	dspp.dso = dsop;
	dspp.tag = FTAG;
	dspp.resumeobj = resumeobj;
	dspp.resumeoff = resumeoff;
	dspp.rawok = rawok;

	if (fromsnap != NULL && strpbrk(fromsnap, "@#") == NULL)
		return (SET_ERROR(EINVAL));

	err = dsl_pool_hold(tosnap, FTAG, &dspp.dp);
	if (err != 0)
		return (err);

	if (strchr(tosnap, '@') == NULL && spa_writeable(dspp.dp->dp_spa)) {
		/*
		 * We are sending a filesystem or volume.  Ensure
		 * that it doesn't change by owning the dataset.
		 */
		err = dsl_dataset_own(dspp.dp, tosnap, dsflags, FTAG, &dspp.to_ds);
		owned = B_TRUE;
	} else {
		err = dsl_dataset_hold(dp, tosnap, dsflags, FTAG, &ds);
	}

	if (err != 0) {
		dsl_pool_rele(dspp.dp, FTAG);
		return (err);
	}

	if (redactbook != NULL) {
		char path[ZFS_MAX_DATASET_NAME_LEN];
		(void) strlcpy(path, tosnap, sizeof (path));
		char *at = strchr(path, '@');
		if (at == NULL) {
			err = EINVAL;
		} else {
			(void) snprintf(at, sizeof (path) - (at - path), "#%s",
			    redactbook);
			err = dsl_bookmark_lookup(dspp.dp, path,
			    NULL, &book);
			dspp.redactbook = &book;
		}
	}

	if (err != 0) {
		dsl_pool_rele(dspp.dp, FTAG);
		if (owned)
			dsl_dataset_disown(dspp.to_ds, FTAG);
		else
			dsl_dataset_rele(dspp.to_ds, FTAG);
		return (err);
	}

	if (fromsnap != NULL) {
		zfs_bookmark_phys_t *zb = &dspp.ancestor_zb;
		int fsnamelen;
		if (strpbrk(tosnap, "@#") != NULL)
			fsnamelen = strpbrk(tosnap, "@#") - tosnap;
		else
			fsnamelen = strlen(tosnap);

		/*
		 * If the fromsnap is in a different filesystem, then
		 * mark the send stream as a clone.
		 */
		if (strncmp(tosnap, fromsnap, fsnamelen) != 0 ||
		    (fromsnap[fsnamelen] != '@' &&
		    fromsnap[fsnamelen] != '#')) {
			dspp.is_clone = B_TRUE;
		}

		if (strchr(fromsnap, '@') != NULL) {
			err = dsl_dataset_hold(dspp.dp, fromsnap, FTAG,
			    &fromds);

			if (err != 0) {
				ASSERT3P(fromds, ==, NULL);
			} else {
				/*
				 * We need to make a deep copy of the redact
				 * snapshots of the from snapshot, because the
				 * array will be freed when we evict from_ds.
				 */
				uint64_t *fromredact;
				if (!dsl_dataset_get_uint64_array_feature(
				    fromds, SPA_FEATURE_REDACTED_DATASETS,
				    &dspp.numfromredactsnaps,
				    &fromredact)) {
					dspp.numfromredactsnaps =
					    NUM_SNAPS_NOT_REDACTED;
				} else if (dspp.numfromredactsnaps > 0) {
					uint64_t size =
					    dspp.numfromredactsnaps *
					    sizeof (uint64_t);
					dspp.fromredactsnaps = kmem_zalloc(size,
					    KM_SLEEP);
					bcopy(fromredact, dspp.fromredactsnaps,
					    size);
				}
				if (!dsl_dataset_is_before(dspp.to_ds, fromds,
				    0)) {
					err = SET_ERROR(EXDEV);
				} else {
					ASSERT3U(dspp.is_clone, ==,
					    (dspp.to_ds->ds_dir !=
					    fromds->ds_dir));
					zb->zbm_creation_txg =
					    dsl_dataset_phys(fromds)->
					    ds_creation_txg;
					zb->zbm_creation_time =
					    dsl_dataset_phys(fromds)->
					    ds_creation_time;
					zb->zbm_guid =
					    dsl_dataset_phys(fromds)->ds_guid;
					zb->zbm_redaction_obj = 0;
				}
				dsl_dataset_rele(fromds, FTAG);
			}
		} else {
			dspp.numfromredactsnaps = NUM_SNAPS_NOT_REDACTED;
			err = dsl_bookmark_lookup(dspp.dp, fromsnap, dspp.to_ds,
			    zb);
			if (err == EXDEV && zb->zbm_redaction_obj != 0 &&
			    zb->zbm_guid ==
			    dsl_dataset_phys(dspp.to_ds)->ds_guid)
				err = 0;
		}

		if (err == 0) {
			/* dmu_send_impl will call dsl_pool_rele for us. */
			err = dmu_send_impl(&dspp);
		} else {
			dsl_pool_rele(dspp.dp, FTAG);
		}
	} else {
		dspp.numfromredactsnaps = NUM_SNAPS_NOT_REDACTED;
		err = dmu_send_impl(&dspp);
	}
	if (owned)
		dsl_dataset_disown(dspp.to_ds, dsflags, FTAG);
	else
		dsl_dataset_rele(dspp.to_ds, dsflags, FTAG);
	return (err);
}

static int
dmu_adjust_send_estimate_for_indirects(dsl_dataset_t *ds, uint64_t uncompressed,
    uint64_t compressed, boolean_t stream_compressed, uint64_t *sizep)
{
	int err = 0;
	uint64_t size;
	/*
	 * Assume that space (both on-disk and in-stream) is dominated by
	 * data.  We will adjust for indirect blocks and the copies property,
	 * but ignore per-object space used (eg, dnodes and DRR_OBJECT records).
	 */
	uint64_t recordsize;
	uint64_t record_count;
	objset_t *os;
	VERIFY0(dmu_objset_from_ds(ds, &os));

	/* Assume all (uncompressed) blocks are recordsize. */
	if (zfs_override_estimate_recordsize != 0) {
		recordsize = zfs_override_estimate_recordsize;
	} else if (os->os_phys->os_type == DMU_OST_ZVOL) {
		err = dsl_prop_get_int_ds(ds,
		    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE), &recordsize);
	} else {
		err = dsl_prop_get_int_ds(ds,
		    zfs_prop_to_name(ZFS_PROP_RECORDSIZE), &recordsize);
	}
	if (err != 0)
		return (err);
	record_count = uncompressed / recordsize;

	/*
	 * If we're estimating a send size for a compressed stream, use the
	 * compressed data size to estimate the stream size. Otherwise, use the
	 * uncompressed data size.
	 */
	size = stream_compressed ? compressed : uncompressed;

	/*
	 * Subtract out approximate space used by indirect blocks.
	 * Assume most space is used by data blocks (non-indirect, non-dnode).
	 * Assume no ditto blocks or internal fragmentation.
	 *
	 * Therefore, space used by indirect blocks is sizeof(blkptr_t) per
	 * block.
	 */
	size -= record_count * sizeof (blkptr_t);

	/* Add in the space for the record associated with each block. */
	size += record_count * sizeof (dmu_replay_record_t);

	*sizep = size;

	return (0);
}

int
dmu_send_estimate_fast(dsl_dataset_t *ds, dsl_dataset_t *fromds,
    zfs_bookmark_phys_t *frombook, boolean_t stream_compressed, uint64_t *sizep)
{
	dsl_pool_t *dp = ds->ds_dir->dd_pool;
	int err;
	uint64_t uncomp, comp;

	ASSERT(dsl_pool_config_held(dp));
	ASSERT(fromds == NULL || frombook == NULL);
	
	/* tosnap must be a snapshot */
	if (!ds->ds_is_snapshot)
		return (SET_ERROR(EINVAL));

	if (fromds != NULL) {
		uint64_t used;
		if (!fromds->ds_is_snapshot)
			return (SET_ERROR(EINVAL));

		if (!dsl_dataset_is_before(ds, fromds, 0))
			return (SET_ERROR(EXDEV));

		err = dsl_dataset_space_written(fromds, ds, &used, &comp,
		    &uncomp);
		if (err != 0)
			return (err);
	} else if (frombook != NULL) {
		uint64_t used;
		err = dsl_dataset_space_written_bookmark(frombook, ds, &used,
		    &comp, &uncomp);
		if (err != 0)
			return (err);
	} else {
		uncomp = dsl_dataset_phys(ds)->ds_uncompressed_bytes;
		comp = dsl_dataset_phys(ds)->ds_compressed_bytes;
	}

	err = dmu_adjust_send_estimate_for_indirects(ds, uncomp, comp,
	    stream_compressed, sizep);
	/*
	 * Add the size of the BEGIN and END records to the estimate.
	 */
	*sizep += 2 * sizeof (dmu_replay_record_t);
	return (err);
}
