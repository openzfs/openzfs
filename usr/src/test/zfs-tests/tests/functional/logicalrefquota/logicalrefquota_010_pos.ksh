#!/bin/ksh -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2013 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/logicalrefquota/logicalrefquota.kshlib

#
# DESCRIPTION:
#	logicalrefquotas are not limited by snapshots.
#
# STRATEGY:
#	1. Setting logicalrefquota < logicalquota
#	2. Create file in filesytem, take snapshot and remove the file
#	3. Verify snapshot will not consume logicalrefquota
#

verify_runnable "both"

function cleanup
{
	log_must $ZFS destroy -rf $TESTPOOL/$TESTFS
	log_must $ZFS create $TESTPOOL/$TESTFS
	log_must $ZFS set mountpoint=$TESTDIR $TESTPOOL/$TESTFS
}

log_assert "logicalrefquotas are not limited by snapshots."
log_onexit cleanup

fs=$TESTPOOL/$TESTFS
log_must $ZFS set logicalquota=25M $fs
log_must $ZFS set logicalrefquota=15M $fs
log_must $ZFS set compression=on $fs

mntpnt=$(get_prop mountpoint $fs)
typeset -i i=0
while ((i < 3)); do
	log_must fill_file $mntpnt/$TESTFILE1.$i 7000000
	log_must $ZFS snapshot $fs@snap.$i
	log_must $RM $mntpnt/$TESTFILE1.$i

	((i += 1))
done

#
# Verify out of the limitation of 'logicalquota'
#
log_mustnot fill_file $mntpnt/$TESTFILE1 7000000

log_pass "logicalrefquotas are not limited by snapshots."
