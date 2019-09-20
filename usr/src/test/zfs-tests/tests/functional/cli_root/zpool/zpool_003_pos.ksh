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
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2012, 2016 by Delphix. All rights reserved.
# Copyright 2019 RackTop Systems.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
#	Verify debugging features of zpool such as ABORT and freeze/unfreeze
#	should run successfully.
#
# STRATEGY:
# 1. Create an array containg each zpool options.
# 2. For each element, execute the zpool command.
# 3. Verify it run successfully.
#

verify_runnable "both"

function cleanup
{
	if [[ -d $corepath ]]; then
		rm -rf $corepath
	fi
}

log_assert "Debugging features of zpool should succeed."
log_onexit cleanup

log_must zpool -? > /dev/null 2>&1

if is_global_zone ; then
	log_must zpool freeze $TESTPOOL
else
	log_mustnot zpool freeze $TESTPOOL
	log_mustnot zpool freeze ${TESTPOOL%%/*}
fi

log_mustnot zpool freeze fakepool

#preparation work for testing
corepath=$TESTDIR/cores
if [[ -d $corepath ]]; then
	rm -rf $corepath
fi
mkdir $corepath

coreadm -p ${corepath}/core.%f
ZFS_ABORT=1; export ZFS_ABORT
zpool > /dev/null 2>&1
unset ZFS_ABORT

corefile=${corepath}/core.zpool
[[ -f $corefile ]] || log_fail "zpool did not dump core by request."

log_pass "Debugging features of zpool succeed."
