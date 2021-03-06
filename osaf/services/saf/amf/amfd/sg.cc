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
#include <sg.h>
#include <proc.h>
#include <si_dep.h>
#include <csi.h>
#include <algorithm>

AmfDb<std::string, AVD_SG> *sg_db = NULL;

static void avd_verify_equal_ranked_su(AVD_SG *avd_sg);

void avd_sg_db_add(AVD_SG *sg)
{
	unsigned int rc;

	if (sg_db->find(Amf::to_string(&sg->name)) == NULL) {
		rc = sg_db->insert(Amf::to_string(&sg->name),sg);
		osafassert(rc == NCSCC_RC_SUCCESS);
	}
}


/**
 * Add the SG to the model
 * @param sg
 */
static void sg_add_to_model(AVD_SG *sg)
{
	SaNameT dn;

	TRACE_ENTER2("%s", sg->name.value);

	/* Check parent link to see if it has been added already */
	if (sg->app != NULL) {
		TRACE("already added");
		goto done;
	}

	avsv_sanamet_init(&sg->name, &dn, "safApp");
	sg->app = app_db->find(Amf::to_string(&dn));

	avd_sg_db_add(sg);
	sg->sg_type = sgtype_db->find(Amf::to_string(&sg->saAmfSGType));
	osafassert(sg->sg_type);
	avd_sgtype_add_sg(sg);
	sg->app->add_sg(sg);

	/* SGs belonging to a magic app will be NCS, TODO Better name! */
	if (!strcmp((char *)dn.value, "safApp=OpenSAF"))
		sg->sg_ncs_spec = true;

	m_AVSV_SEND_CKPT_UPDT_ASYNC_ADD(avd_cb, sg, AVSV_CKPT_AVD_SG_CONFIG);
done:
	TRACE_LEAVE();
}

/**
 * Remove the SG to the model
 * @param sg
 */
static void sg_remove_from_model(AVD_SG *sg)
{
	avd_sgtype_remove_sg(sg);
	sg->app->remove_sg(sg);
	sg_db->erase(Amf::to_string(&sg->name));

	m_AVSV_SEND_CKPT_UPDT_ASYNC_RMV(avd_cb, sg, AVSV_CKPT_AVD_SG_CONFIG);
}

AVD_SG::AVD_SG():
		saAmfSGAutoRepair_configured(false),
		saAmfSGAutoRepair(SA_FALSE),
		saAmfSGAutoAdjust(SA_FALSE),
		saAmfSGNumPrefActiveSUs(0),
		saAmfSGNumPrefStandbySUs(0),
		saAmfSGNumPrefInserviceSUs(~0),
		saAmfSGNumPrefAssignedSUs(0),
		saAmfSGMaxActiveSIsperSU(0),
		saAmfSGMaxStandbySIsperSU(0),
		saAmfSGAutoAdjustProb(0),
		saAmfSGCompRestartProb(0),
		saAmfSGCompRestartMax(0),
		saAmfSGSuRestartProb(0),
		saAmfSGSuRestartMax(0),
		saAmfSGAdminState(SA_AMF_ADMIN_UNLOCKED),
		saAmfSGNumCurrAssignedSUs(0),
		saAmfSGNumCurrInstantiatedSpareSUs(0),
		saAmfSGNumCurrNonInstantiatedSpareSUs(0),
		adjust_state(AVSV_SG_STABLE),
		sg_ncs_spec(false),
		sg_fsm_state(AVD_SG_FSM_STABLE),
		admin_si(NULL),
		sg_redundancy_model(SA_AMF_NO_REDUNDANCY_MODEL),
		list_of_su(NULL),
		list_of_si(NULL),
		sg_list_sg_type_next(NULL),
		sg_type(NULL),
		sg_list_app_next(NULL),
		app(NULL),
		equal_ranked_su(false),
		max_assigned_su(NULL),
		min_assigned_su(NULL),
		si_tobe_redistributed(NULL),
		try_inst_counter(0)
{
	adminOp = static_cast<SaAmfAdminOperationIdT>(0);
	memset(&name, 0, sizeof(SaNameT));
	memset(&saAmfSGType, 0, sizeof(SaNameT));
	memset(&saAmfSGSuHostNodeGroup, 0, sizeof(SaNameT));
	su_oper_list.su = NULL;
	su_oper_list.next = NULL;
	adminOp_invocationId = 0;
	ng_using_saAmfSGAdminState = false;
}

static AVD_SG *sg_new(const SaNameT *dn, SaAmfRedundancyModelT redundancy_model)
{
	AVD_SG *sg;

	if (redundancy_model == SA_AMF_2N_REDUNDANCY_MODEL)
		sg = new SG_2N();
	else if (redundancy_model == SA_AMF_NPM_REDUNDANCY_MODEL)
		sg = new SG_NPM();
	else if (redundancy_model == SA_AMF_N_WAY_REDUNDANCY_MODEL)
		sg = new SG_NWAY();
	else if (redundancy_model == SA_AMF_N_WAY_ACTIVE_REDUNDANCY_MODEL)
		sg = new SG_NACV();
	else if (redundancy_model == SA_AMF_NO_REDUNDANCY_MODEL)
		sg = new SG_NORED();
	else
		assert(0);

	memcpy(sg->name.value, dn->value, dn->length);
	sg->name.length = dn->length;

	return sg;
}

void avd_sg_delete(AVD_SG *sg)
{
	/* by now SU and SI should have been deleted */ 
	osafassert(sg->list_of_su == NULL);
	osafassert(sg->list_of_si == NULL);
	sg_remove_from_model(sg);
	delete sg;
}

void AVD_SG::add_si(AVD_SI* si)
{
	AVD_SI *i_si = AVD_SI_NULL;
	AVD_SI *prev_si = AVD_SI_NULL;

	i_si = this->list_of_si;

	while ((i_si != AVD_SI_NULL) && (i_si->saAmfSIRank < si->saAmfSIRank)) {
		prev_si = i_si;
		i_si = i_si->sg_list_of_si_next;
	}
	si->sg_of_si = this;
	if (prev_si == AVD_SI_NULL) {
		si->sg_list_of_si_next = si->sg_of_si->list_of_si;
		this->list_of_si = si;
	} else {
		prev_si->sg_list_of_si_next = si;
		si->sg_list_of_si_next = i_si;
	}
}

void AVD_SG::remove_si(AVD_SI* si)
{
	AVD_SI *i_si = NULL;
	AVD_SI *prev_si = NULL;

	if (list_of_si != NULL) {
		i_si = list_of_si;

		while ((i_si != NULL) && (i_si != si)) {
			prev_si = i_si;
			i_si = i_si->sg_list_of_si_next;
		}

		if (i_si == si) {
			if (prev_si == NULL) {
				list_of_si = si->sg_list_of_si_next;
			} else {
				prev_si->sg_list_of_si_next = si->sg_list_of_si_next;
			}

			si->sg_list_of_si_next = NULL;
			si->sg_of_si = NULL;
		}
	}
}

/**
 * Validate configuration attributes for an SaAmfSG object
 * 
 * @param dn
 * @param sg
 * 
 * @return int
 */
static int is_config_valid(const SaNameT *dn, const SaImmAttrValuesT_2 **attributes, CcbUtilOperationData_t *opdata)
{
	SaAisErrorT rc;
	SaNameT aname;
	SaBoolT abool;
	SaAmfAdminStateT admstate;
	char *parent;

	if ((parent = strchr((char*)dn->value, ',')) == NULL) {
		report_ccb_validation_error(opdata, "No parent to '%s' ", dn->value);
		return 0;
	}

	if (strncmp(++parent, "safApp=", 7) != 0) {
		report_ccb_validation_error(opdata, "Wrong parent '%s' to '%s' ", parent, dn->value);
		return 0;
	}

	rc = immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGType"), attributes, 0, &aname);
	osafassert(rc == SA_AIS_OK);

	if (sgtype_db->find(Amf::to_string(&aname)) == NULL) {
		if (opdata == NULL) {
			report_ccb_validation_error(opdata, "'%s' does not exist in model", aname.value);
			return 0;
		}

		/* SG type does not exist in current model, check CCB */
		if (ccbutil_getCcbOpDataByDN(opdata->ccbId, &aname) == NULL) {
			report_ccb_validation_error(opdata, "'%s' does not exist in existing model or in CCB", aname.value);
			return 0;
		}
	}

	if ((immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGAutoRepair"), attributes, 0, &abool) == SA_AIS_OK) &&
			(abool > SA_TRUE)) {
		report_ccb_validation_error(opdata, "Invalid saAmfSGAutoRepair %u for '%s'", abool, dn->value);
		return 0;
	}

	if ((immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGAutoAdjust"), attributes, 0, &abool) == SA_AIS_OK) &&
	    (abool > SA_TRUE)) {
		report_ccb_validation_error(opdata, "Invalid saAmfSGAutoAdjust %u for '%s'", abool, dn->value);
		return 0;
	}

	if ((immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGAdminState"), attributes, 0, &admstate) == SA_AIS_OK) &&
	    !avd_admin_state_is_valid(admstate)) {
		report_ccb_validation_error(opdata, "Invalid saAmfSGAdminState %u for '%s'", admstate, dn->value);
		return 0;
	}

	if ((immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGSuHostNodeGroup"), attributes, 0, &aname) == SA_AIS_OK) &&
	    (avd_ng_get(&aname) == NULL)) {
		report_ccb_validation_error(opdata, "Invalid saAmfSGSuHostNodeGroup '%s' for '%s'", aname.value, dn->value);
		return 0;
	}

	return 1;
}

static AVD_SG *sg_create(const SaNameT *sg_name, const SaImmAttrValuesT_2 **attributes)
{
	int rc = -1;
	AVD_SG *sg;
	AVD_AMF_SG_TYPE *sgt;
	SaAisErrorT error;

	TRACE_ENTER2("'%s'", sg_name->value);

	SaNameT sgtype_dn;
	error = immutil_getAttr("saAmfSGType", attributes, 0, &sgtype_dn);
	osafassert(error == SA_AIS_OK);
	sgt = sgtype_db->find(Amf::to_string(&sgtype_dn));
	osafassert(sgt);
	sg = sg_new(sg_name, sgt->saAmfSgtRedundancyModel);
	sg->saAmfSGType = sgtype_dn;
	sg->sg_type = sgt;

	(void)immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGSuHostNodeGroup"), attributes, 0, &sg->saAmfSGSuHostNodeGroup);

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGAutoRepair"), attributes, 0, &sg->saAmfSGAutoRepair) != SA_AIS_OK) {
		sg->saAmfSGAutoRepair = sgt->saAmfSgtDefAutoRepair;
		sg->saAmfSGAutoRepair_configured = false;
	}
	else 
		sg->saAmfSGAutoRepair_configured = true; 

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGAutoAdjust"), attributes, 0, &sg->saAmfSGAutoAdjust) != SA_AIS_OK) {
		sg->saAmfSGAutoAdjust = sgt->saAmfSgtDefAutoAdjust;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGNumPrefActiveSUs"), attributes, 0, &sg->saAmfSGNumPrefActiveSUs) != SA_AIS_OK) {
		sg->saAmfSGNumPrefActiveSUs = 1;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGNumPrefStandbySUs"), attributes, 0, &sg->saAmfSGNumPrefStandbySUs) != SA_AIS_OK) {
		sg->saAmfSGNumPrefStandbySUs = 1;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGNumPrefInserviceSUs"), attributes, 0, &sg->saAmfSGNumPrefInserviceSUs) != SA_AIS_OK) {
		/* empty => later assign number of SUs */
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGNumPrefAssignedSUs"), attributes, 0, &sg->saAmfSGNumPrefAssignedSUs) != SA_AIS_OK) {
		/* empty => later assign to saAmfSGNumPrefInserviceSUs */
	}

	sg->saAmfSGMaxActiveSIsperSU = -1; // magic number for no limit

	/* saAmfSGMaxActiveSIsperSU: "This attribute is only applicable to N+M, N-way, and N-way active redundancy models." */
	if ((sgt->saAmfSgtRedundancyModel == SA_AMF_NPM_REDUNDANCY_MODEL) ||
	    (sgt->saAmfSgtRedundancyModel == SA_AMF_N_WAY_REDUNDANCY_MODEL) ||
	    (sgt->saAmfSgtRedundancyModel == SA_AMF_N_WAY_ACTIVE_REDUNDANCY_MODEL)) {
		(void) immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGMaxActiveSIsperSU"), attributes, 0, &sg->saAmfSGMaxActiveSIsperSU);
	} else {
		SaUint32T tmp;
		if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGMaxActiveSIsperSU"), attributes, 0, &tmp) == SA_AIS_OK) {
			LOG_NO("'%s' attribute saAmfSGMaxActiveSIsperSU ignored, not valid for red model",
				sg->name.value);
		}
	}

	sg->saAmfSGMaxStandbySIsperSU = -1; // magic number for no limit

	/* saAmfSGMaxStandbySIsperSU: "This attribute is only applicable to N+M and N-way redundancy models." */
	if ((sgt->saAmfSgtRedundancyModel == SA_AMF_NPM_REDUNDANCY_MODEL) ||
	    (sgt->saAmfSgtRedundancyModel == SA_AMF_N_WAY_REDUNDANCY_MODEL)) {
		(void) immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGMaxStandbySIsperSU"), attributes, 0, &sg->saAmfSGMaxStandbySIsperSU);
	} else {
		SaUint32T tmp;
		if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGMaxStandbySIsperSU"), attributes, 0, &tmp) == SA_AIS_OK) {
			LOG_NO("'%s' attribute saAmfSGMaxStandbySIsperSU ignored, not valid for red model",
				sg->name.value);
		}
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGAutoAdjustProb"), attributes, 0, &sg->saAmfSGAutoAdjustProb) != SA_AIS_OK) {
		sg->saAmfSGAutoAdjustProb = sgt->saAmfSgtDefAutoAdjustProb;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGCompRestartProb"), attributes, 0, &sg->saAmfSGCompRestartProb) != SA_AIS_OK) {
		sg->saAmfSGCompRestartProb = sgt->saAmfSgtDefCompRestartProb;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGCompRestartMax"), attributes, 0, &sg->saAmfSGCompRestartMax) != SA_AIS_OK) {
		sg->saAmfSGCompRestartMax = sgt->saAmfSgtDefCompRestartMax;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGSuRestartProb"), attributes, 0, &sg->saAmfSGSuRestartProb) != SA_AIS_OK) {
		sg->saAmfSGSuRestartProb = sgt->saAmfSgtDefSuRestartProb;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGSuRestartMax"), attributes, 0, &sg->saAmfSGSuRestartMax) != SA_AIS_OK) {
		sg->saAmfSGSuRestartMax = sgt->saAmfSgtDefSuRestartMax;
	}

	if (immutil_getAttr(const_cast<SaImmAttrNameT>("saAmfSGAdminState"), attributes, 0, &sg->saAmfSGAdminState) != SA_AIS_OK) {
		sg->saAmfSGAdminState = SA_AMF_ADMIN_UNLOCKED;
	}

	/*  TODO use value in type instead? */
	sg->sg_redundancy_model = sgt->saAmfSgtRedundancyModel;

	rc = 0;

	if (rc != 0) {
		avd_sg_delete(sg);
		sg = NULL;
	}

	return sg;
}

/**
 * Get configuration for all AMF SG objects from IMM and
 * create AVD internal objects.
 * @param cb
 * 
 * @return int
 */
SaAisErrorT avd_sg_config_get(const SaNameT *app_dn, AVD_APP *app)
{
	AVD_SG *sg;
	SaAisErrorT error, rc;
	SaImmSearchHandleT searchHandle;
	SaImmSearchParametersT_2 searchParam;
	SaNameT dn;
	const SaImmAttrValuesT_2 **attributes;
	const char *className = "SaAmfSG";
	SaImmAttrNameT configAttributes[] = {
		const_cast<SaImmAttrNameT>("saAmfSGType"),
		const_cast<SaImmAttrNameT>("saAmfSGSuHostNodeGroup"),
		const_cast<SaImmAttrNameT>("saAmfSGAutoRepair"),
		const_cast<SaImmAttrNameT>("saAmfSGAutoAdjust"),
		const_cast<SaImmAttrNameT>("saAmfSGNumPrefActiveSUs"),
		const_cast<SaImmAttrNameT>("saAmfSGNumPrefStandbySUs"),
		const_cast<SaImmAttrNameT>("saAmfSGNumPrefInserviceSUs"),
		const_cast<SaImmAttrNameT>("saAmfSGNumPrefAssignedSUs"),
		const_cast<SaImmAttrNameT>("saAmfSGMaxActiveSIsperSU"),
		const_cast<SaImmAttrNameT>("saAmfSGMaxStandbySIsperSU"),
		const_cast<SaImmAttrNameT>("saAmfSGAutoAdjustProb"),
		const_cast<SaImmAttrNameT>("saAmfSGCompRestartProb"),
		const_cast<SaImmAttrNameT>("saAmfSGCompRestartMax"),
		const_cast<SaImmAttrNameT>("saAmfSGSuRestartProb"),
		const_cast<SaImmAttrNameT>("saAmfSGSuRestartMax"),
		const_cast<SaImmAttrNameT>("saAmfSGAdminState"),
		NULL
	};

	TRACE_ENTER();

	searchParam.searchOneAttr.attrName = const_cast<SaImmAttrNameT>("SaImmAttrClassName");
	searchParam.searchOneAttr.attrValueType = SA_IMM_ATTR_SASTRINGT;
	searchParam.searchOneAttr.attrValue = &className;


	error = immutil_saImmOmSearchInitialize_2(avd_cb->immOmHandle, app_dn, SA_IMM_SUBTREE,
		SA_IMM_SEARCH_ONE_ATTR | SA_IMM_SEARCH_GET_SOME_ATTR, &searchParam,
		configAttributes, &searchHandle);

	if (SA_AIS_OK != error) {
		LOG_ER("%s: saImmOmSearchInitialize_2 failed: %u", __FUNCTION__, error);
		goto done1;
	}

	while ((rc = immutil_saImmOmSearchNext_2(searchHandle, &dn,
		(SaImmAttrValuesT_2 ***)&attributes)) == SA_AIS_OK) {

		if (!is_config_valid(&dn, attributes, NULL)) {
			error = SA_AIS_ERR_FAILED_OPERATION;
			goto done2;
		}

		if ((sg = sg_create(&dn, attributes)) == NULL) {
			error = SA_AIS_ERR_FAILED_OPERATION;
			goto done2;
		}

		sg_add_to_model(sg);

		if (avd_su_config_get(&dn, sg) != SA_AIS_OK) {
			error = SA_AIS_ERR_FAILED_OPERATION;
			goto done2;
		}
	}

	osafassert(rc == SA_AIS_ERR_NOT_EXIST);
	error = SA_AIS_OK;

done2:
	(void)immutil_saImmOmSearchFinalize(searchHandle);
done1:
	TRACE_LEAVE2("%u", error);
	return error;
}

/**
 * Check if the node group represented by ngname is a subset of the node group
 * represented by supername
 * @param ngname
 * @param supername
 * @return true/false
 */
static bool ng_is_subset(const SaNameT *ngname, const AVD_AMF_NG *superng)
{
	const AVD_AMF_NG *ng = avd_ng_get(ngname);

	if (superng->number_nodes() < ng->number_nodes())
		return false;

	if (std::includes(superng->saAmfNGNodeList.begin(), superng->saAmfNGNodeList.end(),
		ng->saAmfNGNodeList.begin(), ng->saAmfNGNodeList.end()) == true) {
		return true;
	}

	return false;
}

/**
 * Validate if the change in node group value is OK, report to IMM otherwise
 * @param sg
 * @param ng_name
 * @param opdata
 * @return true/false
 */
static bool ng_change_is_valid(const AVD_SG *sg, const SaNameT *ng_name,
		const CcbUtilOperationData_t *opdata)
{
	if (sg->saAmfSGSuHostNodeGroup.length > 0) {
		// A node group is currently configured for SG. A user wants
		// to change it. Validate that the old node group is subset
		// of the new one.

		const AVD_AMF_NG *ng = avd_ng_get(ng_name);
		if (ng == NULL) {
			report_ccb_validation_error(opdata,
					"Node Group '%s' not found", ng_name->value);
			return false;
		}

		if (ng_is_subset(&sg->saAmfSGSuHostNodeGroup, ng) == false) {
			report_ccb_validation_error(opdata,
				"'%s' is not a subset of '%s'",
				sg->saAmfSGSuHostNodeGroup.value, ng_name->value);
			return false;
		}
	} else {
		// a node group is currently not configured for this SG
		// don't allow configuring it now, why?
		report_ccb_validation_error(opdata,
			"Attribute saAmfSGSuHostNodeGroup cannot be modified");
		return false;
	}

	return true;
}

static SaAisErrorT ccb_completed_modify_hdlr(const CcbUtilOperationData_t *opdata)
{
	SaAisErrorT rc = SA_AIS_OK;
	AVD_SG *sg;
	const SaImmAttrModificationT_2 *attr_mod;
	int i = 0;
	bool value_is_deleted = false;

	TRACE_ENTER2("'%s'", opdata->objectName.value);

	sg = sg_db->find(Amf::to_string(&opdata->objectName));
	osafassert(sg != NULL);

	/* Validate whether we can modify it. */

	if ((sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED_INSTANTIATION) ||
		(sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED)) {

		i = 0;
		while ((attr_mod = opdata->param.modify.attrMods[i++]) != NULL) {
			const SaImmAttrValuesT_2 *attribute = &attr_mod->modAttr;
			void *value = NULL;

			/* Attribute value removed */
			if ((attr_mod->modType == SA_IMM_ATTR_VALUES_DELETE) ||
					(attribute->attrValues == NULL))
				continue;

			value = attribute->attrValues[0];

			if (!strcmp(attribute->attrName, "saAmfSGType")) {
				SaNameT sg_type_name = *((SaNameT *)value);

				if (sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED) {
					report_ccb_validation_error(opdata,
						"%s: Attribute saAmfSGType cannot be modified"
						" when SG is not in locked instantion", __FUNCTION__);
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				}

				if (sgtype_db->find(Amf::to_string(&sg_type_name)) == NULL) {
					report_ccb_validation_error(opdata,
						"SG Type '%s' not found", sg_type_name.value);
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				}

			} else if (!strcmp(attribute->attrName, "saAmfSGSuHostNodeGroup")) {
				if (ng_change_is_valid(sg, (SaNameT *)value, opdata) == false) {
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				}
			} else if (!strcmp(attribute->attrName, "saAmfSGAutoAdjust")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefActiveSUs")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefStandbySUs")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefInserviceSUs")) {
				uint32_t pref_inservice_su;
				pref_inservice_su = *((SaUint32T *)value);

				if ((pref_inservice_su == 0) ||
					((sg->sg_redundancy_model < SA_AMF_N_WAY_ACTIVE_REDUNDANCY_MODEL) &&
					 (pref_inservice_su < AVSV_SG_2N_PREF_INSVC_SU_MIN))) {
					report_ccb_validation_error(opdata,
						"%s: Minimum preferred num of su should be 2 in"
						" 2N, N+M and NWay red models", __FUNCTION__);
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				}
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefAssignedSUs")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGMaxActiveSIsperSU")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGMaxStandbySIsperSU")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGAutoAdjustProb")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGCompRestartProb")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGCompRestartMax")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGSuRestartProb")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGSuRestartMax")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGAutoRepair")) {
				uint32_t sg_autorepair = *((SaUint32T *)attribute->attrValues[0]);
				if (sg_autorepair > true ) {
					report_ccb_validation_error(opdata,
						"Invalid saAmfSGAutoRepair SG:'%s'", sg->name.value);
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				}
			} else {
				report_ccb_validation_error(opdata,
					"Unknown attribute '%s'",
					attribute->attrName);
				rc = SA_AIS_ERR_BAD_OPERATION;
				goto done;
			}
		}		/* while (attr_mod != NULL) */

	} else {	/* Admin state is UNLOCKED */
		i = 0;
		/* Modifications can be done for the following parameters. */
		while ((attr_mod = opdata->param.modify.attrMods[i++]) != NULL) {
			const SaImmAttrValuesT_2 *attribute = &attr_mod->modAttr;
			void *value = NULL;

			/* Attribute value removed */
			if ((attr_mod->modType == SA_IMM_ATTR_VALUES_DELETE) ||
					(attribute->attrValues == NULL))
				value_is_deleted = true;
			else {
				value_is_deleted = false;
				value = attribute->attrValues[0];
			}
			if (!strcmp(attribute->attrName, "saAmfSGSuRestartProb")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGSuRestartMax")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGCompRestartProb")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGCompRestartMax")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefInserviceSUs")) {
				if (value_is_deleted == true)
					continue;
				uint32_t pref_inservice_su;
				pref_inservice_su = *((SaUint32T *)value);

				if ((pref_inservice_su == 0) ||
					((sg->sg_redundancy_model < SA_AMF_N_WAY_ACTIVE_REDUNDANCY_MODEL) &&
					 (pref_inservice_su < AVSV_SG_2N_PREF_INSVC_SU_MIN))) {
					report_ccb_validation_error(opdata,
						"%s: Minimum preferred num of su should be 2"
						" in 2N, N+M and NWay red models", __FUNCTION__);
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				}
			} else if (!strcmp(attribute->attrName, "saAmfSGAutoRepair")) {
				if (value_is_deleted == true)
					continue;
				uint32_t sg_autorepair = *((SaUint32T *)attribute->attrValues[0]);
				if (sg_autorepair > SA_TRUE) {
					report_ccb_validation_error(opdata,
						"Invalid saAmfSGAutoRepair SG:'%s'", sg->name.value);
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				}
			} else if (!strcmp(attribute->attrName, "saAmfSGSuHostNodeGroup")) {
				if (value_is_deleted == true)
					continue;
				if (ng_change_is_valid(sg, (SaNameT *)value, opdata) == false) {
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				}
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefActiveSUs")) {
				if (value_is_deleted == true)
					continue;
				uint32_t pref_active_su = *static_cast<SaUint32T *>(value);

				if (sg->sg_redundancy_model != SA_AMF_NPM_REDUNDANCY_MODEL) {
					report_ccb_validation_error(opdata,
						"%s: saAmfSGNumPrefActiveSUs for non-N+M model cannot"
						" be modified when SG is unlocked", __FUNCTION__);
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				} else if (pref_active_su < sg->saAmfSGNumPrefActiveSUs) {
					report_ccb_validation_error(opdata,
						"%s: Cannot decrease saAmfSGNumPrefActiveSUs while SG"
						" is unlocked ", __FUNCTION__);
					rc = SA_AIS_ERR_BAD_OPERATION;
					goto done;
				} else {
					uint32_t increase_amount = pref_active_su - sg->saAmfSGNumPrefActiveSUs;

					if (sg->saAmfSGNumCurrInstantiatedSpareSUs < increase_amount) {
						report_ccb_validation_error(opdata,
							"%s: Not enough instantiated spare SUs to do"
							" in-service increase of saAmfSGNumPrefActiveSUs",
							__FUNCTION__);
						rc = SA_AIS_ERR_BAD_OPERATION;
						goto done;
					}
				}
			} else {
				report_ccb_validation_error(opdata,
					"%s: Attribute '%s' cannot be modified when SG is unlocked",
					__FUNCTION__, attribute->attrName);
				rc = SA_AIS_ERR_BAD_OPERATION;
				goto done;
			}
		}		/* while (attr_mod != NULL) */
	}			/* Admin state is UNLOCKED */

done:
	TRACE_LEAVE();
	return rc;
}

/**
 * update these attributes on the nd
 * @param sg
 * @param attrib_id
 * @param value
 */
static void sg_nd_attribute_update(AVD_SG *sg, uint32_t attrib_id)
{
	AVD_SU *su = NULL;
	AVD_AVND *su_node_ptr = NULL;
	AVSV_PARAM_INFO param;
	memset(((uint8_t *)&param), '\0', sizeof(AVSV_PARAM_INFO));

	TRACE_ENTER();

	if (avd_cb->avail_state_avd != SA_AMF_HA_ACTIVE) {
		TRACE_LEAVE2("avd is not in active state");
		return;
	}
	param.class_id = AVSV_SA_AMF_SG;
	param.act = AVSV_OBJ_OPR_MOD;

	switch (attrib_id) {
	case saAmfSGSuRestartProb_ID:
	{
		SaTimeT temp_su_restart_prob;
		param.attr_id = saAmfSGSuRestartProb_ID;
		param.value_len = sizeof(SaTimeT);
		m_NCS_OS_HTONLL_P(&temp_su_restart_prob, sg->saAmfSGSuRestartProb);
		memcpy((char *)&param.value[0], (char *)&temp_su_restart_prob, param.value_len);
		break;
	}
	case saAmfSGSuRestartMax_ID:
	{
		param.attr_id = saAmfSGSuRestartMax_ID;
		param.value_len = sizeof(SaUint32T);
		m_NCS_OS_HTONL_P(&param.value[0], sg->saAmfSGSuRestartMax);
		break;
	}
	case saAmfSGCompRestartProb_ID:
	{
		SaTimeT temp_comp_restart_prob;
		param.attr_id = saAmfSGCompRestartProb_ID;
		param.value_len = sizeof(SaTimeT);
		m_NCS_OS_HTONLL_P(&temp_comp_restart_prob, sg->saAmfSGCompRestartProb);
		memcpy((char *)&param.value[0], (char *)&temp_comp_restart_prob, param.value_len);
		break;
	}
	case saAmfSGCompRestartMax_ID:
	{
		param.attr_id = saAmfSGCompRestartMax_ID;
		param.value_len = sizeof(SaUint32T);
		m_NCS_OS_HTONL_P(&param.value[0], sg->saAmfSGCompRestartMax);	
		break;
	}
	default:
		osafassert(0);
	}

	/* This value has to be updated on each SU on this SG */
	su = sg->list_of_su;
	while (su) {
		su_node_ptr = su->get_node_ptr();

		if ((su_node_ptr) && (su_node_ptr->node_state == AVD_AVND_STATE_PRESENT)) {
			param.name = su->name;

			if (avd_snd_op_req_msg(avd_cb, su_node_ptr, &param) != NCSCC_RC_SUCCESS) {
				LOG_ER("%s::failed for %s",__FUNCTION__, su_node_ptr->name.value);
			}
		}
		su = su->sg_list_su_next;
	}
	TRACE_LEAVE();
}

static void ccb_apply_modify_hdlr(CcbUtilOperationData_t *opdata)
{
	AVD_SG *sg;
	AVD_AMF_SG_TYPE *sg_type;
	const SaImmAttrModificationT_2 *attr_mod;
	int i = 0;
	void *value = NULL;
	bool value_is_deleted;

	TRACE_ENTER2("'%s'", opdata->objectName.value);

	sg = sg_db->find(Amf::to_string(&opdata->objectName));
	assert(sg != NULL);

	sg_type = sgtype_db->find(Amf::to_string(&sg->saAmfSGType));
	osafassert(NULL != sg_type);

	if (sg->saAmfSGAdminState != SA_AMF_ADMIN_UNLOCKED) {
		attr_mod = opdata->param.modify.attrMods[i++];
		while (attr_mod != NULL) {
			const SaImmAttrValuesT_2 *attribute = &attr_mod->modAttr;

			if ((attr_mod->modType == SA_IMM_ATTR_VALUES_DELETE) ||	(attribute->attrValues == NULL)) {
				/* Attribute value is deleted, revert to default value */
				value_is_deleted = true;
			} else {
				/* Attribute value is modified */
				value_is_deleted = false;
				value = attribute->attrValues[0];
			}

			if (!strcmp(attribute->attrName, "saAmfSGType")) {
				SaNameT sg_type_name = *((SaNameT *)value);
				TRACE("saAmfSGType modified from '%s' to '%s' of Sg'%s'", sg->saAmfSGType.value, 
						sg_type_name.value, sg->name.value);

				sg_type = sgtype_db->find(Amf::to_string(&sg_type_name));
				osafassert(NULL != sg_type);

				/* Remove from old type */
				avd_sgtype_remove_sg(sg);

				sg->saAmfSGType = sg_type_name;

				/* Fill the relevant values from sg_type. */
				sg->saAmfSGAutoRepair = sg_type->saAmfSgtDefAutoRepair;
				sg->saAmfSGAutoAdjust = sg_type->saAmfSgtDefAutoAdjust;
				sg->saAmfSGAutoAdjustProb = sg_type->saAmfSgtDefAutoAdjustProb;
				sg->saAmfSGCompRestartProb = sg_type->saAmfSgtDefCompRestartProb;
				sg->saAmfSGCompRestartMax = sg_type->saAmfSgtDefCompRestartMax;
				sg->saAmfSGSuRestartProb = sg_type->saAmfSgtDefSuRestartProb;
				sg->saAmfSGSuRestartMax = sg_type->saAmfSgtDefSuRestartMax;
				sg->sg_redundancy_model = sg_type->saAmfSgtRedundancyModel;

				/* Add to new type */
				sg->sg_type = sg_type;
				avd_sgtype_add_sg(sg);

				/* update all the nodes with the modified values */
				sg_nd_attribute_update(sg, saAmfSGSuRestartProb_ID);
				sg_nd_attribute_update(sg, saAmfSGSuRestartMax_ID);
				sg_nd_attribute_update(sg, saAmfSGCompRestartProb_ID);
				sg_nd_attribute_update(sg, saAmfSGCompRestartMax_ID);
			} else if (!strcmp(attribute->attrName, "saAmfSGAutoRepair")) {
				TRACE("Old saAmfSGAutoRepair is '%u'", sg->saAmfSGAutoRepair);
				if (value_is_deleted) {
					sg->saAmfSGAutoRepair = sg_type->saAmfSgtDefAutoRepair;
					sg->saAmfSGAutoRepair_configured = false;
				} else {
					sg->saAmfSGAutoRepair = static_cast<SaBoolT>(*((SaUint32T *)value));
					sg->saAmfSGAutoRepair_configured = true; 
				}
				TRACE("Modified saAmfSGAutoRepair is '%u'", sg->saAmfSGAutoRepair);
				amflog(LOG_NOTICE, "%s saAmfSGAutoRepair changed to %u",
					sg->name.value, sg->saAmfSGAutoRepair);
			} else if (!strcmp(attribute->attrName, "saAmfSGAutoAdjust")) {
				TRACE("Old saAmfSGAutoAdjust is '%u'", sg->saAmfSGAutoAdjust);
				if (value_is_deleted)
					sg->saAmfSGAutoAdjust = sg_type->saAmfSgtDefAutoAdjust;
				else
					sg->saAmfSGAutoAdjust = static_cast<SaBoolT>(*((SaUint32T *)value));
				TRACE("Modified saAmfSGAutoAdjust is '%u'", sg->saAmfSGAutoAdjust);
			} else if (!strcmp(attribute->attrName, "saAmfSGAutoAdjust")) {
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefActiveSUs")) {
				if (value_is_deleted)
					sg->saAmfSGNumPrefActiveSUs = 1;
				else
					sg->saAmfSGNumPrefActiveSUs = *((SaUint32T *)value);
				TRACE("Modified saAmfSGNumPrefActiveSUs is '%u'", sg->saAmfSGNumPrefActiveSUs);
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefStandbySUs")) {
				if (value_is_deleted)
					sg->saAmfSGNumPrefStandbySUs = 1;
				else
					sg->saAmfSGNumPrefStandbySUs = *((SaUint32T *)value);
				TRACE("Modified saAmfSGNumPrefStandbySUs is '%u'", sg->saAmfSGNumPrefStandbySUs);
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefInserviceSUs")) {
				if (value_is_deleted)
					sg->saAmfSGNumPrefInserviceSUs = ~0;
				else
					sg->saAmfSGNumPrefInserviceSUs = *((SaUint32T *)value);
				TRACE("Modified saAmfSGNumPrefInserviceSUs is '%u'", sg->saAmfSGNumPrefInserviceSUs);
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefAssignedSUs")) {
				if (value_is_deleted)
					sg->saAmfSGNumPrefAssignedSUs = sg->saAmfSGNumPrefInserviceSUs;
				else
					sg->saAmfSGNumPrefAssignedSUs = *((SaUint32T *)value);
				TRACE("Modified saAmfSGNumPrefAssignedSUs is '%u'", sg->saAmfSGNumPrefAssignedSUs);
			} else if (!strcmp(attribute->attrName, "saAmfSGMaxActiveSIsperSU")) {
				if (value_is_deleted)
					sg->saAmfSGMaxActiveSIsperSU = -1;
				else
					sg->saAmfSGMaxActiveSIsperSU = *((SaUint32T *)value);
				TRACE("Modified saAmfSGMaxActiveSIsperSU is '%u'", sg->saAmfSGMaxActiveSIsperSU);
			} else if (!strcmp(attribute->attrName, "saAmfSGMaxStandbySIsperSU")) {
				if (value_is_deleted)
					sg->saAmfSGMaxStandbySIsperSU = -1;
				else
					sg->saAmfSGMaxStandbySIsperSU = *((SaUint32T *)value);
				TRACE("Modified saAmfSGMaxStandbySIsperSU is '%u'", sg->saAmfSGMaxStandbySIsperSU);
			} else if (!strcmp(attribute->attrName, "saAmfSGAutoAdjustProb")) {
				if (value_is_deleted)
					sg->saAmfSGAutoAdjustProb = sg_type->saAmfSgtDefAutoAdjustProb;
				else
					sg->saAmfSGAutoAdjustProb = *((SaTimeT *)value);
				TRACE("Modified saAmfSGAutoAdjustProb is '%llu'", sg->saAmfSGAutoAdjustProb);
			} else if (!strcmp(attribute->attrName, "saAmfSGCompRestartProb")) {
				if (value_is_deleted)
					sg->saAmfSGCompRestartProb = sg_type->saAmfSgtDefCompRestartProb;
				else
					sg->saAmfSGCompRestartProb = *((SaTimeT *)value);
				TRACE("Modified saAmfSGCompRestartProb is '%llu'", sg->saAmfSGCompRestartProb);
				sg_nd_attribute_update(sg, saAmfSGCompRestartProb_ID);
			} else if (!strcmp(attribute->attrName, "saAmfSGCompRestartMax")) {
				if (value_is_deleted)
					sg->saAmfSGCompRestartMax = sg_type->saAmfSgtDefCompRestartMax;
				else
					sg->saAmfSGCompRestartMax = *((SaUint32T *)value);
				TRACE("Modified saAmfSGCompRestartMax is '%u'", sg->saAmfSGCompRestartMax);
				sg_nd_attribute_update(sg, saAmfSGCompRestartMax_ID);
			} else if (!strcmp(attribute->attrName, "saAmfSGSuRestartProb")) {
				if (value_is_deleted)
					sg->saAmfSGSuRestartProb = sg_type->saAmfSgtDefSuRestartProb;
				else
					sg->saAmfSGSuRestartProb = *((SaTimeT *)value);
				TRACE("Modified saAmfSGSuRestartProb is '%llu'", sg->saAmfSGSuRestartProb);
				sg_nd_attribute_update(sg, saAmfSGSuRestartProb_ID);
			} else if (!strcmp(attribute->attrName, "saAmfSGSuRestartMax")) {
				if (value_is_deleted)
					sg->saAmfSGSuRestartMax = sg_type->saAmfSgtDefSuRestartMax;
				else
					sg->saAmfSGSuRestartMax = *((SaUint32T *)value);
				TRACE("Modified saAmfSGSuRestartMax is '%u'", sg->saAmfSGSuRestartMax);
				sg_nd_attribute_update(sg, saAmfSGSuRestartMax_ID);
			} else if (!strcmp(attribute->attrName, "saAmfSGSuHostNodeGroup")) {
				sg->saAmfSGSuHostNodeGroup = *((SaNameT *)value);
			} else {
				osafassert(0);
			}

			attr_mod = opdata->param.modify.attrMods[i++];
		}
	} else {			/* Admin state is UNLOCKED */
		i = 0;
		/* Modifications can be done for the following parameters. */
		attr_mod = opdata->param.modify.attrMods[i++];
		while (attr_mod != NULL) {
			const SaImmAttrValuesT_2 *attribute = &attr_mod->modAttr;

			if ((attr_mod->modType == SA_IMM_ATTR_VALUES_DELETE) ||	(attribute->attrValues == NULL)) {
				/* Attribute value is deleted, revert to default value */
				value_is_deleted = true;
			} else {
				/* Attribute value is modified */
				value_is_deleted = false;
				value = attribute->attrValues[0];
			}

			if (!strcmp(attribute->attrName, "saAmfSGNumPrefInserviceSUs")) {
				if (value_is_deleted)
					sg->saAmfSGNumPrefInserviceSUs = ~0;
				else
					sg->saAmfSGNumPrefInserviceSUs = *((SaUint32T *)value);
				TRACE("Modified saAmfSGNumPrefInserviceSUs is '%u'", sg->saAmfSGNumPrefInserviceSUs);

				if (avd_cb->avail_state_avd == SA_AMF_HA_ACTIVE)  {
					if (avd_sg_app_su_inst_func(avd_cb, sg) != NCSCC_RC_SUCCESS) {
						osafassert(0);
					}
				}
			} else if (!strcmp(attribute->attrName, "saAmfSGCompRestartProb")) {
				if (value_is_deleted)
					sg->saAmfSGCompRestartProb = sg_type->saAmfSgtDefCompRestartProb;
				else
					sg->saAmfSGCompRestartProb = *((SaTimeT *)value);
				TRACE("Modified saAmfSGCompRestartProb is '%llu'", sg->saAmfSGCompRestartProb);
				sg_nd_attribute_update(sg, saAmfSGCompRestartProb_ID);

			} else if (!strcmp(attribute->attrName, "saAmfSGCompRestartMax")) {
				if (value_is_deleted)
					sg->saAmfSGCompRestartMax = sg_type->saAmfSgtDefCompRestartMax;
				else
					sg->saAmfSGCompRestartMax = *((SaUint32T *)value);
				TRACE("Modified saAmfSGCompRestartMax is '%u'", sg->saAmfSGCompRestartMax);
				sg_nd_attribute_update(sg, saAmfSGCompRestartMax_ID);

			} else if (!strcmp(attribute->attrName, "saAmfSGSuRestartProb")) {
				if (value_is_deleted)
					sg->saAmfSGSuRestartProb = sg_type->saAmfSgtDefSuRestartProb;
				else
					sg->saAmfSGSuRestartProb = *((SaTimeT *)value);
				TRACE("Modified saAmfSGSuRestartProb is '%llu'", sg->saAmfSGSuRestartProb);
				sg_nd_attribute_update(sg, saAmfSGSuRestartProb_ID);

			} else if (!strcmp(attribute->attrName, "saAmfSGSuRestartMax")) {
				if (value_is_deleted)
					sg->saAmfSGSuRestartMax = sg_type->saAmfSgtDefSuRestartMax;
				else
					sg->saAmfSGSuRestartMax = *((SaUint32T *)value);
				TRACE("Modified saAmfSGSuRestartMax is '%u'", sg->saAmfSGSuRestartMax);
				sg_nd_attribute_update(sg, saAmfSGSuRestartMax_ID);
			} else if (!strcmp(attribute->attrName, "saAmfSGAutoRepair")) {
				if (value_is_deleted) {
					sg->saAmfSGAutoRepair = sg_type->saAmfSgtDefAutoRepair;
					sg->saAmfSGAutoRepair_configured = false;
				} else {
					sg->saAmfSGAutoRepair = static_cast<SaBoolT>(*((SaUint32T *)value));
					sg->saAmfSGAutoRepair_configured = true; 
				}
				TRACE("Modified saAmfSGAutoRepair is '%u'", sg->saAmfSGAutoRepair);
				amflog(LOG_NOTICE, "%s saAmfSGAutoRepair changed to %u",
					sg->name.value, sg->saAmfSGAutoRepair);
			} else if (!strcmp(attribute->attrName, "saAmfSGSuHostNodeGroup")) {
				sg->saAmfSGSuHostNodeGroup = *((SaNameT *)value);
			} else if (!strcmp(attribute->attrName, "saAmfSGNumPrefActiveSUs")) {
				if (value_is_deleted) {
					/* TODO: This really should be handled in ccb_completed_modify_hdlr */
					LOG_WA("not deleting saAmfSGNumPrefActiveSUs while in UNLOCKED");
				} else {
					sg->saAmfSGNumPrefActiveSUs = *static_cast<SaUint32T *>(value);

					if (avd_cb->avail_state_avd == SA_AMF_HA_ACTIVE)  {
						/* find an instantiated spare SU */
						if (sg->realign(avd_cb, sg) != NCSCC_RC_SUCCESS) {
							osafassert(0);
						}
					}
				}
			} else {
				osafassert(0);
			}

			attr_mod = opdata->param.modify.attrMods[i++];
		}		/* while (attr_mod != NULL) */

	}			/* Admin state is UNLOCKED */
}

/**
 * Terminate SU in reverse order  
 * @param su
 * @return NCSCC_RC_SUCCESS/NCSCC_RC_FAILURE.
 */
static uint32_t avd_sg_su_term_in_reverse(AVD_SU *su)
{
  uint32_t rc = NCSCC_RC_SUCCESS; 
  TRACE_ENTER2("su:'%s'", su ? su->name.value : NULL);
  if (su->sg_list_su_next != NULL)
	rc = avd_sg_su_term_in_reverse(su->sg_list_su_next);
  if ((su->saAmfSUPreInstantiable == true) &&
		  (su->saAmfSUPresenceState != SA_AMF_PRESENCE_UNINSTANTIATED) &&
		  (su->saAmfSUPresenceState != SA_AMF_PRESENCE_INSTANTIATION_FAILED) &&
		  (su->saAmfSUPresenceState != SA_AMF_PRESENCE_TERMINATION_FAILED)) {

	  if (avd_snd_presence_msg(avd_cb, su, true) == NCSCC_RC_SUCCESS) {
		  su->set_term_state(true);
	  } else {
		  rc = NCSCC_RC_FAILURE;
		  LOG_WA("Failed Termination '%s'", su->name.value);
	  }
  }
  return rc ;
}
/**
 * perform lock-instantiation on a given SG
 * @param cb
 * @param sg
 */
static uint32_t sg_app_sg_admin_lock_inst(AVD_CL_CB *cb, AVD_SG *sg)
{
	uint32_t rc = NCSCC_RC_SUCCESS;

	TRACE_ENTER2("%s", sg->name.value);

	/* terminate all the SUs on this Node */
	if (sg->list_of_su != NULL)
		rc = avd_sg_su_term_in_reverse(sg->list_of_su);

	TRACE_LEAVE2("%u", rc);
	return rc;
}

/**
 * perform unlock-instantiation on a given SG
 * 
 * @param cb
 * @param sg
 */
static void sg_app_sg_admin_unlock_inst(AVD_CL_CB *cb, AVD_SG *sg)
{
	AVD_SU *su;
	uint32_t su_try_inst;

	TRACE_ENTER2("%s", sg->name.value);

	/* Instantiate the SUs in this SG */
	for (su = sg->list_of_su, su_try_inst = 0; su != NULL; su = su->sg_list_su_next) {
		if ((su->saAmfSUAdminState != SA_AMF_ADMIN_LOCKED_INSTANTIATION) &&
				(su->su_on_node->saAmfNodeAdminState != SA_AMF_ADMIN_LOCKED_INSTANTIATION) &&
				(su->saAmfSUOperState == SA_AMF_OPERATIONAL_ENABLED) &&
				(su->saAmfSUPresenceState == SA_AMF_PRESENCE_UNINSTANTIATED)) {

			if (su->saAmfSUPreInstantiable == true) {
				if (su->su_on_node->node_state == AVD_AVND_STATE_PRESENT) {
					if (su->sg_of_su->saAmfSGNumPrefInserviceSUs > su_try_inst) {
						if (avd_snd_presence_msg(cb, su, false) != NCSCC_RC_SUCCESS) {
							LOG_NO("%s: Failed to send Instantiation order of '%s' to %x",
									__FUNCTION__, su->name.value,
									su->su_on_node->node_info.nodeId);
						} else {
							su_try_inst ++;
						}
					}
				}

				/* set uncondionally of msg snd outcome */
				su->set_term_state(false);
			} 
		}
	}

	TRACE_LEAVE();
}

/**
 * @brief       Checks if Tolerance timer is running for any of the SI's of the SG  
 *
 * @param[in]  sg
 *
 * @return  true/false
 **/
bool sg_is_tolerance_timer_running_for_any_si(AVD_SG *sg)
{
	AVD_SI  *si;

	for (si = sg->list_of_si; si != NULL; si = si->sg_list_of_si_next) {
		if (si->si_dep_state == AVD_SI_TOL_TIMER_RUNNING) {
			TRACE("Tolerance timer running for si: %s",si->name.value);
			return true;
		}
	}

	return false;
}
static void sg_admin_op_cb(SaImmOiHandleT immOiHandle, SaInvocationT invocation,
	const SaNameT *object_name, SaImmAdminOperationIdT op_id,
	const SaImmAdminOperationParamsT_2 **params)
{
	AVD_SG *sg;
	SaAmfAdminStateT adm_state;
	AVD_SU *su;
	AVD_AVND *node;

	TRACE_ENTER2("'%s', %llu", object_name->value, op_id);
	sg = sg_db->find(Amf::to_string(object_name));

	if (sg->sg_ncs_spec == true) {
		report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
				"Admin Op on OpenSAF MW SG is not allowed");
		goto done;
	}

	if (sg->sg_fsm_state != AVD_SG_FSM_STABLE) {
		report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_TRY_AGAIN, NULL,
				"SG not in STABLE state (%s)", sg->name.value);
		goto done;
	}

	/* Avoid multiple admin operations on other SUs belonging to the same SG. */
	for (su = sg->list_of_su; su != NULL; su = su->sg_list_su_next) {
		node = su->get_node_ptr();
		if (su->pend_cbk.invocation != 0) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_TRY_AGAIN, NULL,
					"Admin operation'%u' is already going on su'%s' belonging to the same SG",
					su->pend_cbk.admin_oper, su->name.value);
			goto done;
		} else if (node->admin_node_pend_cbk.admin_oper != 0) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_TRY_AGAIN, NULL,
					"Node'%s' hosting SU'%s' belonging to the same SG, undergoing admin"
					" operation'%u'", node->name.value, su->name.value,
					node->admin_node_pend_cbk.admin_oper);
			goto done;
		}
	}

	if ((sg->adminOp_invocationId  != 0) || (sg->adminOp != 0))  {
		report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_TRY_AGAIN, NULL,
				"Admin operation is going on (%s)", sg->name.value);
		goto done;
	}
        /* Avoid if any single Csi assignment is undergoing on SG. */
        if (csi_assignment_validate(sg) == true) {
		report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_TRY_AGAIN, NULL,
				"Single Csi assignment undergoing on (sg'%s')", sg->name.value);
                goto done;
        }

	/* if Tolerance timer is running for any SI's withing this SG, then return SA_AIS_ERR_TRY_AGAIN */
	if (sg_is_tolerance_timer_running_for_any_si(sg)) {
		report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_TRY_AGAIN, NULL,
				"Tolerance timer is running for some of the SI's in the SG '%s',"
				" so differing admin opr", sg->name.value);
		goto done;
	}
	switch (op_id) {
	case SA_AMF_ADMIN_UNLOCK:
		if (sg->saAmfSGAdminState == SA_AMF_ADMIN_UNLOCKED) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_NO_OP, NULL,
					"%s is already unlocked", object_name->value);
			goto done;
		}

		if (sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED_INSTANTIATION) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
					"%s is locked instantiation", object_name->value);
			goto done;
		}

		adm_state = sg->saAmfSGAdminState;
		avd_sg_admin_state_set(sg, SA_AMF_ADMIN_UNLOCKED);
		if (avd_cb->init_state == AVD_INIT_DONE) {
			for (su = sg->list_of_su; su != NULL; su = su->sg_list_su_next) {
				if (su->is_in_service() == true) {
					su->set_readiness_state(SA_AMF_READINESS_IN_SERVICE);
				}
			}
			break;
		}
		if (avd_sg_app_sg_admin_func(avd_cb, sg) != NCSCC_RC_SUCCESS) {
			avd_sg_admin_state_set(sg, adm_state);
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
					NULL);
			goto done;
		}
		sg->adminOp = SA_AMF_ADMIN_UNLOCK;
		break;

	case SA_AMF_ADMIN_LOCK:
		if (sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_NO_OP, NULL,
					"%s is already locked", object_name->value);
			goto done;
		}

		if (sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED_INSTANTIATION) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
					"%s is locked instantiation", object_name->value);
			goto done;
		}

		adm_state = sg->saAmfSGAdminState;
		avd_sg_admin_state_set(sg, SA_AMF_ADMIN_LOCKED);

		if (avd_cb->init_state == AVD_INIT_DONE) {
			for (su = sg->list_of_su; su != NULL; su = su->sg_list_su_next) {
				su->set_readiness_state(SA_AMF_READINESS_OUT_OF_SERVICE);
			}
			break;
		}

		if (avd_sg_app_sg_admin_func(avd_cb, sg) != NCSCC_RC_SUCCESS) {
			avd_sg_admin_state_set(sg, adm_state);
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
					NULL);
			goto done;
		}

		sg->adminOp = SA_AMF_ADMIN_LOCK;
		break;
	case SA_AMF_ADMIN_SHUTDOWN:
		if (sg->saAmfSGAdminState == SA_AMF_ADMIN_SHUTTING_DOWN) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_NO_OP, NULL,
					"%s is shutting down", object_name->value);
			goto done;
		}

		if ((sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED) ||
		    (sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED_INSTANTIATION)) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
					"%s is locked (instantiation)", object_name->value);
			goto done;
		}

		adm_state = sg->saAmfSGAdminState;
		avd_sg_admin_state_set(sg, SA_AMF_ADMIN_SHUTTING_DOWN);
		if (avd_sg_app_sg_admin_func(avd_cb, sg) != NCSCC_RC_SUCCESS) {
			avd_sg_admin_state_set(sg, adm_state);
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
					NULL);
			goto done;
		}
		sg->adminOp = SA_AMF_ADMIN_SHUTDOWN;
		break;
	case SA_AMF_ADMIN_LOCK_INSTANTIATION:
		if (sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED_INSTANTIATION) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_NO_OP, NULL,
					"%s is already locked-instantiation", object_name->value);
			goto done;
		}

		if (sg->saAmfSGAdminState != SA_AMF_ADMIN_LOCKED) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
					"%s is not locked", object_name->value);
			goto done;
		}

		adm_state = sg->saAmfSGAdminState;
		avd_sg_admin_state_set(sg, SA_AMF_ADMIN_LOCKED_INSTANTIATION);
		if (sg_app_sg_admin_lock_inst(avd_cb, sg) != NCSCC_RC_SUCCESS) {
			avd_sg_admin_state_set(sg, adm_state);
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
					NULL);
			goto done;
		}

		
		sg->adminOp = SA_AMF_ADMIN_LOCK_INSTANTIATION;
		break;
	case SA_AMF_ADMIN_UNLOCK_INSTANTIATION:
		if (sg->saAmfSGAdminState == SA_AMF_ADMIN_LOCKED) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_NO_OP, NULL,
					"%s is already locked", object_name->value);
			goto done;
		}

		if (sg->saAmfSGAdminState != SA_AMF_ADMIN_LOCKED_INSTANTIATION) {
			report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_BAD_OPERATION, NULL,
					"%s is not in locked instantiation state", object_name->value);
			goto done;
		}

		/* If any su is in terminating state, that means lock-in op
		   has not completed. Allow su to move into permanent state
		   i.e. either in uninstanted or term failed state. */
		for (su = sg->list_of_su; su != NULL; su = su->sg_list_su_next) {
			if (su->saAmfSUPresenceState == SA_AMF_PRESENCE_TERMINATING) {
				report_admin_op_error(immOiHandle, invocation,
						SA_AIS_ERR_TRY_AGAIN, NULL,
						"su'%s' in terminating state",
						su->name.value);
				goto done;
			}
		}

		avd_sg_admin_state_set(sg, SA_AMF_ADMIN_LOCKED);

		if ((sg->list_of_su != NULL) && (sg->list_of_su->saAmfSUPreInstantiable == false)) {
			avd_saImmOiAdminOperationResult(immOiHandle, invocation, SA_AIS_OK);
			goto done;
		}
		sg_app_sg_admin_unlock_inst(avd_cb, sg);
		sg->adminOp = SA_AMF_ADMIN_UNLOCK_INSTANTIATION;

		break;
	case SA_AMF_ADMIN_SG_ADJUST:
	default:
		report_admin_op_error(immOiHandle, invocation, SA_AIS_ERR_NOT_SUPPORTED, NULL,
				"Admin Operation '%llu' not supported", op_id);
		goto done;
	}

	if ((op_id != SA_AMF_ADMIN_UNLOCK_INSTANTIATION) && (op_id != SA_AMF_ADMIN_LOCK_INSTANTIATION) 
		&& (sg->sg_fsm_state == AVD_SG_FSM_STABLE)) {
		avd_saImmOiAdminOperationResult(immOiHandle, invocation, SA_AIS_OK);
		sg->adminOp = static_cast<SaAmfAdminOperationIdT>(0);
		goto done;
	}

	if ((op_id == SA_AMF_ADMIN_UNLOCK_INSTANTIATION) || (op_id == SA_AMF_ADMIN_LOCK_INSTANTIATION)) {
		if (sg_stable_after_lock_in_or_unlock_in(sg) == true) {
			avd_saImmOiAdminOperationResult(immOiHandle, invocation, SA_AIS_OK);
			sg->adminOp = static_cast<SaAmfAdminOperationIdT>(0);
			goto done;
		}
	}

	sg->adminOp_invocationId = invocation;
done:
	TRACE_LEAVE();
}

static SaAisErrorT sg_rt_attr_cb(SaImmOiHandleT immOiHandle,
	const SaNameT *objectName, const SaImmAttrNameT *attributeNames)
{
	AVD_SG *sg = sg_db->find(Amf::to_string(objectName));
	SaImmAttrNameT attributeName;
	int i = 0;

	TRACE_ENTER2("'%s'", objectName->value);
	osafassert(sg != NULL);

	while ((attributeName = attributeNames[i++]) != NULL) {
		if (!strcmp("saAmfSGNumCurrAssignedSUs", attributeName)) {
			avd_saImmOiRtObjectUpdate_sync(objectName, attributeName,
				SA_IMM_ATTR_SAUINT32T, &sg->saAmfSGNumCurrAssignedSUs);
		} else if (!strcmp("saAmfSGNumCurrNonInstantiatedSpareSUs", attributeName)) {
			avd_saImmOiRtObjectUpdate_sync(objectName, attributeName,
				SA_IMM_ATTR_SAUINT32T, &sg->saAmfSGNumCurrNonInstantiatedSpareSUs);
		} else if (!strcmp("saAmfSGNumCurrInstantiatedSpareSUs", attributeName)) {
			avd_saImmOiRtObjectUpdate_sync(objectName, attributeName,
				SA_IMM_ATTR_SAUINT32T, &sg->saAmfSGNumCurrInstantiatedSpareSUs);
		} else {
			LOG_ER("Ignoring unknown attribute '%s'", attributeName);
		}
	}

	return SA_AIS_OK;
}

/**
 * Validate SaAmfSG CCB completed
 * @param opdata
 * 
 * @return SaAisErrorT
 */
static SaAisErrorT sg_ccb_completed_cb(CcbUtilOperationData_t *opdata)
{
	SaAisErrorT rc = SA_AIS_ERR_BAD_OPERATION;
	AVD_SG *sg;
	AVD_SI *si;
	bool si_exist = false;
	CcbUtilOperationData_t *t_opData;

	TRACE_ENTER2("CCB ID %llu, '%s'", opdata->ccbId, opdata->objectName.value);

	switch (opdata->operationType) {
	case CCBUTIL_CREATE:
		if (is_config_valid(&opdata->objectName, opdata->param.create.attrValues, opdata))
			rc = SA_AIS_OK;
		break;
	case CCBUTIL_MODIFY:
		rc = ccb_completed_modify_hdlr(opdata);
		break;
	case CCBUTIL_DELETE:
		sg = sg_db->find(Amf::to_string(&opdata->objectName));
		if (sg->list_of_si != NULL) {
			/* check whether there is parent app delete */
			t_opData = ccbutil_getCcbOpDataByDN(opdata->ccbId, &sg->app->name);
			if (t_opData == NULL || t_opData->operationType != CCBUTIL_DELETE) {
				/* check whether there exists a delete operation for 
				 * each of the SIs in the SG's list in the current CCB
				 */
				si = sg->list_of_si;
				while (si != NULL) {
					t_opData = ccbutil_getCcbOpDataByDN(opdata->ccbId, &si->name);
					if ((t_opData == NULL) || (t_opData->operationType != CCBUTIL_DELETE)) {
						si_exist = true;
						break;
					}
					si = si->sg_list_of_si_next;
				}
			}
			if (si_exist == true) {
				report_ccb_validation_error(opdata, "SIs still exist in SG '%s'", sg->name.value);
				goto done;
			}
		}
		rc = SA_AIS_OK;
		opdata->userData = sg;	/* Save for later use in apply */
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
 * Apply SaAmfSG CCB changes
 * @param opdata
 * 
 * @return SaAisErrorT
 */
static void sg_ccb_apply_cb(CcbUtilOperationData_t *opdata)
{
	AVD_SG *sg;

	TRACE_ENTER2("CCB ID %llu, '%s'", opdata->ccbId, opdata->objectName.value);

	switch (opdata->operationType) {
	case CCBUTIL_CREATE:
		sg = sg_create(&opdata->objectName, opdata->param.create.attrValues);
		osafassert(sg);
		sg_add_to_model(sg);
		break;
	case CCBUTIL_DELETE:
		avd_sg_delete(static_cast<AVD_SG*>(opdata->userData));
		break;
	case CCBUTIL_MODIFY:
		ccb_apply_modify_hdlr(opdata);
		break;
	default:
		osafassert(0);
	}
}

void avd_sg_remove_su(AVD_SU* su)
{
	AVD_SU *i_su = NULL;
	AVD_SU *prev_su = NULL;
	AVD_SG *sg = su->sg_of_su;

	if (su->sg_of_su != NULL) {
		/* remove SU from SG */
		i_su = su->sg_of_su->list_of_su;

		while ((i_su != NULL) && (i_su != su)) {
			prev_su = i_su;
			i_su = i_su->sg_list_su_next;
		}

		if (i_su != su) {
			/* Log a fatal error */
		} else {
			if (prev_su == NULL) {
				su->sg_of_su->list_of_su = su->sg_list_su_next;
			} else {
				prev_su->sg_list_su_next = su->sg_list_su_next;
			}
		}

		su->sg_list_su_next = NULL;
		su->sg_of_su = NULL;
	} /* if (su->sg_of_su != AVD_SG_NULL) */

	avd_verify_equal_ranked_su(sg);
	return;
}

void avd_sg_add_su(AVD_SU* su)
{
	AVD_SU *i_su = NULL;
	AVD_SU *prev_su = NULL;

	if ((su == NULL) || (su->sg_of_su == NULL))
		return;

	i_su = su->sg_of_su->list_of_su;

	while ((i_su != NULL) && (i_su->saAmfSURank < su->saAmfSURank)) {
		prev_su = i_su;
		i_su = i_su->sg_list_su_next;
	}

	if (prev_su == NULL) {
		su->sg_list_su_next = su->sg_of_su->list_of_su;
		su->sg_of_su->list_of_su = su;
	} else {
		prev_su->sg_list_su_next = su;
		su->sg_list_su_next = i_su;
	}
	avd_verify_equal_ranked_su(su->sg_of_su);
}

void avd_sg_constructor(void)
{
	sg_db = new AmfDb<std::string, AVD_SG>;
	avd_class_impl_set("SaAmfSG", sg_rt_attr_cb, sg_admin_op_cb,
		sg_ccb_completed_cb, sg_ccb_apply_cb);
}

void avd_sg_admin_state_set(AVD_SG* sg, SaAmfAdminStateT state)
{
	SaAmfAdminStateT old_state = sg->saAmfSGAdminState;
	
	osafassert(state <= SA_AMF_ADMIN_SHUTTING_DOWN);
	TRACE_ENTER2("%s AdmState %s => %s", sg->name.value,
			avd_adm_state_name[old_state], avd_adm_state_name[state]);
	saflog(LOG_NOTICE, amfSvcUsrName, "%s AdmState %s => %s", sg->name.value,
                  avd_adm_state_name[old_state], avd_adm_state_name[state]);      
	sg->saAmfSGAdminState = state;
	avd_saImmOiRtObjectUpdate(&sg->name,
				  const_cast<SaImmAttrNameT>("saAmfSGAdminState"), SA_IMM_ATTR_SAUINT32T, &sg->saAmfSGAdminState);
	m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(avd_cb, sg, AVSV_CKPT_SG_ADMIN_STATE);

	/* Notification */ 
	avd_send_admin_state_chg_ntf(&sg->name,
					SA_AMF_NTFID_SG_ADMIN_STATE,
					old_state,
					sg->saAmfSGAdminState);
}

void AVD_SG::set_admin_state(SaAmfAdminStateT state) {
	SaAmfAdminStateT old_state = saAmfSGAdminState;

	osafassert(state <= SA_AMF_ADMIN_SHUTTING_DOWN);
	TRACE_ENTER2("%s AdmState %s => %s", name.value,
			avd_adm_state_name[old_state], avd_adm_state_name[state]);
	saflog(LOG_NOTICE, amfSvcUsrName, "%s AdmState %s => %s", name.value,
                  avd_adm_state_name[old_state], avd_adm_state_name[state]);
	saAmfSGAdminState = state;
	avd_saImmOiRtObjectUpdate(&name,
		  const_cast<SaImmAttrNameT>("saAmfSGAdminState"), SA_IMM_ATTR_SAUINT32T,
		  &saAmfSGAdminState);
	m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(avd_cb, this, AVSV_CKPT_SG_ADMIN_STATE);

	avd_send_admin_state_chg_ntf(&name, SA_AMF_NTFID_SG_ADMIN_STATE, old_state,
		saAmfSGAdminState);
}

void AVD_SG::set_fsm_state(AVD_SG_FSM_STATE state) {
	if (sg_fsm_state != state) {
		TRACE("%s sg_fsm_state %u => %u", name.value, sg_fsm_state, state);
		sg_fsm_state = state;
		m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(avd_cb, this, AVSV_CKPT_SG_FSM_STATE);
	}

	if (state == AVD_SG_FSM_STABLE) {
		osafassert(su_oper_list.su == NULL);
		if (adminOp_invocationId != 0) {
			avd_saImmOiAdminOperationResult(avd_cb->immOiHandle, adminOp_invocationId, SA_AIS_OK);
			adminOp_invocationId = 0;
			adminOp = static_cast<SaAmfAdminOperationIdT>(0);
		}
		for (AVD_SI* si = list_of_si; si != NULL; si = si->sg_list_of_si_next) {
			if (si->invocation != 0) {
				avd_saImmOiAdminOperationResult(avd_cb->immOiHandle,
						si->invocation, SA_AIS_OK);
				si->invocation = 0;
			}
		}
	}
}

void AVD_SG::set_adjust_state(SaAdjustState state) {
	TRACE("%s adjust_state %u => %u", name.value, adjust_state, state);
	adjust_state = state;
	m_AVSV_SEND_CKPT_UPDT_ASYNC_UPDT(avd_cb, this, AVSV_CKPT_SG_ADJUST_STATE);
}

void AVD_SG::set_admin_si(AVD_SI *si) {
	TRACE("%s admin_si set to %s", name.value, si->name.value);
	admin_si = si;
	m_AVSV_SEND_CKPT_UPDT_ASYNC_ADD(avd_cb, this, AVSV_CKPT_AVD_SG_ADMIN_SI);
}

void AVD_SG::clear_admin_si() {
	if (admin_si != NULL) {
		TRACE("%s admin_si cleared", name.value);
		m_AVSV_SEND_CKPT_UPDT_ASYNC_RMV(avd_cb, this, AVSV_CKPT_AVD_SG_ADMIN_SI);
		admin_si = NULL;
	}
}

void AVD_SG::for_all_su_set_readiness_state(SaAmfReadinessStateT state) {
	for (AVD_SU *su = list_of_su; su != NULL; su = su->sg_list_su_next)
		su->set_readiness_state(state);
}

bool AVD_SG::in_su_oper_list(const AVD_SU *i_su) {
	if (su_oper_list.su == i_su) {
		return true;
	} else if (su_oper_list.next != NULL) {
		AVD_SG_OPER *l_suopr = su_oper_list.next;
		while (l_suopr != NULL) {
			if (l_suopr->su == i_su)
				return true;

			l_suopr = l_suopr->next;
		}
	}

	return false;
}

uint32_t AVD_SG::su_oper_list_add(AVD_SU *su) {
	// TODO(hafe) move implementation here later when all uses this method
	return avd_sg_su_oper_list_add(avd_cb, su, false);
}

uint32_t AVD_SG::su_oper_list_del(AVD_SU *su) {
	// TODO(hafe) move implementation here later when all uses this method
	return avd_sg_su_oper_list_del(avd_cb, su, false);
}

/**
 *
 * @brief  This function verifies whether SU ranks are equal or not in an SG.
 *
 * @param[in]  avd_sg - pointer to service group
 *
 * @return     none
 *
 */
static void avd_verify_equal_ranked_su(AVD_SG *avd_sg)
{
	AVD_SU *pre_temp_su = NULL, *temp_su;

	TRACE_ENTER();
	/* Check ranks are still equal or not. Mark true in the begining*/
	avd_sg->equal_ranked_su = true;
	temp_su = avd_sg->list_of_su;
	while(temp_su) {
		if (pre_temp_su && temp_su->saAmfSURank != pre_temp_su->saAmfSURank) {
			avd_sg->equal_ranked_su = false;
			break;
		}
		pre_temp_su = temp_su;
		temp_su = temp_su->sg_list_su_next;
	}
	TRACE_LEAVE2("avd_sg->equal_ranked_su '%u'", avd_sg->equal_ranked_su);

	return;
}

/**
 * Adjust @a sg configuration attributes after it has been added to the model
 *
 * This function should only be called at the end of a configuration change
 * when all instances has been added to the model.
 * For more info see sai-im-xmi-a.04.02.xml
 */
void avd_sg_adjust_config(AVD_SG *sg)
{
	// SUs in an SG are equal, only need to look at the first one
	if ((sg->list_of_su != NULL) && !sg->list_of_su->saAmfSUPreInstantiable) {
		// NPI SUs can only be assigned one SI, see 3.2.1.1
		if ((sg->saAmfSGMaxActiveSIsperSU != static_cast<SaUint32T>(-1)) && (sg->saAmfSGMaxActiveSIsperSU > 1)) {
			LOG_WA("invalid saAmfSGMaxActiveSIsperSU (%u) for NPI SU '%s', adjusting...",
					sg->saAmfSGMaxActiveSIsperSU, sg->name.value);
		}
		sg->saAmfSGMaxActiveSIsperSU = 1;

		if ((sg->saAmfSGMaxStandbySIsperSU != static_cast<SaUint32T>(-1)) && (sg->saAmfSGMaxStandbySIsperSU > 1)) {
			LOG_WA("invalid saAmfSGMaxStandbySIsperSU (%u) for NPI SU '%s', adjusting...",
					sg->saAmfSGMaxStandbySIsperSU, sg->name.value);
		}
		sg->saAmfSGMaxStandbySIsperSU = 1;
		/* saAmfSUFailover must be true for a NPI SU sec 3.11.1.3.2 AMF-B.04.01 spec */
		for (AVD_SU *su = sg->list_of_su; su != NULL; su = su->sg_list_su_next) {
			if (!su->saAmfSUFailover) {
				su->set_su_failover(true);
			}
		}

	}

	/* adjust saAmfSGNumPrefAssignedSUs if not configured, only applicable for
	 * the N-way and N-way active redundancy models
	 */
	if ((sg->saAmfSGNumPrefAssignedSUs == 0) &&
			((sg->sg_type->saAmfSgtRedundancyModel == SA_AMF_N_WAY_REDUNDANCY_MODEL) ||
				(sg->sg_type->saAmfSgtRedundancyModel == SA_AMF_N_WAY_ACTIVE_REDUNDANCY_MODEL))) {
		sg->saAmfSGNumPrefAssignedSUs = sg->saAmfSGNumPrefInserviceSUs;
		LOG_NO("'%s' saAmfSGNumPrefAssignedSUs adjusted to %u",
				sg->name.value, sg->saAmfSGNumPrefAssignedSUs);
	}
}


/**
 * @brief Counts number of instantiated su in the sg.
 * @param Service Group
 *
 * @return Number of instantiated su in the sg.
 */
uint32_t sg_instantiated_su_count(const AVD_SG *sg)
{
	uint32_t inst_su_count;
	AVD_SU *su;

	for (su = sg->list_of_su, inst_su_count = 0; su != NULL; su = su->sg_list_su_next) {
		TRACE_1("su'%s', pres state'%u', in_serv'%u', PrefIn'%u'", su->name.value,
				su->saAmfSUPresenceState, su->saAmfSuReadinessState, sg->saAmfSGNumPrefInserviceSUs);
		if (((su->saAmfSUPresenceState == SA_AMF_PRESENCE_INSTANTIATED) ||
					(su->saAmfSUPresenceState == SA_AMF_PRESENCE_INSTANTIATING) ||
					(su->saAmfSUPresenceState == SA_AMF_PRESENCE_RESTARTING))) {
			inst_su_count ++;
		}
	}
	TRACE_LEAVE2("%u", inst_su_count);
	return inst_su_count;
}

// default implementation
SaAisErrorT AVD_SG::si_swap(AVD_SI *si, SaInvocationT invocation) {
	saflog(LOG_NOTICE, amfSvcUsrName,
		"SI SWAP is not supported for redundancy model %u", sg_redundancy_model);

	return SA_AIS_ERR_NOT_SUPPORTED;
}

// default implementation
void AVD_SG::ng_admin(AVD_SU* su, AVD_AMF_NG* ng) {
	return;
}
/**
 * @brief  Checks if SG is stable with respect to lock-in or unlock-on operation.  
 *
 * @param[in]  sg
 *
 * @return  true/false
 **/
bool sg_stable_after_lock_in_or_unlock_in(AVD_SG *sg)
{
	uint32_t instantiated_sus = 0, to_be_instantiated_sus = 0;
	SaAmfAdminStateT node_admin_state;

	switch (sg->adminOp) {
	case SA_AMF_ADMIN_LOCK_INSTANTIATION :
		for (AVD_SU *su = sg->list_of_su; su; su = su->sg_list_su_next) {
			if ((su->saAmfSUPresenceState != SA_AMF_PRESENCE_UNINSTANTIATED) &&
				(su->saAmfSUPresenceState != SA_AMF_PRESENCE_INSTANTIATION_FAILED) &&
				(su->saAmfSUPresenceState != SA_AMF_PRESENCE_TERMINATION_FAILED)) 
				return false;
		}
		break;
	case SA_AMF_ADMIN_UNLOCK_INSTANTIATION :
		/* Unlock-in of SG will not instantiate any component in NPI SU.*/
		if ((sg->list_of_su != NULL) && (sg->list_of_su->saAmfSUPreInstantiable == false))
			return true;
			
		for (AVD_SU *su = sg->list_of_su; su; su = su->sg_list_su_next) {
			node_admin_state = su->su_on_node->saAmfNodeAdminState;
			
			if ((su->saAmfSUPresenceState == SA_AMF_PRESENCE_INSTANTIATION_FAILED) ||
					(su->saAmfSUPresenceState == SA_AMF_PRESENCE_TERMINATION_FAILED))
				continue;

			if (su->saAmfSUPresenceState == SA_AMF_PRESENCE_INSTANTIATED)  {
				instantiated_sus++;
				continue;
			}

			if (node_admin_state == SA_AMF_ADMIN_LOCKED_INSTANTIATION)
				continue;
			
			if ((node_admin_state != SA_AMF_ADMIN_LOCKED_INSTANTIATION) && 
					(su->saAmfSUAdminState != SA_AMF_ADMIN_LOCKED_INSTANTIATION) &&
					(su->saAmfSUOperState == SA_AMF_OPERATIONAL_ENABLED) &&
					(su->su_on_node->node_state == AVD_AVND_STATE_PRESENT)) { 

				if (su->saAmfSUPresenceState == SA_AMF_PRESENCE_INSTANTIATING)
					return false;
				if (su->saAmfSUPresenceState == SA_AMF_PRESENCE_UNINSTANTIATED) {
					to_be_instantiated_sus++;
					continue;
				}
					
			}
			
		}

		if (instantiated_sus >= sg->saAmfSGNumPrefInserviceSUs)
			return true;
		else  {
			if (to_be_instantiated_sus == 0)
				return true;
			else
				return false;
		}

		break;
	default:
		TRACE("Called for wrong admin operation");
		break;
	}
		
	return true;
}
/*
 * @brief      Checks if SG has assignments only in the nodes of nodegroup.
 * @param[in]  ptr to Nodegroup (AVD_AMF_NG).   
 * @return     true/false.
 */
bool AVD_SG::is_sg_assigned_only_in_ng(const AVD_AMF_NG *ng)
{
	for (AVD_SU *su = list_of_su; su; su = su->sg_list_su_next) {
		if (su->list_of_susi == NULL)
			continue;
		//Return if this assigned su is not in this ng.
		if (node_in_nodegroup(Amf::to_string(&su->su_on_node->name), ng) == false)
			return false;
	}
	return true;
}
/*
 * @brief      Checks if there exists atleast one instantiable or inservice
 *             SU outside the nodegroup. 
 * @param[in]  ptr to Nodegroup (AVD_AMF_NG).
 * @return     true/false.
 */
bool AVD_SG::is_sg_serviceable_outside_ng(const AVD_AMF_NG *ng)
{
	for (AVD_SU *su = list_of_su; su; su = su->sg_list_su_next) {
		//Check if any su exists outside nodegroup for this sg. 
		if (node_in_nodegroup(Amf::to_string(&su->su_on_node->name), ng) == false) {
			/* 
			   During lock or shutdown operation on Node or SU, AMF
			   instantiates new SUs or it will switch-over existing 
			   assignments of locked Node or SU to other SUs.
			   To avoid service outage of complete SG check if 
			   there exists atleast one instantiable or atleast one 
			   in service SU outside the nodegroup.
			 */
			//TODO_NG:Evaluate if more parameters can be checked.
			if ((su->is_instantiable() == true) || 
					 (su->is_in_service() == true))
				return false;
		}
	}
	return true;
}
/**
 * @brief  Verify if SG is stable for admin operation on entity 
           Node, App and Nodegroup etc.
 * @param  ptr to SG(AVD_SG).
 * @Return SA_AIS_OK/SA_AIS_ERR_TRY_AGAIN/SA_AIS_ERR_BAD_OPERATION.
*/
SaAisErrorT AVD_SG::check_sg_stability()
{
	SaAisErrorT rc = SA_AIS_OK;
	if (sg_fsm_state != AVD_SG_FSM_STABLE) {
		LOG_NO("'%s' is in unstable/transition state", name.value);
		rc = SA_AIS_ERR_TRY_AGAIN;
		goto done;
	}
	if ((adminOp_invocationId != 0) || (adminOp != 0))  {
		LOG_NO("Admin operation is already going on '%s' ", name.value);
		rc = SA_AIS_ERR_TRY_AGAIN;
		goto done;
        }
	if (sg_is_tolerance_timer_running_for_any_si(this)) {
		LOG_NO("Tolerance timer is running for some of the SI's in the SG '%s',"
				" so differing admin opr", name.value);
		rc = SA_AIS_ERR_TRY_AGAIN;
                goto done;
        }
	if (csi_assignment_validate(this) == true) {
                LOG_NO("Single Csi assignment undergoing in '%s'", name.value);
		rc = SA_AIS_ERR_TRY_AGAIN;
                goto done;
        }
done:
	return rc;

}
