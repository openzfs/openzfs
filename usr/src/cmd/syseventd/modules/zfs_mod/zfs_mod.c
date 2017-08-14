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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012 by Delphix. All rights reserved.
 * Copyright 2017 Nexenta Systems, Inc.
 */

/*
 * ZFS syseventd module.
 *
 * The purpose of this module is to process ZFS related events.
 *
 * EC_DEV_ADD
 *  ESC_DISK		Search for associated vdevs matching devid, physpath,
 *			or FRU, and appropriately online or replace the device.
 *
 * EC_DEV_STATUS
 *  ESC_DEV_DLE		Device capacity dynamically changed.  Process the change
 *			according to 'autoexpand' property.
 *
 * EC_ZFS
 *  ESC_ZFS_VDEV_CHECK	This event indicates that a device failed to open during
 *			pool load, but the autoreplace property was set.  In
 *			this case the associated FMA fault was deferred until
 *			the module had a chance to process the autoreplace
 *			logic.  If the device could not be replaced, then the
 *			second online attempt will trigger the FMA fault that
 *			was skipped earlier.
 *  ESC_ZFS_VDEV_ADD
 *  ESC_ZFS_VDEV_ATTACH
 *  ESC_ZFS_VDEV_CLEAR
 *  ESC_ZFS_VDEV_ONLINE
 *  ESC_ZFS_POOL_CREATE
 *  ESC_ZFS_POOL_IMPORT	All of the above events will trigger the update of
 *			FRU for all associated devices.
 */

#include <alloca.h>
#include <devid.h>
#include <fcntl.h>
#include <libnvpair.h>
#include <libsysevent.h>
#include <libzfs.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/list.h>
#include <sys/sunddi.h>
#include <sys/fs/zfs.h>
#include <sys/sysevent/eventdefs.h>
#include <sys/sysevent/dev.h>
#include <thread_pool.h>
#include <unistd.h>
#include "syseventd.h"

#if defined(__i386) || defined(__amd64)
#define	WD_MINOR	":q"
#elif defined(__sparc)
#define	WD_MINOR	":c"
#else
#error Unknown architecture
#endif

#define	DEVICE_PREFIX	"/devices"

typedef void (*zfs_process_func_t)(zpool_handle_t *, nvlist_t *, const char *);

libzfs_handle_t *g_zfshdl;
list_t g_pool_list;
tpool_t *g_tpool;
boolean_t g_enumeration_done;
thread_t g_zfs_tid;

typedef struct unavailpool {
	zpool_handle_t	*uap_zhp;
	list_node_t	uap_node;
} unavailpool_t;

int
zfs_toplevel_state(zpool_handle_t *zhp)
{
	nvlist_t *nvroot;
	vdev_stat_t *vs;
	unsigned int c;

	verify(nvlist_lookup_nvlist(zpool_get_config(zhp, NULL),
	    ZPOOL_CONFIG_VDEV_TREE, &nvroot) == 0);
	verify(nvlist_lookup_uint64_array(nvroot, ZPOOL_CONFIG_VDEV_STATS,
	    (uint64_t **)&vs, &c) == 0);
	return (vs->vs_state);
}

static int
zfs_unavail_pool(zpool_handle_t *zhp, void *data)
{
	if (zfs_toplevel_state(zhp) < VDEV_STATE_DEGRADED) {
		unavailpool_t *uap;
		uap = malloc(sizeof (unavailpool_t));
		uap->uap_zhp = zhp;
		list_insert_tail((list_t *)data, uap);
	} else {
		zpool_close(zhp);
	}
	return (0);
}

/*
 * The device associated with the given vdev (matched by devid, physical path,
 * or FRU) has been added to the system.
 */
static void
zfs_process_add(zpool_handle_t *zhp, nvlist_t *vdev, const char *newrawpath)
{
	vdev_state_t newstate;
	nvlist_t *nvroot = NULL, *newvd = NULL;
	uint64_t wholedisk = 0ULL;
	uint64_t offline = 0ULL;
	boolean_t avail_spare, l2cache;
	const char *zc_type = ZPOOL_CONFIG_CHILDREN;
	char *devpath;			/* current /dev path */
	char *physpath;			/* current /devices node */
	char fullpath[PATH_MAX];	/* current /dev path without slice */
	char fullphyspath[PATH_MAX];	/* full /devices phys path */
	char newdevpath[PATH_MAX];	/* new /dev path */
	char newphyspath[PATH_MAX];	/* new /devices node */
	char diskname[PATH_MAX];	/* disk device without /dev and slice */
	const char *adevid = NULL;	/* devid to attach */
	const char *adevpath;		/* /dev path to attach */
	const char *aphyspath = NULL;	/* /devices node to attach */
	zpool_boot_label_t boot_type;
	uint64_t boot_size;

	if (nvlist_lookup_string(vdev, ZPOOL_CONFIG_PATH, &devpath) != 0)
		return;
	(void) nvlist_lookup_string(vdev, ZPOOL_CONFIG_PHYS_PATH, &physpath);
	(void) nvlist_lookup_uint64(vdev, ZPOOL_CONFIG_WHOLE_DISK, &wholedisk);
	(void) nvlist_lookup_uint64(vdev, ZPOOL_CONFIG_OFFLINE, &offline);

	/* Do nothing if vdev is explicitly marked offline */
	if (offline)
		return;

	(void) strlcpy(fullpath, devpath, sizeof (fullpath));
	/* Chop off slice for whole disks */
	if (wholedisk)
		fullpath[strlen(fullpath) - 2] = '\0';

	/*
	 * Device could still have valid label, so first attempt to online the
	 * device undoing any spare operation. If online succeeds and new state
	 * is either HEALTHY or DEGRADED, we are done.
	 */
	if (zpool_vdev_online(zhp, fullpath,
	    ZFS_ONLINE_CHECKREMOVE | ZFS_ONLINE_UNSPARE, &newstate) == 0 &&
	    (newstate == VDEV_STATE_HEALTHY || newstate == VDEV_STATE_DEGRADED))
		return;

	/*
	 * If the pool doesn't have the autoreplace property set or this is a
	 * non-whole disk vdev, there's nothing else we can do so attempt a true
	 * online (without the unspare flag), which will trigger a FMA fault.
	 */
	if (zpool_get_prop_int(zhp, ZPOOL_PROP_AUTOREPLACE, NULL) == 0 ||
	    !wholedisk) {
		(void) zpool_vdev_online(zhp, fullpath, ZFS_ONLINE_FORCEFAULT,
		    &newstate);
		return;
	}

	/*
	 * Attempt to replace the device.
	 *
	 * If newrawpath is set (not NULL), then we matched by FRU and need to
	 * use new /dev and /devices paths for attach.
	 *
	 * First, construct the short disk name to label, chopping off any
	 * leading /dev path and slice (which newrawpath doesn't include).
	 */
	if (newrawpath != NULL) {
		(void) strlcpy(diskname, newrawpath +
		    strlen(ZFS_RDISK_ROOTD), sizeof (diskname));
	} else {
		(void) strlcpy(diskname, fullpath +
		    strlen(ZFS_DISK_ROOTD), sizeof (diskname));
	}

	/* Write out the label */
	if (zpool_is_bootable(zhp))
		boot_type = ZPOOL_COPY_BOOT_LABEL;
	else
		boot_type = ZPOOL_NO_BOOT_LABEL;

	boot_size = zpool_get_prop_int(zhp, ZPOOL_PROP_BOOTSIZE, NULL);
	if (zpool_label_disk(g_zfshdl, zhp, diskname, boot_type, boot_size,
	    NULL) != 0) {
		syseventd_print(9, "%s: failed to write the label\n", __func__);
		return;
	}

	/* Define "path" and "physpath" to be used for attach */
	if (newrawpath != NULL) {
		/* Construct newdevpath from newrawpath */
		(void) snprintf(newdevpath, sizeof (newdevpath), "%s%s%s",
		    ZFS_DISK_ROOTD, newrawpath + strlen(ZFS_RDISK_ROOTD),
		    (boot_size > 0) ? "s1" : "s0");
		/* Use replacing vdev's "path" and "physpath" */
		adevpath = newdevpath;
		/* Resolve /dev path to /devices node */
		aphyspath = realpath(newdevpath, newphyspath) +
		    strlen(DEVICE_PREFIX);
	} else {
		/* Use original vdev's "path" and "physpath" */
		adevpath = devpath;
		aphyspath = physpath;
	}

	/* Construct new devid */
	(void) snprintf(fullphyspath, sizeof (fullphyspath), "%s%s",
	    DEVICE_PREFIX, aphyspath);
	adevid = devid_str_from_path(fullphyspath);

	/*
	 * Check if replaced vdev is "available" (not swapped in) spare
	 * or l2cache device.
	 */
	(void) zpool_find_vdev(zhp, fullpath, &avail_spare, &l2cache, NULL);
	if (avail_spare)
		zc_type = ZPOOL_CONFIG_SPARES;
	else if (l2cache)
		zc_type = ZPOOL_CONFIG_L2CACHE;

	/* Construct the root vdev */
	if (nvlist_alloc(&nvroot, NV_UNIQUE_NAME, 0) != 0 ||
	    nvlist_alloc(&newvd, NV_UNIQUE_NAME, 0) != 0)
		goto fail;

	if (nvlist_add_string(newvd, ZPOOL_CONFIG_TYPE, VDEV_TYPE_DISK) != 0 ||
	    (adevid != NULL &&
	    nvlist_add_string(newvd, ZPOOL_CONFIG_DEVID, adevid) != 0) ||
	    nvlist_add_string(newvd, ZPOOL_CONFIG_PATH, adevpath) != 0 ||
	    (aphyspath != NULL &&
	    nvlist_add_string(newvd, ZPOOL_CONFIG_PHYS_PATH, aphyspath) != 0) ||
	    nvlist_add_uint64(newvd, ZPOOL_CONFIG_WHOLE_DISK, wholedisk) != 0 ||
	    nvlist_add_string(nvroot, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT) != 0 ||
	    nvlist_add_nvlist_array(nvroot, zc_type, &newvd, 1) != 0)
		goto fail;

	if (avail_spare || l2cache) {
		/*
		 * For spares/l2cache, we need to explicitly remove the device
		 * and add the new one.
		 */
		(void) zpool_vdev_remove(zhp, fullpath);
		(void) zpool_add(zhp, nvroot);
	} else {
		/* Do the replace for regular vdevs */
		(void) zpool_vdev_attach(zhp, fullpath, adevpath, nvroot,
		    B_TRUE);
	}

fail:
	if (adevid != NULL)
		devid_str_free((char *)adevid);
	nvlist_free(newvd);
	nvlist_free(nvroot);
}

/*
 * Utility functions to find a vdev matching given criteria.
 */
typedef struct dev_data {
	const char		*dd_compare;
	const char		*dd_prop;
	const char		*dd_devpath;
	zfs_process_func_t	dd_func;
	int			(*dd_cmp_func)(libzfs_handle_t *, const char *,
				    const char *, size_t);
	boolean_t		dd_found;
	uint64_t		dd_pool_guid;
	uint64_t		dd_vdev_guid;
} dev_data_t;

static void
zfs_iter_vdev(zpool_handle_t *zhp, nvlist_t *nvl, void *data)
{
	dev_data_t *dp = data;
	boolean_t nested = B_FALSE;
	char *cmp_str;
	nvlist_t **cnvl, **snvl, **lnvl;
	uint_t i, nc, ns, nl;
	uint64_t guid;

	/* Iterate over child vdevs */
	if (nvlist_lookup_nvlist_array(nvl, ZPOOL_CONFIG_CHILDREN,
	    &cnvl, &nc) == 0) {
		for (i = 0; i < nc; i++)
			zfs_iter_vdev(zhp, cnvl[i], data);
		nested = B_TRUE;
	}
	/* Iterate over spare vdevs */
	if (nvlist_lookup_nvlist_array(nvl, ZPOOL_CONFIG_SPARES,
	    &snvl, &ns) == 0) {
		for (i = 0; i < ns; i++)
			zfs_iter_vdev(zhp, snvl[i], data);
		nested = B_TRUE;
	}
	/* Iterate over l2cache vdevs */
	if (nvlist_lookup_nvlist_array(nvl, ZPOOL_CONFIG_L2CACHE,
	    &lnvl, &nl) == 0) {
		for (i = 0; i < nl; i++)
			zfs_iter_vdev(zhp, lnvl[i], data);
		nested = B_TRUE;
	}

	if (nested)
		return;

	if (dp->dd_vdev_guid != 0 && (nvlist_lookup_uint64(nvl,
	    ZPOOL_CONFIG_GUID, &guid) != 0 || guid != dp->dd_vdev_guid))
			return;

	if (dp->dd_compare != NULL && (nvlist_lookup_string(nvl, dp->dd_prop,
	    &cmp_str) != 0 || dp->dd_cmp_func(g_zfshdl, dp->dd_compare, cmp_str,
	    strlen(dp->dd_compare)) != 0))
			return;

	dp->dd_found = B_TRUE;
	(dp->dd_func)(zhp, nvl, dp->dd_devpath);
}

void
zfs_enable_ds(void *arg)
{
	unavailpool_t *pool = (unavailpool_t *)arg;

	(void) zpool_enable_datasets(pool->uap_zhp, NULL, 0);
	zpool_close(pool->uap_zhp);
	free(pool);
}

static int
zfs_iter_pool(zpool_handle_t *zhp, void *data)
{
	nvlist_t *config, *nvl;
	dev_data_t *dp = data;
	uint64_t pool_guid;
	unavailpool_t *pool;

	if ((config = zpool_get_config(zhp, NULL)) != NULL) {
		if (dp->dd_pool_guid == 0 ||
		    (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
		    &pool_guid) == 0 && pool_guid == dp->dd_pool_guid)) {
			(void) nvlist_lookup_nvlist(config,
			    ZPOOL_CONFIG_VDEV_TREE, &nvl);
			zfs_iter_vdev(zhp, nvl, data);
		}
	}
	if (g_enumeration_done)  {
		for (pool = list_head(&g_pool_list); pool != NULL;
		    pool = list_next(&g_pool_list, pool)) {

			if (strcmp(zpool_get_name(zhp),
			    zpool_get_name(pool->uap_zhp)))
				continue;
			if (zfs_toplevel_state(zhp) >= VDEV_STATE_DEGRADED) {
				list_remove(&g_pool_list, pool);
				(void) tpool_dispatch(g_tpool, zfs_enable_ds,
				    pool);
				break;
			}
		}
	}

	zpool_close(zhp);
	return (0);
}

/*
 * Wrap strncmp() to be used as comparison function for devid_iter() and
 * physpath_iter().
 */
/* ARGSUSED */
static int
strncmp_wrap(libzfs_handle_t *hdl, const char *a, const char *b, size_t len)
{
	return (strncmp(a, b, len));
}

/*
 * Given a physical device path, iterate over all (pool, vdev) pairs which
 * correspond to the given path's FRU.
 */
static boolean_t
devfru_iter(const char *devpath, const char *physpath, zfs_process_func_t func)
{
	dev_data_t data = { 0 };
	const char *fru;

	/*
	 * Need to refresh the fru cache otherwise we won't find the newly
	 * inserted disk.
	 */
	libzfs_fru_refresh(g_zfshdl);

	fru = libzfs_fru_lookup(g_zfshdl, physpath);
	if (fru == NULL)
		return (B_FALSE);

	data.dd_compare = fru;
	data.dd_func = func;
	data.dd_cmp_func = libzfs_fru_cmp_slot;
	data.dd_prop = ZPOOL_CONFIG_FRU;
	data.dd_found = B_FALSE;
	data.dd_devpath = devpath;

	(void) zpool_iter(g_zfshdl, zfs_iter_pool, &data);

	return (data.dd_found);
}

/*
 * Given a physical device path, iterate over all (pool, vdev) pairs which
 * correspond to the given path.
 */
/*ARGSUSED*/
static boolean_t
physpath_iter(const char *devpath, const char *physpath,
    zfs_process_func_t func)
{
	dev_data_t data = { 0 };

	data.dd_compare = physpath;
	data.dd_func = func;
	data.dd_cmp_func = strncmp_wrap;
	data.dd_prop = ZPOOL_CONFIG_PHYS_PATH;
	data.dd_found = B_FALSE;
	data.dd_devpath = NULL;

	(void) zpool_iter(g_zfshdl, zfs_iter_pool, &data);

	return (data.dd_found);
}

/*
 * Given a devid, iterate over all (pool, vdev) pairs which correspond to the
 * given vdev.
 */
/*ARGSUSED*/
static boolean_t
devid_iter(const char *devpath, const char *physpath, zfs_process_func_t func)
{
	char fullphyspath[PATH_MAX];
	char *devidstr;
	char *s;
	dev_data_t data = { 0 };

	/* Try to open a known minor node */
	(void) snprintf(fullphyspath, sizeof (fullphyspath), "%s%s%s",
	    DEVICE_PREFIX, physpath, WD_MINOR);

	devidstr = devid_str_from_path(fullphyspath);
	if (devidstr == NULL)
		return (B_FALSE);
	/* Chop off the minor node */
	if ((s = strrchr(devidstr, '/')) != NULL)
		*(s + 1) = '\0';

	data.dd_compare = devidstr;
	data.dd_func = func;
	data.dd_cmp_func = strncmp_wrap;
	data.dd_prop = ZPOOL_CONFIG_DEVID;
	data.dd_found = B_FALSE;
	data.dd_devpath = NULL;

	(void) zpool_iter(g_zfshdl, zfs_iter_pool, &data);

	devid_str_free(devidstr);

	return (data.dd_found);
}

/*
 * This function is called when we receive a devfs add event.
 */
static int
zfs_deliver_add(nvlist_t *nvl)
{
	char *devpath, *physpath;

	if (nvlist_lookup_string(nvl, DEV_NAME, &devpath) != 0 ||
	    nvlist_lookup_string(nvl, DEV_PHYS_PATH, &physpath) != 0)
		return (-1);

	/*
	 * Iterate over all vdevs with a matching devid, then those with a
	 * matching /devices path, and finally those with a matching FRU slot
	 * number, only paying attention to vdevs marked as whole disks.
	 */
	if (!devid_iter(devpath, physpath, zfs_process_add) &&
	    !physpath_iter(devpath, physpath, zfs_process_add) &&
	    !devfru_iter(devpath, physpath, zfs_process_add)) {
		syseventd_print(9, "%s: match failed devpath=%s physpath=%s\n",
		    __func__, devpath, physpath);
	}

	return (0);
}

/*
 * Called when we receive a VDEV_CHECK event, which indicates a device could not
 * be opened during initial pool open, but the autoreplace property was set on
 * the pool.  In this case, we treat it as if it were an add event.
 */
static int
zfs_deliver_check(nvlist_t *nvl)
{
	dev_data_t data = { 0 };

	if (nvlist_lookup_uint64(nvl, ZFS_EV_POOL_GUID,
	    &data.dd_pool_guid) != 0 ||
	    nvlist_lookup_uint64(nvl, ZFS_EV_VDEV_GUID,
	    &data.dd_vdev_guid) != 0 ||
	    data.dd_vdev_guid == 0)
		return (0);

	data.dd_func = zfs_process_add;

	(void) zpool_iter(g_zfshdl, zfs_iter_pool, &data);

	return (0);
}

static int
zfsdle_vdev_online(zpool_handle_t *zhp, void *data)
{
	char *devname = data;
	boolean_t avail_spare, l2cache;
	vdev_state_t newstate;
	nvlist_t *tgt;

	syseventd_print(9, "%s: searching for %s in pool %s\n", __func__,
	    devname, zpool_get_name(zhp));

	if ((tgt = zpool_find_vdev_by_physpath(zhp, devname,
	    &avail_spare, &l2cache, NULL)) != NULL) {
		char *path, fullpath[MAXPATHLEN];
		uint64_t wholedisk = 0ULL;

		verify(nvlist_lookup_string(tgt, ZPOOL_CONFIG_PATH,
		    &path) == 0);
		verify(nvlist_lookup_uint64(tgt, ZPOOL_CONFIG_WHOLE_DISK,
		    &wholedisk) == 0);

		(void) strlcpy(fullpath, path, sizeof (fullpath));
		if (wholedisk) {
			fullpath[strlen(fullpath) - 2] = '\0';

			/*
			 * We need to reopen the pool associated with this
			 * device so that the kernel can update the size
			 * of the expanded device.
			 */
			(void) zpool_reopen(zhp);
		}

		if (zpool_get_prop_int(zhp, ZPOOL_PROP_AUTOEXPAND, NULL)) {
			syseventd_print(9, "%s: setting device '%s' to ONLINE "
			    "state in pool %s\n", __func__, fullpath,
			    zpool_get_name(zhp));
			if (zpool_get_state(zhp) != POOL_STATE_UNAVAIL)
				(void) zpool_vdev_online(zhp, fullpath, 0,
				    &newstate);
		}
		zpool_close(zhp);
		return (1);
	}
	zpool_close(zhp);
	return (0);
}

/*
 * This function is called for each vdev of a pool for which any of the
 * following events was received:
 *  - ESC_ZFS_vdev_add
 *  - ESC_ZFS_vdev_attach
 *  - ESC_ZFS_vdev_clear
 *  - ESC_ZFS_vdev_online
 *  - ESC_ZFS_pool_create
 *  - ESC_ZFS_pool_import
 * It will update the vdevs FRU property if it is out of date.
 */
/*ARGSUSED*/
static void
zfs_update_vdev_fru(zpool_handle_t *zhp, nvlist_t *vdev, const char *devpath)
{
	char *physpath, *cptr, *oldfru = NULL;
	const char *newfru;
	uint64_t vdev_guid;

	(void) nvlist_lookup_uint64(vdev, ZPOOL_CONFIG_GUID, &vdev_guid);
	(void) nvlist_lookup_string(vdev, ZPOOL_CONFIG_PHYS_PATH, &physpath);
	(void) nvlist_lookup_string(vdev, ZPOOL_CONFIG_FRU, &oldfru);

	/* Remove :<slice> from physpath */
	cptr = strrchr(physpath, ':');
	if (cptr != NULL)
		*cptr = '\0';

	newfru = libzfs_fru_lookup(g_zfshdl, physpath);
	if (newfru == NULL) {
		syseventd_print(9, "%s: physpath=%s newFRU=<none>\n", __func__,
		    physpath);
		return;
	}

	/* Do nothing if the FRU hasn't changed */
	if (oldfru != NULL && libzfs_fru_compare(g_zfshdl, oldfru, newfru)) {
		syseventd_print(9, "%s: physpath=%s newFRU=<unchanged>\n",
		    __func__, physpath);
		return;
	}

	syseventd_print(9, "%s: physpath=%s newFRU=%s\n", __func__, physpath,
	    newfru);

	(void) zpool_fru_set(zhp, vdev_guid, newfru);
}

/*
 * This function handles the following events:
 *  - ESC_ZFS_vdev_add
 *  - ESC_ZFS_vdev_attach
 *  - ESC_ZFS_vdev_clear
 *  - ESC_ZFS_vdev_online
 *  - ESC_ZFS_pool_create
 *  - ESC_ZFS_pool_import
 * It will iterate over the pool vdevs to update the FRU property.
 */
int
zfs_deliver_update(nvlist_t *nvl)
{
	dev_data_t dd = { 0 };
	char *pname;
	zpool_handle_t *zhp;
	nvlist_t *config, *vdev;

	if (nvlist_lookup_string(nvl, "pool_name", &pname) != 0) {
		syseventd_print(9, "%s: no pool name\n", __func__);
		return (-1);
	}

	/*
	 * If this event was triggered by a pool export or destroy we cannot
	 * open the pool. This is not an error, just return 0 as we don't care
	 * about these events.
	 */
	zhp = zpool_open_canfail(g_zfshdl, pname);
	if (zhp == NULL)
		return (0);

	config = zpool_get_config(zhp, NULL);
	if (config == NULL) {
		syseventd_print(9, "%s: failed to get pool config for %s\n",
		    __func__, pname);
		zpool_close(zhp);
		return (-1);
	}

	if (nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, &vdev) != 0) {
		syseventd_print(0, "%s: failed to get vdev tree for %s\n",
		    __func__, pname);
		zpool_close(zhp);
		return (-1);
	}

	libzfs_fru_refresh(g_zfshdl);

	dd.dd_func = zfs_update_vdev_fru;
	zfs_iter_vdev(zhp, vdev, &dd);

	zpool_close(zhp);
	return (0);
}

int
zfs_deliver_dle(nvlist_t *nvl)
{
	char *physpath;

	if (nvlist_lookup_string(nvl, DEV_PHYS_PATH, &physpath) != 0) {
		syseventd_print(9, "%s: no physpath\n", __func__);
		return (-1);
	}
	if (strncmp(physpath, DEVICE_PREFIX, strlen(DEVICE_PREFIX)) != 0) {
		syseventd_print(9, "%s: invalid device '%s'", __func__,
		    physpath);
		return (-1);
	}

	/*
	 * We try to find the device using the physical
	 * path that has been supplied. We need to strip off
	 * the /devices prefix before starting our search.
	 */
	physpath += strlen(DEVICE_PREFIX);
	if (zpool_iter(g_zfshdl, zfsdle_vdev_online, physpath) != 1) {
		syseventd_print(9, "%s: device '%s' not  found\n",
		    __func__, physpath);
		return (1);
	}
	return (0);
}


/*ARGSUSED*/
static int
zfs_deliver_event(sysevent_t *ev, int unused)
{
	const char *class = sysevent_get_class_name(ev);
	const char *subclass = sysevent_get_subclass_name(ev);
	nvlist_t *nvl;
	int ret;
	boolean_t is_check = B_FALSE;
	boolean_t is_dle = B_FALSE;
	boolean_t is_update = B_FALSE;

	if (strcmp(class, EC_DEV_ADD) == 0) {
		/* We're only interested in disk additions */
		if (strcmp(subclass, ESC_DISK) != 0)
			return (0);
	} else if (strcmp(class, EC_ZFS) == 0) {
		if (strcmp(subclass, ESC_ZFS_VDEV_CHECK) == 0) {
			/*
			 * This event signifies that a device failed to open
			 * during pool load, but the 'autoreplace' property was
			 * set, so we should pretend it's just been added.
			 */
			is_check = B_TRUE;
		} else if ((strcmp(subclass, ESC_ZFS_VDEV_ADD) == 0) ||
		    (strcmp(subclass, ESC_ZFS_VDEV_ATTACH) == 0) ||
		    (strcmp(subclass, ESC_ZFS_VDEV_CLEAR) == 0) ||
		    (strcmp(subclass, ESC_ZFS_VDEV_ONLINE) == 0) ||
		    (strcmp(subclass, ESC_ZFS_POOL_CREATE) == 0) ||
		    (strcmp(subclass, ESC_ZFS_POOL_IMPORT) == 0)) {
			/*
			 * When we receive these events we check the pool
			 * configuration and update the vdev FRUs if necessary.
			 */
			is_update = B_TRUE;
		}
	} else if (strcmp(class, EC_DEV_STATUS) == 0 &&
	    strcmp(subclass, ESC_DEV_DLE) == 0) {
		is_dle = B_TRUE;
	} else {
		return (0);
	}

	if (sysevent_get_attr_list(ev, &nvl) != 0)
		return (-1);

	if (is_dle)
		ret = zfs_deliver_dle(nvl);
	else if (is_update)
		ret = zfs_deliver_update(nvl);
	else if (is_check)
		ret = zfs_deliver_check(nvl);
	else
		ret = zfs_deliver_add(nvl);

	nvlist_free(nvl);
	return (ret);
}

/*ARGSUSED*/
void *
zfs_enum_pools(void *arg)
{
	(void) zpool_iter(g_zfshdl, zfs_unavail_pool, (void *)&g_pool_list);
	if (!list_is_empty(&g_pool_list))
		g_tpool = tpool_create(1, sysconf(_SC_NPROCESSORS_ONLN),
		    0, NULL);
	g_enumeration_done = B_TRUE;
	return (NULL);
}

static struct slm_mod_ops zfs_mod_ops = {
	SE_MAJOR_VERSION, SE_MINOR_VERSION, 10, zfs_deliver_event
};

struct slm_mod_ops *
slm_init()
{
	if ((g_zfshdl = libzfs_init()) == NULL)
		return (NULL);
	/*
	 * collect a list of unavailable pools (asynchronously,
	 * since this can take a while)
	 */
	list_create(&g_pool_list, sizeof (struct unavailpool),
	    offsetof(struct unavailpool, uap_node));
	if (thr_create(NULL, 0, zfs_enum_pools, NULL, 0, &g_zfs_tid) != 0)
		return (NULL);
	return (&zfs_mod_ops);
}

void
slm_fini()
{
	unavailpool_t *pool;

	(void) thr_join(g_zfs_tid, NULL, NULL);
	if (g_tpool != NULL) {
		tpool_wait(g_tpool);
		tpool_destroy(g_tpool);
	}
	while ((pool = (list_head(&g_pool_list))) != NULL) {
		list_remove(&g_pool_list, pool);
		zpool_close(pool->uap_zhp);
		free(pool);
	}
	list_destroy(&g_pool_list);
	libzfs_fini(g_zfshdl);
}
