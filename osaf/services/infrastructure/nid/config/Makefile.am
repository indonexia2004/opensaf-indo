#      -*- OpenSAF  -*-
#
# (C) Copyright 2009 The OpenSAF Foundation
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
# Author(s): Wind River Systems
#

include $(top_srcdir)/Makefile.common

MAINTAINERCLEANFILES = Makefile.in

dist_pkgsysconf_DATA = \
	$(top_srcdir)/osaf/services/infrastructure/nid/config/nodeinit.conf.controller \
	$(top_srcdir)/osaf/services/infrastructure/nid/config/nodeinit.conf.payload \
	$(top_srcdir)/osaf/services/infrastructure/nid/config/nid.conf

if ENABLE_SYSTEMD
dist_systemd_DATA = \
        $(top_builddir)/osaf/services/infrastructure/nid/config/opensafd.service
endif

set-default-node-config:
	@echo "*** Setting default controller node configuration ***"
	echo "2" > $(DESTDIR)$(pkgsysconfdir)/chassis_id
	echo "1" > $(DESTDIR)$(pkgsysconfdir)/slot_id
	echo "15" > $(DESTDIR)$(pkgsysconfdir)/subslot_id
	echo "controller" > $(DESTDIR)$(pkgsysconfdir)/node_type

install-data-hook: set-default-node-config
if !ENABLE_AIS_PLM
	sed -i '/plmd/d'  $(DESTDIR)$(pkgsysconfdir)/nodeinit.conf.controller
endif

install-data-hook:
if ENABLE_SYSTEMD
	if [ ${systemdmodversion} -gt 204 ]; then \
		sed -i '/ControlGroup/d'  $(DESTDIR)$(systemddir)/opensafd.service; \
	fi
endif
