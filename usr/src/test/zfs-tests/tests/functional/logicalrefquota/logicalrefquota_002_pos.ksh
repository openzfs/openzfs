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

#
# DESCRIPTION:
#	Quotas are enforced using the minimum of the two properties:
#	quota & logicalrefquota
#
# STRATEGY:
#	1. Set value for quota and logicalrefquota. Quota less than refquota.
#	2. Creating file which should be limited by quota.
#	3. Switch the value of quota and logicalrefquota.
#	4. Verify file should be limited by logicalrefquota.
#

verify_runnable "both"

function cleanup
{
	log_must $ZFS destroy -rf $TESTPOOL/$TESTFS
	log_must $ZFS create $TESTPOOL/$TESTFS
	log_must $ZFS set mountpoint=$TESTDIR $TESTPOOL/$TESTFS
}

log_assert "Logical quotas are enforced using the minimum of the two properties"
log_onexit cleanup

TESTFILE='testfile'
fs=$TESTPOOL/$TESTFS
log_must $ZFS set logicalquota=15M $fs
log_must $ZFS set logicalrefquota=25M $fs

mntpnt=$(get_prop mountpoint $fs)
log_mustnot $MKFILE 20M $mntpnt/$TESTFILE
typeset -i logicalused quota
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
log_must $RM $mntpnt/$TESTFILE
log_must $ZFS set logicalquota=25M $fs
log_must $ZFS set logicalrefquota=15M $fs

log_mustnot $MKFILE 20M $mntpnt/$TESTFILE
logicalused=$(get_prop logicalused $fs)
logicalrefquota=$(get_prop logicalrefquota $fs)
((logicalused = logicalused / (1024 * 1024)))
((logicalrefquota = logicalrefquota / (1024 * 1024)))
if [[ $logicalused -ne $logicalrefquota ]]; then
	log_fail "ERROR: $logicalused -ne $logicalrefquota Quotas are not limited by refquota"
fi

log_pass "Quotas are enforced using the minimum of the two properties"
