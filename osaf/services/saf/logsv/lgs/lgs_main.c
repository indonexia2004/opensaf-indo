/*      -*- OpenSAF  -*-
 *
 * (C) Copyright 2008-2010 The OpenSAF Foundation
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
 *            Wind River Systems
 *
 */

/* ========================================================================
 *   INCLUDE FILES
 * ========================================================================
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <poll.h>

#include <configmake.h>
#include <rda_papi.h>
#include <daemon.h>
#include <nid_api.h>
#include <ncs_main_papi.h>

#include "lgs.h"
#include "lgs_util.h"
#include "lgs_file.h"
#include "osaf_utility.h"

/* ========================================================================
 *   DEFINITIONS
 * ========================================================================
 */

#define FD_TERM 0
#define FD_AMF 1
#define FD_MBCSV 2
#define FD_MBX 3
#define FD_IMM 4		/* Must be the last in the fds array */

#ifndef LOG_STREAM_LOW_LIMIT_PERCENT
#define LOG_STREAM_LOW_LIMIT_PERCENT 0.6 // default value for low is 60%
#endif

/* ========================================================================
 *   TYPE DEFINITIONS
 * ========================================================================
 */

/* ========================================================================
 *   DATA DECLARATIONS
 * ========================================================================
 */

static lgs_cb_t _lgs_cb;
lgs_cb_t *lgs_cb = &_lgs_cb;
SYSF_MBX lgs_mbx; /* LGS's mailbox */

/* Upper limit where the queue enters FULL state */
uint32_t mbox_high[NCS_IPC_PRIORITY_MAX];

/* Current number of messages in queue, IPC maintained */
uint32_t mbox_msgs[NCS_IPC_PRIORITY_MAX];

/* Queue state FULL or not */
bool mbox_full[NCS_IPC_PRIORITY_MAX];

/* Lower limit which determines when to leave FULL state */
uint32_t mbox_low[NCS_IPC_PRIORITY_MAX];

/* The mailbox and mailbox handling variables (limits) may be reinitialized
 * in runtime. This happen in the main thread. The mailbox and variables are
 * used in the mds thread.
 */
pthread_mutex_t lgs_mbox_init_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct pollfd fds[5];
static nfds_t nfds = 5;
static NCS_SEL_OBJ usr1_sel_obj;

/* ========================================================================
 *   FUNCTION PROTOTYPES
 * ========================================================================
 */

/**
 * Callback from RDA. Post a message/event to the lgs mailbox.
 * @param cb_hdl
 * @param cb_info
 * @param error_code
 */
static void rda_cb(uint32_t cb_hdl, PCS_RDA_CB_INFO *cb_info, PCSRDA_RETURN_CODE error_code)
{
	uint32_t rc;
	lgsv_lgs_evt_t *evt;

	TRACE_ENTER();

	evt = calloc(1, sizeof(lgsv_lgs_evt_t));
	if (NULL == evt) {
		LOG_ER("calloc failed");
		goto done;
	}

	evt->evt_type = LGSV_EVT_RDA;
	evt->info.rda_info.io_role = cb_info->info.io_role;

	rc = ncs_ipc_send(&lgs_mbx, (NCS_IPC_MSG *)evt, LGS_IPC_PRIO_CTRL_MSGS);
	if (rc != NCSCC_RC_SUCCESS)
		LOG_ER("IPC send failed %d", rc);

done:
	TRACE_LEAVE();
}

/**
 * USR1 signal is used when AMF wants instantiate us as a
 * component. Wake up the main thread so it can register with
 * AMF.
 * 
 * @param i_sig_num
 */
static void sigusr1_handler(int sig)
{
	(void)sig;
	signal(SIGUSR1, SIG_IGN);
	ncs_sel_obj_ind(&usr1_sel_obj);
	TRACE("Got USR1 signal");
}

/**
 * Configure mailbox properties from configuration variables
 * Low limit is by default configured as a percentage of the
 * high limit (if not configured explicitly).
 * 
 * @return uint32_t
 */
uint32_t lgs_configure_mailbox(void)
{
	uint32_t limit = 0;
	bool errorflag;
	uint32_t rc = NCSCC_RC_SUCCESS;

	TRACE_ENTER();
	/* Do not initialize if the mailbox is being used in the mds thread. Wait
	 * until done.
	 */
	osaf_mutex_lock_ordie(&lgs_mbox_init_mutex);
	
	limit = *(uint32_t*) lgs_imm_logconf_get(LGS_IMM_LOG_STREAM_SYSTEM_HIGH_LIMIT, &errorflag);
	if (errorflag != false) {
		LOG_ER("Illegal value for LOG_STREAM_SYSTEM_HIGH_LIMIT - %s", strerror(errno));
		rc = NCSCC_RC_FAILURE;
		goto done;
	}

	mbox_high[LGS_IPC_PRIO_SYS_STREAM] = limit;
	mbox_low[LGS_IPC_PRIO_SYS_STREAM] = LOG_STREAM_LOW_LIMIT_PERCENT * limit;

	ncs_ipc_config_max_msgs(&lgs_mbx, LGS_IPC_PRIO_SYS_STREAM,
			mbox_high[LGS_IPC_PRIO_SYS_STREAM]);
	ncs_ipc_config_usr_counters(&lgs_mbx, LGS_IPC_PRIO_SYS_STREAM,
			&mbox_msgs[LGS_IPC_PRIO_SYS_STREAM]);

	if (limit != 0) {
		limit = *(uint32_t*) lgs_imm_logconf_get(LGS_IMM_LOG_STREAM_SYSTEM_LOW_LIMIT, &errorflag);
		if (errorflag != false) {
			LOG_ER("Illegal value for LOG_STREAM_SYSTEM_LOW_LIMIT - %s", strerror(errno));
			rc = NCSCC_RC_FAILURE;
			goto done;
		}

		mbox_low[LGS_IPC_PRIO_SYS_STREAM] = limit;
	}

	limit = *(uint32_t*) lgs_imm_logconf_get(LGS_IMM_LOG_STREAM_APP_HIGH_LIMIT, &errorflag);
	if (errorflag != false) {
		LOG_ER("Illegal value for LOG_STREAM_APP_HIGH_LIMIT - %s", strerror(errno));
		rc = NCSCC_RC_FAILURE;
		goto done;
	}

	mbox_high[LGS_IPC_PRIO_APP_STREAM] = limit;
	mbox_low[LGS_IPC_PRIO_APP_STREAM] = LOG_STREAM_LOW_LIMIT_PERCENT * limit;

	ncs_ipc_config_max_msgs(&lgs_mbx, LGS_IPC_PRIO_APP_STREAM,
			mbox_high[LGS_IPC_PRIO_APP_STREAM]);
	ncs_ipc_config_usr_counters(&lgs_mbx, LGS_IPC_PRIO_APP_STREAM,
			&mbox_msgs[LGS_IPC_PRIO_APP_STREAM]);

	if (limit != 0) {
		limit = *(uint32_t*) lgs_imm_logconf_get(LGS_IMM_LOG_STREAM_APP_LOW_LIMIT, &errorflag);
		if (errorflag != false) {
			LOG_ER("Illegal value for LOG_STREAM_APP_LOW_LIMIT - %s", strerror(errno));
			rc = NCSCC_RC_FAILURE;
			goto done;
		}

		mbox_low[LGS_IPC_PRIO_APP_STREAM] = limit;
	}

	TRACE("sys low:%u, high:%u", mbox_low[LGS_IPC_PRIO_SYS_STREAM], mbox_high[LGS_IPC_PRIO_SYS_STREAM]);
	TRACE("app low:%u, high:%u", mbox_low[LGS_IPC_PRIO_APP_STREAM], mbox_high[LGS_IPC_PRIO_APP_STREAM]);

	osaf_mutex_unlock_ordie(&lgs_mbox_init_mutex);
	
	done:
	
	TRACE_LEAVE2("rc = %d", rc);
	return rc;
}

/**
 * Initialize log
 * 
 * @return uns32
 */
static uint32_t log_initialize(void)
{
	uint32_t rc = NCSCC_RC_FAILURE;

	TRACE_ENTER();

	if (lgs_file_init() != NCSCC_RC_SUCCESS) {
		LOG_ER("lgs_file_init FAILED");
		goto done;
	}

	/* Determine how this process was started, by NID or AMF */
	if (getenv("SA_AMF_COMPONENT_NAME") == NULL)
		lgs_cb->nid_started = true;

	if (ncs_agents_startup() != NCSCC_RC_SUCCESS) {
		LOG_ER("ncs_agents_startup FAILED");
		goto done;
	}

	/* Initialize lgs control block */
	if (lgs_cb_init(lgs_cb) != NCSCC_RC_SUCCESS) {
		LOG_ER("lgs_cb_init FAILED");
		goto done;
	}

	if ((rc = rda_get_role(&lgs_cb->ha_state)) != NCSCC_RC_SUCCESS) {
		LOG_ER("rda_get_role FAILED");
		goto done;
	}

	if ((rc = rda_register_callback(0, rda_cb)) != NCSCC_RC_SUCCESS) {
		LOG_ER("rda_register_callback FAILED %u", rc);
		goto done;
	}

	/*
	 * Get LOGSV root directory path. All created log files will be stored
	 * relative to this directory as described in spec.
	 */
	bool errorflag;
	lgs_cb->logsv_root_dir = lgs_imm_logconf_get(LGS_IMM_LOG_ROOT_DIRECTORY, &errorflag);

	if (errorflag != false) {
		LOG_ER("Valid LOGSV_ROOT_DIRECTORY not found");
		goto done;
	}
	LOG_NO("log root directory is: %s", lgs_cb->logsv_root_dir);

	lgs_cb->logsv_data_groupname = lgs_imm_logconf_get(LGS_IMM_DATA_GROUPNAME, &errorflag);
	if (errorflag != false) {
		LOG_ER("Valid LOGSV_LOG_DATA_GROUPNAME not found");
		goto done;
	}
	LOG_NO("LOG data group is: %s", lgs_cb->logsv_data_groupname);

	/* Initialize stream class */
	if (log_stream_init() != NCSCC_RC_SUCCESS) {
		LOG_ER("log_stream_init FAILED");
		goto done;
	}

	m_NCS_EDU_HDL_INIT(&lgs_cb->edu_hdl);

	/* Create the mailbox used for communication with LGS */
	if ((rc = m_NCS_IPC_CREATE(&lgs_mbx)) != NCSCC_RC_SUCCESS) {
		LOG_ER("m_NCS_IPC_CREATE FAILED %d", rc);
		goto done;
	}

	/* Attach mailbox to this thread */
	if ((rc = m_NCS_IPC_ATTACH(&lgs_mbx) != NCSCC_RC_SUCCESS)) {
		LOG_ER("m_NCS_IPC_ATTACH FAILED %d", rc);
		goto done;
	}

	if (lgs_configure_mailbox() != NCSCC_RC_SUCCESS) {
		LOG_ER("configure_mailbox FAILED");
		goto done;
	}

	if ((rc = lgs_mds_init(lgs_cb)) != NCSCC_RC_SUCCESS) {
		LOG_ER("lgs_mds_init FAILED %d", rc);
		goto done;
	}

	if ((rc = lgs_mbcsv_init(lgs_cb)) != NCSCC_RC_SUCCESS) {
		LOG_ER("lgs_mbcsv_init FAILED");
		goto done;
	}

	if ((rc = lgs_imm_init(lgs_cb)) != SA_AIS_OK) {
		LOG_ER("lgs_imm_init FAILED");
		goto done;
	}

	/* Create a selection object */
	if (lgs_cb->nid_started &&
		(rc = ncs_sel_obj_create(&usr1_sel_obj)) != NCSCC_RC_SUCCESS) {
		LOG_ER("ncs_sel_obj_create failed");
		goto done;
	}

	/*
	 * Initialize a signal handler that will use the selection object.
	 * The signal is sent from our script when AMF does instantiate.
	 */
	if (lgs_cb->nid_started &&
		signal(SIGUSR1, sigusr1_handler) == SIG_ERR) {
		LOG_ER("signal USR1 failed: %s", strerror(errno));
		rc = NCSCC_RC_FAILURE;
		goto done;
	}

	if (lgs_cb->ha_state == SA_AMF_HA_ACTIVE) {
		if (lgs_imm_create_configStream(lgs_cb) != SA_AIS_OK) {
			LOG_ER("lgs_imm_create_configStream FAILED");
			rc = NCSCC_RC_FAILURE;
			goto done;
		}
	}

	/* If AMF started register immediately */
	if (!lgs_cb->nid_started && lgs_amf_init(lgs_cb) != SA_AIS_OK) {
		rc = NCSCC_RC_FAILURE;
		goto done;
	}

done:
	if (lgs_cb->nid_started &&
	    nid_notify("LOGD", rc, NULL) != NCSCC_RC_SUCCESS) {
		LOG_ER("nid_notify failed");
		rc = NCSCC_RC_FAILURE;
	}

	TRACE_LEAVE();
	return (rc);
}

/**
 * Wait a while on IMM and initialize, sel obj get & implementer
 * set.
 * 
 * @param _cb
 * 
 * @return void*
 */
static void *imm_reinit_thread(void *_cb)
{
	SaAisErrorT error;
	lgs_cb_t *cb = (lgs_cb_t *)_cb;
	lgsv_lgs_evt_t *lgsv_evt;

	TRACE_ENTER();

	if ((error = lgs_imm_init(cb)) != SA_AIS_OK) {
		LOG_ER("lgs_imm_init FAILED: %u", error);
		exit(EXIT_FAILURE);
	}

	lgs_imm_impl_set(cb);

	/* Wake up the main thread so it discovers the new imm descriptor. */
	lgsv_evt = calloc(1, sizeof(lgsv_lgs_evt_t));
	osafassert(lgsv_evt);
	lgsv_evt->evt_type = LGSV_EVT_NO_OP;
	if (m_NCS_IPC_SEND(&lgs_mbx, lgsv_evt, LGS_IPC_PRIO_CTRL_MSGS) !=
		NCSCC_RC_SUCCESS) {
		LOG_WA("imm_reinit_thread failed to send IPC message to main thread");
		/*
		 * Se no reason why thos would happen. But if it does at least there
		 * is something in the syslog. The main thread should still pick up
		 * the new imm FD when there is a healthcheck, but it could take
		 *minutes.
		 */
		free(lgsv_evt);
	}

	TRACE_LEAVE();
	return NULL;
}

/**
 * Start a background thread to do IMM reinitialization.
 * 
 * @param cb
 */
static void imm_reinit_bg(lgs_cb_t *cb)
{
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	TRACE_ENTER();
	if (pthread_create(&thread, &attr, imm_reinit_thread, cb) != 0) {
		LOG_ER("pthread_create FAILED: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	pthread_attr_destroy(&attr);
	
	TRACE_LEAVE();
}

/**
 * The main routine for the lgs daemon.
 * @param argc
 * @param argv
 * 
 * @return int
 */
int main(int argc, char *argv[])
{
	NCS_SEL_OBJ mbx_fd;
	SaAisErrorT error = SA_AIS_OK;
	uint32_t rc;
	int term_fd;

	daemonize(argc, argv);
	
	if (log_initialize() != NCSCC_RC_SUCCESS) {
		LOG_ER("log_initialize failed");
		goto done;
	}
	
	mbx_fd = ncs_ipc_get_sel_obj(&lgs_mbx);
	daemon_sigterm_install(&term_fd);

	/* Set up all file descriptors to listen to */
	fds[FD_TERM].fd = term_fd;
	fds[FD_TERM].events = POLLIN;
	fds[FD_AMF].fd = lgs_cb->nid_started ?
		usr1_sel_obj.rmv_obj : lgs_cb->amfSelectionObject;
	fds[FD_AMF].events = POLLIN;
	fds[FD_MBCSV].fd = lgs_cb->mbcsv_sel_obj;
	fds[FD_MBCSV].events = POLLIN;
	fds[FD_MBX].fd = mbx_fd.rmv_obj;
	fds[FD_MBX].events = POLLIN;
	fds[FD_IMM].fd = lgs_cb->immSelectionObject;
	fds[FD_IMM].events = POLLIN;

	while (1) {

		if (lgs_cb->immOiHandle != 0) {
			fds[FD_IMM].fd = lgs_cb->immSelectionObject;
			fds[FD_IMM].events = POLLIN;
			nfds = FD_IMM + 1;
		} else {
			nfds = FD_IMM;
		}

		int ret = poll(fds, nfds, -1);

		if (ret == -1) {
			if (errno == EINTR)
				continue;

			LOG_ER("poll failed - %s", strerror(errno));
			break;
		}

		if (fds[FD_TERM].revents & POLLIN) {
			daemon_exit();
		}

		if (fds[FD_AMF].revents & POLLIN) {
			if (lgs_cb->amf_hdl != 0) {
				if ((error = saAmfDispatch(lgs_cb->amf_hdl, SA_DISPATCH_ALL)) != SA_AIS_OK) {
					LOG_ER("saAmfDispatch failed: %u", error);
					break;
				}
			} else {
				TRACE("SIGUSR1 event rec");
				ncs_sel_obj_rmv_ind(&usr1_sel_obj, true, true);
				ncs_sel_obj_destroy(&usr1_sel_obj);

				if (lgs_amf_init(lgs_cb) != NCSCC_RC_SUCCESS)
					break;

				TRACE("AMF Initialization SUCCESS......");
				fds[FD_AMF].fd = lgs_cb->amfSelectionObject;
			}
		}

		if (fds[FD_MBCSV].revents & POLLIN) {
			if ((rc = lgs_mbcsv_dispatch(lgs_cb->mbcsv_hdl)) != NCSCC_RC_SUCCESS) {
				LOG_ER("MBCSv Dispatch Failed");
				break;
			}
		}

		if (fds[FD_MBX].revents & POLLIN)
			lgs_process_mbx(&lgs_mbx);

		if (lgs_cb->immOiHandle && fds[FD_IMM].revents & POLLIN) {
			error = saImmOiDispatch(lgs_cb->immOiHandle, SA_DISPATCH_ALL);

			/*
			 * BAD_HANDLE is interpreted as an IMM service restart. Try 
			 * reinitialize the IMM OI API in a background thread and let 
			 * this thread do business as usual especially handling write 
			 * requests.
			 *
			 * All other errors are treated as non-recoverable (fatal) and will
			 * cause an exit of the process.
			 */
			if (error == SA_AIS_ERR_BAD_HANDLE) {
				TRACE("saImmOiDispatch returned BAD_HANDLE");

				/* 
				 * Invalidate the IMM OI handle, this info is used in other
				 * locations. E.g. giving TRY_AGAIN responses to a create and
				 * close app stream requests. That is needed since the IMM OI
				 * is used in context of these functions.
				 * 
				 * Also closing the handle. Finalize is ok with a bad handle
				 * that is bad because it is stale and this actually clears
				 * the handle from internal agent structures.  In any case
				 * we ignore the return value from Finalize here.
				 */
				saImmOiFinalize(lgs_cb->immOiHandle);
				lgs_cb->immOiHandle = 0;

				/* Initiate IMM reinitializtion in the background */
				imm_reinit_bg(lgs_cb);
			} else if (error != SA_AIS_OK) {
				LOG_ER("saImmOiDispatch FAILED: %u", error);
				break;
			}
		}
	}

done:
	LOG_ER("Failed, exiting...");
	TRACE_LEAVE();
	exit(1);
}
