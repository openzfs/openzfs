#! /bin/ksh -p
#
# CDDL HEADER START
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#
# CDDL HEADER END
#

#
# Copyright (c) 2018 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/removal/removal.kshlib

#
# DESCRIPTION:
#
# Ensure that cancelling a removal midway does not panic the
# system for whatever reason. [The original issue was a
# regression due to the Log Space Map feature]
#
# STRATEGY:
#
# 1. Create a pool with one vdev and do some writes on it.
# 2. Add a new vdev to the pool and start the removal of
#    the first vdev.
# 3. Cancel the removal after some segments have been copied
#    over to the new vdev.
# 4. Run zdb to ensure the on-disk state of the pool is ok.
#


function cleanup
{
	#
	# Reset tunable.
	#
	mdb_ctf_set_int zfs_remove_max_bytes_pause -0t1
	default_cleanup_noexit
}

log_onexit cleanup

SAMPLEFILE=/$TESTDIR/00

#
# Create pool with one disk.
#
log_must default_setup_noexit "$REMOVEDISK"

#
# Create a file of size 1GB and then do some random writes.
# Since randwritecomp does 8K writes we do 12500 writes
# which means we write ~100MB to the vdev.
#
log_must mkfile -n 1g $SAMPLEFILE
log_must randwritecomp $SAMPLEFILE 12500

#
# Add second device where all the data will be evacuated.
#
log_must zpool add -f $TESTPOOL $NOTREMOVEDISK

#
# Set maximum bytes to be transfered by removal to be 20MB.
# The idea is to make sure that it stops at some point after
# it has copied over some data but not all of them (100MB).
#
mdb_ctf_set_int zfs_remove_max_bytes_pause 1400000

#
# Start removal.
#
log_must zpool remove $TESTPOOL $REMOVEDISK

#
# Sleep a bit to allow removal to copy some data.
#
log_must sleep 10

#
# Only for debugging purposes in test logs.
#
log_must zpool status $TESTPOOL

#
# Cancel removal.
#
log_must zpool remove -s $TESTPOOL

#
# Verify on-disk state.
#
log_must zdb $TESTPOOL

log_pass "Device removal thread cancelled successfully."
