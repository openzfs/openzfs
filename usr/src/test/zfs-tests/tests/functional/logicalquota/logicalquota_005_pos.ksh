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
# Verify that logicalquota doesn't inherit its value from parent.
#
# STRATEGY:
# 1) Set logicalquota for parents
# 2) Create a filesystem tree
# 3) Verify that the 'logicalquota' for descendent doesnot inherit the value.
#
###############################################################################

verify_runnable "both"

function cleanup
{
	datasetexists $fs_child && \
		log_must zfs destroy $fs_child

	log_must zfs set logicalquota=$logicalquota_val $fs
}

log_onexit cleanup

log_assert "Verify that logicalquota doesnot inherit its value from parent."
log_onexit cleanup

fs=$TESTPOOL/$TESTFS
fs_child=$TESTPOOL/$TESTFS/$TESTFS

space_avail=$(get_prop available $fs)
logicalquota_val=$(get_prop logicalquota $fs)
typeset -i logicalquotasize=$space_avail
((logicalquotasize = logicalquotasize * 2 ))
log_must zfs set logicalquota=$logicalquotasize $fs

log_must zfs create $fs_child
logicalquota_space=$(get_prop logicalquota $fs_child)
[[ $logicalquota_space == $logicalquotasize ]] && \
	log_fail "The logicalquota of child dataset inherits its value from parent."

log_pass "logicalquota doesnot inherit its value from parent as expected."
