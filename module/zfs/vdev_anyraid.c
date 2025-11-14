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
 * Copyright (c) 2025, Klara Inc.
 */

/*
 * Anyraid vdevs are a way to get the benefits of mirror (and, in the future,
 * raidz) vdevs while using disks with mismatched sizes. The primary goal of
 * this feature is maximizing the available space of the provided devices.
 * Performance is secondary to that goal; nice to have, but not required. This
 * feature is also designed to work on modern hard drives: while the feature
 * will work on drives smaller than 1TB, the default tuning values are
 * optimized for drives of at least that size.
 *
 * Anyraid works by splitting the vdev into "tiles". Each tile is the same
 * size; by default, 1/64th of the size of the smallest disk in the vdev, or
 * 16GiB, whichever is larger. A tile represents an area of
 * logical-to-physical mapping: bytes within that logical tile are stored
 * physically together. Subsequent tiles may be stored in different locations
 * on the same disk, or different disks altogether. A mapping is stored on each
 * disk to enable the vdev to be read normally.
 *
 * When parity is not considered, this provides some small benefits (device
 * removal within the vdev is not yet implemented, but is very feasible, as is
 * rebalancing data onto new disks), but is not generally recommended. However,
 * if parity is considered, it is more useful. With mirror parity P, each
 * tile is allocated onto P separate disks, providing the reliability and
 * performance characteristics of a mirror vdev. In addition, because each tile
 * can be allocated separately, smaller drives can work together to mirror
 * larger ones dynamically and seamlessly.
 *
 * The mapping for these tiles is stored in a special area at the start of
 * each device. Each disk has 4 full copies of the tile map, which rotate
 * per txg in a similar manner to uberblocks. The tile map itself is 64MiB,
 * plus a small header (~8KiB) before it.
 *
 * The exact space that is allocatable in an anyraid vdev is not easy to
 * calculate in the general case. It's a variant of the bin-packing problem, so
 * an optimal solution is complex. However, this case seems to be a sub-problem
 * where greedy algorithms give optimal solutions, so that is what we do here.
 * Each tile is allocated from the P disks that have the most available
 * capacity. This does mean that calculating the size of a disk requires
 * running the allocation algorithm until completion, but for the relatively
 * small number of tiles we are working with, an O(n * log n) runtime is
 * acceptable.
 *
 * Currently, there is a limit of 2^24 tiles in an anyraid vdev: 2^8 disks,
 * and 2^16 tiles per disk. This means that by default, the largest device
 * that can be fully utilized by an anyraid vdev is 1024 times the size of the
 * smallest device that was present during device creation. This is not a
 * fundamental limit, and could be expanded in the future. However, this does
 * affect the size of the tile map. Currently, the tile map can always
 * store all tiles without running out of space; 2^24 4-byte entries is 2^26
 * bytes = 64MiB. Expanding the maximum number of tiles per disk or disks per
 * vdev would necessarily involve either expanding the tile map or adding
 * handling for the tile map running out of space.
 *
 * When it comes to performance, there is a tradeoff. While the per-disk I/O
 * rates are equivalent to using mirrors (because only a small amount of extra
 * logic is used on top of the mirror code), the overall vdev throughput may
 * not be. This is because the actively used tiles may be allocated to the
 * same devices, leaving other devices idle for writes. This is especially true
 * as the variation in drive sizes increases. To some extent, this problem is
 * fundamental: writes fill up disks. If we want to fill all the disks, smaller
 * disks will not be able to satisfy as many writes. Rewrite- and read-heavy
 * workloads will encounter this problem to a lesser extent. The performance
 * downsides can be mitigated with smaller tile sizes, larger metaslabs,
 * and more active metaslab allocators.
 *
 * Checkpoints are currently supported by storing the maximum allocated tile
 * at the time of the checkpoint, and then discarding all tiles after that
 * when a checkpoint is rolled back. Because device addition is forbidden while
 * a checkpoint is outstanding, no more complex logic is required.
 *
 * Currently, anyraid vdevs only work with mirror-type parity. However, plans
 * for future work include:
 *   Raidz-type parity
 *   Anyraid vdev shrinking via device removal
 *   Rebalancing after device addition
 *
 * Possible future work also includes:
 *   Enabling rebalancing with an outstanding checkpoint
 *   Trim and initialize beyond the end of the allocated tiles
 *   Store device asizes so we can make better allocation decisions while a
 *     device is faulted
 */

#include <sys/zfs_context.h>
#include <sys/metaslab_impl.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_anyraid.h>
#include <sys/vdev_anyraid_impl.h>
#include <sys/vdev_mirror.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_raidz_impl.h>
#include <sys/dsl_scan.h>

/*
 * The smallest allowable tile size. Shrinking this is mostly useful for
 * testing. Increasing it may be useful if you plan to add much larger disks to
 * an array in the future, and want to be sure their full capacity will be
 * usable.
 */
uint64_t zfs_anyraid_min_tile_size = (16ULL << 30);
/*
 * This controls how many tiles we have per disk (based on the smallest disk
 * present at creation time)
 */
int anyraid_disk_shift = 6;

/*
 * Maximum amount of copy io's outstanding at once.
 */
#ifdef _ILP32
static unsigned long anyraid_rebalance_max_copy_bytes = SPA_MAXBLOCKSIZE;
#else
static unsigned long anyraid_rebalance_max_copy_bytes = 10 * SPA_MAXBLOCKSIZE;
#endif

/*
 * Automatically start a pool scrub when a RAIDZ expansion completes in
 * order to verify the checksums of all blocks which have been copied
 * during the expansion.  Automatic scrubbing is enabled by default and
 * is strongly recommended.
 */
static int zfs_scrub_after_rebalance = 1;


static inline int
af_compar(const void *p1, const void *p2)
{
	const anyraid_free_node_t *af1 = p1, *af2 = p2;

	return (TREE_CMP(af2->afn_tile, af1->afn_tile));
}

void
anyraid_freelist_create(anyraid_freelist_t *af, uint16_t off)
{
	avl_create(&af->af_list, af_compar,
	    sizeof (anyraid_free_node_t),
	    offsetof(anyraid_free_node_t, afn_node));
	af->af_next_off = off;
}

void
anyraid_freelist_destroy(anyraid_freelist_t *af)
{
	void *cookie = NULL;
	anyraid_free_node_t *node;
	while ((node = avl_destroy_nodes(&af->af_list, &cookie)) != NULL)
		kmem_free(node, sizeof (*node));
	avl_destroy(&af->af_list);
}

void
anyraid_freelist_add(anyraid_freelist_t *af, uint16_t off)
{
	avl_tree_t *t = &af->af_list;
	ASSERT3U(off, <, af->af_next_off);
	if (off != af->af_next_off - 1) {
		anyraid_free_node_t *new = kmem_alloc(sizeof (*new), KM_SLEEP);
		new->afn_tile = off;
		avl_add(t, new);
		return;
	}
	af->af_next_off--;
	for (anyraid_free_node_t *tail = avl_last(t);
	    tail->afn_tile == af->af_next_off - 1; tail = avl_last(t)) {
		af->af_next_off--;
		avl_remove(t, tail);
		kmem_free(tail, sizeof (*tail));
	}
}

void
anyraid_freelist_remove(anyraid_freelist_t *af, uint16_t off)
{
	avl_tree_t *t = &af->af_list;
	anyraid_free_node_t search;
	search.afn_tile = off;
	avl_index_t where;
	anyraid_free_node_t *node = avl_find(t, &search, &where);
	if (node) {
		avl_remove(t, node);
		kmem_free(node, sizeof (*node));
		return;
	}
	ASSERT3U(off, >=, af->af_next_off);
	while (off > af->af_next_off) {
		node = kmem_alloc(sizeof (*node), KM_SLEEP);
		node->afn_tile = af->af_next_off++;
		avl_add(t, node);
	}
	af->af_next_off++;
	return;

}

uint16_t
anyraid_freelist_pop(anyraid_freelist_t *af)
{
	avl_tree_t *t = &af->af_list;
	if (avl_numnodes(t) == 0) {
		return (af->af_next_off++);
	}

	anyraid_free_node_t *head = avl_first(t);
	avl_remove(t, head);
	uint16_t ret = head->afn_tile;
	kmem_free(head, sizeof (*head));
	return (ret);
}

uint16_t
anyraid_freelist_alloc(const anyraid_freelist_t *af)
{
	return (af->af_next_off - avl_numnodes(&af->af_list));
}

static inline uint64_t
vdev_anyraid_header_offset(vdev_t *vd, int id)
{
	uint64_t full_size = VDEV_ANYRAID_SINGLE_MAP_SIZE(vd->vdev_ashift);
	if (id < VDEV_ANYRAID_START_COPES)
		return (VDEV_LABEL_START_SIZE + id * full_size);
	else
		return (vd->vdev_psize - VDEV_LABEL_END_SIZE -
		    (VDEV_ANYRAID_MAP_COPIES - id) * full_size);
}

static inline int
anyraid_tile_compare(const void *p1, const void *p2)
{
	const anyraid_tile_t *r1 = p1, *r2 = p2;

	return (TREE_CMP(r1->at_tile_id, r2->at_tile_id));
}

static inline int
anyraid_child_compare(const void *p1, const void *p2)
{
	const vdev_anyraid_node_t *van1 = p1, *van2 = p2;

	int cmp = TREE_CMP(
	    van2->van_capacity - anyraid_freelist_alloc(&van2->van_freelist),
	    van1->van_capacity - anyraid_freelist_alloc(&van1->van_freelist));
	if (cmp != 0)
		return (cmp);

	return (TREE_CMP(van1->van_id, van2->van_id));
}

/*
 * Initialize private VDEV specific fields from the nvlist.
 */
static int
vdev_anyraid_init(spa_t *spa, nvlist_t *nv, void **tsd)
{
	(void) spa;
	uint_t children;
	nvlist_t **child;
	int error = nvlist_lookup_nvlist_array(nv,
	    ZPOOL_CONFIG_CHILDREN, &child, &children);
	if (error != 0 || children > VDEV_ANYRAID_MAX_DISKS)
		return (SET_ERROR(EINVAL));

	uint64_t nparity;
	if (nvlist_lookup_uint64(nv, ZPOOL_CONFIG_NPARITY, &nparity) != 0)
		return (SET_ERROR(EINVAL));

	vdev_anyraid_parity_type_t parity_type = VAP_TYPES;
	if (nvlist_lookup_uint8(nv, ZPOOL_CONFIG_ANYRAID_PARITY_TYPE,
	    (uint8_t *)&parity_type) != 0)
		return (SET_ERROR(EINVAL));
	uint8_t ndata = 1;
	if (nvlist_lookup_uint8(nv, ZPOOL_CONFIG_ANYRAID_NDATA,
	    &ndata) != 0 && parity_type == VAP_RAIDZ) {
		return (SET_ERROR(EINVAL));
	}

	if (ndata + nparity > children) {
		zfs_dbgmsg("width too high when creating anyraid vdev");
		return (SET_ERROR(EINVAL));
	}

	vdev_anyraid_t *var = kmem_zalloc(sizeof (*var), KM_SLEEP);
	var->vd_parity_type = parity_type;
	var->vd_ndata = ndata;
	var->vd_nparity = nparity;
	switch (parity_type) {
		case VAP_MIRROR:
			var->vd_width = ndata;
			break;
		case VAP_RAIDZ:
			var->vd_width = ndata + nparity;
			break;
		default:
			PANIC("Invalid parity type %d", parity_type);
	}
	rw_init(&var->vd_lock, NULL, RW_DEFAULT, NULL);
	avl_create(&var->vd_tile_map, anyraid_tile_compare,
	    sizeof (anyraid_tile_t), offsetof(anyraid_tile_t, at_node));
	avl_create(&var->vd_children_tree, anyraid_child_compare,
	    sizeof (vdev_anyraid_node_t),
	    offsetof(vdev_anyraid_node_t, van_node));
	zfs_rangelock_init(&var->var_rangelock, NULL, NULL);

	var->vd_children = kmem_zalloc(sizeof (*var->vd_children) * children,
	    KM_SLEEP);
	for (int c = 0; c < children; c++) {
		vdev_anyraid_node_t *van = kmem_zalloc(sizeof (*van), KM_SLEEP);
		van->van_id = c;
		avl_add(&var->vd_children_tree, van);
		var->vd_children[c] = van;
	}

	*tsd = var;
	return (0);
}

static void
vdev_anyraid_fini(vdev_t *vd)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	avl_destroy(&var->vd_tile_map);

	vdev_anyraid_node_t *node;
	void *cookie = NULL;
	while ((node = avl_destroy_nodes(&var->vd_children_tree, &cookie))) {
		kmem_free(node, sizeof (*node));
	}
	avl_destroy(&var->vd_children_tree);
	zfs_rangelock_fini(&var->var_rangelock);

	rw_destroy(&var->vd_lock);
	kmem_free(var->vd_children,
	    sizeof (*var->vd_children) * vd->vdev_children);
	kmem_free(var, sizeof (*var));
}

/*
 * Add ANYRAID specific fields to the config nvlist.
 */
static void
vdev_anyraid_config_generate(vdev_t *vd, nvlist_t *nv)
{
	ASSERT(vdev_is_anyraid(vd));
	vdev_anyraid_t *var = vd->vdev_tsd;

	fnvlist_add_uint64(nv, ZPOOL_CONFIG_NPARITY, var->vd_nparity);
	fnvlist_add_uint8(nv, ZPOOL_CONFIG_ANYRAID_PARITY_TYPE,
	    (uint8_t)var->vd_parity_type);
	fnvlist_add_uint8(nv, ZPOOL_CONFIG_ANYRAID_NDATA,
	    (uint8_t)var->vd_ndata);
}

/*
 * Import/open related functions.
 */

/*
 * Add an entry to the tile map for the provided tile.
 */
static void
create_tile_entry(vdev_anyraid_t *var, anyraid_map_loc_entry_t *amle,
    uint8_t *pat_cnt, anyraid_tile_t **out_at, uint32_t *cur_tile)
{
	uint8_t disk = amle_get_disk(amle);
	uint16_t offset = amle_get_offset(amle);
	anyraid_tile_t *at = *out_at;

	if (*pat_cnt == 0) {
		at = kmem_alloc(sizeof (*at), KM_SLEEP);
		at->at_tile_id = *cur_tile;
		avl_add(&var->vd_tile_map, at);
		list_create(&at->at_list,
		    sizeof (anyraid_tile_node_t),
		    offsetof(anyraid_tile_node_t, atn_node));

		(*cur_tile)++;
	}

	anyraid_tile_node_t *atn = kmem_alloc(sizeof (*atn), KM_SLEEP);
	atn->atn_disk = disk;
	atn->atn_offset = offset;
	list_insert_tail(&at->at_list, atn);
	*pat_cnt = (*pat_cnt + 1) % (var->vd_nparity + var->vd_ndata);

	vdev_anyraid_node_t *van = var->vd_children[disk];
	avl_remove(&var->vd_children_tree, van);
	
	anyraid_freelist_remove(&van->van_freelist, offset);
	avl_add(&var->vd_children_tree, van);
	*out_at = at;
}

static void
child_read_done(zio_t *zio)
{
	zio_t *pio = zio_unique_parent(zio);
	abd_t **cbp = pio->io_private;

	if (zio->io_error == 0) {
		mutex_enter(&pio->io_lock);
		if (*cbp == NULL)
			*cbp = zio->io_abd;
		else
			abd_free(zio->io_abd);
		mutex_exit(&pio->io_lock);
	} else {
		abd_free(zio->io_abd);
	}
}

static void
child_read(zio_t *zio, vdev_t *vd, uint64_t offset, uint64_t size,
    int checksum, void *private, int flags)
{
	for (int c = 0; c < vd->vdev_children; c++) {
		child_read(zio, vd->vdev_child[c], offset, size, checksum,
		    private, flags);
	}

	if (vd->vdev_ops->vdev_op_leaf && vdev_readable(vd)) {
		zio_nowait(zio_read_phys(zio, vd, offset, size,
		    abd_alloc_linear(size, B_TRUE), checksum,
		    child_read_done, private, ZIO_PRIORITY_SYNC_READ, flags,
		    B_FALSE));
	}
}

/*
 * This function is non-static for ZDB, and shouldn't be used for anything else.
 * Utility function that issues the read for the header and parses out the
 * nvlist.
 */
int
vdev_anyraid_open_header(vdev_t *cvd, int header, anyraid_header_t *out_header)
{
	spa_t *spa = cvd->vdev_spa;
	uint64_t ashift = cvd->vdev_ashift;
	uint64_t header_offset = vdev_anyraid_header_offset(cvd, header);
	uint64_t header_size = VDEV_ANYRAID_MAP_HEADER_SIZE(ashift);
	int flags = ZIO_FLAG_CONFIG_WRITER | ZIO_FLAG_CANFAIL |
	    ZIO_FLAG_SPECULATIVE;

	abd_t *header_abd = NULL;
	zio_t *rio = zio_root(spa, NULL, &header_abd, flags);
	child_read(rio, cvd, header_offset, header_size, ZIO_CHECKSUM_LABEL,
	    NULL, flags);

	int error;
	if ((error = zio_wait(rio)) != 0) {
		zfs_dbgmsg("Error %d reading anyraid header %d on vdev %s",
		    error, header, cvd->vdev_path);
		abd_free(header_abd);
		return (error);
	}

	char *header_buf = abd_borrow_buf(header_abd, header_size);
	nvlist_t *header_nvl;
	error = nvlist_unpack(header_buf, header_size, &header_nvl,
	    KM_SLEEP);
	if (error != 0) {
		zfs_dbgmsg("Error %d unpacking anyraid header %d on vdev %s",
		    error, header, cvd->vdev_path);
		abd_return_buf(header_abd, header_buf, header_size);
		abd_free(header_abd);
		return (error);
	}
	out_header->ah_abd = header_abd;
	out_header->ah_buf = header_buf;
	out_header->ah_nvl = header_nvl;

	return (0);
}

static void
free_header(anyraid_header_t *header, uint64_t header_size) {
	fnvlist_free(header->ah_nvl);
	abd_return_buf(header->ah_abd, header->ah_buf, header_size);
	abd_free(header->ah_abd);
}

/*
 * This function is non-static for ZDB, and shouldn't be used for anything else.
 *
 * Iterate over all the copies of the map for the given child vdev and select
 * the best one.
 */
int
vdev_anyraid_pick_best_mapping(vdev_t *cvd, uint64_t *out_txg,
    anyraid_header_t *out_header, int *out_mapping)
{
	spa_t *spa = cvd->vdev_spa;
	uint64_t ashift = cvd->vdev_ashift;
	int error = 0;
	uint64_t header_size = VDEV_ANYRAID_MAP_HEADER_SIZE(ashift);

	int best_mapping = -1;
	uint64_t best_txg = 0;
	anyraid_header_t best_header = {0};
	boolean_t checkpoint_rb = spa_importing_checkpoint(spa);

	for (int i = 0; i < VDEV_ANYRAID_MAP_COPIES; i++) {
		anyraid_header_t header;
		error = vdev_anyraid_open_header(cvd, i, &header);

		if (error)
			continue;

		nvlist_t *hnvl = header.ah_nvl;
		uint16_t version;
		if ((error = nvlist_lookup_uint16(hnvl,
		    VDEV_ANYRAID_HEADER_VERSION, &version)) != 0) {
			free_header(&header, header_size);
			zfs_dbgmsg("Anyraid header %d on vdev %s: missing "
			    "version", i, cvd->vdev_path);
			continue;
		}
		if (version != 0) {
			free_header(&header, header_size);
			error = SET_ERROR(ENOTSUP);
			zfs_dbgmsg("Anyraid header %d on vdev %s: invalid "
			    "version", i, cvd->vdev_path);
			continue;
		}

		uint64_t pool_guid = 0;
		if (nvlist_lookup_uint64(hnvl, VDEV_ANYRAID_HEADER_GUID,
		    &pool_guid) != 0 || pool_guid != spa_guid(spa)) {
			free_header(&header, header_size);
			error = SET_ERROR(EINVAL);
			zfs_dbgmsg("Anyraid header %d on vdev %s: guid "
			    "mismatch: %llu %llu", i, cvd->vdev_path,
			    (u_longlong_t)pool_guid,
			    (u_longlong_t)spa_guid(spa));
			continue;
		}

		uint64_t written_txg;
		if (nvlist_lookup_uint64(hnvl, VDEV_ANYRAID_HEADER_TXG,
		    &written_txg) != 0) {
			free_header(&header, header_size);
			error = SET_ERROR(EINVAL);
			zfs_dbgmsg("Anyraid header %d on vdev %s: no txg",
			    i, cvd->vdev_path);
			continue;
		}
		/*
		 * If we're reopening, the current txg hasn't been synced out
		 * yet; look for one txg earlier.
		 */
		uint64_t min_txg = spa_current_txg(spa) -
		    (cvd->vdev_parent->vdev_reopening ? 1 : 0);
		if ((written_txg < min_txg && !checkpoint_rb) ||
		    written_txg > spa_load_max_txg(spa)) {
			free_header(&header, header_size);
			error = SET_ERROR(EINVAL);
			zfs_dbgmsg("Anyraid header %d on vdev %s: txg %llu out "
			    "of bounds (%llu, %llu)", i, cvd->vdev_path,
			    (u_longlong_t)written_txg,
			    (u_longlong_t)min_txg,
			    (u_longlong_t)spa_load_max_txg(spa));
			continue;
		}
		if (written_txg > best_txg) {
			best_txg = written_txg;
			best_mapping = i;
			if (best_header.ah_nvl)
				free_header(&best_header, header_size);

			best_header = header;
		} else {
			free_header(&header, header_size);
		}
	}

	if (best_txg != 0) {
		*out_txg = best_txg;
		*out_mapping = best_mapping;
		*out_header = best_header;
		return (0);
	}
	ASSERT(error);
	return (error);
}

static int
anyraid_open_existing(vdev_t *vd, uint64_t child, uint16_t **child_capacities)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	vdev_t *cvd = vd->vdev_child[child];
	uint64_t ashift = cvd->vdev_ashift;
	spa_t *spa = vd->vdev_spa;
	int flags = ZIO_FLAG_CONFIG_WRITER | ZIO_FLAG_CANFAIL |
	    ZIO_FLAG_SPECULATIVE;
	uint64_t header_size = VDEV_ANYRAID_MAP_HEADER_SIZE(ashift);
	boolean_t checkpoint_rb = spa_importing_checkpoint(spa);

	anyraid_header_t header;
	int mapping;
	uint64_t txg;
	int error = vdev_anyraid_pick_best_mapping(cvd, &txg, &header,
	    &mapping);
	if (error)
		return (error);

	uint8_t disk_id;
	if (nvlist_lookup_uint8(header.ah_nvl, VDEV_ANYRAID_HEADER_DISK,
	    &disk_id) != 0) {
		zfs_dbgmsg("Error opening anyraid vdev %llu: No disk ID",
		    (u_longlong_t)vd->vdev_id);
		free_header(&header, header_size);
		return (SET_ERROR(EINVAL));
	}

	uint64_t tile_size;
	if (nvlist_lookup_uint64(header.ah_nvl, VDEV_ANYRAID_HEADER_TILE_SIZE,
	    &tile_size) != 0) {
		zfs_dbgmsg("Error opening anyraid vdev %llu: No tile size",
		    (u_longlong_t)vd->vdev_id);
		free_header(&header, header_size);
		return (SET_ERROR(EINVAL));
	}

	uint32_t map_length;
	if (nvlist_lookup_uint32(header.ah_nvl, VDEV_ANYRAID_HEADER_LENGTH,
	    &map_length) != 0) {
		zfs_dbgmsg("Error opening anyraid vdev %llu: No map length",
		    (u_longlong_t)vd->vdev_id);
		free_header(&header, header_size);
		return (SET_ERROR(EINVAL));
	}

	uint16_t *caps = NULL;
	uint_t count;
	if (nvlist_lookup_uint16_array(header.ah_nvl,
	    VDEV_ANYRAID_HEADER_DISK_SIZES, &caps, &count) != 0) {
		zfs_dbgmsg("Error opening anyraid vdev %llu: No child sizes",
		    (u_longlong_t)vd->vdev_id);
		free_header(&header, header_size);
		return (SET_ERROR(EINVAL));
	}
	if (count != vd->vdev_children) {
		zfs_dbgmsg("Error opening anyraid vdev %llu: Incorrect child "
		    "count %u vs %u", (u_longlong_t)vd->vdev_id, count,
		    (uint_t)vd->vdev_children);
		free_header(&header, header_size);
		return (SET_ERROR(EINVAL));
	}

	*child_capacities = kmem_alloc(sizeof (*caps) * count, KM_SLEEP);
	memcpy(*child_capacities, caps, sizeof (*caps) * count);
	if (vd->vdev_reopening) {
		free_header(&header, header_size);
		return (0);
	}

	var->vd_checkpoint_tile = UINT32_MAX;
	(void) nvlist_lookup_uint32(header.ah_nvl,
	    VDEV_ANYRAID_HEADER_CHECKPOINT, &var->vd_checkpoint_tile);

	/*
	 * Because the tile map is 64 MiB and the maximum IO size is 16MiB,
	 * we may need to issue up to 4 reads to read in the whole thing.
	 * Similarly, when processing the mapping, we need to iterate across
	 * the 4 separate buffers.
	 */
	zio_t *rio = zio_root(spa, NULL, NULL, flags);
	abd_t *map_abds[VDEV_ANYRAID_MAP_COPIES] = {0};
	uint64_t header_offset = vdev_anyraid_header_offset(cvd, mapping);
	uint64_t map_offset = header_offset + header_size;
	int i;
	for (i = 0; i <= (map_length / SPA_MAXBLOCKSIZE); i++) {
		zio_eck_t *cksum = (zio_eck_t *)
		    &header.ah_buf[VDEV_ANYRAID_NVL_BYTES(ashift) +
		    i * sizeof (*cksum)];
		zio_t *nio = zio_null(rio, spa, cvd, NULL, &map_abds[i], flags);
		child_read(nio, cvd, map_offset + i * SPA_MAXBLOCKSIZE,
		    SPA_MAXBLOCKSIZE, ZIO_CHECKSUM_ANYRAID_MAP, cksum, flags);
		zio_nowait(nio);
	}
	i--;

	if ((error = zio_wait(rio))) {
		for (; i >= 0; i--)
			abd_free(map_abds[i]);
		free_header(&header, header_size);
		zfs_dbgmsg("Error opening anyraid vdev %llu: map read error %d",
		    (u_longlong_t)vd->vdev_id, error);
		return (error);
	}
	free_header(&header, header_size);

	uint32_t map = -1, cur_tile = 0;
	/*
	 * For now, all entries are the size of a uint32_t. If that
	 * ever changes, the logic here needs to be altered to work for
	 * adaptive sizes, including entries split across 16MiB boundaries.
	 */
	uint32_t size = sizeof (anyraid_map_loc_entry_t);
	uint8_t *map_buf = NULL;
	uint8_t pat_cnt = 0;
	anyraid_tile_t *at = NULL;
	for (uint32_t off = 0; off < map_length; off += size) {
		if (checkpoint_rb && cur_tile > var->vd_checkpoint_tile &&
		    pat_cnt == 0)
			break;

		int next_map = off / SPA_MAXBLOCKSIZE;
		if (map != next_map) {
			// switch maps
			if (map != -1) {
				abd_return_buf(map_abds[map], map_buf,
				    SPA_MAXBLOCKSIZE);
			}
			map_buf = abd_borrow_buf(map_abds[next_map],
			    SPA_MAXBLOCKSIZE);
			map = next_map;

#ifdef _ZFS_BIG_ENDIAN
			uint32_t length = map_length -
			    next_map * SPA_MAXBLOCKSIZE;
			byteswap_uint32_array(map_buf, MIN(length,
			    SPA_MAXBLOCKSIZE));
#endif
		}
		anyraid_map_entry_t *entry =
		    (anyraid_map_entry_t *)(map_buf + (off % SPA_MAXBLOCKSIZE));
		uint8_t type = ame_get_type(entry);
		switch (type) {
			case AMET_SKIP: {
				anyraid_map_skip_entry_t *amse =
				    &entry->ame_u.ame_amse;
				ASSERT0(pat_cnt);
				cur_tile += amse_get_skip_count(amse);
				break;
			}
			case AMET_LOC: {
				anyraid_map_loc_entry_t *amle =
				    &entry->ame_u.ame_amle;
				create_tile_entry(var, amle, &pat_cnt, &at,
				    &cur_tile);
				break;
			}
			default:
				PANIC("Invalid entry type %d", type);
		}
	}
	if (map_buf)
		abd_return_buf(map_abds[map], map_buf, SPA_MAXBLOCKSIZE);

	var->vd_tile_size = tile_size;

	for (; i >= 0; i--)
		abd_free(map_abds[i]);

	/*
	 * Now that we have the tile map read in, we have to reopen the
	 * children to properly set and handle the min_asize
	 */
	for (; i < vd->vdev_children; i++) {
		vdev_t *cvd = vd->vdev_child[i];
		vdev_reopen(cvd);
	}

	int lasterror = 0;
	int numerrors = 0;
	for (int c = 0; c < vd->vdev_children; c++) {
		vdev_t *cvd = vd->vdev_child[c];

		if (cvd->vdev_open_error != 0) {
			lasterror = cvd->vdev_open_error;
			numerrors++;
			continue;
		}
	}

	if (numerrors > var->vd_nparity) {
		vd->vdev_stat.vs_aux = VDEV_AUX_NO_REPLICAS;
		return (lasterror);
	}

	return (0);
}

/*
 * When creating a new anyraid vdev, this function calculates the tile size
 * to use. We take (by default) 1/64th of the size of the smallest disk or 16
 * GiB, whichever is larger.
 */
static int
anyraid_calculate_size(vdev_t *vd)
{
	vdev_anyraid_t *var = vd->vdev_tsd;

	uint64_t smallest_disk_size = UINT64_MAX;
	for (int c = 0; c < vd->vdev_children; c++) {
		vdev_t *cvd = vd->vdev_child[c];
		smallest_disk_size = MIN(smallest_disk_size, cvd->vdev_asize -
		    VDEV_ANYRAID_TOTAL_MAP_SIZE(cvd->vdev_ashift));
	}

	uint64_t disk_shift = anyraid_disk_shift;
	uint64_t min_size = zfs_anyraid_min_tile_size;
	if (smallest_disk_size < 1 << disk_shift ||
	    smallest_disk_size < min_size) {
		return (SET_ERROR(ENOLCK));
	}


	ASSERT3U(smallest_disk_size, !=, UINT64_MAX);
	uint64_t tile_size = smallest_disk_size >> disk_shift;
	tile_size = MAX(tile_size, min_size);
	var->vd_tile_size = 1ULL << (highbit64(tile_size - 1));

	/*
	 * Later, we're going to cap the metaslab size at the tile
	 * size, so we need a tile to hold at least enough to store a
	 * max-size block, or we'll assert in that code.
	 */
	if (var->vd_tile_size * var->vd_ndata < SPA_MAXBLOCKSIZE)
		return (SET_ERROR(ENOSPC));
	return (0);
}

struct tile_count {
	avl_node_t node;
	int disk;
	int remaining;
};

static int
rc_compar(const void *a, const void *b)
{
	const struct tile_count *ra = a;
	const struct tile_count *rb = b;

	int cmp = TREE_CMP(rb->remaining, ra->remaining);
	if (cmp != 0)
		return (cmp);
	return (TREE_CMP(rb->disk, ra->disk));
}

/*
 * I think the only way to calculate the asize for anyraid devices is to
 * actually run the allocation algorithm and see what we end up with. It's a
 * variant of the bin-packing problem, which is NP-hard. Thankfully
 * a first-fit descending algorithm seems to give optimal results for this
 * variant.
 */
static uint64_t
calculate_asize(vdev_t *vd, uint64_t *num_tiles)
{
	vdev_anyraid_t *var = vd->vdev_tsd;

	if (var->vd_nparity == 0) {
		uint64_t count = 0;
		for (int c = 0; c < vd->vdev_children; c++) {
			count += num_tiles[c];
		}
		return (count * var->vd_tile_size);
	}

	/*
	 * Sort the disks by the number of additional tiles they can store.
	 */
	avl_tree_t t;
	avl_create(&t, rc_compar, sizeof (struct tile_count),
	    offsetof(struct tile_count, node));
	for (int c = 0; c < vd->vdev_children; c++) {
		if (num_tiles[c] == 0) {
			ASSERT(vd->vdev_child[c]->vdev_open_error);
			continue;
		}
		struct tile_count *rc = kmem_alloc(sizeof (*rc), KM_SLEEP);
		rc->disk = c;
		rc->remaining = num_tiles[c] -
		    anyraid_freelist_alloc(&var->vd_children[c]->van_freelist);
		avl_add(&t, rc);
	}

	uint32_t map_width = var->vd_nparity + var->vd_ndata;
	uint64_t count = avl_numnodes(&var->vd_tile_map);
	struct tile_count **cur = kmem_alloc(sizeof (*cur) * map_width,
	    KM_SLEEP);
	for (;;) {
		/* Grab the nparity + 1 children with the most free capacity */
		for (int c = 0; c < map_width; c++) {
			struct tile_count *rc = avl_first(&t);
			ASSERT(rc);
			cur[c] = rc;
			avl_remove(&t, rc);
		}
		struct tile_count *rc = cur[map_width - 1];
		struct tile_count *next = avl_first(&t);
		uint64_t next_rem = next == NULL ? 0 : next->remaining;
		ASSERT3U(next_rem, <=, rc->remaining);
		/* If one of the top N + 1 has no capacity left, we're done */
		if (rc->remaining == 0)
			break;

		/*
		 * This is a performance optimization; if the child with the
		 * lowest free capacity of the ones we've selected has N more
		 * capacity than the next child, the next N iterations would
		 * all select the same children. So to save time, we add N
		 * tiles right now and reduce our iteration count.
		 */
		uint64_t this_iter = MAX(1, rc->remaining - next_rem);
		count += this_iter;

		/* Re-add the selected children with their reduced capacity */
		for (int c = 0; c < map_width; c++) {
			ASSERT3U(cur[c]->remaining, >=, this_iter);
			cur[c]->remaining -= this_iter;
			avl_add(&t, cur[c]);
		}
	}
	for (int c = 0; c < map_width; c++)
		kmem_free(cur[c], sizeof (*cur[c]));
	kmem_free(cur, sizeof (*cur) * map_width);
	void *cookie = NULL;
	struct tile_count *node;

	while ((node = avl_destroy_nodes(&t, &cookie)) != NULL)
		kmem_free(node, sizeof (*node));
	avl_destroy(&t);
	return (count * var->vd_width * var->vd_tile_size);
}

static int
vdev_anyraid_open(vdev_t *vd, uint64_t *asize, uint64_t *max_asize,
    uint64_t *logical_ashift, uint64_t *physical_ashift)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	int lasterror = 0;
	int numerrors = 0;

	vdev_open_children(vd);

	for (int c = 0; c < vd->vdev_children; c++) {
		vdev_t *cvd = vd->vdev_child[c];

		if (cvd->vdev_open_error != 0) {
			lasterror = cvd->vdev_open_error;
			numerrors++;
			continue;
		}
	}

	/*
	 * If we have more faulted disks than parity, we can't open the device.
	 */
	if (numerrors > var->vd_nparity) {
		vd->vdev_stat.vs_aux = VDEV_AUX_NO_REPLICAS;
		return (lasterror);
	}

	uint16_t *child_capacities = NULL;
	if (vd->vdev_reopening) {
		child_capacities = kmem_alloc(sizeof (*child_capacities) *
		    vd->vdev_children, KM_SLEEP);
		for (uint64_t c = 0; c < vd->vdev_children; c++) {
			child_capacities[c] = var->vd_children[c]->van_capacity;
		}
	} else if (spa_load_state(vd->vdev_spa) != SPA_LOAD_CREATE &&
	    spa_load_state(vd->vdev_spa) != SPA_LOAD_ERROR &&
	    spa_load_state(vd->vdev_spa) != SPA_LOAD_NONE) {
		for (uint64_t c = 0; c < vd->vdev_children; c++) {
			vdev_t *cvd = vd->vdev_child[c];
			if (cvd->vdev_open_error != 0)
				continue;
			if ((lasterror = anyraid_open_existing(vd, c,
			    &child_capacities)) == 0)
				break;
		}
		if (lasterror)
			return (lasterror);
	} else if ((lasterror = anyraid_calculate_size(vd))) {
		return (lasterror);
	}

	uint64_t max_size = VDEV_ANYRAID_MAX_TPD * var->vd_tile_size;

	/*
	 * Calculate the number of tiles each child could fit, then use that
	 * to calculate the asize and min_asize.
	 */
	uint64_t *num_tiles = kmem_zalloc(vd->vdev_children *
	    sizeof (*num_tiles), KM_SLEEP);
	for (int c = 0; c < vd->vdev_children; c++) {
		vdev_t *cvd = vd->vdev_child[c];

		uint64_t casize;
		if (cvd->vdev_open_error == 0) {
			vdev_set_min_asize(cvd);
			casize = MIN(max_size, cvd->vdev_asize -
			    VDEV_ANYRAID_TOTAL_MAP_SIZE(cvd->vdev_ashift));
		} else {
			ASSERT(child_capacities);
			casize = (child_capacities[c] + 1) * var->vd_tile_size;
		}

		num_tiles[c] = casize / var->vd_tile_size;
		avl_remove(&var->vd_children_tree, var->vd_children[c]);
		/*
		 * We store the capacity minus 1, since a vdev can never have 0
		 * and they can have (which would overflow a uint16_t).
		 */
		var->vd_children[c]->van_capacity = num_tiles[c] - 1;
		avl_add(&var->vd_children_tree, var->vd_children[c]);
	}
	*asize = calculate_asize(vd, num_tiles);

	for (int c = 0; c < vd->vdev_children; c++) {
		vdev_t *cvd = vd->vdev_child[c];

		uint64_t cmasize;
		if (cvd->vdev_open_error == 0) {
			cmasize = MIN(max_size, cvd->vdev_max_asize -
			    VDEV_ANYRAID_TOTAL_MAP_SIZE(cvd->vdev_ashift));
		} else {
			cmasize = (child_capacities[c] + 1) * var->vd_tile_size;
		}

		num_tiles[c] = cmasize / var->vd_tile_size;
	}
	*max_asize = calculate_asize(vd, num_tiles);

	if (child_capacities) {
		kmem_free(child_capacities, sizeof (*child_capacities) *
		    vd->vdev_children);
	}
	for (int c = 0; c < vd->vdev_children; c++) {
		vdev_t *cvd = vd->vdev_child[c];

		if (cvd->vdev_open_error != 0)
			continue;

		*logical_ashift = MAX(*logical_ashift, cvd->vdev_ashift);
		*physical_ashift = vdev_best_ashift(*logical_ashift,
		    *physical_ashift, cvd->vdev_physical_ashift);
	}
	kmem_free(num_tiles, vd->vdev_children * sizeof (*num_tiles));
	return (0);
}

/*
 * We cap the metaslab size at the tile size. This prevents us from having to
 * split IOs across multiple tiles, which would be complex extra logic for
 * little gain.
 */
static void
vdev_anyraid_metaslab_size(vdev_t *vd, uint64_t *shiftp)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	*shiftp = MIN(*shiftp, highbit64(var->vd_tile_size * var->vd_width) -
	    1);
}

static void
vdev_anyraid_close(vdev_t *vd)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	for (int c = 0; c < vd->vdev_children; c++) {
		if (vd->vdev_child[c] != NULL)
			vdev_close(vd->vdev_child[c]);
	}
	if (vd->vdev_reopening)
		return;
	anyraid_tile_t *tile = NULL;
	void *cookie = NULL;
	while ((tile = avl_destroy_nodes(&var->vd_tile_map, &cookie))) {
		if (var->vd_nparity != 0) {
			anyraid_tile_node_t *atn = NULL;
			while ((atn = list_remove_head(&tile->at_list))) {
				kmem_free(atn, sizeof (*atn));
			}
			list_destroy(&tile->at_list);
		}
		kmem_free(tile, sizeof (*tile));
	}
}

/*
 * Configure the mirror_map and then hand the write off to the normal mirror
 * logic.
 */
static void
vdev_anyraid_mirror_start(zio_t *zio, anyraid_tile_t *tile,
    zfs_locked_range_t *lr)
{
	vdev_t *vd = zio->io_vd;
	vdev_anyraid_t *var = vd->vdev_tsd;
	mirror_map_t *mm = vdev_mirror_map_alloc(var->vd_nparity + 1, B_FALSE,
	    B_FALSE);
	uint64_t tsize = var->vd_tile_size;

	anyraid_tile_node_t *atn = list_head(&tile->at_list);
	for (int c = 0; c < mm->mm_children; c++) {
		ASSERT(atn);
		mirror_child_t *mc = &mm->mm_child[c];
		mc->mc_vd = vd->vdev_child[atn->atn_disk];
		mc->mc_offset = VDEV_ANYRAID_START_OFFSET(vd->vdev_ashift) +
		    atn->atn_offset * tsize + zio->io_offset % tsize;
		ASSERT3U(mc->mc_offset, <, mc->mc_vd->vdev_psize -
		    VDEV_LABEL_END_SIZE);
		mm->mm_rebuilding = mc->mc_rebuilding = B_FALSE;
		atn = list_next(&tile->at_list, atn);
	}
	ASSERT(atn == NULL);

	zio->io_aux_vsd = lr;
	zio->io_vsd = mm;
	zio->io_vsd_ops = &vdev_mirror_vsd_ops;

	vdev_mirror_io_start_impl(zio, mm);
}

/*
 * Translate the allocated and configured raidz map to use the proper disks
 * based on the anyraid tile mapping.
 */
static void
vdev_anyraid_raidz_map_translate(vdev_t *vd, raidz_map_t *rm,
    anyraid_tile_t *tile)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	ASSERT3U(rm->rm_nrows, ==, 1);
	raidz_row_t *rr = rm->rm_row[0];
	anyraid_tile_node_t **mapping = kmem_zalloc(sizeof (*mapping) *
	    var->vd_width, KM_SLEEP);
	ASSERT(tile);
	anyraid_tile_node_t *atn = list_head(&tile->at_list);
	for (int i = 0; i < var->vd_width; i++) {
		ASSERT(atn);
		mapping[i] = atn;
		atn = list_next(&tile->at_list, atn);
	}
	ASSERT3U(rr->rr_scols, <=, var->vd_width);
	for (uint64_t c = 0; c < rr->rr_scols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];
		atn = mapping[rc->rc_devidx];
		uint64_t tile_off = rc->rc_offset % var->vd_tile_size;
		uint64_t disk_off = tile_off +
		    atn->atn_offset * var->vd_tile_size;
		rc->rc_offset = VDEV_ANYRAID_TOTAL_MAP_SIZE(vd->vdev_ashift) +
		    disk_off;
		rc->rc_devidx = atn->atn_disk;
	}
	kmem_free(mapping, sizeof (*mapping) * var->vd_width);
}

/*
 * Configure the raidz_map and then hand the write off to the normal raidz
 * logic.
 */
static void
vdev_anyraid_raidz_start(zio_t *zio, anyraid_tile_t *tile, zfs_locked_range_t *lr)
{
	vdev_t *vd = zio->io_vd;
	vdev_anyraid_t *var = vd->vdev_tsd;
	raidz_map_t *rm = vdev_raidz_map_alloc(zio, vd->vdev_ashift,
	    var->vd_width, var->vd_nparity);
	vdev_anyraid_raidz_map_translate(vd, rm, tile);

	zio->io_vsd = rm;
	zio->io_vsd_ops = &vdev_raidz_vsd_ops;
	zio->io_aux_vsd = lr;
	vdev_raidz_io_start_impl(zio, rm, var->vd_width, var->vd_width);
}

typedef struct anyraid_map {
	abd_t *am_abd;
} anyraid_map_t;

static void
vdev_anyraid_child_done(zio_t *zio)
{
	zio_t *pio = zio->io_private;
	mutex_enter(&pio->io_lock);
	pio->io_error = zio_worst_error(pio->io_error, zio->io_error);
	mutex_exit(&pio->io_lock);
}

static void
vdev_anyraid_map_free_vsd(zio_t *zio)
{
	anyraid_map_t *am = zio->io_vsd;
	abd_free(am->am_abd);
	am->am_abd = NULL;
	kmem_free(am, sizeof (*am));
}

const zio_vsd_ops_t vdev_anyraid_vsd_ops = {
	.vsd_free = vdev_anyraid_map_free_vsd,
};

static void
vdev_anyraid_io_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_anyraid_t *var = vd->vdev_tsd;
	uint64_t tsize = var->vd_tile_size * var->vd_width;

	uint64_t start_tile_id = zio->io_offset / tsize;
	anyraid_tile_t search;
	search.at_tile_id = start_tile_id;
	avl_index_t where;
	rw_enter(&var->vd_lock, RW_READER);
	anyraid_tile_t *tile = avl_find(&var->vd_tile_map, &search,
	    &where);

	/*
	 * If we're doing an I/O somewhere that hasn't been allocated yet, we
	 * may need to allocate a new tile. Upgrade to a write lock so we can
	 * safely modify the data structure, and then check if someone else
	 * beat us to it.
	 */
	if (tile == NULL) {
		rw_exit(&var->vd_lock);
		rw_enter(&var->vd_lock, RW_WRITER);
		tile = avl_find(&var->vd_tile_map, &search, &where);
	}
	if (tile == NULL) {
		ASSERT3U(zio->io_type, ==, ZIO_TYPE_WRITE);
		zfs_dbgmsg("Allocating tile %llu for zio %px",
		    (u_longlong_t)start_tile_id, zio);
		tile = kmem_alloc(sizeof (*tile), KM_SLEEP);
		tile->at_tile_id = start_tile_id;
		list_create(&tile->at_list, sizeof (anyraid_tile_node_t),
		    offsetof(anyraid_tile_node_t, atn_node));

		uint_t width = var->vd_nparity + var->vd_ndata;
		vdev_anyraid_node_t **vans = kmem_alloc(sizeof (*vans) * width,
		    KM_SLEEP);
		for (int i = 0; i < width; i++) {
			vans[i] = avl_first(&var->vd_children_tree);
			avl_remove(&var->vd_children_tree, vans[i]);

			anyraid_tile_node_t *atn =
			    kmem_alloc(sizeof (*atn), KM_SLEEP);
			atn->atn_disk = vans[i]->van_id;
			atn->atn_offset =
			    anyraid_freelist_pop(&vans[i]->van_freelist);
			list_insert_tail(&tile->at_list, atn);
			zfs_dbgmsg("Entry %d: %u %u", i, atn->atn_disk,
			    atn->atn_offset);
		}
		for (int i = 0; i < width; i++)
			avl_add(&var->vd_children_tree, vans[i]);

		kmem_free(vans, sizeof (*vans) * width);
		avl_insert(&var->vd_tile_map, tile, where);
	}
	rw_exit(&var->vd_lock);

	uint64_t end = zio->io_offset % tsize + zio->io_size;
	ASSERT3U(end, <=, tsize);
	zfs_locked_range_t *lr = zfs_rangelock_enter(&var->var_rangelock,
	    zio->io_offset, zio->io_size, RL_READER);

	switch (var->vd_parity_type) {
		case VAP_MIRROR:
			if (var->vd_nparity > 0) {
				vdev_anyraid_mirror_start(zio, tile, lr);
				zio_execute(zio);
				return;
			}
			break;
		case VAP_RAIDZ:
			vdev_anyraid_raidz_start(zio, tile, lr);
			zio_execute(zio);
			return;
		default:
			ASSERT0(1);
			PANIC("Invalid parity type: %d", var->vd_parity_type);
	}


	anyraid_tile_node_t *atn = list_head(&tile->at_list);
	vdev_t *cvd = vd->vdev_child[atn->atn_disk];
	uint64_t child_offset = atn->atn_offset * tsize +
	    zio->io_offset % tsize;
	child_offset += VDEV_ANYRAID_START_OFFSET(vd->vdev_ashift);

	anyraid_map_t *mm = kmem_alloc(sizeof (*mm), KM_SLEEP);
	mm->am_abd = abd_get_offset(zio->io_abd, 0);
	zio->io_vsd = mm;
	zio->io_vsd_ops = &vdev_anyraid_vsd_ops;
	zio->io_aux_vsd = lr;

	zio_t *cio = zio_vdev_child_io(zio, NULL, cvd, child_offset,
	    mm->am_abd, zio->io_size, zio->io_type, zio->io_priority, 0,
	    vdev_anyraid_child_done, zio);
	zio_nowait(cio);

	zio_execute(zio);
}

static void
vdev_anyraid_io_done(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_anyraid_t *var = vd->vdev_tsd;

	zfs_locked_range_t *lr = zio->io_aux_vsd;
	ASSERT(lr);
	zfs_rangelock_exit(lr);
	zio->io_aux_vsd = NULL;

	switch (var->vd_parity_type) {
		case VAP_MIRROR:
			if (var->vd_nparity > 0) {
				vdev_mirror_io_done(zio);
				return;
			}
			break;
		case VAP_RAIDZ:
			vdev_raidz_io_done(zio);
			return;
		default:
			panic("Invalid parity type: %d", var->vd_parity_type);
	}
}

static void
vdev_anyraid_state_change(vdev_t *vd, int faulted, int degraded)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	if (faulted > var->vd_nparity) {
		vdev_set_state(vd, B_FALSE, VDEV_STATE_CANT_OPEN,
		    VDEV_AUX_NO_REPLICAS);
	} else if (degraded + faulted != 0) {
		vdev_set_state(vd, B_FALSE, VDEV_STATE_DEGRADED, VDEV_AUX_NONE);
	} else {
		vdev_set_state(vd, B_FALSE, VDEV_STATE_HEALTHY, VDEV_AUX_NONE);
	}
}

/*
 * Determine if any portion of the provided block resides on a child vdev
 * with a dirty DTL and therefore needs to be resilvered.  The function
 * assumes that at least one DTL is dirty which implies that full stripe
 * width blocks must be resilvered.
 */
static boolean_t
vdev_anyraid_need_resilver(vdev_t *vd, const dva_t *dva, size_t psize,
    uint64_t phys_birth)
{
	(void) psize;
	vdev_anyraid_t *var = vd->vdev_tsd;
	if (!vdev_dtl_contains(vd, DTL_PARTIAL, phys_birth, 1))
		return (B_FALSE);

	uint64_t tsize = var->vd_tile_size * var->vd_width;
	uint64_t start_tile_id = DVA_GET_OFFSET(dva) / tsize;
	anyraid_tile_t search;
	search.at_tile_id = start_tile_id;
	avl_index_t where;
	rw_enter(&var->vd_lock, RW_READER);
	anyraid_tile_t *tile = avl_find(&var->vd_tile_map, &search,
	    &where);
	rw_exit(&var->vd_lock);
	ASSERT(tile);

	for (anyraid_tile_node_t *atn = list_head(&tile->at_list);
	    atn != NULL; atn = list_next(&tile->at_list, atn)) {
		vdev_t *cvd = vd->vdev_child[atn->atn_disk];

		if (!vdev_dtl_empty(cvd, DTL_PARTIAL))
			return (B_TRUE);
	}

	return (B_FALSE);
}

/*
 * Right now, we don't translate anything beyond the end of the allocated
 * ranges for the target leaf vdev. This means that trim and initialize won't
 * affect those areas on anyraid devices. Given the target use case, this is
 * not a significant concern, but a rework of the xlate logic could enable this
 * in the future.
 */
static void
vdev_anyraid_xlate(vdev_t *cvd, const zfs_range_seg64_t *logical_rs,
    zfs_range_seg64_t *physical_rs, zfs_range_seg64_t *remain_rs)
{
	vdev_t *anyraidvd = cvd->vdev_parent;
	ASSERT(vdev_is_anyraid(anyraidvd));
	vdev_anyraid_t *var = anyraidvd->vdev_tsd;
	uint64_t ptsize = var->vd_tile_size;
	uint64_t ltsize = ptsize * var->vd_width;

	uint64_t start_tile_id = logical_rs->rs_start / ltsize;
	ASSERT3U(start_tile_id, ==, (logical_rs->rs_end - 1) / ltsize);
	anyraid_tile_t search;
	search.at_tile_id = start_tile_id;
	avl_index_t where;
	rw_enter(&var->vd_lock, RW_READER);
	anyraid_tile_t *tile = avl_find(&var->vd_tile_map, &search,
	    &where);
	rw_exit(&var->vd_lock);
	// This tile doesn't exist yet
	if (tile == NULL) {
		physical_rs->rs_start = physical_rs->rs_end = 0;
		return;
	}
	uint64_t idx = 0;
	anyraid_tile_node_t *atn = list_head(&tile->at_list);
	for (; atn != NULL; atn = list_next(&tile->at_list, atn), idx++)
		if (anyraidvd->vdev_child[atn->atn_disk] == cvd)
			break;
	// The tile exists, but isn't stored on this child
	if (atn == NULL) {
		physical_rs->rs_start = physical_rs->rs_end = 0;
		return;
	}

	switch (var->vd_parity_type) {
		case VAP_MIRROR:
		{
			uint64_t child_offset = atn->atn_offset * ptsize +
			    logical_rs->rs_start % ptsize;
			child_offset +=
			    VDEV_ANYRAID_START_OFFSET(anyraidvd->vdev_ashift);
			uint64_t size = logical_rs->rs_end -
			    logical_rs->rs_start;

			physical_rs->rs_start = child_offset;
			physical_rs->rs_end = child_offset + size;
			break;
		}
		case VAP_RAIDZ:
		{
			/*
			 * The problem here is that the tgt_col shouldn't just
			 * be the vdev_id, we need to get the idx of this
			 * specific atn in the tile? Or something like that.
			 */
			uint64_t width = var->vd_width;
			uint64_t tgt_col = idx;
			uint64_t ashift = anyraidvd->vdev_ashift;
			uint64_t tile_start = VDEV_ANYRAID_TOTAL_MAP_SIZE(
			    anyraidvd->vdev_ashift) + atn->atn_offset * ptsize;

			uint64_t b_start =
			    (logical_rs->rs_start % ltsize) >> ashift;
			uint64_t b_end =
			    (logical_rs->rs_end % ltsize) >> ashift;

			uint64_t start_row = 0;
			if (b_start > tgt_col) /* avoid underflow */
				start_row = ((b_start - tgt_col - 1) / width) +
				    1;

			uint64_t end_row = 0;
			if (b_end > tgt_col)
				end_row = ((b_end - tgt_col - 1) / width) + 1;

			physical_rs->rs_start =
			    tile_start + (start_row << ashift);
			physical_rs->rs_end =
			    tile_start + (end_row << ashift);
			break;
		}
		default:
			panic("Invalid parity type: %d", var->vd_parity_type);
	}
	remain_rs->rs_start = 0;
	remain_rs->rs_end = 0;
}

static uint64_t
vdev_anyraid_nparity(vdev_t *vd)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	return (var->vd_nparity);
}

static uint64_t
vdev_anyraid_ndisks(vdev_t *vd)
{
	return (vd->vdev_children);
}

/*
 * Functions related to syncing out the tile map each TXG.
 */
static boolean_t
map_write_loc_entry(anyraid_tile_node_t *atn, void *buf, uint32_t *offset)
{
	anyraid_map_loc_entry_t *entry = (void *)((char *)buf + *offset);
	amle_set_type(entry);
	amle_set_disk(entry, atn->atn_disk);
	amle_set_offset(entry, atn->atn_offset);
	*offset += sizeof (*entry);
	return (*offset == SPA_MAXBLOCKSIZE);
}

static boolean_t
map_write_skip_entry(uint32_t tile, void *buf, uint32_t *offset,
    uint32_t prev_id)
{
	anyraid_map_skip_entry_t *entry = (void *)((char *)buf + *offset);
	amse_set_type(entry);
	amse_set_skip_count(entry, tile - prev_id - 1);
	*offset += sizeof (*entry);
	return (*offset == SPA_MAXBLOCKSIZE);
}

static void
anyraid_map_write_done(zio_t *zio)
{
	abd_free(zio->io_abd);
}

static void
map_write_issue(zio_t *zio, vdev_t *vd, uint64_t base_offset,
    uint8_t idx, uint32_t length, abd_t *abd, zio_eck_t *cksum_out,
    int flags)
{
#ifdef _ZFS_BIG_ENDIAN
	void *buf = abd_borrow_buf(abd, SPA_MAXBLOCKSIZE);
	byteswap_uint32_array(buf, length);
	abd_return_buf(abd, buf, SPA_MAXBLOCKSIZE);
#else
	(void) length;
#endif

	zio_nowait(zio_write_phys(zio, vd, base_offset +
	    idx * VDEV_ANYRAID_MAP_SIZE +
	    VDEV_ANYRAID_MAP_HEADER_SIZE(vd->vdev_ashift), SPA_MAXBLOCKSIZE,
	    abd, ZIO_CHECKSUM_ANYRAID_MAP, anyraid_map_write_done, cksum_out,
	    ZIO_PRIORITY_SYNC_WRITE, flags, B_FALSE));
}

static void
vdev_anyraid_write_map_done(zio_t *zio)
{
	uint64_t *good_writes = zio->io_private;

	if (zio->io_error == 0 && good_writes != NULL)
		atomic_inc_64(good_writes);
}

void
vdev_anyraid_write_map_sync(vdev_t *vd, zio_t *pio, uint64_t txg,
    uint64_t *good_writes, int flags, vdev_config_sync_status_t status)
{
	vdev_t *anyraidvd = vd->vdev_parent;
	ASSERT(vdev_is_anyraid(anyraidvd));
	spa_t *spa = vd->vdev_spa;
	vdev_anyraid_t *var = anyraidvd->vdev_tsd;
	uint32_t header_size = VDEV_ANYRAID_MAP_HEADER_SIZE(vd->vdev_ashift);
	uint32_t nvl_bytes = VDEV_ANYRAID_NVL_BYTES(vd->vdev_ashift);
	uint8_t update_target = txg % VDEV_ANYRAID_MAP_COPIES;
	uint64_t base_offset = vdev_anyraid_header_offset(vd, update_target);

	abd_t *header_abd =
	    abd_alloc_linear(header_size, B_TRUE);
	abd_zero(header_abd, header_size);
	void *header_buf = abd_borrow_buf(header_abd, header_size);
	zio_eck_t *cksums = (zio_eck_t *)&((char *)header_buf)[nvl_bytes];

	abd_t *map_abd = abd_alloc_linear(SPA_MAXBLOCKSIZE, B_TRUE);
	uint8_t written = 0;
	void *buf = abd_borrow_buf(map_abd, SPA_MAXBLOCKSIZE);

	rw_enter(&var->vd_lock, RW_READER);
	anyraid_tile_t *cur = avl_first(&var->vd_tile_map);
	anyraid_tile_node_t *curn = cur != NULL ?
	    list_head(&cur->at_list) : NULL;
	uint32_t buf_offset = 0, prev_id = UINT32_MAX;
	zio_t *zio = zio_root(spa, NULL, NULL, flags);
	/* Write out each sub-tile in turn */
	while (cur) {
		if (status == VDEV_CONFIG_REWINDING_CHECKPOINT &&
		    cur->at_tile_id > var->vd_checkpoint_tile)
			break;

		anyraid_tile_t *next = AVL_NEXT(&var->vd_tile_map, cur);
		IMPLY(prev_id != UINT32_MAX, cur->at_tile_id >= prev_id);
		/*
		 * Determine if we need to write a skip entry before the
		 * current one.
		 */
		boolean_t skip =
		    (prev_id == UINT32_MAX && cur->at_tile_id != 0) ||
		    (prev_id != UINT32_MAX && cur->at_tile_id > prev_id + 1);
		if ((skip && map_write_skip_entry(cur->at_tile_id, buf,
		    &buf_offset, prev_id)) ||
		    (!skip && map_write_loc_entry(curn, buf, &buf_offset))) {
			// Let the final write handle it
			if (next == NULL)
				break;
			abd_return_buf_copy(map_abd, buf, SPA_MAXBLOCKSIZE);
			map_write_issue(zio, vd, base_offset, written,
			    buf_offset, map_abd, &cksums[written], flags);

			map_abd = abd_alloc_linear(SPA_MAXBLOCKSIZE, B_TRUE);
			written++;
			ASSERT3U(written, <,
			    VDEV_ANYRAID_MAP_SIZE / SPA_MAXBLOCKSIZE);
			buf = abd_borrow_buf(map_abd, SPA_MAXBLOCKSIZE);
			buf_offset = 0;
		}
		prev_id = cur->at_tile_id;
		/*
		 * Advance the current sub-tile; if it moves us past the end
		 * of the current list of sub-tiles, start the next tile.
		 */
		if (!skip) {
			curn = list_next(&cur->at_list, curn);
			if (curn == NULL) {
				cur = next;
				curn = cur != NULL ?
				    list_head(&cur->at_list) : NULL;
			}
		}
	}

	if (status == VDEV_CONFIG_NO_CHECKPOINT ||
	    status == VDEV_CONFIG_REWINDING_CHECKPOINT) {
		var->vd_checkpoint_tile = UINT32_MAX;
	} else if (status == VDEV_CONFIG_CREATING_CHECKPOINT) {
		anyraid_tile_t *at = avl_last(&var->vd_tile_map);
		ASSERT(at);
		var->vd_checkpoint_tile = at->at_tile_id;
	}
	rw_exit(&var->vd_lock);

	abd_return_buf_copy(map_abd, buf, SPA_MAXBLOCKSIZE);
	map_write_issue(zio, vd, base_offset, written, buf_offset, map_abd,
	    &cksums[written], flags);

	if (zio_wait(zio))
		return;

	// Populate the header
	uint16_t *sizes = kmem_zalloc(sizeof (*sizes) *
	    anyraidvd->vdev_children, KM_SLEEP);
	uint64_t disk_id = 0;
	for (uint64_t i = 0; i < anyraidvd->vdev_children; i++) {
		if (anyraidvd->vdev_child[i] == vd)
			disk_id = i;
		sizes[i] = var->vd_children[i]->van_capacity;
	}
	ASSERT3U(disk_id, <, anyraidvd->vdev_children);
	nvlist_t *header = fnvlist_alloc();
	fnvlist_add_uint16(header, VDEV_ANYRAID_HEADER_VERSION, 0);
	fnvlist_add_uint8(header, VDEV_ANYRAID_HEADER_DISK, disk_id);
	fnvlist_add_uint64(header, VDEV_ANYRAID_HEADER_TXG, txg);
	fnvlist_add_uint64(header, VDEV_ANYRAID_HEADER_GUID, spa_guid(spa));
	fnvlist_add_uint64(header, VDEV_ANYRAID_HEADER_TILE_SIZE,
	    var->vd_tile_size);
	fnvlist_add_uint32(header, VDEV_ANYRAID_HEADER_LENGTH,
	    written * SPA_MAXBLOCKSIZE + buf_offset);
	fnvlist_add_uint16_array(header, VDEV_ANYRAID_HEADER_DISK_SIZES, sizes,
	    anyraidvd->vdev_children);
	kmem_free(sizes, sizeof (*sizes) * anyraidvd->vdev_children);

	if (var->vd_checkpoint_tile != UINT32_MAX) {
		fnvlist_add_uint32(header, VDEV_ANYRAID_HEADER_CHECKPOINT,
		    var->vd_checkpoint_tile);
	}
	size_t packed_size;
	char *packed = NULL;
	VERIFY0(nvlist_pack(header, &packed, &packed_size, NV_ENCODE_XDR,
	    KM_SLEEP));
	fnvlist_free(header);
	ASSERT3U(packed_size, <, nvl_bytes);
	memcpy(header_buf, packed, packed_size);
	fnvlist_pack_free(packed, packed_size);
	abd_return_buf_copy(header_abd, header_buf, header_size);

	// Write out the header
	zio_t *header_zio = zio_write_phys(pio, vd, base_offset, header_size,
	    header_abd, ZIO_CHECKSUM_LABEL, vdev_anyraid_write_map_done,
	    good_writes, ZIO_PRIORITY_SYNC_WRITE, flags, B_FALSE);
	zio_nowait(header_zio);
	abd_free(header_abd);
}

static uint64_t
vdev_anyraid_min_attach_size(vdev_t *vd)
{
	ASSERT(vdev_is_anyraid(vd));
	ASSERT3U(spa_config_held(vd->vdev_spa, SCL_ALL, RW_READER), !=, 0);
	vdev_anyraid_t *var = vd->vdev_tsd;
	ASSERT(var->vd_tile_size);
	return (VDEV_ANYRAID_TOTAL_MAP_SIZE(vd->vdev_ashift) +
	    var->vd_tile_size);
}

static uint64_t
vdev_anyraid_min_asize(vdev_t *pvd, vdev_t *cvd)
{
	ASSERT(vdev_is_anyraid(pvd));
	ASSERT3U(spa_config_held(pvd->vdev_spa, SCL_ALL, RW_READER), !=, 0);
	vdev_anyraid_t *var = pvd->vdev_tsd;
	if (var->vd_tile_size == 0)
		return (VDEV_ANYRAID_TOTAL_MAP_SIZE(cvd->vdev_ashift));

	rw_enter(&var->vd_lock, RW_READER);
	uint64_t size = VDEV_ANYRAID_TOTAL_MAP_SIZE(cvd->vdev_ashift) +
	    (var->vd_children[cvd->vdev_id]->van_capacity + 1) *
	    var->vd_tile_size;
	rw_exit(&var->vd_lock);
	return (size);
}

void
vdev_anyraid_expand(vdev_t *tvd, vdev_t *newvd)
{
	vdev_anyraid_t *var = tvd->vdev_tsd;
	uint64_t old_children = tvd->vdev_children - 1;

	ASSERT3U(spa_config_held(tvd->vdev_spa, SCL_ALL, RW_WRITER), ==,
	    SCL_ALL);
	vdev_anyraid_node_t **nc = kmem_alloc(tvd->vdev_children * sizeof (*nc),
	    KM_SLEEP);
	vdev_anyraid_node_t *newchild = kmem_alloc(sizeof (*newchild),
	    KM_SLEEP);
	newchild->van_id = newvd->vdev_id;
	anyraid_freelist_create(&newchild->van_freelist, 0);
	uint64_t max_size = VDEV_ANYRAID_MAX_TPD * var->vd_tile_size;
	newchild->van_capacity = (MIN(max_size, (newvd->vdev_asize -
	    VDEV_ANYRAID_TOTAL_MAP_SIZE(newvd->vdev_ashift))) /
	    var->vd_tile_size) - 1;
	rw_enter(&var->vd_lock, RW_WRITER);
	memcpy(nc, var->vd_children, old_children * sizeof (*nc));
	kmem_free(var->vd_children, old_children * sizeof (*nc));
	var->vd_children = nc;
	var->vd_children[old_children] = newchild;
	avl_add(&var->vd_children_tree, newchild);
	rw_exit(&var->vd_lock);
}

boolean_t
vdev_anyraid_mapped(vdev_t *vd, uint64_t offset)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	anyraid_tile_t search;
	search.at_tile_id = offset / var->vd_tile_size;

	rw_enter(&var->vd_lock, RW_READER);
	anyraid_tile_t *tile = avl_find(&var->vd_tile_map, &search, NULL);
	boolean_t result = tile != NULL;
	rw_exit(&var->vd_lock);

	return (result);
}

/*
 * TODO: Look into this for raidz parity
 * Return the maximum asize for a rebuild zio in the provided range
 * given the following constraints.  An anyraid chunk may not:
 *
 * - Exceed the maximum allowed block size (SPA_MAXBLOCKSIZE), or
 * - Span anyraid tiles
 */
static uint64_t
vdev_anyraid_rebuild_asize(vdev_t *vd, uint64_t start, uint64_t asize,
    uint64_t max_segment)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	ASSERT(vdev_is_anyraid(vd));

	uint64_t psize = MIN(P2ROUNDUP(max_segment, 1 << vd->vdev_ashift),
	    SPA_MAXBLOCKSIZE);

	if (start / var->vd_tile_size !=
	    (start + psize) / var->vd_tile_size) {
		psize = P2ROUNDUP(start, var->vd_tile_size) - start;
	}

	return (MIN(asize, vdev_psize_to_asize(vd, psize)));
}

static uint64_t
vdev_anyraid_asize(vdev_t *vd, uint64_t psize, uint64_t txg)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	ASSERT(vdev_is_anyraid(vd));
	if (var->vd_parity_type == VAP_MIRROR)
		return (vdev_default_asize(vd, psize, txg));

	uint64_t ashift = vd->vdev_top->vdev_ashift;
	uint64_t nparity = var->vd_nparity;
	uint64_t cols = var->vd_width;

	uint64_t asize = ((psize - 1) >> ashift) + 1;
	asize += nparity * ((asize + cols - nparity - 1) / (cols - nparity));
	asize = roundup(asize, nparity + 1) << ashift;

#ifdef ZFS_DEBUG
	uint64_t asize_new = ((psize - 1) >> ashift) + 1;
	uint64_t ncols_new = cols;
	asize_new += nparity * ((asize_new + ncols_new - nparity - 1) /
	    (ncols_new - nparity));
	asize_new = roundup(asize_new, nparity + 1) << ashift;
	VERIFY3U(asize_new, <=, asize);
#endif

	return (asize);
}

static uint64_t
vdev_anyraid_psize(vdev_t *vd, uint64_t asize, uint64_t txg)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	ASSERT(vdev_is_anyraid(vd));
	if (var->vd_parity_type == VAP_MIRROR)
		return (vdev_default_psize(vd, asize, txg));

	uint64_t ashift = vd->vdev_top->vdev_ashift;
	uint64_t nparity = var->vd_nparity;
	uint64_t cols = var->vd_width;

	ASSERT0(asize % (1 << ashift));

	uint64_t psize = (asize >> ashift);
	/*
	 * If the roundup to nparity + 1 caused us to spill into a new row, we
	 * need to ignore that row entirely (since it can't store data or
	 * parity).
	 */
	uint64_t rows = psize / cols;
	psize = psize - (rows * cols) <= nparity ? rows * cols : psize;
	/*  Subtract out parity sectors for each row storing data. */
	psize -= nparity * DIV_ROUND_UP(psize, cols);
	psize <<= ashift;

	return (psize);
}

uint64_t
vdev_anyraid_child_num_tiles(vdev_t *vd, vdev_t *cvd)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	ASSERT(vdev_is_anyraid(vd));

	uint64_t total = 0;
	rw_enter(&var->vd_lock, RW_READER);
	if (cvd != NULL) {
		vdev_anyraid_node_t *n = var->vd_children[cvd->vdev_id];
		total = anyraid_freelist_alloc(&n->van_freelist);
	} else {
		for (int i = 0; i < vd->vdev_children; i++) {
			vdev_anyraid_node_t *n = var->vd_children[i];
			total += anyraid_freelist_alloc(&n->van_freelist);
		}
	}
	rw_exit(&var->vd_lock);
	return (total);
}

uint64_t
vdev_anyraid_child_capacity(vdev_t *vd, vdev_t *cvd)
{
	vdev_anyraid_t *var = vd->vdev_tsd;
	ASSERT(vdev_is_anyraid(vd));

	uint64_t total = 0;
	rw_enter(&var->vd_lock, RW_READER);
	if (cvd != NULL) {
		vdev_anyraid_node_t *n = var->vd_children[cvd->vdev_id];
		total = n->van_capacity + 1;
	} else {
		for (int i = 0; i < vd->vdev_children; i++) {
			vdev_anyraid_node_t *n = var->vd_children[i];
			total += n->van_capacity + 1;
		}
	}
	rw_exit(&var->vd_lock);
	return (total);
}

vdev_ops_t vdev_anymirror_ops = {
	.vdev_op_init = vdev_anyraid_init,
	.vdev_op_fini = vdev_anyraid_fini,
	.vdev_op_open = vdev_anyraid_open,
	.vdev_op_close = vdev_anyraid_close,
	.vdev_op_psize_to_asize = vdev_anyraid_asize,
	.vdev_op_asize_to_psize = vdev_anyraid_psize, // TODO
	.vdev_op_min_asize = vdev_anyraid_min_asize,
	.vdev_op_min_attach_size = vdev_anyraid_min_attach_size,
	.vdev_op_min_alloc = NULL,
	.vdev_op_io_start = vdev_anyraid_io_start,
	.vdev_op_io_done = vdev_anyraid_io_done,
	.vdev_op_state_change = vdev_anyraid_state_change,
	.vdev_op_need_resilver = vdev_anyraid_need_resilver,
	.vdev_op_hold = NULL,
	.vdev_op_rele = NULL,
	.vdev_op_remap = NULL,
	.vdev_op_xlate = vdev_anyraid_xlate,
	.vdev_op_rebuild_asize = vdev_anyraid_rebuild_asize,
	.vdev_op_metaslab_init = NULL,
	.vdev_op_config_generate = vdev_anyraid_config_generate,
	.vdev_op_nparity = vdev_anyraid_nparity,
	.vdev_op_ndisks = vdev_anyraid_ndisks,
	.vdev_op_metaslab_size = vdev_anyraid_metaslab_size,
	.vdev_op_type = VDEV_TYPE_ANYMIRROR,	/* name of this vdev type */
	.vdev_op_leaf = B_FALSE			/* not a leaf vdev */
};

vdev_ops_t vdev_anyraidz_ops = {
	.vdev_op_init = vdev_anyraid_init,
	.vdev_op_fini = vdev_anyraid_fini,
	.vdev_op_open = vdev_anyraid_open,
	.vdev_op_close = vdev_anyraid_close,
	.vdev_op_psize_to_asize = vdev_anyraid_asize,
	.vdev_op_asize_to_psize = vdev_anyraid_psize, // TODO
	.vdev_op_min_asize = vdev_anyraid_min_asize,
	.vdev_op_min_attach_size = vdev_anyraid_min_attach_size,
	.vdev_op_min_alloc = NULL,
	.vdev_op_io_start = vdev_anyraid_io_start,
	.vdev_op_io_done = vdev_anyraid_io_done,
	.vdev_op_state_change = vdev_anyraid_state_change,
	.vdev_op_need_resilver = vdev_anyraid_need_resilver,
	.vdev_op_hold = NULL,
	.vdev_op_rele = NULL,
	.vdev_op_remap = NULL,
	.vdev_op_xlate = vdev_anyraid_xlate,
	.vdev_op_rebuild_asize = vdev_anyraid_rebuild_asize,
	.vdev_op_metaslab_init = NULL,
	.vdev_op_config_generate = vdev_anyraid_config_generate,
	.vdev_op_nparity = vdev_anyraid_nparity,
	.vdev_op_ndisks = vdev_anyraid_ndisks,
	.vdev_op_metaslab_size = vdev_anyraid_metaslab_size,
	.vdev_op_type = VDEV_TYPE_ANYRAIDZ,	/* name of this vdev type */
	.vdev_op_leaf = B_FALSE			/* not a leaf vdev */
};


/*
 * ==========================================================================
 * TILE MOTION & REBALANCE LOGIC 
 * ==========================================================================
 */

vdev_anyraid_rebalance_t *
vdev_anyraid_rebalance_status(vdev_t *vd)
{
	ASSERT(vdev_is_anyraid(vd));
	vdev_anyraid_t *var = vd->vdev_tsd;
	return (var->vd_rebalance);
}

static void
anyraid_rebalance_complete_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = arg;
	vdev_anyraid_rebalance_t *var = spa->spa_anyraid_rebalance;
	vdev_t *vd = vdev_lookup_top(spa, var->var_vd);

	for (int i = 0; i < TXG_SIZE; i++)
		VERIFY0(var->var_offset_pertxg[i]);

	/*
	 * Dirty the config so that the updated ZPOOL_CONFIG_RAIDZ_EXPAND_TXGS
	 * will get written (based on vd_expand_txgs).
	 */
	vdev_config_dirty(vd);

	var->var_end_time = gethrestime_sec();
	var->var_state = DSS_FINISHED;

/*	uint64_t end_time = var->var_end_time;
	VERIFY0(zap_update(spa->spa_meta_objset,
	    vd->vdev_top_zap, VDEV_TOP_ZAP_RAIDZ_EXPAND_END_TIME,
	    sizeof (end_time), 1, &end_time, tx));*/

	spa_history_log_internal(spa, "anyraid rebalanace completed",  tx,
	    "%s vdev %llu", spa_name(spa),
	    (unsigned long long)vd->vdev_id);

	spa->spa_anyraid_rebalance = NULL;

	spa_async_request(spa, SPA_ASYNC_INITIALIZE_RESTART);
	spa_async_request(spa, SPA_ASYNC_TRIM_RESTART);
	spa_async_request(spa, SPA_ASYNC_AUTOTRIM_RESTART);

	spa_notify_waiters(spa);

	// TODO expand the vdev here
	/*
	 * While we're in syncing context take the opportunity to
	 * setup a scrub. All the data has been sucessfully copied
	 * but we have not validated any checksums.
	 */
	setup_sync_arg_t setup_sync_arg = {
		.func = POOL_SCAN_SCRUB,
		.txgstart = 0,
		.txgend = 0,
	};
	if (zfs_scrub_after_rebalance &&
	    dsl_scan_setup_check(&setup_sync_arg.func, tx) == 0) {
		dsl_scan_setup_sync(&setup_sync_arg, tx);
	}
}

struct rebal_node {
	avl_node_t node;
	int cvd;
	int64_t diff; // positive: wants more tiles, negative: wants fewer
	int64_t *arr;
};

static int
rebal_cmp(const void *a, const void *b)
{
	const struct rebal_node *ra = a;
	const struct rebal_node *rb = b;
	int cmp = TREE_CMP(rb->diff, ra->diff);
	if (likely(cmp != 0))
		return (cmp);
	return (TREE_CMP(rb->cvd, ra->cvd));
}

static void
populate_child_array(vdev_anyraid_t *var, int child, int64_t *arr)
{
	for (anyraid_tile_t *tile = avl_first(&var->vd_tile_map);
	    tile; tile = AVL_NEXT(&var->vd_tile_map, tile)) {
		for (anyraid_tile_node_t *atn = list_head(&tile->at_list);
		    atn; atn = list_next(&tile->at_list, atn)) {
			if (atn->atn_disk == child)
				arr[atn->atn_offset] = tile->at_tile_id;
		}
	}
}

static boolean_t
rebal_try_move_one(vdev_anyraid_t *var, struct rebal_node *donor,
    struct rebal_node *receiver)
{
	vdev_anyraid_node_t *dvan = var->vd_children[donor->cvd];
	vdev_anyraid_node_t *rvan = var->vd_children[receiver->cvd];

	for (int i = 0; i < dvan->van_freelist.af_next_off; i++) {
		if (donor->arr[i] == -1)
			continue;
		boolean_t found = B_FALSE;
		for (int j = 0; j < rvan->van_freelist.af_next_off;
		    j++) {
			/*
			 * TODO we need to check here if doing this move would
			 * cause the total number of allocatable tiles to drop;
			 * if so, we have to skip it.
			*/
			if (donor->arr[i] == donor->arr[j]) {
				found = B_TRUE;
				break;
			}
		}
		if (found)
			continue;
		vdev_anyraid_rebalance_task_t *task =
		    kmem_zalloc(sizeof(*task), KM_SLEEP);
		task->vart_source_disk = (uint8_t)donor->cvd;
		task->vart_dest_disk = (uint8_t)receiver->cvd;
		task->vart_source_off = i;
		task->vart_dest_off = anyraid_freelist_pop(
		    &rvan->van_freelist);
		task->vart_tile = donor->arr[i];
		list_insert_tail(&var->vd_rebalance->var_list, task);
		receiver->arr[task->vart_dest_off] = donor->arr[i];
		donor->arr[i] = -1;
		return (B_TRUE);
	}
	return (B_FALSE);
}

void
vdev_anyraid_setup_rebalance(vdev_t *vd, dmu_tx_t *tx)
{
	(void)tx;
	ASSERT(vdev_is_anyraid(vd));
	vdev_anyraid_t *var = vd->vdev_tsd;

	vdev_config_dirty(vd);

	var->vd_rebalance = kmem_alloc(sizeof (*var->vd_rebalance), KM_SLEEP);
	var->vd_rebalance->var_start_time = gethrestime_sec();
	var->vd_rebalance->var_end_time = 0;
	var->vd_rebalance->var_state = DSS_SCANNING;
	var->vd_rebalance->var_bytes_copied = 0;
	var->vd_rebalance->var_vd = vd->vdev_id;
	list_create(&var->vd_rebalance->var_list,
	    sizeof (vdev_anyraid_rebalance_task_t),
	    offsetof(vdev_anyraid_rebalance_task_t, vart_node));

	vd->vdev_spa->spa_anyraid_rebalance = var->vd_rebalance;

	rw_enter(&var->vd_lock, RW_READER);
	uint64_t cap = vd->vdev_asize / var->vd_tile_size;
	uint64_t avg = (var->vd_width * avl_numnodes(&var->vd_tile_map)) *
	    100000 / cap;
	avl_tree_t t;
	avl_create(&t, rebal_cmp, sizeof (struct rebal_node), offsetof (struct rebal_node, node));
	for (int i = 0; i < vd->vdev_children; i++) {
		struct rebal_node *rn = kmem_zalloc(sizeof (*rn), KM_SLEEP);
		rn->cvd = i;
		vdev_anyraid_node_t *n = var->vd_children[i];
		uint16_t cap = n->van_capacity;
		rn->diff = (avg * cap) / 100000 -
		    anyraid_freelist_alloc(&n->van_freelist);
		int64_t *arr = kmem_alloc(sizeof (*arr) * cap, KM_SLEEP);
		    memset(arr, -1, sizeof (*arr) *cap);
		populate_child_array(var, i, rn->arr);
		avl_add(&t, rn);
	}
	struct rebal_node *donor = avl_first(&t);
	avl_remove(&t, donor);
	struct rebal_node *receiver = avl_last(&t);
	for (;;) {
		if (donor->diff >= 0)
			break;

		struct rebal_node *prev = AVL_PREV(&t, receiver);
		boolean_t moved = rebal_try_move_one(var, donor, receiver);
		if (!moved) {
			receiver = prev;
			if (receiver->diff <= 0)
				break;
			continue;
		}
		avl_remove(&t, receiver);
		receiver->diff--;
		donor->diff++;
		avl_add(&t, receiver);
		if (((struct rebal_node *)avl_first(&t))->diff < donor->diff) {
			avl_add(&t, donor);
			donor = avl_first(&t);
			avl_remove(&t, donor);
			receiver = avl_last(&t);
			continue;
		}
		if (AVL_PREV(&t, receiver) != prev)
			receiver = prev;
	}
	avl_add(&t, donor);
	rw_exit(&var->vd_lock);
}

static boolean_t
spa_anyraid_rebalance_thread_check(void *arg, zthr_t *zthr)
{
	(void) zthr;
	spa_t *spa = arg;

	return (spa->spa_anyraid_rebalance != NULL);
}

/*
 * Write of the new location on one child is done.  Once all of them are done
 * we can unlock and free everything.
 */
static void
anyraid_rebalance_write_done(zio_t *zio)
{
	anyraid_move_arg_t *ama = zio->io_private;
	vdev_anyraid_rebalance_t *var = ama->ama_var;

	abd_free(zio->io_abd);

	mutex_enter(&var->var_lock);
	if (zio->io_error != 0) {
		/* Force a rebalance pause on errors */
		var->var_failed_offset =
		    MIN(var->var_failed_offset, ama->ama_lr->lr_offset);
	}
	ASSERT3U(var->var_outstanding_bytes, >=, zio->io_size);
	var->var_outstanding_bytes -= zio->io_size;
	if (ama->ama_lr->lr_offset + ama->ama_lr->lr_length <
	    var->var_failed_offset) {
		var->var_bytes_copied_pertxg[ama->ama_txg & TXG_MASK] +=
		    zio->io_size;
	}
	cv_signal(&var->var_cv);
	mutex_exit(&var->var_lock);

	spa_config_exit(zio->io_spa, SCL_STATE, zio->io_spa);
	zfs_rangelock_exit(ama->ama_lr);
	kmem_free(ama, sizeof (*ama));
}

/*
 * Read of the old location on one child is done.  Once all of them are done
 * writes should have all the data and we can issue them.
 */
static void
anyraid_rebalance_read_done(zio_t *zio)
{
	anyraid_move_arg_t *ama = zio->io_private;
	vdev_anyraid_rebalance_t *var = ama->ama_var;
	abd_free(zio->io_abd);

	/*
	 * If the read failed, or if it was done on a vdev that is not fully
	 * healthy (e.g. a child that has a resilver in progress), we may not
	 * have the correct data.  Note that it's OK if the write proceeds.
	 * It may write garbage but the location is otherwise unused and we
	 * will retry later due to vre_failed_offset.
	 */
	if (zio->io_error != 0 || !vdev_dtl_empty(zio->io_vd, DTL_MISSING)) {
		zfs_dbgmsg("rebalance read failed off=%llu size=%llu txg=%llu "
		    "err=%u partial_dtl_empty=%u missing_dtl_empty=%u",
		    (long long)ama->ama_lr->lr_offset,
		    (long long)ama->ama_lr->lr_length,
		    (long long)ama->ama_txg,
		    zio->io_error,
		    vdev_dtl_empty(zio->io_vd, DTL_PARTIAL),
		    vdev_dtl_empty(zio->io_vd, DTL_MISSING));
		mutex_enter(&var->var_lock);
		/* Force a rebalance pause on errors */
		var->var_failed_offset =
		    MIN(var->var_failed_offset, ama->ama_lr->lr_offset);
		mutex_exit(&var->var_lock);
	}
	zio_nowait(ama->ama_zio);
}

static void
anyraid_rebalance_record_progress(vdev_anyraid_rebalance_t *var,
    uint64_t offset, dmu_tx_t *tx)
{
	int txgoff = dmu_tx_get_txg(tx) & TXG_MASK;
	//spa_t *spa = dmu_tx_pool(tx)->dp_spa;

	if (offset == 0)
		return;

	mutex_enter(&var->var_lock);
	ASSERT3U(var->var_offset, <=, offset);
	var->var_offset = offset;
	mutex_exit(&var->var_lock);

/*	if (var->var_offset_pertxg[txgoff] == 0) {
		dsl_sync_task_nowait(dmu_tx_pool(tx), anyraid_rebalance_sync,
		    spa, tx);
	}*/
	var->var_offset_pertxg[txgoff] = offset;
}

static boolean_t
anyraid_rebalance_impl(vdev_t *vd, vdev_anyraid_rebalance_t *var,
    zfs_range_tree_t *rt, dmu_tx_t *tx)
{
	spa_t *spa = vd->vdev_spa;
	uint_t ashift = vd->vdev_top->vdev_ashift;
	vdev_anyraid_rebalance_task_t *vart = list_head(&var->var_list);
	vdev_anyraid_t *va = vd->vdev_tsd;

	zfs_range_seg_t *rs = zfs_range_tree_first(rt);
	if (rt == NULL)
		return (B_FALSE);
	uint64_t offset = zfs_rs_get_start(rs, rt);
	ASSERT(IS_P2ALIGNED(offset, 1 << ashift));
	uint64_t size = zfs_rs_get_end(rs, rt) - offset;
	ASSERT3U(size, >=, 1 << ashift);
	ASSERT(IS_P2ALIGNED(size, 1 << ashift));

	size = MIN(size, anyraid_rebalance_max_copy_bytes);
	size = MAX(size, 1 << ashift);

	zfs_range_tree_remove(rt, offset, size);

	anyraid_move_arg_t *ama = kmem_zalloc(sizeof (*ama), KM_SLEEP);
	ama->ama_var = var;
	ama->ama_lr = zfs_rangelock_enter(&va->var_rangelock,
	    offset, size, RL_WRITER);
	ama->ama_txg = dmu_tx_get_txg(tx);
	ama->ama_size = size;

	anyraid_rebalance_record_progress(var, offset + size, tx);

	/*
	 * SCL_STATE will be released when the read and write are done,
	 * by raidz_reflow_write_done().
	 */
	spa_config_enter(spa, SCL_STATE, spa, RW_READER);

	mutex_enter(&var->var_lock);
	var->var_outstanding_bytes += size;
	mutex_exit(&var->var_lock);

	/* Allocate ABD and ZIO for each child we write. */
	int txgoff = dmu_tx_get_txg(tx) & TXG_MASK;
	zio_t *pio = spa->spa_txg_zio[txgoff];
	abd_t *abd = abd_alloc_for_io(size, B_FALSE);
	ama->ama_zio = zio_vdev_child_io(pio, NULL,
	    vd->vdev_child[vart->vart_dest_disk],
	    vart->vart_dest_off * va->vd_tile_size + (offset % va->vd_tile_size), abd, size, ZIO_TYPE_WRITE, ZIO_PRIORITY_REMOVAL,
	    ZIO_FLAG_CANFAIL, anyraid_rebalance_write_done, ama);

	zio_nowait(zio_vdev_child_io(pio, NULL,
	    vd->vdev_child[vart->vart_source_disk],
	    offset, abd, size, ZIO_TYPE_READ, ZIO_PRIORITY_REMOVAL,
	    ZIO_FLAG_CANFAIL, anyraid_rebalance_read_done, ama));
	return (B_FALSE);
}

struct physify_arg {
	zfs_range_tree_t *rt;
	vdev_t *vd;
};

static void
anyraid_rt_physify(void *arg, uint64_t start, uint64_t size)
{
	struct physify_arg *pa = (struct physify_arg *)arg;
	zfs_range_tree_t *rt = pa->rt;
	vdev_t *vd = pa->vd;

	zfs_range_seg64_t logical, physical, remain;
	logical.rs_start = start;
	logical.rs_end = start + size;
	vdev_xlate(vd, &logical, &physical, &remain);
	
	ASSERT3U(remain.rs_end, ==, remain.rs_start);
	zfs_range_tree_add(rt, physical.rs_start,
	    physical.rs_end - physical.rs_start);
}

/*
 * AnyRAID rebalance background thread
 */
static void
spa_anyraid_rebalance_thread(void *arg, zthr_t *zthr)
{
	spa_t *spa = arg;
	vdev_anyraid_rebalance_t *var = spa->spa_anyraid_rebalance;

	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	vdev_t *pvd = vdev_lookup_top(spa, var->var_vd);
	vdev_anyraid_t *va = pvd->vdev_tsd;

	uint64_t guid = pvd->vdev_guid;

	/* Iterate over all the tasks */
	for (vdev_anyraid_rebalance_task_t *vart =
	    list_head(&var->var_list);
	    vart != NULL && !zthr_iscancelled(zthr);
	    vart = list_head(&var->var_list)) {
		vdev_t *source_vd = pvd->vdev_child[vart->vart_source_disk];
		rw_enter(&va->vd_lock, RW_READER);
		uint64_t start = (vart->vart_tile * va->vd_tile_size) <<
		    pvd->vdev_ms_shift;
		uint64_t end = start + (va->vd_tile_size << pvd->vdev_ms_shift);
		for (uint64_t i = start; i < end && !zthr_iscancelled(zthr);
		    i++) {
			metaslab_t *msp = pvd->vdev_ms[i];

			metaslab_disable(msp);
			mutex_enter(&msp->ms_lock);

			/*
			 * The metaslab may be newly created (for the expanded
			 * space), in which case its trees won't exist yet,
			 * so we need to bail out early.
			 */
			if (msp->ms_new) {
				mutex_exit(&msp->ms_lock);
				metaslab_enable(msp, B_FALSE, B_FALSE);
				continue;
			}

			VERIFY0(metaslab_load(msp));

			/*
			 * We want to copy everything except the free 
			 * (allocatable) space.  Note that there may be a
			 * little bit more free space (e.g. in ms_defer), and
			 * it's fine to copy that too.
			 */
			uint64_t shift, start;
			zfs_range_seg_type_t type =
			    metaslab_calculate_range_tree_type(
			    pvd, msp, &start, &shift);
			zfs_range_tree_t *rt = zfs_range_tree_create_flags(
			    NULL, type, NULL, start, shift, ZFS_RT_F_DYN_NAME,
			    metaslab_rt_name(msp->ms_group, msp,
			    "spa_anyraid_rebalance_thread:rt"));
			zfs_range_tree_add(rt, msp->ms_start, msp->ms_size);
			zfs_range_tree_walk(msp->ms_allocatable,
			    zfs_range_tree_remove, rt);
			mutex_exit(&msp->ms_lock);

			/*
			 * Now we need to convert the logical offsets of the
			 * metaslab into the physical offsets on disk. We also
			 * skip any extents that don't map to to the source
			 * tile.
			 */
			zfs_range_tree_t *phys = zfs_range_tree_create_flags(
			    NULL, type, NULL, start, shift, ZFS_RT_F_DYN_NAME,
			    metaslab_rt_name(msp->ms_group, msp,
			    "spa_anyraid_rebalance_thread2:rt"));
			struct physify_arg pa;
			pa.rt = phys;
			pa.vd = source_vd;
			zfs_range_tree_walk(rt, anyraid_rt_physify, &pa);
			zfs_range_tree_vacate(rt, NULL, NULL);
			zfs_range_tree_destroy(rt);

		/*
		 * Force the last sector of each metaslab to be copied.  This
		 * ensures that we advance the on-disk progress to the end of
		 * this metaslab while the metaslab is disabled.  Otherwise, we
		 * could move past this metaslab without advancing the on-disk
		 * progress, and then an allocation to this metaslab would not
		 * be copied.
		 
		int sectorsz = 1 << raidvd->vdev_ashift;
		uint64_t ms_last_offset = msp->ms_start +
		    msp->ms_size - sectorsz;
		if (!zfs_range_tree_contains(rt, ms_last_offset, sectorsz)) {
			zfs_range_tree_add(rt, ms_last_offset, sectorsz);
		}*/

		/*
		 * When we are resuming from a paused expansion (i.e.
		 * when importing a pool with a expansion in progress),
		 * discard any state that we have already processed.
		 *
		if (var->var_offset > msp->ms_start) {
			zfs_range_tree_clear(rt, msp->ms_start,
			    var->var_offset - msp->ms_start);
		} */

			while (!zthr_iscancelled(zthr) &&
			    !zfs_range_tree_is_empty(rt) &&
			    var->var_failed_offset == UINT64_MAX) {

				/*
				 * We need to periodically drop the config lock so that
				 * writers can get in.  Additionally, we can't wait
				 * for a txg to sync while holding a config lock
				 * (since a waiting writer could cause a 3-way deadlock
				 * with the sync thread, which also gets a config
				 * lock for reader).  So we can't hold the config lock
				 * while calling dmu_tx_assign().
				 */
				spa_config_exit(spa, SCL_CONFIG, FTAG);
				rw_exit(&va->vd_lock);

			/*
			 * If requested, pause the reflow when the amount
			 * specified by raidz_expand_max_reflow_bytes is reached
			 *
			 * This pause is only used during testing or debugging.
			 *
			while (raidz_expand_max_reflow_bytes != 0 &&
			    raidz_expand_max_reflow_bytes <=
			    var->var_bytes_copied && !zthr_iscancelled(zthr)) {
				delay(hz);
			}*/

				mutex_enter(&var->var_lock);
				while (var->var_outstanding_bytes >
				    anyraid_rebalance_max_copy_bytes) {
					cv_wait(&var->var_cv, &var->var_lock);
				}
				mutex_exit(&var->var_lock);

				dmu_tx_t *tx = dmu_tx_create_dd(
				    spa_get_dsl(spa)->dp_mos_dir);

				VERIFY0(dmu_tx_assign(tx,
				    DMU_TX_WAIT | DMU_TX_SUSPEND));
				uint64_t txg = dmu_tx_get_txg(tx);

				/*
				 * Reacquire the vdev_config lock.  Theoretically, the
				 * vdev_t that we're expanding may have changed.
				 */
				rw_enter(&va->vd_lock, RW_READER);
				spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
				pvd = vdev_lookup_top(spa, var->var_vd);

				boolean_t needsync =
				    anyraid_rebalance_impl(pvd, var, phys, tx);

				dmu_tx_commit(tx);

				if (needsync) {
					spa_config_exit(spa, SCL_CONFIG, FTAG);
					rw_exit(&va->vd_lock);
					txg_wait_synced(spa->spa_dsl_pool, txg);
					rw_enter(&va->vd_lock, RW_READER);
					spa_config_enter(spa, SCL_CONFIG, FTAG,
					    RW_READER);
				}
			}

			spa_config_exit(spa, SCL_CONFIG, FTAG);

			metaslab_enable(msp, B_FALSE, B_FALSE);
			zfs_range_tree_vacate(phys, NULL, NULL);
			zfs_range_tree_destroy(phys);

			spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
			pvd = vdev_lookup_top(spa, var->var_vd);
		}
		list_remove(&var->var_list, vart);
		kmem_free(vart, sizeof (*vart));
		rw_exit(&va->vd_lock);
	}

	spa_config_exit(spa, SCL_CONFIG, FTAG);

	/*
	 * The txg_wait_synced() here ensures that all rebalance zio's have
	 * completed, and var_failed_offset has been set if necessary.  It
	 * also ensures that the progress of the last anyraid_rebalance_sync() is
	 * written to disk before anyraid_rebalance_complete_sync() changes the
	 * in-memory var_state.  vdev_anyraid_io_start() uses var_state to
	 * determine if a rebalance is in progress, in which case we may need to
	 * write to both old and new locations.  Therefore we can only change
	 * var_state once this is not necessary, which is once the on-disk
	 * progress (in spa_ubsync) has been set past any possible writes (to
	 * the end of the last metaslab).
	 */
	txg_wait_synced(spa->spa_dsl_pool, 0);

	if (!zthr_iscancelled(zthr) && list_head(&var->var_list) == NULL) {
		/*
		 * We are not being canceled or paused, so the reflow must be
		 * complete. In that case also mark it as completed on disk.
		 */
		ASSERT3U(var->var_failed_offset, ==, UINT64_MAX);
		VERIFY0(dsl_sync_task(spa_name(spa), NULL,
		    anyraid_rebalance_complete_sync, spa,
		    0, ZFS_SPACE_CHECK_NONE));
		(void) vdev_online(spa, guid, ZFS_ONLINE_EXPAND, NULL);
	} else {
		/*
		 * Wait for all copy zio's to complete and for all the
		 * raidz_reflow_sync() synctasks to be run.
		 */
		spa_history_log_internal(spa, "rebalance pause",
		    NULL, "offset=%llu failed_offset=%lld",
		    (long long)var->var_offset,
		    (long long)var->var_failed_offset);
		mutex_enter(&var->var_lock);
		if (var->var_failed_offset != UINT64_MAX) {
			/*
			 * Reset progress so that we will retry everything
			 * after the point that something failed.
			 */
			var->var_offset = var->var_failed_offset;
			var->var_failed_offset = UINT64_MAX;
			var->var_waiting_for_resilver = B_TRUE;
		}
		mutex_exit(&var->var_lock);
	}
}

void
spa_start_anyraid_rebalance_thread(spa_t *spa)
{
	ASSERT0P(spa->spa_anyraid_rebalance_zthr);
	spa->spa_anyraid_rebalance_zthr = zthr_create("anyraid_rebalance",
	    spa_anyraid_rebalance_thread_check, spa_anyraid_rebalance_thread,
	    spa, defclsyspri);
}

ZFS_MODULE_PARAM(zfs_anyraid, zfs_anyraid_, min_tile_size, U64, ZMOD_RW,
	"Minimum tile size for anyraid");