#!/bin/ksh -p
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
# Copyright (c) 2016 by Delphix. All rights reserved.
#

verify_runnable "global"

. $STF_SUITE/tests/functional/channel_program/channel_common.kshlib

#
# DESCRIPTION:
#       Passing timeout options to channel programs should work correctly.
#       Programs that exceed these timeouts should fail gracefully.
#

verify_runnable "both"

log_assert "Timeouts work correctly."

log_assert "non-terminating program fails (with default timeout)"
log_mustnot_checkerror_program "timed out" \
    $TESTPOOL $ZCP_ROOT/lua_core/tst.timeout.zcp

function test_timeout
{
	typeset to=$1
	elapsed=$(dtrace -q \
	    -n "zcp_eval_sync:entry{self->begin = timestamp;}" \
	    -n "zcp_eval_sync:return/self->begin/{trace((timestamp - self->begin)/1000/1000); self->begin = 0;}" \
	    -c "zfs program -t $to $TESTPOOL $ZCP_ROOT/lua_core/tst.timeout.zcp")
	if [[ $elapsed -lt $to ]]; then
		log_fail "Execution time (${elapsed}ms) less than timeout (${to}ms)"
	elif [[ $elapsed -gt $(( $to + 1 )) ]]; then
		log_fail "Execution time (${elapsed}ms) more than limit (${to}ms + 1ms)"
	fi
}

log_assert "timeout options work"
test_timeout 1
test_timeout 10
test_timeout 100
test_timeout 1000
test_timeout 2000

log_pass "Timeouts work correctly."
