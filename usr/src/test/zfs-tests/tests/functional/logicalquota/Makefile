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
TESTDIR = $(ROOTOPTPKG)/tests/functional/logicalquota

PROGS = cleanup \
	logicalquota_001_pos \
	logicalquota_002_pos \
	logicalquota_003_pos \
	logicalquota_004_pos \
	logicalquota_005_pos \
	logicalquota_006_neg \
	logicalquota_007_pos \
	logicalquota_008_pos \
	logicalquota_009_pos \
	logicalquota_010_pos \
	logicalquota_011_neg \
	setup

FILES = logicalquota.cfg \
	logicalquota.kshlib

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
