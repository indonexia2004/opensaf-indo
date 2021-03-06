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
 * Author(s): Ericsson AB
 *
 * This file implements the AMF interface for LGS.
 * The interface exist of one exported function: lgs_amf_init().
 * The AMF callback functions except a number of exported functions from
 * other modules.
 */

#include <nid_start_util.h>
#include "lgs.h"
#include "immutil.h"

extern struct ImmutilWrapperProfile immutilWrapperProfile;

static void close_all_files(void)
{
	log_stream_t *stream;

	stream = log_stream_getnext_by_name(NULL);
	while (stream != NULL) {
		if (log_stream_file_close(stream) != 0)
			LOG_WA("Could not close file for stream %s", stream->name);

		stream = log_stream_getnext_by_name(stream->name);
	}
}

/****************************************************************************
 * Name          : amf_active_state_handler
 *
 * Description   : This function is called upon receiving an active state
 *                 assignment from AMF.
 *
 * Arguments     : invocation - Designates a particular invocation.
 *                 cb         - A pointer to the LGS control block. 
 *
 * Return Values : None
 *
 * Notes         : None 
 *****************************************************************************/
static SaAisErrorT amf_active_state_handler(lgs_cb_t *cb, SaInvocationT invocation)
{
	log_stream_t *stream;
	SaAisErrorT error = SA_AIS_OK;

	TRACE_ENTER2("HA ACTIVE request");

	if (cb->ha_state == SA_AMF_HA_ACTIVE) {
		/* State change was already processed in RDA callback */
		goto done;
	}

	/* switch over, become implementer
	 */
	immutilWrapperProfile.nTries = 250; /* LOG will be blocked until IMM responds */
	immutilWrapperProfile.errorsAreFatal = 0;
	if ((error = immutil_saImmOiImplementerSet(lgs_cb->immOiHandle, "safLogService"))
			!= SA_AIS_OK) {
		LOG_ER("saImmOiClassImplementerSet (safLogService) failed: %d", error);
		goto done;
	}
	if ((error = immutil_saImmOiClassImplementerSet(lgs_cb->immOiHandle,
			"SaLogStreamConfig")) != SA_AIS_OK) {
		LOG_ER("saImmOiClassImplementerSet (SaLogStreamConfig) failed: %d", error);
		goto done;
	}
	/* Do this only if the class exists */
	if (*(bool*) lgs_imm_logconf_get(LGS_IMM_LOG_OPENSAFLOGCONFIG_CLASS_EXIST, NULL)) {
		if ((error = immutil_saImmOiClassImplementerSet(cb->immOiHandle, "OpenSafLogConfig"))
				!= SA_AIS_OK) {
			LOG_ER("saImmOiClassImplementerSet (OpenSafLogConfig) failed: %d", error);
			goto done;
		}
	}

	/* check existing streams */
	stream = log_stream_getnext_by_name(NULL);
	if (!stream)
		LOG_ER("No streams exist!");
	while (stream != NULL) {
		*stream->p_fd = -1; /* First Initialize fd */
		stream = log_stream_getnext_by_name(stream->name);
	}
	
 done:
	immutilWrapperProfile.nTries = 20; /* Reset retry time to more normal value. */
	immutilWrapperProfile.errorsAreFatal = 1;
	/* Update role independent of stream processing */
	lgs_cb->mds_role = V_DEST_RL_ACTIVE;
	TRACE_LEAVE();
	return error;
}

/****************************************************************************
 * Name          : amf_standby_state_handler
 *
 * Description   : This function is called upon receving an standby state
 *                 assignment from AMF.
 *
 * Arguments     : invocation - Designates a particular invocation.
 *                 cb         - A pointer to the LGS control block. 
 *
 * Return Values : None
 *
 * Notes         : None
 *****************************************************************************/
static SaAisErrorT amf_standby_state_handler(lgs_cb_t *cb, SaInvocationT invocation)
{
	SaAisErrorT error = SA_AIS_OK;

	TRACE_ENTER2("HA STANDBY request");

	cb->ha_state = SA_AMF_HA_STANDBY;
	cb->mds_role = V_DEST_RL_STANDBY;

	TRACE_LEAVE();
	return error;
}

/****************************************************************************
 * Name          : amf_quiescing_state_handler
 *
 * Description   : This function is called upon receving an Quiescing state
 *                 assignment from AMF.
 *
 * Arguments     : invocation - Designates a particular invocation.
 *                 cb         - A pointer to the LGS control block. 
 *
 * Return Values : None
 *
 * Notes         : None
 *****************************************************************************/
static SaAisErrorT amf_quiescing_state_handler(lgs_cb_t *cb, SaInvocationT invocation)
{
	TRACE_ENTER2("HA QUIESCING request");
	close_all_files();

	/* Give up our IMM OI implementer role */
	immutilWrapperProfile.errorsAreFatal = 0;
	(void)immutil_saImmOiImplementerClear(cb->immOiHandle);
	immutilWrapperProfile.errorsAreFatal = 1;

	return saAmfCSIQuiescingComplete(cb->amf_hdl, invocation, SA_AIS_OK);
}

/****************************************************************************
 * Name          : amf_quiesced_state_handler
 *
 * Description   : This function is called upon receving an Quiesced state
 *                 assignment from AMF.
 *
 * Arguments     : invocation - Designates a particular invocation.
 *                 cb         - A pointer to the LGS control block. 
 *
 * Return Values : None
 *
 * Notes         : None
 *****************************************************************************/
static SaAisErrorT amf_quiesced_state_handler(lgs_cb_t *cb, SaInvocationT invocation)
{
	V_DEST_RL mds_role;
	SaAisErrorT rc = SA_AIS_OK;

	TRACE_ENTER2("HA AMF QUIESCED STATE request");
	close_all_files();

	/* Give up our IMM OI implementer role */
	immutilWrapperProfile.errorsAreFatal = 0;
	(void)immutil_saImmOiImplementerClear(cb->immOiHandle);
	immutilWrapperProfile.errorsAreFatal = 1;

	/*
	 ** Change the MDS VDSET role to Quiesced. Wait for MDS callback with type
	 ** MDS_CALLBACK_QUIESCED_ACK. Then change MBCSv role. Don't change
	 ** cb->ha_state now.
	 */

	mds_role = cb->mds_role;
	cb->mds_role = V_DEST_RL_QUIESCED;
	if ((rc = lgs_mds_change_role(cb)) != NCSCC_RC_SUCCESS) {
		LOG_ER("lgs_mds_change_role FAILED");
		rc = SA_AIS_ERR_FAILED_OPERATION;
		cb->mds_role = mds_role;
		goto done;
	}

	cb->amf_invocation_id = invocation;
	cb->is_quiesced_set = true;
done:
	return rc;
}
/****************************************************************************
 * Name          : amf_health_chk_callback
 *
 * Description   : This is the callback function which will be called
 *                 when the AMF framework needs to health check for the component.
 *
 * Arguments     : invocation - Designates a particular invocation.
 *                 compName       - A pointer to the name of the component
 *                                  whose readiness state the Availability
 *                                  Management Framework is setting.
 *                 checkType      - The type of healthcheck to be executed.
 *
 * Return Values : None
 *
 * Notes         : None
 *****************************************************************************/
static void amf_health_chk_callback(SaInvocationT invocation,
		const SaNameT *compName, SaAmfHealthcheckKeyT *checkType)
{
	saAmfResponse(lgs_cb->amf_hdl, invocation, SA_AIS_OK);
}
/****************************************************************************
 * Name          : amf_csi_set_callback
 *
 * Description   : AMF callback function called 
 *                 when there is any change in the HA state.
 *
 * Arguments     : invocation     - This parameter designated a particular
 *                                  invocation of this callback function. The
 *                                  invoke process return invocation when it
 *                                  responds to the Availability Management
 *                                  FrameWork using the saAmfResponse()
 *                                  function.
 *                 compName       - A pointer to the name of the component
 *                                  whose readiness state the Availability
 *                                  Management Framework is setting.
 *                 haState        - The new HA state to be assumed by the
 *                                  component service instance identified by
 *                                  csiName.
 *                 csiDescriptor - This will indicate whether or not the
 *                                  component service instance for
 *                                  ativeCompName went through quiescing.
 *
 * Return Values : None.
 *
 * Notes         : None.
 *****************************************************************************/
static void amf_csi_set_callback(SaInvocationT invocation,
		const SaNameT *compName, SaAmfHAStateT new_haState,
		SaAmfCSIDescriptorT csiDescriptor)
{
	SaAisErrorT error = SA_AIS_OK;
	SaAmfHAStateT prev_haState;
	bool role_change = true;
	uint32_t rc = NCSCC_RC_SUCCESS;

	TRACE_ENTER();

	/*
	 *  Handle Active to Active role change.
	 */
	prev_haState = lgs_cb->ha_state;

	/* Invoke the appropriate state handler routine */
	switch (new_haState) {
	case SA_AMF_HA_ACTIVE:
		error = amf_active_state_handler(lgs_cb, invocation);
		break;
	case SA_AMF_HA_STANDBY:
		error = amf_standby_state_handler(lgs_cb, invocation);
		break;
	case SA_AMF_HA_QUIESCED:
		/* switch-over */
		error = amf_quiesced_state_handler(lgs_cb, invocation);
		break;
	case SA_AMF_HA_QUIESCING:
		/* shutdown admin op */
		error = amf_quiescing_state_handler(lgs_cb, invocation);
		break;
	default:
		LOG_WA("invalid state: %d ", new_haState);
		error = SA_AIS_ERR_FAILED_OPERATION;
		break;
	}

	if (error != SA_AIS_OK)
		goto response;

	if (new_haState == SA_AMF_HA_QUIESCED) {
		/* AMF response will be done later when MDS quiesced ack has been received */
		goto done;
	}

	/* Update control block */
	lgs_cb->ha_state = new_haState;

	if (lgs_cb->csi_assigned == false) {
		lgs_cb->csi_assigned = true;
		/* We shall open checkpoint only once in our life time. currently doing at lib init  */
	} else if ((new_haState == SA_AMF_HA_ACTIVE) || (new_haState == SA_AMF_HA_STANDBY)) {	/* It is a switch over */
		lgs_cb->ckpt_state = COLD_SYNC_IDLE;
		/* NOTE: This behaviour has to be checked later, when scxb redundancy is available 
		 * Also, change role of mds, mbcsv during quiesced has to be done after mds
		 * supports the same.  TBD
		 */
	}

	/* Handle active to active role change. */
	if ((prev_haState == SA_AMF_HA_ACTIVE) && (new_haState == SA_AMF_HA_ACTIVE))
		role_change = false;

	/* Handle Stby to Stby role change. */
	if ((prev_haState == SA_AMF_HA_STANDBY) && (new_haState == SA_AMF_HA_STANDBY))
		role_change = false;

	if (role_change == true) {
		if ((rc = lgs_mds_change_role(lgs_cb)) != NCSCC_RC_SUCCESS) {
			LOG_ER("lgs_mds_change_role FAILED");
			error = SA_AIS_ERR_FAILED_OPERATION;
			goto response;
		}

		/* Inform MBCSV of HA state change */
		if (NCSCC_RC_SUCCESS != (error = lgs_mbcsv_change_HA_state(lgs_cb)))
			error = SA_AIS_ERR_FAILED_OPERATION;
	}

	if ((role_change == true) && (new_haState == SA_AMF_HA_ACTIVE)) {
		/* Read log configuration object and update mailbox limits.
		 * Mailbox limits may have been changed.
		 * Note: Also see rda callback in lgs_evt.c
		 */
		update_mailbox_limits();
	}
	
 response:
	saAmfResponse(lgs_cb->amf_hdl, invocation, error);
 done:
	TRACE_LEAVE();
}

/****************************************************************************
 * Name          : amf_comp_terminate_callback
 *
 * Description   : This is the callback function which will be called
 *                 when the AMF framework needs to terminate LGS. This does
 *                 all required to destroy LGS(except to unregister from AMF)
 *
 * Arguments     : invocation     - This parameter designated a particular
 *                                  invocation of this callback function. The
 *                                  invoke process return invocation when it
 *                                  responds to the Availability Management
 *                                  FrameWork using the saAmfResponse()
 *                                  function.
 *                 compName       - A pointer to the name of the component
 *                                  whose readiness state the Availability
 *                                  Management Framework is setting.
 *
 * Return Values : None
 *
 * Notes         : None
 *****************************************************************************/
static void amf_comp_terminate_callback(SaInvocationT invocation, const SaNameT *compName)
{
	TRACE_ENTER();

	saAmfResponse(lgs_cb->amf_hdl, invocation, SA_AIS_OK);

	/* Detach from IPC */
	m_NCS_IPC_DETACH(&lgs_mbx, NULL, lgs_cb);

	/* Disconnect from MDS */
	lgs_mds_finalize(lgs_cb);
	sleep(1);
	LOG_NO("Received AMF component terminate callback, exiting");
	exit(0);
}

/****************************************************************************
 * Name          : amf_csi_rmv_callback
 *
 * Description   : This callback routine is invoked by AMF during a
 *                 CSI set removal operation. 
 *
 * Arguments     : invocation     - This parameter designated a particular
 *                                  invocation of this callback function. The
 *                                  invoke process return invocation when it
 *                                  responds to the Availability Management
 *                                  FrameWork using the saAmfResponse()
 *                                  function.
 *                 compName       - A pointer to the name of the component
 *                                  whose readiness state the Availability
 *                                  Management Framework is setting.
 *                 csiName        - A const pointer to csiName
 *                 csiFlags       - csi Flags
 * Return Values : None 
 *****************************************************************************/
static void amf_csi_rmv_callback(SaInvocationT invocation,
				 const SaNameT *compName, const SaNameT *csiName, const SaAmfCSIFlagsT csiFlags)
{
	TRACE_ENTER();
	saAmfResponse(lgs_cb->amf_hdl, invocation, SA_AIS_OK);
	TRACE_LEAVE();
}

/*****************************************************************************\
 *  Name:          lgs_healthcheck_start                           * 
 *                                                                            *
 *  Description:   To start the health check                                  *
 *                                                                            *
 *  Arguments:     NCSSA_CB* - Control Block                                  * 
 *                                                                            * 
 *  Returns:       SA_AIS_OK    - everything is OK                            *
 *                 SA_AIS_ERR_* -  failure                                    *
 *  NOTE:                                                                     * 
\******************************************************************************/
static SaAisErrorT amf_healthcheck_start(lgs_cb_t *lgs_cb)
{
	SaAisErrorT error;
	SaAmfHealthcheckKeyT healthy;
	char *health_key;

	TRACE_ENTER();

    /** start the AMF health check **/
	memset(&healthy, 0, sizeof(healthy));
	health_key = getenv("LGSV_ENV_HEALTHCHECK_KEY");

	if (health_key == NULL)
		strcpy((char *)healthy.key, "F1B2");
	else
		strcpy((char *)healthy.key, health_key);

	healthy.keyLen = strlen((char *)healthy.key);

	error = saAmfHealthcheckStart(lgs_cb->amf_hdl, &lgs_cb->comp_name, &healthy,
				      SA_AMF_HEALTHCHECK_AMF_INVOKED, SA_AMF_COMPONENT_FAILOVER);

	if (error != SA_AIS_OK)
		LOG_ER("saAmfHealthcheckStart FAILED: %u", error);

	TRACE_LEAVE();
	return error;
}

/**************************************************************************
 Function: lgs_amf_register

 Purpose:  Function which registers LGS with AMF.  

 Input:    None 

 Returns:  SA_AIS_OK    - everything is OK
           SA_AIS_ERR_* -  failure

**************************************************************************/
SaAisErrorT lgs_amf_init(lgs_cb_t *cb)
{
	SaAmfCallbacksT amfCallbacks;
	SaVersionT amf_version;
	SaAisErrorT error;

	TRACE_ENTER();

	if (cb->nid_started &&
		amf_comp_name_get_set_from_file("LOGD_COMP_NAME_FILE",
		&cb->comp_name) != NCSCC_RC_SUCCESS) {
		error = SA_AIS_ERR_NOT_EXIST;
		goto done;
	}

	/* Initialize AMF callbacks */
	memset(&amfCallbacks, 0, sizeof(SaAmfCallbacksT));
	amfCallbacks.saAmfHealthcheckCallback = amf_health_chk_callback;
	amfCallbacks.saAmfCSISetCallback = amf_csi_set_callback;
	amfCallbacks.saAmfComponentTerminateCallback = amf_comp_terminate_callback;
	amfCallbacks.saAmfCSIRemoveCallback = amf_csi_rmv_callback;

	amf_version.releaseCode = 'B';
	amf_version.majorVersion = 0x01;
	amf_version.minorVersion = 0x01;

	/* Initialize the AMF library */
	error = saAmfInitialize(&cb->amf_hdl, &amfCallbacks, &amf_version);
	if (error != SA_AIS_OK) {
		LOG_ER("saAmfInitialize() FAILED: %u", error);
		goto done;
	}

	/* Obtain the AMF selection object to wait for AMF events */
	error = saAmfSelectionObjectGet(cb->amf_hdl, &cb->amfSelectionObject);
	if (error != SA_AIS_OK) {
		LOG_ER("saAmfSelectionObjectGet() FAILED: %u", error);
		goto done;
	}

	/* Get the component name */
	error = saAmfComponentNameGet(cb->amf_hdl, &cb->comp_name);
	if (error != SA_AIS_OK) {
		LOG_ER("saAmfComponentNameGet() FAILED: %u", error);
		goto done;
	}

	/* Register component with AMF */
	error = saAmfComponentRegister(cb->amf_hdl, &cb->comp_name, (SaNameT *)NULL);
	if (error != SA_AIS_OK) {
		LOG_ER("saAmfComponentRegister() FAILED");
		goto done;
	}

	/* Start AMF healthchecks */
	if ((error = amf_healthcheck_start(cb)) != SA_AIS_OK){
		LOG_ER("amf_healthcheck_start() failed with error %u",error);
		goto done;
	}

 done:
	TRACE_LEAVE();
	return error;
}
