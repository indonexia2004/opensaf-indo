#      -*- OpenSAF  -*-
#
# (C) Copyright 2008 The OpenSAF Foundation
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. This file and program are licensed
# under the GNU Lesser General Public License Version 2.1, February 1999.
# The complete license can be accessed from the following location:
# http://opensource.org/licenses/lgpl-license.php
# See the Copying file included with the OpenSAF distribution for full
# licensing terms.
#
# Author(s): Ericsson AB
#

include $(top_srcdir)/Makefile.common

MAINTAINERCLEANFILES = Makefile.in

bin_PROGRAMS = immcfg

immcfg_CXXFLAGS = \
	$(AM_CXXFLAGS) \
	-I$(top_srcdir)/osaf/tools/safimm/include \
	-I$(top_srcdir)/osaf/libs/common/immsv/include \
	@XML2_CFLAGS@

immcfg_CPPFLAGS = \
	-DSA_EXTENDED_NAME_SOURCE \
	$(AM_CPPFLAGS) \
	-I$(top_srcdir)/osaf/tools/safimm/include \
	-I$(top_srcdir)/osaf/libs/common/immsv/include \
	@XML2_CFLAGS@

immcfg_SOURCES = \
	imm_import.cc \
	imm_cfg.c

immcfg_LDFLAGS = \
	@XML2_LIBS@ \
	-ldl

immcfg_LDADD = \
	$(top_builddir)/osaf/tools/safimm/src/libimmutil.la \
	$(top_builddir)/osaf/libs/core/libopensaf_core.la \
	$(top_builddir)/osaf/libs/saf/libSaImm/libSaImmOm.la \
	$(top_builddir)/osaf/libs/saf/libSaImm/libSaImmOi.la
