#      -*- OpenSAF  -*-
#
# (C) Copyright 2010 The OpenSAF Foundation
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
# Author(s): Emerson 
#

include $(top_srcdir)/Makefile.common

MAINTAINERCLEANFILES = Makefile.in

SUBDIRS = plmc include

noinst_LTLIBRARIES = libplmsv_common.la

libplmsv_common_la_CPPFLAGS = \
	-DNCS_PLMSV_LOG=1 \
	$(AM_CPPFLAGS) \
	-I$(top_srcdir)/osaf/libs/common/plmsv/include \
	-I$(top_srcdir)/osaf/libs/common/plmsv/plmc/include \
	-I$(top_srcdir)/osaf/libs/common/immsv/include \
	-I$(top_srcdir)/contrib/plmc/include

libplmsv_common_la_CFLAGS = $(AM_CFLAGS) @HPI_CFLAGS@

libplmsv_common_la_SOURCES = \
	plms_edu.c \
	plms_common_mds.c \
	plms_common_utils.c
