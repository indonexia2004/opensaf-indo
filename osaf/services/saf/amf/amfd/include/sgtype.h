/*      -*- OpenSAF  -*-            
 *
 * (C) Copyright 2008-2014 The OpenSAF Foundation
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
 * Author(s): Emerson Network Power, Ericsson
 *
 */

/*****************************************************************************

  DESCRIPTION: Service Group Type class
  
******************************************************************************
*/

#ifndef AVD_SGTYPE_H
#define AVD_SGTYPE_H

#include <saAmf.h>
#include <include/db_template.h>

class AVD_SG;

typedef struct avd_amf_sg_type_tag {
	SaNameT name;
	bool saAmfSgtDefAutoRepair_configured; /* True when user configures saAmfSGDefAutoRepair else false */
   /******************** B.04 model *************************************************/
	SaNameT *saAmfSGtValidSuTypes;	/* array of DNs, size in number_su_type */
	SaAmfRedundancyModelT saAmfSgtRedundancyModel;
	SaBoolT saAmfSgtDefAutoRepair;
	SaBoolT saAmfSgtDefAutoAdjust;
	SaTimeT saAmfSgtDefAutoAdjustProb;
	SaTimeT saAmfSgtDefCompRestartProb;
	SaUint32T saAmfSgtDefCompRestartMax;
	SaTimeT saAmfSgtDefSuRestartProb;
	SaUint32T saAmfSgtDefSuRestartMax;
   /******************** B.04 model *************************************************/

	uint32_t number_su_type;	/* size of array saAmfSGtValidSuTypes */
	AVD_SG *list_of_sg;

} AVD_AMF_SG_TYPE;

extern AmfDb<std::string, AVD_AMF_SG_TYPE> *sgtype_db;
SaAisErrorT avd_sgtype_config_get(void);
AVD_AMF_SG_TYPE *avd_sgtype_get(const SaNameT *dn);
void avd_sgtype_add_sg(AVD_SG *sg);
void avd_sgtype_remove_sg(AVD_SG *sg);
void avd_sgtype_constructor(void);

#endif
