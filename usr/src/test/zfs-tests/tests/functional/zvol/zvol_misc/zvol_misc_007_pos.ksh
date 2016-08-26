#! /usr/bin/ksh -p
#
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

#
# Copyright 2016 Nexenta Systems, Inc. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/zvol/zvol_common.shlib

#
# DESCRIPTION:
# Check that name collision between just destroyed filesystem and newly created
# volume with the same name doesn't cause panic.
#
# STRATEGY:
# 1. Create ZFS filesystem.
# 2. Create nested ZFS volume.
# 3. Read and display information about the ZFS volume.
# 4. Recursively destroy ZFS filesystem.
# 5. Create ZFS volume with the same name as ZFS filesystem.
# 6. Read and display information about the ZFS volume.
#

verify_runnable "global"
log_assert "zfs can handle race volume create operation."
log_onexit cleanup

log_must zfs create $TESTPOOL/$TESTFS
log_must zfs create -V 1M $TESTPOOL/$TESTFS/$TESTVOL
log_must stat /dev/zvol/rdsk/$TESTPOOL/$TESTFS/$TESTVOL
log_must zfs destroy -r $TESTPOOL/$TESTFS
log_must zfs create -V 1M $TESTPOOL/$TESTFS
log_must stat /dev/zvol/rdsk/$TESTPOOL/$TESTFS

log_pass "zfs handle race volume create operation."
