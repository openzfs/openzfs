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
# Copyright (c) 2013, 2016 by Delphix. All rights reserved.
# Copyright (c) 2016 OVH [ovh.com].
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/logicalrefquota/logicalrefquota.kshlib

#
# DESCRIPTION:
#	logicalrefquota limits the amount of space a dataset can consume, but does
#	not include space used by descendents.
#
# STRATEGY:
#	1. Setting logicalrefquota in given filesystem
#	2. Create descendent filesystem
#	3. Verify logicalrefquota limits the amount of space a dataset can consume
#	4. Verify the limit does not impact descendents
#

verify_runnable "both"

function cleanup
{
	log_must $ZFS destroy -rf $TESTPOOL/$TESTFS
	log_must $ZFS create $TESTPOOL/$TESTFS
	log_must $ZFS set mountpoint=$TESTDIR $TESTPOOL/$TESTFS
}

log_assert "logicalrefquota limits the amount of logical space a dataset can consume, " \
	"but does not include space used by descendents."
log_onexit cleanup

fs=$TESTPOOL/$TESTFS
sub=$fs/sub
log_must $ZFS create $sub

log_must $ZFS set logicalrefquota=10M $fs
log_must $ZFS set compression=on $fs
mntpnt=$(get_prop mountpoint $fs)

#
# Fills the logicalrefquota & attempts to write another file
#
log_must fill_logicalrefquota $TESTPOOL/$TESTFS $TESTDIR
log_must $ZFS snapshot $fs@snap

#
# Fills the logicalrefquota & attempts to write another file
# after taking snapshot
#
log_must fill_logicalrefquota $TESTPOOL/$TESTFS $TESTDIR
log_must exceed_logicalrefquota $TESTPOOL/$TESTFS $TESTDIR

#
# Try to write file on descendent
#
mntpnt=$(get_prop mountpoint $sub)
log_must fill_file $mntpnt/$TESTFILE1 
log_must $ZFS snapshot $sub@snap
log_must fill_file $mntpnt/$TESTFILE2

log_pass "logicalrefquota limits the amount of space a dataset can consume, " \
	"but does not include space used by descendents."
