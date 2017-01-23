#! /bin/ksh -p
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
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2013, 2016 by Delphix. All rights reserved.
# Copyright (c) 2016 OVH [ovh.com].
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/logicalquota/logicalquota.kshlib

#
# DESCRIPTION:
#
# Can't set a quota to less than currently being logicaly used by the dataset.
#
# STRATEGY:
# 1) Apply logicalquota to the ZFS file system
# 2) Create a filesystem
# 3) Set a logicalquota on the filesystem that is lower than the space
#	currently in use.
# 4) Verify that the attempt fails.
#

verify_runnable "both"

log_assert "Verify cannot set logicalquota lower than the space currently in use"

function cleanup
{
	log_must zfs set logicalquota=none $TESTPOOL/$TESTFS
}

log_onexit cleanup

log_must zfs set compression=on $TESTPOOL/$TESTFS
typeset -i logicalquota_integer_size=0
typeset invalid_size="123! @456 7#89 0\$ abc123% 123%s 12%s3 %c123 123%d %x123 12%p3 \
	^def456 789&ghi"
typeset -i logicalspace_used=`get_prop logicalused $TESTPOOL/$TESTFS`
(( logicalquota_integer_size = logicalspace_used  - 1 ))
logicalquota_fp_size=${logicalquota_integer_size}.123

for size in 0 -1 $logicalquota_integer_size -$logicalquota_integer_size $logicalquota_fp_size -$logicalquota_fp_size \
	$invalid_size ; do
	log_mustnot zfs set logicalquota=$size $TESTPOOL/$TESTFS
done
log_must zfs set logicalquota=$logicalspace_used $TESTPOOL/$TESTFS

log_pass "As expected cannot set logicalquota lower than logicalspace currently in use"
