// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
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
 * Copyright (c) 2011, 2018 by Delphix. All rights reserved.
 * Copyright (c) 2014, Joyent, Inc. All rights reserved.
 * Copyright 2014 HybridCluster. All rights reserved.
 * Copyright 2016 RackTop Systems.
 * Copyright (c) 2016 Actifio, Inc. All rights reserved.
 * Copyright (c) 2019, 2024, Klara, Inc.
 * Copyright (c) 2019, Allan Jude
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
#include <sys/spa_impl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zap.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_znode.h>
#include <zfs_fletcher.h>
#include <sys/avl.h>
#include <sys/ddt.h>
#include <sys/zfs_onexit.h>
#include <sys/dmu_send.h>
#include <sys/dmu_recv.h>
#include <sys/dsl_destroy.h>
#include <sys/blkptr.h>
#include <sys/dsl_bookmark.h>
#include <sys/zfeature.h>
#include <sys/bqueue.h>
#include <sys/zvol.h>
#include <sys/policy.h>
#include <sys/objlist.h>
#ifdef _KERNEL
#include <sys/zfs_vfsops.h>
#endif

/* Set this tunable to TRUE to replace corrupt data with 0x2f5baddb10c */
static int zfs_send_corrupt_data = B_FALSE;
/*
 * This tunable controls the amount of data (measured in bytes) that will be
 * prefetched by zfs send.  If the main thread is blocking on reads that haven't
 * completed, this variable might need to be increased.  If instead the main
 * thread is issuing new reads because the prefetches have fallen out of the
 * cache, this may need to be decreased.
 */
static uint_t zfs_send_queue_length = SPA_MAXBLOCKSIZE;
/*
 * These tunables control the fill fraction of the queues by zfs send.  The fill
 * fraction controls the frequency with which threads have to be cv_signaled.
 * If a lot of cpu time is being spent on cv_signal, then these should be tuned
 * down.  If the queues empty before the signalled thread can catch up, then
 * these should be tuned up.
 */
static uint_t zfs_send_queue_ff = 20;

/*
 * Use this to override the recordsize calculation for fast zfs send estimates.
 */
static uint_t zfs_override_estimate_recordsize = 0;

/* Set this tunable to FALSE to disable setting of DRR_FLAG_FREERECORDS */
static const boolean_t zfs_send_set_freerecords_bit = B_TRUE;

/* Set this tunable to FALSE is disable sending unmodified spill blocks. */
static int zfs_send_unmodified_spill_blocks = B_TRUE;

static inline boolean_t
overflow_multiply(uint64_t a, uint64_t b, uint64_t *c)
{
	uint64_t temp = a * b;
	if (b != 0 && temp / b != a)
		return (B_FALSE);
	*c = temp;
	return (B_TRUE);
}

struct send_thread_arg {
	bqueue_t	q;
	objset_t	*os;		/* Objset to traverse */
	uint64_t	fromtxg;	/* Traverse from this txg */
	int		flags;		/* flags to pass to traverse_dataset */
	int		error_code;
	boolean_t	cancel;
	zbookmark_phys_t resume;
	uint64_t	*num_blocks_visited;
};

struct send_block_record {
	boolean_t		eos_marker; /* Marks the end of the stream */
	blkptr_t		bp;
	zbookmark_phys_t	zb;
	uint8_t			indblkshift;
	uint16_t		datablkszsec;
	bqueue_node_t		ln;
};

/*
 * The list of data whose inclusion in a send stream can be pending from
 * one call to backup_cb to another.  Multiple calls to dump_free(),
 * dump_freeobjects() can be aggregated into a single
 * DRR_FREE, DRR_FREEOBJECTS replay record.
 */
typedef enum {
	PENDING_NONE,
	PENDING_FREE,
	PENDING_FREEOBJECTS
} dmu_pendop_t;

typedef struct dmu_send_cookie {
	dmu_replay_record_t *dsc_drr;
	dmu_send_outparams_t *dsc_dso;
	offset_t *dsc_off;
	objset_t *dsc_os;
	zio_cksum_t dsc_zc;
	uint64_t dsc_toguid;
	uint64_t dsc_fromtxg;
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
	dscp->dsc_err = dso->dso_outfunc(dscp->dsc_os, dscp->dsc_drr,
	    sizeof (dmu_replay_record_t), dso->dso_arg);
	if (dscp->dsc_err != 0)
		return (SET_ERROR(EINTR));
	if (payload_len != 0) {
		*dscp->dsc_off += payload_len;
		/*
		 * payload is null when dso_dryrun == B_TRUE (i.e. when we're
		 * doing a send size calculation)
		 */
		if (payload != NULL) {
			(void) fletcher_4_incremental_native(
			    payload, payload_len, &dscp->dsc_zc);
		}

		/*
		 * The code does not rely on this (len being a multiple of 8).
		 * We keep this assertion because of the corresponding assertion
		 * in receive_read().  Keeping this assertion ensures that we do
		 * not inadvertently break backwards compatibility (causing the
		 * assertion in receive_read() to trigger on old software).
		 *
		 * Raw sends cannot be received on old software, and so can
		 * bypass this assertion.
		 */

		ASSERT((payload_len % 8 == 0) ||
		    (dscp->dsc_featureflags & DMU_BACKUP_FEATURE_RAW));

		dscp->dsc_err = dso->dso_outfunc(dscp->dsc_os, payload,
		    payload_len, dso->dso_arg);
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
			if (offset + length < offset || length == UINT64_MAX)
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
	memset(dscp->dsc_drr, 0, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_FREE;
	drrf->drr_object = object;
	drrf->drr_offset = offset;
	if (offset + length < offset)
		drrf->drr_length = DMU_OBJECT_END;
	else
		drrf->drr_length = length;
	drrf->drr_toguid = dscp->dsc_toguid;
	if (length == DMU_OBJECT_END) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
	} else {
		dscp->dsc_pending_op = PENDING_FREE;
	}

	return (0);
}

static int
dmu_dump_write(dmu_send_cookie_t *dscp, dmu_object_type_t type, uint64_t object,
    uint64_t offset, int lsize, int psize, const blkptr_t *bp,
    void *data)
{
	uint64_t payload_size;
	boolean_t raw = (dscp->dsc_featureflags & DMU_BACKUP_FEATURE_RAW);
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
	memset(dscp->dsc_drr, 0, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_WRITE;
	drrw->drr_object = object;
	drrw->drr_type = type;
	drrw->drr_offset = offset;
	drrw->drr_toguid = dscp->dsc_toguid;
	drrw->drr_logical_size = lsize;

	/* only set the compression fields if the buf is compressed or raw */
	if (raw || lsize != psize) {
		ASSERT(bp != NULL);
		ASSERT(raw || dscp->dsc_featureflags &
		    DMU_BACKUP_FEATURE_COMPRESSED);
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
			ASSERT(dscp->dsc_featureflags &
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
		 * plaintext, so (like fletcher4-checksummed blocks) userland
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
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	ASSERT(BP_IS_EMBEDDED(bp));

	memset(dscp->dsc_drr, 0, sizeof (dmu_replay_record_t));
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

	uint32_t psize = drrw->drr_psize;
	uint32_t rsize = P2ROUNDUP(psize, 8);

	if (psize != rsize)
		memset(buf + psize, 0, rsize - psize);

	if (dump_record(dscp, buf, rsize) != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static int
dump_spill(dmu_send_cookie_t *dscp, const blkptr_t *bp, uint64_t object,
    void *data)
{
	struct drr_spill *drrs = &(dscp->dsc_drr->drr_u.drr_spill);
	uint64_t blksz = BP_GET_LSIZE(bp);
	uint64_t payload_size = blksz;

	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	/* write a SPILL record */
	memset(dscp->dsc_drr, 0, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_SPILL;
	drrs->drr_object = object;
	drrs->drr_length = blksz;
	drrs->drr_toguid = dscp->dsc_toguid;

	/* See comment in piggyback_unmodified_spill() for full details */
	if (zfs_send_unmodified_spill_blocks &&
	    (BP_GET_LOGICAL_BIRTH(bp) <= dscp->dsc_fromtxg)) {
		drrs->drr_flags |= DRR_SPILL_UNMODIFIED;
	}

	/* handle raw send fields */
	if (dscp->dsc_featureflags & DMU_BACKUP_FEATURE_RAW) {
		ASSERT(BP_IS_PROTECTED(bp));

		if (BP_SHOULD_BYTESWAP(bp))
			drrs->drr_flags |= DRR_RAW_BYTESWAP;
		drrs->drr_compressiontype = BP_GET_COMPRESS(bp);
		drrs->drr_compressed_size = BP_GET_PSIZE(bp);
		zio_crypt_decode_params_bp(bp, drrs->drr_salt, drrs->drr_iv);
		zio_crypt_decode_mac_bp(bp, drrs->drr_mac);
		payload_size = drrs->drr_compressed_size;
	}

	if (dump_record(dscp, data, payload_size) != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static int
dump_freeobjects(dmu_send_cookie_t *dscp, uint64_t firstobj, uint64_t numobjs)
{
	struct drr_freeobjects *drrfo = &(dscp->dsc_drr->drr_u.drr_freeobjects);
	uint64_t maxobj = DNODES_PER_BLOCK *
	    (DMU_META_DNODE(dscp->dsc_os)->dn_maxblkid + 1);

	/*
	 * ZoL < 0.7 does not handle large FREEOBJECTS records correctly,
	 * leading to zfs recv never completing. to avoid this issue, don't
	 * send FREEOBJECTS records for object IDs which cannot exist on the
	 * receiving side.
	 */
	if (maxobj > 0) {
		if (maxobj <= firstobj)
			return (0);

		if (maxobj < firstobj + numobjs)
			numobjs = maxobj - firstobj;
	}

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
	memset(dscp->dsc_drr, 0, sizeof (dmu_replay_record_t));
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
	memset(dscp->dsc_drr, 0, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_OBJECT;
	drro->drr_object = object;
	drro->drr_type = dnp->dn_type;
	drro->drr_bonustype = dnp->dn_bonustype;
	drro->drr_blksz = dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	drro->drr_bonuslen = dnp->dn_bonuslen;
	drro->drr_dn_slots = dnp->dn_extra_slots + 1;
	drro->drr_checksumtype = dnp->dn_checksum;
	drro->drr_compress = dnp->dn_compress;
	drro->drr_toguid = dscp->dsc_toguid;

	if (!(dscp->dsc_featureflags & DMU_BACKUP_FEATURE_LARGE_BLOCKS) &&
	    drro->drr_blksz > SPA_OLD_MAXBLOCKSIZE)
		drro->drr_blksz = SPA_OLD_MAXBLOCKSIZE;

	bonuslen = P2ROUNDUP(dnp->dn_bonuslen, 8);

	if ((dscp->dsc_featureflags & DMU_BACKUP_FEATURE_RAW)) {
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
			if (drro->drr_bonuslen > DN_MAX_BONUS_LEN(dnp))
				return (SET_ERROR(EINVAL));
			drro->drr_raw_bonuslen = DN_MAX_BONUS_LEN(dnp);
			bonuslen = drro->drr_raw_bonuslen;
		}
	}

	/*
	 * DRR_OBJECT_SPILL is set for every dnode which references a
	 * spill block.	 This allows the receiving pool to definitively
	 * determine when a spill block should be kept or freed.
	 */
	if (dnp->dn_flags & DNODE_FLAG_SPILL_BLKPTR)
		drro->drr_flags |= DRR_OBJECT_SPILL;

	if (dump_record(dscp, DN_BONUS(dnp), bonuslen) != 0)
		return (SET_ERROR(EINTR));

	/* Free anything past the end of the file. */
	if (dump_free(dscp, object, (dnp->dn_maxblkid + 1) *
	    (dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT), DMU_OBJECT_END) != 0)
		return (SET_ERROR(EINTR));

	if (dscp->dsc_err != 0)
		return (SET_ERROR(EINTR));

	return (0);
}

static int
dump_object_range(dmu_send_cookie_t *dscp, const blkptr_t *bp,
    uint64_t firstobj, uint64_t numslots)
{
	struct drr_object_range *drror =
	    &(dscp->dsc_drr->drr_u.drr_object_range);

	/* we only use this record type for raw sends */
	ASSERT(BP_IS_PROTECTED(bp));
	ASSERT(dscp->dsc_featureflags & DMU_BACKUP_FEATURE_RAW);
	ASSERT3U(BP_GET_COMPRESS(bp), ==, ZIO_COMPRESS_OFF);
	ASSERT3U(BP_GET_TYPE(bp), ==, DMU_OT_DNODE);
	ASSERT0(BP_GET_LEVEL(bp));

	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	memset(dscp->dsc_drr, 0, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_OBJECT_RANGE;
	drror->drr_firstobj = firstobj;
	drror->drr_numslots = numslots;
	drror->drr_toguid = dscp->dsc_toguid;
	if (BP_SHOULD_BYTESWAP(bp))
		drror->drr_flags |= DRR_RAW_BYTESWAP;
	zio_crypt_decode_params_bp(bp, drror->drr_salt, drror->drr_iv);
	zio_crypt_decode_mac_bp(bp, drror->drr_mac);

	if (dump_record(dscp, NULL, 0) != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static boolean_t
send_do_embed(const blkptr_t *bp, uint64_t featureflags)
{
	if (!BP_IS_EMBEDDED(bp))
		return (B_FALSE);

	/*
	 * Compression function must be legacy, or explicitly enabled.
	 */
	if ((BP_GET_COMPRESS(bp) >= ZIO_COMPRESS_LEGACY_FUNCTIONS &&
	    !(featureflags & DMU_BACKUP_FEATURE_LZ4)))
		return (B_FALSE);

	/*
	 * If we have not set the ZSTD feature flag, we can't send ZSTD
	 * compressed embedded blocks, as the receiver may not support them.
	 */
	if ((BP_GET_COMPRESS(bp) == ZIO_COMPRESS_ZSTD &&
	    !(featureflags & DMU_BACKUP_FEATURE_ZSTD)))
		return (B_FALSE);

	/*
	 * Embed type must be explicitly enabled.
	 */
	switch (BPE_GET_ETYPE(bp)) {
	case BP_EMBEDDED_TYPE_DATA:
		if (featureflags & DMU_BACKUP_FEATURE_EMBED_DATA)
			return (B_TRUE);
		break;
	default:
		return (B_FALSE);
	}
	return (B_FALSE);
}

#define BP_SPAN(datablkszsec, indblkshift, level) \
        (((uint64_t)datablkszsec) << (SPA_MINBLOCKSHIFT + \
        (level) * (indblkshift - SPA_BLKPTRSHIFT)))

/*
 * This function actually handles figuring out what kind of record needs to be
 * dumped, reading the data (which has hopefully been prefetched), and calling
 * the appropriate helper function.
 */
static int
do_dump(dmu_send_cookie_t *dsa, struct send_block_record *data)
{
 	dsl_dataset_t *ds = dmu_objset_ds(dsa->dsc_os);
        const blkptr_t *bp = &data->bp;
        const zbookmark_phys_t *zb = &data->zb;
	uint8_t indblkshift = data->indblkshift;
        uint16_t dblkszsec = data->datablkszsec;
	spa_t *spa = ds->ds_dir->dd_pool->dp_spa;
        dmu_object_type_t type = bp ? BP_GET_TYPE(bp) : DMU_OT_NONE;
        int err = 0;

	ASSERT3U(zb->zb_level, >=, 0);

        ASSERT(zb->zb_object == DMU_META_DNODE_OBJECT ||
	    zb->zb_object >= dsa->dsc_resume_object);

	/*
	 * All bps of an encrypted os should have the encryption bit set.
	 * If this is not true it indicates tampering and we report an error.
	 */
	if (dsa->dsc_os->os_encrypted &&
            !BP_IS_HOLE(bp) && !BP_USES_CRYPT(bp)) {
                spa_log_error(spa, zb, BP_GET_BIRTH(bp));
                zfs_panic_recover("unencrypted block in encrypted "
                    "object set %llu", ds->ds_object);
                return (SET_ERROR(EIO));
        }

        if (zb->zb_object != DMU_META_DNODE_OBJECT &&
            DMU_OBJECT_IS_SPECIAL(zb->zb_object)) {
                return (0);
        } else if (BP_IS_HOLE(bp) &&
            zb->zb_object == DMU_META_DNODE_OBJECT) {
                uint64_t span = BP_SPAN(dblkszsec, indblkshift, zb->zb_level);
                uint64_t dnobj = (zb->zb_blkid * span) >> DNODE_SHIFT;
                err = dump_freeobjects(dsa, dnobj, span >> DNODE_SHIFT);
        } else if (BP_IS_HOLE(bp)) {
                uint64_t span = BP_SPAN(dblkszsec, indblkshift, zb->zb_level);
                uint64_t offset = zb->zb_blkid * span;
                /* Don't dump free records for offsets > DMU_OBJECT_END */
                if (zb->zb_blkid == 0 || span <= DMU_OBJECT_END / zb->zb_blkid)
                        err = dump_free(dsa, zb->zb_object, offset, span);
        } else if (zb->zb_level > 0 || type == DMU_OT_OBJSET) {
                return (0);
        } else if (type == DMU_OT_DNODE) {
                int epb = BP_GET_LSIZE(bp) >> DNODE_SHIFT;
                arc_flags_t aflags = ARC_FLAG_WAIT;
		arc_buf_t *abuf;
                zio_flag_t zioflags = ZIO_FLAG_CANFAIL;

                if (dsa->dsc_featureflags & DMU_BACKUP_FEATURE_RAW) {
                        ASSERT(BP_IS_ENCRYPTED(bp));
                        ASSERT3U(BP_GET_COMPRESS(bp), ==, ZIO_COMPRESS_OFF);
                        zioflags |= ZIO_FLAG_RAW;
                }

                ASSERT0(zb->zb_level);

                if (arc_read(NULL, spa, bp, arc_getbuf_func, &abuf,
                    ZIO_PRIORITY_ASYNC_READ, zioflags, &aflags, zb) != 0)
                        return (SET_ERROR(EIO));

                dnode_phys_t *blk = abuf->b_data;
                uint64_t dnobj = zb->zb_blkid * epb;

                /*
                 * Raw sends require sending encryption parameters for the
                 * block of dnodes. Regular sends do not need to send this
                 * info.
                 */
                if (dsa->dsc_featureflags & DMU_BACKUP_FEATURE_RAW) {
                        ASSERT(arc_is_encrypted(abuf));
                        err = dump_object_range(dsa, bp, dnobj, epb);
                }

		if (err == 0) {
                        for (int i = 0; i < epb;
                            i += blk[i].dn_extra_slots + 1) {
                                err = dump_dnode(dsa, bp, dnobj + i, blk + i);
                                if (err != 0)
                                        break;
                        }
                }
                arc_buf_destroy(abuf, &abuf);
        } else if (type == DMU_OT_SA) {
                arc_flags_t aflags = ARC_FLAG_WAIT;
                arc_buf_t *abuf;
                zio_flag_t zioflags = ZIO_FLAG_CANFAIL;

                if (dsa->dsc_featureflags & DMU_BACKUP_FEATURE_RAW) {
                        ASSERT(BP_IS_PROTECTED(bp));
                        zioflags |= ZIO_FLAG_RAW;
                }

                if (arc_read(NULL, spa, bp, arc_getbuf_func, &abuf,
                    ZIO_PRIORITY_ASYNC_READ, zioflags, &aflags, zb) != 0)
                        return (SET_ERROR(EIO));

                err = dump_spill(dsa, bp, zb->zb_object, abuf->b_data);
                arc_buf_destroy(abuf, &abuf);
        } else if (send_do_embed(bp, dsa->dsc_featureflags)) {
		/* it's an embedded level-0 block of a regular object */
		int blksz = dblkszsec << SPA_MINBLOCKSHIFT;
                ASSERT0(zb->zb_level);
                err = dump_write_embedded(dsa, zb->zb_object,
                    zb->zb_blkid * blksz, blksz, bp);
        } else {
                /* it's a level-0 block of a regular object */
                arc_flags_t aflags = ARC_FLAG_WAIT;
                arc_buf_t *abuf;
                int blksz = dblkszsec << SPA_MINBLOCKSHIFT;
                uint64_t offset;

                /*
                 * If we have large blocks stored on disk but the send flags
                 * don't allow us to send large blocks, we split the data from
                 * the arc buf into chunks.
                 */
                boolean_t split_large_blocks = blksz > SPA_OLD_MAXBLOCKSIZE &&
                    !(dsa->dsc_featureflags & DMU_BACKUP_FEATURE_LARGE_BLOCKS);

                /*
                 * Raw sends require that we always get raw data as it exists
                 * on disk, so we assert that we are not splitting blocks here.
                 */
                boolean_t request_raw =
                    (dsa->dsc_featureflags & DMU_BACKUP_FEATURE_RAW) != 0;

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
                    (dsa->dsc_featureflags & DMU_BACKUP_FEATURE_COMPRESSED) &&
                    !split_large_blocks && !BP_SHOULD_BYTESWAP(bp) &&
                    !BP_IS_EMBEDDED(bp) && !DMU_OT_IS_METADATA(BP_GET_TYPE(bp));

                IMPLY(request_raw, !split_large_blocks);
                IMPLY(request_raw, BP_IS_PROTECTED(bp));
		ASSERT0(zb->zb_level);
                ASSERT(zb->zb_object > dsa->dsc_resume_object ||
                    (zb->zb_object == dsa->dsc_resume_object &&
                    zb->zb_blkid * blksz >= dsa->dsc_resume_offset));

                ASSERT3U(blksz, ==, BP_GET_LSIZE(bp));

		zio_flag_t zioflags = ZIO_FLAG_CANFAIL;
                if (request_raw)
			zioflags |= ZIO_FLAG_RAW;
                else if (request_compressed)
                        zioflags |= ZIO_FLAG_RAW_COMPRESS;

                if (arc_read(NULL, spa, bp, arc_getbuf_func, &abuf,
                    ZIO_PRIORITY_ASYNC_READ, zioflags, &aflags, zb) != 0) {
                        if (zfs_send_corrupt_data) {
                                /* Send a block filled with 0x"zfs badd bloc" */
                                abuf = arc_alloc_buf(spa, &abuf, ARC_BUFC_DATA,
                                    blksz);
                                uint64_t *ptr;
                                for (ptr = abuf->b_data;
                                    (char *)ptr < (char *)abuf->b_data + blksz;
                                    ptr++)
                                        *ptr = 0x2f5baddb10cULL;
                        } else {
                                return (SET_ERROR(EIO));
                        }
                }

                offset = zb->zb_blkid * blksz;

                if (split_large_blocks) {
                        ASSERT0(arc_is_encrypted(abuf));
                        ASSERT3U(arc_get_compression(abuf), ==,
                            ZIO_COMPRESS_OFF);
                        char *buf = abuf->b_data;
			while (blksz > 0 && err == 0) {
                                int n = MIN(blksz, SPA_OLD_MAXBLOCKSIZE);
                                err = dmu_dump_write(dsa, type, zb->zb_object,
                                    offset, n, n, NULL, buf);
                                offset += n;
                                buf += n;
                                blksz -= n;
                        }
                } else {
                        err = dmu_dump_write(dsa, type, zb->zb_object, offset,
                            blksz, arc_buf_size(abuf), bp, abuf->b_data);
                }
                arc_buf_destroy(abuf, &abuf);
        }

        ASSERT(err == 0 || err == EINTR);
        return (err);
}

/*
 * Pop the new data off the queue, and free the old data.
 */
static struct send_block_record *
get_next_record(bqueue_t *bq, struct send_block_record *data)
{
	struct send_block_record *tmp = bqueue_dequeue(bq);
        kmem_free(data, sizeof (*data));
        return (tmp);
}

/*
 * This is the callback function to traverse_dataset that acts as a worker
 * thread for dmu_send_impl.
 */
static int
send_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const struct dnode_phys *dnp, void *arg)
{
	(void) spa;
	(void) zilog;
        struct send_thread_arg *sta = arg;
        struct send_block_record *record;
        uint64_t record_size;
        int err = 0;

        ASSERT(zb->zb_object == DMU_META_DNODE_OBJECT ||
            zb->zb_object >= sta->resume.zb_object);

        if (sta->cancel)
        	return (SET_ERROR(EINTR));

	if (bp == NULL) {
		ASSERT3U(zb->zb_level, ==, ZB_DNODE_LEVEL);
		return (0);
	} else if (zb->zb_level < 0) {
                return (0);
        }

	record = kmem_zalloc(sizeof (struct send_block_record), KM_SLEEP);
        record->eos_marker = B_FALSE;
        record->bp = *bp;
        record->zb = *zb;
        record->indblkshift = dnp->dn_indblkshift;
        record->datablkszsec = dnp->dn_datablkszsec;
        record_size = dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT;
        bqueue_enqueue(&sta->q, record, record_size);

        return (err);
}

/*
 * This function kicks off the traverse_dataset.  It also handles setting the
 * error code of the thread in case something goes wrong, and pushes the End of
 * Stream record when the traverse_dataset call has finished.
 */
static __attribute__((noreturn)) void
send_traverse_thread(void *arg)
{
	struct send_thread_arg *st_arg = arg;
	int err = 0;
	struct send_block_record *record;
	fstrans_cookie_t cookie = spl_fstrans_mark();

	err = traverse_dataset_resume(st_arg->os->os_dsl_dataset,
	    st_arg->fromtxg, &st_arg->resume,
	    st_arg->flags, send_cb, st_arg);

	if (err != EINTR)
		st_arg->error_code = err;
	record = kmem_zalloc(sizeof (struct send_block_record), KM_SLEEP);
	record->eos_marker = B_TRUE;
	bqueue_enqueue_flush(&st_arg->q, record, sizeof (*record));
	spl_fstrans_unmark(cookie);
	thread_exit();
}

struct dmu_send_params {
	/* Pool args */
	const void *tag; // Tag dp was held with, will be used to release dp.
	dsl_pool_t *dp;
	/* To snapshot args */
	const char *tosnap;
	dsl_dataset_t *to_ds;
	/* From snapshot args */
	zfs_bookmark_phys_t ancestor_zb;
	/* Stream params */
	boolean_t is_clone;
	boolean_t embedok;
	boolean_t large_block_ok;
	boolean_t compressok;
	boolean_t rawok;
	boolean_t savedok;
	uint64_t resumeobj;
	uint64_t resumeoff;
	uint64_t saved_guid;
	/* Stream output params */
	dmu_send_outparams_t *dso;

	/* Stream progress params */
	offset_t *off;
	int outfd;
	char saved_toname[MAXNAMELEN];
};

static int
setup_featureflags(struct dmu_send_params *dspp, objset_t *os,
    uint64_t *featureflags)
{
	dsl_dataset_t *to_ds = dspp->to_ds;
	dsl_pool_t *dp = dspp->dp;

	if (dmu_objset_type(os) == DMU_OST_ZFS) {
		uint64_t version;
		if (zfs_get_zplprop(os, ZFS_PROP_VERSION, &version) != 0)
			return (SET_ERROR(EINVAL));

		if (version >= ZPL_VERSION_SA)
			*featureflags |= DMU_BACKUP_FEATURE_SA_SPILL;
	}

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
		*featureflags |= DMU_BACKUP_FEATURE_RAW;

	if ((*featureflags &
	    (DMU_BACKUP_FEATURE_EMBED_DATA | DMU_BACKUP_FEATURE_COMPRESSED |
	    DMU_BACKUP_FEATURE_RAW)) != 0 &&
	    spa_feature_is_active(dp->dp_spa, SPA_FEATURE_LZ4_COMPRESS)) {
		*featureflags |= DMU_BACKUP_FEATURE_LZ4;
	}

	/*
	 * We specifically do not include DMU_BACKUP_FEATURE_EMBED_DATA here to
	 * allow sending ZSTD compressed datasets to a receiver that does not
	 * support ZSTD
	 */
	if ((*featureflags &
	    (DMU_BACKUP_FEATURE_COMPRESSED | DMU_BACKUP_FEATURE_RAW)) != 0 &&
	    dsl_dataset_feature_is_active(to_ds, SPA_FEATURE_ZSTD_COMPRESS)) {
		*featureflags |= DMU_BACKUP_FEATURE_ZSTD;
	}

	if (dspp->resumeobj != 0 || dspp->resumeoff != 0) {
		*featureflags |= DMU_BACKUP_FEATURE_RESUMING;
	}

	if (dsl_dataset_feature_is_active(to_ds, SPA_FEATURE_LARGE_DNODE)) {
		*featureflags |= DMU_BACKUP_FEATURE_LARGE_DNODE;
	}

	if (dsl_dataset_feature_is_active(to_ds, SPA_FEATURE_LONGNAME)) {
		*featureflags |= DMU_BACKUP_FEATURE_LONGNAME;
	}

	if (dsl_dataset_feature_is_active(to_ds, SPA_FEATURE_LARGE_MICROZAP)) {
		/*
		 * We must never split a large microzap block, so we can only
		 * send large microzaps if LARGE_BLOCKS is already enabled.
		 */
		if (!(*featureflags & DMU_BACKUP_FEATURE_LARGE_BLOCKS))
			return (SET_ERROR(ZFS_ERR_STREAM_LARGE_MICROZAP));
		*featureflags |= DMU_BACKUP_FEATURE_LARGE_MICROZAP;
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
	drr->drr_u.drr_begin.drr_flags |= DRR_FLAG_SPILL_BLOCK;

	if (dspp->savedok) {
		drrb->drr_toguid = dspp->saved_guid;
		strlcpy(drrb->drr_toname, dspp->saved_toname,
		    sizeof (drrb->drr_toname));
	} else {
		dsl_dataset_name(to_ds, drrb->drr_toname);
		if (!to_ds->ds_is_snapshot) {
			(void) strlcat(drrb->drr_toname, "@--head--",
			    sizeof (drrb->drr_toname));
		}
	}
	return (drr);
}

static void
setup_to_thread(struct send_thread_arg *to_arg, objset_t *to_os,
    dmu_sendstatus_t *dssp, uint64_t fromtxg, boolean_t rawok)
{
	VERIFY0(bqueue_init(&to_arg->q, zfs_send_queue_ff,
	    MAX(zfs_send_queue_length, 2 * zfs_max_recordsize),
	    offsetof(struct send_block_record, ln)));
	to_arg->error_code = 0;
	to_arg->cancel = B_FALSE;
	to_arg->os = to_os;
	to_arg->fromtxg = fromtxg;
	to_arg->flags = TRAVERSE_PRE | TRAVERSE_PREFETCH_METADATA;
	if (rawok)
		to_arg->flags |= TRAVERSE_NO_DECRYPT;
	if (zfs_send_corrupt_data)
		to_arg->flags |= TRAVERSE_HARD;
	to_arg->num_blocks_visited = &dssp->dss_blocks;
	(void) thread_create(NULL, 0, send_traverse_thread, to_arg, 0,
	    curproc, TS_RUN, minclsyspri);
}

static int
setup_resume_points(struct dmu_send_params *dspp,
    struct send_thread_arg *to_arg,  boolean_t resuming, objset_t *os, nvlist_t *nvl)
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

	SET_BOOKMARK(&to_arg->resume, to_ds->ds_object, obj, 0, blkid);
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
	struct send_thread_arg *to_arg;
	struct send_block_record *record;
	boolean_t resuming = (dspp->resumeobj != 0 || dspp->resumeoff != 0);
	boolean_t book_resuming = resuming;

	dsl_dataset_t *to_ds = dspp->to_ds;
	zfs_bookmark_phys_t *ancestor_zb = &dspp->ancestor_zb;
	dsl_pool_t *dp = dspp->dp;
	const void *tag = dspp->tag;

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
	if (!dspp->rawok && os->os_encrypted &&
	    arc_is_unauthenticated(os->os_phys_buf)) {
		zbookmark_phys_t zb;

		SET_BOOKMARK(&zb, to_ds->ds_object, ZB_ROOT_OBJECT,
		    ZB_ROOT_LEVEL, ZB_ROOT_BLKID);
		err = arc_untransform(os->os_phys_buf, os->os_spa,
		    &zb, B_FALSE);
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

	dsl_dataset_long_hold(to_ds, FTAG);

	to_arg = kmem_zalloc(sizeof (*to_arg), KM_SLEEP);

	drr = create_begin_record(dspp, os, featureflags);
	dssp = setup_send_progress(dspp);

	dsc.dsc_drr = drr;
	dsc.dsc_dso = dspp->dso;
	dsc.dsc_os = os;
	dsc.dsc_off = dspp->off;
	dsc.dsc_toguid = dsl_dataset_phys(to_ds)->ds_guid;
	dsc.dsc_fromtxg = fromtxg;
	dsc.dsc_pending_op = PENDING_NONE;
	dsc.dsc_featureflags = featureflags;
	dsc.dsc_resume_object = dspp->resumeobj;
	dsc.dsc_resume_offset = dspp->resumeoff;

	dsl_pool_rele(dp, tag);

	void *payload = NULL;
	size_t payload_len = 0;
	nvlist_t *nvl = fnvlist_alloc();

	if (resuming || book_resuming) {
		err = setup_resume_points(dspp, to_arg, resuming, os, nvl);
		if (err != 0)
			goto out;
	}

	if (featureflags & DMU_BACKUP_FEATURE_RAW) {
		uint64_t ivset_guid = ancestor_zb->zbm_ivset_guid;
		nvlist_t *keynvl = NULL;
		ASSERT(os->os_encrypted);

		err = dsl_crypto_populate_key_nvlist(os, ivset_guid,
		    &keynvl);
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

	setup_to_thread(to_arg, os, dssp, fromtxg, dspp->rawok);

	record = bqueue_dequeue(&to_arg->q);
	while (err == 0 && !record->eos_marker) {
		err = do_dump(&dsc, record);
		record = get_next_record(&to_arg->q, record);
		if (issig())
			err = SET_ERROR(EINTR);
	}

	/*
	 * If we hit an error or are interrupted, cancel our worker threads and
	 * clear the queue of any pending records.  The threads will pass the
	 * cancel up the tree of worker threads, and each one will clean up any
	 * pending records before exiting.
	 */
	if (err != 0) {
		to_arg->cancel = B_TRUE;
		while (!record->eos_marker) {
			record = get_next_record(&to_arg->q, record);
		}
	}
	kmem_free(record, sizeof (*record));

	bqueue_destroy(&to_arg->q);

	if (err == 0 && to_arg->error_code != 0)
		err = to_arg->error_code;

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

	/*
	 * Send the DRR_END record if this is not a saved stream.
	 * Otherwise, the omitted DRR_END record will signal to
	 * the receive side that the stream is incomplete.
	 */
	if (!dspp->savedok) {
		memset(drr, 0, sizeof (dmu_replay_record_t));
		drr->drr_type = DRR_END;
		drr->drr_u.drr_end.drr_checksum = dsc.dsc_zc;
		drr->drr_u.drr_end.drr_toguid = dsc.dsc_toguid;

		if (dump_record(&dsc, NULL, 0) != 0)
			err = dsc.dsc_err;
	}
out:
	mutex_enter(&to_ds->ds_sendstream_lock);
	list_remove(&to_ds->ds_sendstreams, dssp);
	mutex_exit(&to_ds->ds_sendstream_lock);

	VERIFY(err != 0 || (dsc.dsc_sent_begin &&
	    (dsc.dsc_sent_end || dspp->savedok)));

	kmem_free(drr, sizeof (dmu_replay_record_t));
	kmem_free(dssp, sizeof (dmu_sendstatus_t));
	kmem_free(to_arg, sizeof (*to_arg));

	dsl_dataset_long_rele(to_ds, FTAG);

	return (err);
}

int
dmu_send_obj(const char *pool, uint64_t tosnap, uint64_t fromsnap,
    boolean_t embedok, boolean_t large_block_ok, boolean_t compressok,
    boolean_t rawok, boolean_t savedok, int outfd, offset_t *off,
    dmu_send_outparams_t *dsop)
{
	int err;
	dsl_dataset_t *fromds;
	ds_hold_flags_t dsflags;
	struct dmu_send_params dspp = {0};
	dspp.embedok = embedok;
	dspp.large_block_ok = large_block_ok;
	dspp.compressok = compressok;
	dspp.outfd = outfd;
	dspp.off = off;
	dspp.dso = dsop;
	dspp.tag = FTAG;
	dspp.rawok = rawok;
	dspp.savedok = savedok;

	dsflags = (rawok) ? DS_HOLD_FLAG_NONE : DS_HOLD_FLAG_DECRYPT;
	err = dsl_pool_hold(pool, FTAG, &dspp.dp);
	if (err != 0)
		return (err);

	err = dsl_dataset_hold_obj_flags(dspp.dp, tosnap, dsflags, FTAG,
	    &dspp.to_ds);
	if (err != 0) {
		dsl_pool_rele(dspp.dp, FTAG);
		return (err);
	}

	if (fromsnap != 0) {
		err = dsl_dataset_hold_obj_flags(dspp.dp, fromsnap, dsflags,
		    FTAG, &fromds);
		if (err != 0) {
			dsl_dataset_rele_flags(dspp.to_ds, dsflags, FTAG);
			dsl_pool_rele(dspp.dp, FTAG);
			return (err);
		}
		dspp.ancestor_zb.zbm_guid = dsl_dataset_phys(fromds)->ds_guid;
		dspp.ancestor_zb.zbm_creation_txg =
		    dsl_dataset_phys(fromds)->ds_creation_txg;
		dspp.ancestor_zb.zbm_creation_time =
		    dsl_dataset_phys(fromds)->ds_creation_time;

		if (dsl_dataset_is_zapified(fromds)) {
			(void) zap_lookup(dspp.dp->dp_meta_objset,
			    fromds->ds_object, DS_FIELD_IVSET_GUID, 8, 1,
			    &dspp.ancestor_zb.zbm_ivset_guid);
		}

		boolean_t is_before =
		    dsl_dataset_is_before(dspp.to_ds, fromds, 0);
		dspp.is_clone = (dspp.to_ds->ds_dir !=
		    fromds->ds_dir);
		dsl_dataset_rele(fromds, FTAG);
		if (!is_before) {
			dsl_pool_rele(dspp.dp, FTAG);
			err = SET_ERROR(EXDEV);
		} else {
			err = dmu_send_impl(&dspp);
		}
	} else {
		err = dmu_send_impl(&dspp);
	}

	dsl_dataset_rele(dspp.to_ds, FTAG);
	return (err);
}

int
dmu_send(const char *tosnap, const char *fromsnap, boolean_t embedok,
    boolean_t large_block_ok, boolean_t compressok, boolean_t rawok,
    boolean_t savedok, uint64_t resumeobj, uint64_t resumeoff, int outfd, offset_t *off,
    dmu_send_outparams_t *dsop)
{
	int err = 0;
	ds_hold_flags_t dsflags;
	boolean_t owned = B_FALSE;
	dsl_dataset_t *fromds = NULL;
	struct dmu_send_params dspp = {0};

	dsflags = (rawok) ? DS_HOLD_FLAG_NONE : DS_HOLD_FLAG_DECRYPT;
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
	dspp.savedok = savedok;

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

		if (savedok) {
			/*
			 * We are looking for the dataset that represents the
			 * partially received send stream. If this stream was
			 * received as a new snapshot of an existing dataset,
			 * this will be saved in a hidden clone named
			 * "<pool>/<dataset>/%recv". Otherwise, the stream
			 * will be saved in the live dataset itself. In
			 * either case we need to use dsl_dataset_own_force()
			 * because the stream is marked as inconsistent,
			 * which would normally make it unavailable to be
			 * owned.
			 */
			char *name = kmem_asprintf("%s/%s", tosnap,
			    recv_clone_name);
			err = dsl_dataset_own_force(dspp.dp, name, dsflags,
			    FTAG, &dspp.to_ds);
			if (err == ENOENT) {
				err = dsl_dataset_own_force(dspp.dp, tosnap,
				    dsflags, FTAG, &dspp.to_ds);
			}

			if (err == 0) {
				owned = B_TRUE;
				err = zap_lookup(dspp.dp->dp_meta_objset,
				    dspp.to_ds->ds_object,
				    DS_FIELD_RESUME_TOGUID, 8, 1,
				    &dspp.saved_guid);
			}

			if (err == 0) {
				err = zap_lookup(dspp.dp->dp_meta_objset,
				    dspp.to_ds->ds_object,
				    DS_FIELD_RESUME_TONAME, 1,
				    sizeof (dspp.saved_toname),
				    dspp.saved_toname);
			}
			/* Only disown if there was an error in the lookups */
			if (owned && (err != 0))
				dsl_dataset_disown(dspp.to_ds, dsflags, FTAG);

			kmem_strfree(name);
		} else {
			err = dsl_dataset_own(dspp.dp, tosnap, dsflags,
			    FTAG, &dspp.to_ds);
			if (err == 0)
				owned = B_TRUE;
		}
	} else {
		err = dsl_dataset_hold_flags(dspp.dp, tosnap, dsflags, FTAG,
		    &dspp.to_ds);
	}

	if (err != 0) {
		/* Note: dsl dataset is not owned at this point */
		dsl_pool_rele(dspp.dp, FTAG);
		return (err);
	}

	if (err != 0) {
		dsl_pool_rele(dspp.dp, FTAG);
		if (owned)
			dsl_dataset_disown(dspp.to_ds, dsflags, FTAG);
		else
			dsl_dataset_rele_flags(dspp.to_ds, dsflags, FTAG);
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
				if (!dsl_dataset_is_before(dspp.to_ds, fromds,
				    0)) {
					err = SET_ERROR(EXDEV);
				} else {
					zb->zbm_creation_txg =
					    dsl_dataset_phys(fromds)->
					    ds_creation_txg;
					zb->zbm_creation_time =
					    dsl_dataset_phys(fromds)->
					    ds_creation_time;
					zb->zbm_guid =
					    dsl_dataset_phys(fromds)->ds_guid;

					if (dsl_dataset_is_zapified(fromds)) {
						(void) zap_lookup(
						    dspp.dp->dp_meta_objset,
						    fromds->ds_object,
						    DS_FIELD_IVSET_GUID, 8, 1,
						    &zb->zbm_ivset_guid);
					}
				}
				dsl_dataset_rele(fromds, FTAG);
			}
		} else {
			err = dsl_bookmark_lookup(dspp.dp, fromsnap, dspp.to_ds,
			    zb);
		}

		if (err == 0) {
			/* dmu_send_impl will call dsl_pool_rele for us. */
			err = dmu_send_impl(&dspp);
		} else {
			dsl_pool_rele(dspp.dp, FTAG);
		}
	} else {
		err = dmu_send_impl(&dspp);
	}
	if (owned)
		dsl_dataset_disown(dspp.to_ds, dsflags, FTAG);
	else
		dsl_dataset_rele_flags(dspp.to_ds, dsflags, FTAG);
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
dmu_send_estimate_fast(dsl_dataset_t *origds, dsl_dataset_t *fromds,
    zfs_bookmark_phys_t *frombook, boolean_t stream_compressed,
    boolean_t saved, uint64_t *sizep)
{
	int err;
	dsl_dataset_t *ds = origds;
	uint64_t uncomp, comp;

	ASSERT(dsl_pool_config_held(origds->ds_dir->dd_pool));
	ASSERT(fromds == NULL || frombook == NULL);

	/*
	 * If this is a saved send we may actually be sending
	 * from the %recv clone used for resuming.
	 */
	if (saved) {
		objset_t *mos = origds->ds_dir->dd_pool->dp_meta_objset;
		uint64_t guid;
		char dsname[ZFS_MAX_DATASET_NAME_LEN + 6];

		dsl_dataset_name(origds, dsname);
		(void) strcat(dsname, "/");
		(void) strlcat(dsname, recv_clone_name, sizeof (dsname));

		err = dsl_dataset_hold(origds->ds_dir->dd_pool,
		    dsname, FTAG, &ds);
		if (err != ENOENT && err != 0) {
			return (err);
		} else if (err == ENOENT) {
			ds = origds;
		}

		/* check that this dataset has partially received data */
		err = zap_lookup(mos, ds->ds_object,
		    DS_FIELD_RESUME_TOGUID, 8, 1, &guid);
		if (err != 0) {
			err = SET_ERROR(err == ENOENT ? EINVAL : err);
			goto out;
		}

		err = zap_lookup(mos, ds->ds_object,
		    DS_FIELD_RESUME_TONAME, 1, sizeof (dsname), dsname);
		if (err != 0) {
			err = SET_ERROR(err == ENOENT ? EINVAL : err);
			goto out;
		}
	}

	/* tosnap must be a snapshot or the target of a saved send */
	if (!ds->ds_is_snapshot && ds == origds)
		return (SET_ERROR(EINVAL));

	if (fromds != NULL) {
		uint64_t used;
		if (!fromds->ds_is_snapshot) {
			err = SET_ERROR(EINVAL);
			goto out;
		}

		if (!dsl_dataset_is_before(ds, fromds, 0)) {
			err = SET_ERROR(EXDEV);
			goto out;
		}

		err = dsl_dataset_space_written(fromds, ds, &used, &comp,
		    &uncomp);
		if (err != 0)
			goto out;
	} else if (frombook != NULL) {
		uint64_t used;
		err = dsl_dataset_space_written_bookmark(frombook, ds, &used,
		    &comp, &uncomp);
		if (err != 0)
			goto out;
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

out:
	if (ds != origds)
		dsl_dataset_rele(ds, FTAG);
	return (err);
}

ZFS_MODULE_PARAM(zfs_send, zfs_send_, corrupt_data, INT, ZMOD_RW,
	"Allow sending corrupt data");

ZFS_MODULE_PARAM(zfs_send, zfs_send_, queue_length, UINT, ZMOD_RW,
	"Maximum send queue length");

ZFS_MODULE_PARAM(zfs_send, zfs_send_, unmodified_spill_blocks, INT, ZMOD_RW,
	"Send unmodified spill blocks");

ZFS_MODULE_PARAM(zfs_send, zfs_send_, queue_ff, UINT, ZMOD_RW,
	"Send queue fill fraction");

ZFS_MODULE_PARAM(zfs_send, zfs_, override_estimate_recordsize, UINT, ZMOD_RW,
	"Override block size estimate with fixed size");
