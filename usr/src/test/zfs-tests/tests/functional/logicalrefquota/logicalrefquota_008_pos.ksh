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
#	Quotas are enforced using the minimum of the two properties:
#	quota & logicalrefquota
#
# STRATEGY:
#	1. Set value for logicalquota and logicalrefquota. Quota less than refquota.
#	2. Creating file which should be limited by quota.
#	3. Switch the value of quota and logicalrefquota.
#	4. Verify file should be limited by logicalrefquota.
#

verify_runnable "both"

function cleanup
{
	log_must zfs destroy -rf $TESTPOOL/$TESTFS
	log_must zfs create $TESTPOOL/$TESTFS
	log_must zfs set mountpoint=$TESTDIR $TESTPOOL/$TESTFS
}

log_assert "Logical quotas are enforced using the minimum of the two properties"
log_onexit cleanup

fs=$TESTPOOL/$TESTFS
log_must zfs set compression=on $fs
log_must zfs set logicalquota=15M $fs
log_must zfs set logicalrefquota=25M $fs

mntpnt=$(get_prop mountpoint $fs)
 
# fill a file of the size 20M
log_mustnot fill_file $mntpnt/$TESTFILE1 20000000

typeset -i logicalused logicalquota
logicalused=$(get_prop logicalused $fs)
logicalquota=$(get_prop logicalquota $fs)
((logicalused = logicalused / (1024 * 1024)))
((logicalquota = logicalquota / (1024 * 1024)))
if [[ $logicalused -ne $logicalquota ]]; then
	log_fail "ERROR: $logicalused -ne $logicalquota Logical quotas are not limited by logicalquota"
fi

#
# Switch the value of them and try again
#
log_must rm $mntpnt/$TESTFILE1
log_must zfs set logicalquota=25M $fs
log_must zfs set logicalrefquota=15M $fs

# fill a file of the size 20M
log_mustnot fill_file $mntpnt/$TESTFILE1 20000000

logicalused=$(get_prop logicalused $fs)
logicalrefquota=$(get_prop logicalrefquota $fs)
((logicalused = logicalused / (1024 * 1024)))
((logicalrefquota = logicalrefquota / (1024 * 1024)))
if [[ $logicalused -ne $logicalrefquota ]]; then
	log_fail "ERROR: $logicalused -ne $logicalrefquota Quotas are not limited by refquota"
fi

log_pass "Quotas are enforced using the minimum of the two properties"
