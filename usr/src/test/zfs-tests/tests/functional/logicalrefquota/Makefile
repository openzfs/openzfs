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
# Copyright (c) 2013 by Delphix. All rights reserved.
#

include $(SRC)/Makefile.master

ROOTOPTPKG = $(ROOT)/opt/zfs-tests
TESTDIR = $(ROOTOPTPKG)/tests/functional/logicalrefquota

PROGS = cleanup \
	logicalrefquota_001_pos \
	logicalrefquota_002_pos \
	logicalrefquota_003_pos \
	logicalrefquota_004_pos \
	logicalrefquota_005_pos \
	logicalrefquota_006_neg \
	logicalrefquota_007_pos \
	logicalrefquota_008_pos \
	logicalrefquota_009_pos \
	logicalrefquota_010_pos \
	logicalrefquota_011_pos \
	setup

FILES = logicalrefquota.cfg \
    logicalrefquota.kshlib

CMDS = $(PROGS:%=$(TESTDIR)/%) $(FILES:%=$(TESTDIR)/%)
    $(CMDS) := FILEMODE = 0555

all lint clean clobber:

install: $(CMDS)

$(CMDS): $(TESTDIR)

$(TESTDIR):
	$(INS.dir)

$(TESTDIR)/%: %.ksh
	$(INS.rename)

$(TESTDIR)/%: %
	$(INS.file)
