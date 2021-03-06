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
 *            Ericsson AB
 *
 */

#include <logtrace.h>
#include <util.h>
#include <cluster.h>
#include <app.h>
#include <imm.h>
#include <sutype.h>
#include <ckpt_msg.h>
#include <ntf.h>
#include <sgtype.h>
#include <proc.h>

AmfDb<std::string, AVD_AMF_SG_TYPE> *sgtype_db = NULL;

void avd_sgtype_add_sg(AVD_SG *sg)
{
	sg->sg_list_sg_type_next = sg->sg_type->list_of_sg;
	sg->sg_type->list_of_sg = sg;
}

void avd_sgtype_remove_sg(AVD_SG *sg)
{
	AVD_SG *i_sg = NULL;
	AVD_SG *prev_sg = NULL;

	if (sg->sg_type != NULL) {
		i_sg = sg->sg_type->list_of_sg;

		while ((i_sg != NULL) && (i_sg != sg)) {
			prev_sg = i_sg;
			i_sg = i_sg->sg_list_sg_type_next;
		}

		if (i_sg != sg) {
			/* Log a fatal error */
			osafassert(0);
		} else {
			if (prev_sg == NULL) {
				sg->sg_type->list_of_sg = sg->sg_list_sg_type_next;
			} else {
				prev_sg->sg_list_sg_type_next = sg->sg_list_sg_type_next;
			}
		}

		sg->sg_list_sg_type_next = NULL;
		sg->sg_type = NULL;
	}
}

static void sgtype_delete(AVD_AMF_SG_TYPE *sg_type)
{
	sgtype_db->erase(Amf::to_string(&sg_type->name));
	delete [] sg_type->saAmfSGtValidSuTypes;
	delete sg_type;
}

/**
 * Add the SG type to the DB
 * @param sgt
 */
static void sgtype_add_to_model(AVD_AMF_SG_TYPE *sgt)
{
	unsigned int rc = sgtype_db->insert(Amf::to_string(&sgt->name),sgt);
	osafassert(rc == NCSCC_RC_SUCCESS);
}

/**
 * Validate configuration attributes for an SaAmfSGType object
 * 
 * @param dn
 * @param sgt
 * @param opdata
 * 
 * @return int
 */
static int is_config_valid(const SaNameT *dn, const SaImmAttrValuesT_2 **attributes,
	const CcbUtilOperationData_t *opdata)
{
	SaAisErrorT rc;
	int i = 0;
	unsigned j = 0;
	char *parent;
	SaUint32T value;
	SaBoolT abool;
	struct avd_sutype *sut;
	const SaImmAttrValuesT_2 *attr;

	if ((parent = strchr((char*)dn->value, ',')) == NULL) {
		report_ccb_validation_error(opdata, "No parent to '%s' ", dn->value);
		return 0;
	}

	/* SGType should be children to SGBaseType */
	if (strncmp(++parent, "safSgType=", 10) != 0) {
		report_ccb_validation_error(opdata, "Wrong parent '%s' to '%s' ", parent, dn->value);
		return 0;
	}

	rc = immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtRedundancyModel"), attributes, 0, &value);
	osafassert(rc == SA_AIS_OK);

	if ((value < SA_AMF_2N_REDUNDANCY_MODEL) || (value > SA_AMF_NO_REDUNDANCY_MODEL)) {
		report_ccb_validation_error(opdata, "Invalid saAmfSgtRedundancyModel %u for '%s'",
				value, dn->value);
		return 0;
	}

	while ((attr = attributes[i++]) != NULL)
		if (!strcmp(attr->attrName, "saAmfSgtValidSuTypes"))
			break;

	osafassert(attr);
	osafassert(attr->attrValuesNumber > 0);

	for (j = 0; j < attr->attrValuesNumber; j++) {
		SaNameT *name = (SaNameT *)attr->attrValues[j];
		sut = sutype_db->find(Amf::to_string(name));
		if (sut == NULL) {
			if (opdata == NULL) {
				report_ccb_validation_error(opdata, "'%s' does not exist in model", name->value);
				return 0;
			}
			
			/* SG type does not exist in current model, check CCB */
			if (ccbutil_getCcbOpDataByDN(opdata->ccbId, name) == NULL) {
				report_ccb_validation_error(opdata, "'%s' does not exist either in model or CCB",
						name->value);
				return 0;
			}
		}
	}

	if (j == 0) {
		report_ccb_validation_error(opdata, "No SU types configured for '%s'", dn->value);
		return 0;
	}

	if ((immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtDefAutoRepair"), attributes, 0, &abool) == SA_AIS_OK) &&
	    (abool > SA_TRUE)) {
		report_ccb_validation_error(opdata, "Invalid saAmfSgtDefAutoRepair %u for '%s'", abool, dn->value);
		return 0;
	}

	if ((immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtDefAutoAdjust"), attributes, 0, &abool) == SA_AIS_OK) &&
	    (abool > SA_TRUE)) {
		report_ccb_validation_error(opdata, "Invalid saAmfSgtDefAutoAdjust %u for '%s'", abool, dn->value);
		return 0;
	}

	return 1;
}

/**
 * Create a new SG type object and initialize its attributes from the attribute list.
 * @param dn
 * @param attributes
 * 
 * @return AVD_AMF_SG_TYPE*
 */
static AVD_AMF_SG_TYPE *sgtype_create(SaNameT *dn, const SaImmAttrValuesT_2 **attributes)
{
	int i = 0, rc = -1;
	unsigned j = 0;
	AVD_AMF_SG_TYPE *sgt;
	const SaImmAttrValuesT_2 *attr;
	SaAisErrorT error;

	TRACE_ENTER2("'%s'", dn->value);

	sgt = new AVD_AMF_SG_TYPE();

	memcpy(sgt->name.value, dn->value, dn->length);
	sgt->name.length = dn->length;

	error = immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtRedundancyModel"), attributes, 0, &sgt->saAmfSgtRedundancyModel);
	osafassert(error == SA_AIS_OK);

	while ((attr = attributes[i++]) != NULL)
		if (!strcmp(attr->attrName, "saAmfSgtValidSuTypes"))
			break;

	osafassert(attr);
	osafassert(attr->attrValuesNumber > 0);

	sgt->number_su_type = attr->attrValuesNumber;
	sgt->saAmfSGtValidSuTypes = new SaNameT[attr->attrValuesNumber];
	for (j = 0; j < attr->attrValuesNumber; j++)
		sgt->saAmfSGtValidSuTypes[j] = *((SaNameT *)attr->attrValues[j]);

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtDefAutoRepair"), attributes, 0, &sgt->saAmfSgtDefAutoRepair) != SA_AIS_OK) {
		sgt->saAmfSgtDefAutoRepair = SA_TRUE;
		sgt->saAmfSgtDefAutoRepair_configured = false;
	}
	else
		sgt->saAmfSgtDefAutoRepair_configured = true;

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtDefAutoAdjust"), attributes, 0, &sgt->saAmfSgtDefAutoAdjust) != SA_AIS_OK) {
		sgt->saAmfSgtDefAutoAdjust = SA_FALSE;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtDefAutoAdjustProb"), attributes, 0, &sgt->saAmfSgtDefAutoAdjustProb) != SA_AIS_OK) {
		LOG_ER("Get saAmfSgtDefAutoAdjustProb FAILED for '%s'", dn->value);
		goto done;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtDefCompRestartProb"), attributes, 0, &sgt->saAmfSgtDefCompRestartProb) != SA_AIS_OK) {
		LOG_ER("Get saAmfSgtDefCompRestartProb FAILED for '%s'", dn->value);
		goto done;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtDefCompRestartMax"), attributes, 0, &sgt->saAmfSgtDefCompRestartMax) != SA_AIS_OK) {
		LOG_ER("Get saAmfSgtDefCompRestartMax FAILED for '%s'", dn->value);
		goto done;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtDefSuRestartProb"), attributes, 0, &sgt->saAmfSgtDefSuRestartProb) != SA_AIS_OK) {
		LOG_ER("Get saAmfSgtDefSuRestartProb FAILED for '%s'", dn->value);
		goto done;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSgtDefSuRestartMax"), attributes, 0, &sgt->saAmfSgtDefSuRestartMax) != SA_AIS_OK) {
		LOG_ER("Get saAmfSgtDefSuRestartMax FAILED for '%s'", dn->value);
		goto done;
	}

	rc = 0;

 done:
	if (rc != 0) {
		delete [] sgt->saAmfSGtValidSuTypes;
		delete sgt;
		sgt = NULL;
	}

	return sgt;
}

/**
 * Get configuration for all SaAmfSGType objects from IMM and create AVD internal objects.
 * @param cb
 * 
 * @return int
 */
SaAisErrorT avd_sgtype_config_get(void)
{
	SaAisErrorT error = SA_AIS_ERR_FAILED_OPERATION;
	AVD_AMF_SG_TYPE *sgt;
	SaImmSearchHandleT searchHandle;
	SaImmSearchParametersT_2 searchParam;
	SaNameT dn;
	const SaImmAttrValuesT_2 **attributes;
	const char *className = "SaAmfSGType";

	TRACE_ENTER();

	searchParam.searchOneAttr.attrName = const_cast<SaImmAttrNameT>("SaImmAttrClassName");
	searchParam.searchOneAttr.attrValueType = SA_IMM_ATTR_SASTRINGT;
	searchParam.searchOneAttr.attrValue = &className;

	error = immutil_saImmOmSearchInitialize_2(avd_cb->immOmHandle, NULL, SA_IMM_SUBTREE,
						  SA_IMM_SEARCH_ONE_ATTR | SA_IMM_SEARCH_GET_ALL_ATTR, &searchParam,
						  NULL, &searchHandle);

	if (SA_AIS_OK != error) {
		LOG_ER("saImmOmSearchInitialize_2 failed: %u", error);
		goto done1;
	}

	while (immutil_saImmOmSearchNext_2(searchHandle, &dn, (SaImmAttrValuesT_2 ***)&attributes) == SA_AIS_OK) {
		if (!is_config_valid(&dn, attributes, NULL))
			goto done2;
		if (( sgt = sgtype_db->find(Amf::to_string(&dn))) == NULL) {
			if ((sgt = sgtype_create(&dn, attributes)) == NULL)
				goto done2;

			sgtype_add_to_model(sgt);
		}
	}

	error = SA_AIS_OK;

 done2:
	(void)immutil_saImmOmSearchFinalize(searchHandle);
 done1:
	TRACE_LEAVE2("%u", error);
	return error;
}


/**
 * This routine validates modify CCB operations on SaAmfSGType objects.
 * @param   Ccb Util Oper Data
 * @return 
 */

static SaAisErrorT sgtype_ccb_completed_modify_hdlr(CcbUtilOperationData_t *opdata)
{
	SaAisErrorT rc = SA_AIS_OK;
	const SaImmAttrModificationT_2 *attr_mod;
	int i = 0;
	AVD_AMF_SG_TYPE *sgt = sgtype_db->find(Amf::to_string(&opdata->objectName));

	TRACE_ENTER2("CCB ID %llu, '%s'", opdata->ccbId, opdata->objectName.value);
	while ((attr_mod = opdata->param.modify.attrMods[i++]) != NULL) {
		
		if ((attr_mod->modType == SA_IMM_ATTR_VALUES_DELETE) || (attr_mod->modAttr.attrValues == NULL))
			continue;

		if (!strcmp(attr_mod->modAttr.attrName, "saAmfSgtDefAutoRepair")) {
			uint32_t sgt_autorepair = *((SaUint32T *)attr_mod->modAttr.attrValues[0]);
			if (sgt_autorepair > SA_TRUE) {
				report_ccb_validation_error(opdata, "invalid saAmfSgtDefAutoRepair in:'%s'",
						sgt->name.value);
				rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
			}	
		} else {
			report_ccb_validation_error(opdata, "Modification of attribute is Not Supported:'%s' ",
					attr_mod->modAttr.attrName);
			rc = SA_AIS_ERR_BAD_OPERATION;
			goto done;
		}
	}

done:
	return rc;
}

/**
 * Validate SaAmfSGType CCB completed
 * @param opdata
 * 
 * @return SaAisErrorT
 */
static SaAisErrorT sgtype_ccb_completed_cb(CcbUtilOperationData_t *opdata)
{
	SaAisErrorT rc = SA_AIS_ERR_BAD_OPERATION;
	AVD_AMF_SG_TYPE *sgt;
	AVD_SG *sg; 
	bool sg_exist = false;
	CcbUtilOperationData_t *t_opData;

	TRACE_ENTER2("CCB ID %llu, '%s'", opdata->ccbId, opdata->objectName.value);

	switch (opdata->operationType) {
	case CCBUTIL_CREATE:
		if (is_config_valid(&opdata->objectName, opdata->param.create.attrValues, opdata))
			rc = SA_AIS_OK;
		break;
	case CCBUTIL_MODIFY:
		rc = sgtype_ccb_completed_modify_hdlr(opdata);
		break;
	case CCBUTIL_DELETE:
		sgt = sgtype_db->find(Amf::to_string(&opdata->objectName));
		if (sgt->list_of_sg != NULL) {
			/* check whether there exists a delete operation for 
			 * each of the SG in the sg_type list in the current CCB 
			 */                      
			sg = sgt->list_of_sg;
			while (sg != NULL) {  
				t_opData = ccbutil_getCcbOpDataByDN(opdata->ccbId, &sg->name);
				if ((t_opData == NULL) || (t_opData->operationType != CCBUTIL_DELETE)) {
					sg_exist = true;   
					break;                  
				}                       
				sg = sg->sg_list_sg_type_next;
			}                       
			if (sg_exist == true) {
				report_ccb_validation_error(opdata, "SGs exist of this SG type '%s'",sgt->name.value);
				goto done;
			}                       
		}
		opdata->userData = sgt;	/* Save for later use in apply */
		rc = SA_AIS_OK;
		break;
	default:
		osafassert(0);
		break;
	}

done:
	TRACE_LEAVE2("%u", rc);
	return rc;
}

/**
 * This function handles modify operations on SaAmfSGType objects. 
 * @param ptr to opdata 
 */
static void sgtype_ccb_apply_modify_hdlr(struct CcbUtilOperationData *opdata)
{
	const SaImmAttrModificationT_2 *attr_mod;
	int i = 0;

	TRACE_ENTER2("CCB ID %llu, '%s'", opdata->ccbId, opdata->objectName.value);

	AVD_AMF_SG_TYPE *sgt = sgtype_db->find(Amf::to_string(&opdata->objectName));

	while ((attr_mod = opdata->param.modify.attrMods[i++]) != NULL) {
		bool value_is_deleted; 
		if ((attr_mod->modType == SA_IMM_ATTR_VALUES_DELETE) ||	(attr_mod->modAttr.attrValues == NULL))
			/* Attribute value is deleted, revert to default value */
			value_is_deleted = true;
		else 
			/* Attribute value is modified */
			value_is_deleted = false;
		
		if (!strcmp(attr_mod->modAttr.attrName, "saAmfSgtDefAutoRepair")) {
			SaBoolT old_value = sgt->saAmfSgtDefAutoRepair;
			if (value_is_deleted) {
				sgt->saAmfSgtDefAutoRepair = static_cast<SaBoolT>(true);
				sgt->saAmfSgtDefAutoRepair_configured = false;
			} else {
				sgt->saAmfSgtDefAutoRepair = static_cast<SaBoolT>(*((SaUint32T *)attr_mod->modAttr.attrValues[0]));
				sgt->saAmfSgtDefAutoRepair_configured = true; 
			}
			TRACE("Modified saAmfSgtDefAutoRepair is '%u'", sgt->saAmfSgtDefAutoRepair);
			amflog(LOG_NOTICE, "%s saAmfSgtDefAutoRepair changed to %u",
				sgt->name.value, sgt->saAmfSgtDefAutoRepair);

			/* Modify saAmfSGAutoRepair for SGs which had inherited saAmfSgtDefAutoRepair.*/
			if (old_value != sgt->saAmfSgtDefAutoRepair) {
				for (AVD_SG *sg = sgt->list_of_sg; sg; sg = sg->sg_list_sg_type_next) {  
					if (!sg->saAmfSGAutoRepair_configured) {
						sg->saAmfSGAutoRepair = static_cast<SaBoolT>(sgt->saAmfSgtDefAutoRepair);
						TRACE("Modified saAmfSGAutoRepair is '%u'", sg->saAmfSGAutoRepair);
						amflog(LOG_NOTICE, "%s inherited saAmfSGAutoRepair value changed to %u",
							sg->name.value, sg->saAmfSGAutoRepair);
					}
				}
			}
		} 
	}

	TRACE_LEAVE();
}


/**
 * Apply SaAmfSGType CCB changes
 * @param opdata
 * 
 * @return SaAisErrorT
 */
static void sgtype_ccb_apply_cb(CcbUtilOperationData_t *opdata)
{
	AVD_AMF_SG_TYPE *sgt;

	TRACE_ENTER2("CCB ID %llu, '%s'", opdata->ccbId, opdata->objectName.value);

	switch (opdata->operationType) {
	case CCBUTIL_CREATE:
		sgt = sgtype_create(&opdata->objectName, opdata->param.create.attrValues);
		osafassert(sgt);
		sgtype_add_to_model(sgt);
		break;
	case CCBUTIL_MODIFY:
		sgtype_ccb_apply_modify_hdlr(opdata);
		break;
	case CCBUTIL_DELETE:
		sgtype_delete(static_cast<AVD_AMF_SG_TYPE*>(opdata->userData));
		break;
	default:
		osafassert(0);
	}

	TRACE_LEAVE();
}

void avd_sgtype_constructor(void)
{

	sgtype_db = new AmfDb<std::string, AVD_AMF_SG_TYPE>;
	avd_class_impl_set("SaAmfSGType", NULL, NULL, sgtype_ccb_completed_cb,
		sgtype_ccb_apply_cb);
	avd_class_impl_set("SaAmfSGBaseType", NULL, NULL,
		avd_imm_default_OK_completed_cb, NULL);
}
