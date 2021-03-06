/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2008 The OpenSAF Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. This file and program are licensed
 * under the GNU Lesser General Public License Version 2.1, February 1999.
 * The complete license can be accessed from the following location:
 * http://opensource.org/licenses/lgpl-license.php
 * See the Copying file included with the OpenSAF distribution for full
 * licensing terms.
 *
 * Author(s): Emerson Network Power
 *
 */

/****************************************************************************
 * POSIX Message-queues Primitive definition
 * The actual function ncs_os_posix_mq must be resolved in the os_defs.h file.
 *
 * Macro arguments
 *  'req'        is an NCS_OS_POSIX_MQ_REQ_INFO structure.
 *
 * Macro return codes
 * The ncs_os_posix_mq implemention must return one of the following codes:
 *   NCSCC_RC_SUCCESS - interface call successful (normal return code)
 *   NCSCC_RC_FAILURE - interface call failed.
 *
 ***************************************************************************/
