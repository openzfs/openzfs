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
#	Try returning various values that lua allows you to construct,
#       but that cannot be represented as nvlists and therefore should
#       cause the script to fail (but not panic).
#

verify_runnable "both"

set -A args "function() return 1 end" \
	"{[{}]=true}" \
	"{[function() return 1 end]=0}" \
	"assert" \
	"0, assert" \
	"true, {[{}]=0}" \
	"{[0]=true, [\"0\"]=false}" \
	"{val=true}, {val=false}" \
	"{1, 2, 3}, {[4]=5}" \
	"nil, true, 1, \"test\", {}, {val=true}" \
	"{[false]=true, [\"false\"]=false}"

log_assert "Returning lua constructs that cannot be converted to " \
    "nvlists fails as expected."

typeset -i i=0
while (( i < ${#args[*]} )); do
	log_note "running program: ${args[i]}"
	log_mustnot_checkerror_program "execution failed" $TESTPOOL - <<-EOF
		return ${args[i]}
	EOF
	((i = i + 1))
done

typeset -i i=0
while (( i < ${#args[*]} )); do
	log_note "running program: ${args[i]}"
	log_mustnot_checkerror_program "execution failed" $TESTPOOL - <<-EOF
		error(${args[i]})
	EOF
	((i = i + 1))
done

log_pass "Returning lua constructs that cannot be converted to " \
    "nvlists fails as expected."
