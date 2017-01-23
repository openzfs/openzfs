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

#
# DESCRIPTION:
#	Sub-filesystem quotas are not enforced by property 'logicalrefquota'
#
# STRATEGY:
#	1. Setting logicalquota and logicalrefquota for parent. lrefquota < lquota
#	2. Verify sub-filesystem will not be limited by logicalrefquota
#	3. Verify sub-filesystem will only be limited by logicalquota
#

verify_runnable "both"

function cleanup
{
	log_must zfs destroy -rf $TESTPOOL/$TESTFS
	log_must zfs create $TESTPOOL/$TESTFS
	log_must zfs set mountpoint=$TESTDIR $TESTPOOL/$TESTFS
}

log_assert "Sub-filesystem quotas are not enforced by property 'logicalrefquota'"
log_onexit cleanup

TESTFILE='testfile'
fs=$TESTPOOL/$TESTFS
log_must zfs set logicalquota=25M $fs
log_must zfs set logicalrefquota=10M $fs
log_must zfs create $fs/subfs

mntpnt=$(get_prop mountpoint $fs/subfs)
log_must mkfile 20M $mntpnt/$TESTFILE

typeset -i logicalused logicalquota logicalrefquota
logicalused=$(get_prop logicalused $fs)
logicalrefquota=$(get_prop logicalrefquota $fs)
((logicalused = logicalused / (1024 * 1024)))
((logicalrefquota = logicalrefquota / (1024 * 1024)))
if [[ $logicalused -lt $logicalrefquota ]]; then
	log_fail "ERROR: $logicalused < $logicalrefquota subfs quotas are limited by logicalrefquota"
fi

log_mustnot mkfile 20M $mntpnt/$TESTFILE.2
logicalused=$(get_prop logicalused $fs)
logicalquota=$(get_prop logicalquota $fs)
((logicalused = logicalused / (1024 * 1024)))
((logicalquota = logicalquota / (1024 * 1024)))
if [[ $logicalused -gt $logicalquota ]]; then
	log_fail "ERROR: $logicalused > $logicalquota subfs logicalquotas aren't limited by logicalquota"
fi

log_pass "Sub-filesystem logicalquotas are not enforced by property 'logicalrefquota'"
