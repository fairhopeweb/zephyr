/*
 * Copyright (c) 2021 Demant
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>

#include "util/mem.h"
#include "util/memq.h"
#include "util/mayfly.h"
#include "ticker/ticker.h"
#include "hal/ccm.h"
#include "hal/ticker.h"

#include "pdu.h"
#include "lll.h"
#include "lll_conn.h"
#include "lll_clock.h"
#include "lll_peripheral_iso.h"
#include "lll_central_iso.h"

#if !defined(CONFIG_BT_LL_SW_LLCP_LEGACY)
#include "ull_tx_queue.h"
#endif

#include "isoal.h"
#include "ull_iso_types.h"
#include "ull_iso_internal.h"
#include "ull_conn_types.h"
#include "lll_conn_iso.h"
#include "ull_conn_iso_types.h"
#include "ull_conn_internal.h"
#include "ull_conn_iso_internal.h"
#include "ull_peripheral_iso_internal.h"
#include "ull_internal.h"
#include "lll/lll_vendor.h"

#include "ll.h"

#include <zephyr/bluetooth/hci.h>

#include "hal/debug.h"

/* Used by LISTIFY */
#define _INIT_MAYFLY_ARRAY(_i, _l, _fp) \
	{ ._link = &_l[_i], .fp = _fp },

/* Declare static initialized array of mayflies with associated link element */
#define DECLARE_MAYFLY_ARRAY(_name, _fp, _cnt) \
	static memq_link_t _links[_cnt]; \
	static struct mayfly _name[_cnt] = \
		{ LISTIFY(_cnt, _INIT_MAYFLY_ARRAY, (), _links, _fp) }


static int init_reset(void);
static void ticker_update_cig_op_cb(uint32_t status, void *param);
static void ticker_resume_op_cb(uint32_t status, void *param);
static void ticker_resume_cb(uint32_t ticks_at_expire, uint32_t ticks_drift,
			     uint32_t remainder, uint16_t lazy, uint8_t force,
			     void *param);
static void cis_disabled_cb(void *param);
static void ticker_stop_op_cb(uint32_t status, void *param);
static void cig_disable(void *param);
static void cig_disabled_cb(void *param);
static void disable(uint16_t handle);
static void cis_tx_lll_flush(void *param);

static struct ll_conn_iso_stream cis_pool[CONFIG_BT_CTLR_CONN_ISO_STREAMS];
static void *cis_free;

static struct ll_conn_iso_group cig_pool[CONFIG_BT_CTLR_CONN_ISO_GROUPS];
static void *cig_free;

/* BT. 5.3 Spec - Vol 4, Part E, Sect 6.7 */
#define CONN_ACCEPT_TIMEOUT_DEFAULT 0x1F40
#define CONN_ACCEPT_TIMEOUT_MAX     0xB540
#define CONN_ACCEPT_TIMEOUT_MIN     0x0001
static uint16_t conn_accept_timeout;

struct ll_conn_iso_group *ll_conn_iso_group_acquire(void)
{
	return mem_acquire(&cig_free);
}

void ll_conn_iso_group_release(struct ll_conn_iso_group *cig)
{
	cig->cig_id  = 0xFF;
	cig->started = 0;

	mem_release(cig, &cig_free);
}

uint16_t ll_conn_iso_group_handle_get(struct ll_conn_iso_group *cig)
{
	return mem_index_get(cig, cig_pool, sizeof(struct ll_conn_iso_group));
}

struct ll_conn_iso_group *ll_conn_iso_group_get(uint16_t handle)
{
	return mem_get(cig_pool, sizeof(struct ll_conn_iso_group), handle);
}

struct ll_conn_iso_group *ll_conn_iso_group_get_by_id(uint8_t id)
{
	struct ll_conn_iso_group *cig;

	for (int h = 0; h < CONFIG_BT_CTLR_CONN_ISO_GROUPS; h++) {
		cig = ll_conn_iso_group_get(h);
		if (id == cig->cig_id) {
			return cig;
		}
	}

	return NULL;
}

struct ll_conn_iso_stream *ll_conn_iso_stream_acquire(void)
{
	struct ll_conn_iso_stream *cis = mem_acquire(&cis_free);

	if (cis) {
		(void)memset(&cis->hdr, 0U, sizeof(cis->hdr));
	}

	return cis;
}

void ll_conn_iso_stream_release(struct ll_conn_iso_stream *cis)
{
	cis->cis_id = 0;
	cis->group = NULL;

	mem_release(cis, &cis_free);
}

uint16_t ll_conn_iso_stream_handle_get(struct ll_conn_iso_stream *cis)
{
	return mem_index_get(cis, cis_pool,
			     sizeof(struct ll_conn_iso_stream)) +
			     LL_CIS_HANDLE_BASE;
}

struct ll_conn_iso_stream *ll_conn_iso_stream_get(uint16_t handle)
{
	return mem_get(cis_pool, sizeof(struct ll_conn_iso_stream), handle -
		       LL_CIS_HANDLE_BASE);
}

struct ll_conn_iso_stream *ll_iso_stream_connected_get(uint16_t handle)
{
	struct ll_conn_iso_stream *cis;

	if (handle >= CONFIG_BT_CTLR_CONN_ISO_STREAMS +
		      LL_CIS_HANDLE_BASE) {
		return NULL;
	}

	cis = ll_conn_iso_stream_get(handle);
	if ((cis->group == NULL) || (cis->lll.handle != handle)) {
		/* CIS does not belong to a group or has inconsistent handle */
		return NULL;
	}

	return cis;
}

struct ll_conn_iso_stream *ll_conn_iso_stream_get_by_acl(struct ll_conn *conn, uint16_t *cis_iter)
{
	uint8_t cis_iter_start = (cis_iter == NULL) || (*cis_iter) == UINT16_MAX;
	uint8_t cig_handle;

	/* Find CIS associated with ACL conn */
	for (cig_handle = 0; cig_handle < CONFIG_BT_CTLR_CONN_ISO_GROUPS; cig_handle++) {
		struct ll_conn_iso_stream *cis;
		struct ll_conn_iso_group *cig;
		uint16_t handle_iter;
		int8_t cis_idx;

		cig = ll_conn_iso_group_get(cig_handle);
		if (!cig) {
			continue;
		}

		handle_iter = UINT16_MAX;

		for (cis_idx = 0; cis_idx < cig->lll.num_cis; cis_idx++) {
			cis = ll_conn_iso_stream_get_by_group(cig, &handle_iter);
			LL_ASSERT(cis);

			uint16_t cis_handle = cis->lll.handle;

			cis = ll_iso_stream_connected_get(cis_handle);
			if (!cis) {
				continue;
			}

			if (!cis_iter_start) {
				/* Look for iterator start handle */
				cis_iter_start = cis_handle == (*cis_iter);
			} else if (cis->lll.acl_handle == conn->lll.handle) {
				if (cis_iter) {
					(*cis_iter) = cis_handle;
				}
				return cis;
			}
		}
	}

	return NULL;
}

struct ll_conn_iso_stream *ll_conn_iso_stream_get_by_group(struct ll_conn_iso_group *cig,
							   uint16_t *handle_iter)
{
	struct ll_conn_iso_stream *cis;
	uint16_t handle_start;
	uint16_t handle;

	handle_start = (handle_iter == NULL) || ((*handle_iter) == UINT16_MAX) ?
			LL_CIS_HANDLE_BASE : (*handle_iter) + 1;

	for (handle = handle_start; handle <= LAST_VALID_CIS_HANDLE; handle++) {
		cis = ll_conn_iso_stream_get(handle);
		if (cis->group == cig) {
			if (handle_iter) {
				(*handle_iter) = handle;
			}
			return cis;
		}
	}

	return NULL;
}

uint8_t ll_conn_iso_accept_timeout_get(uint16_t *timeout)
{
	*timeout = conn_accept_timeout;

	return 0;
}

uint8_t ll_conn_iso_accept_timeout_set(uint16_t timeout)
{
	if (!IN_RANGE(timeout, CONN_ACCEPT_TIMEOUT_MIN,
			       CONN_ACCEPT_TIMEOUT_MAX)) {
		return BT_HCI_ERR_INVALID_LL_PARAM;
	}

	conn_accept_timeout = timeout;

	return 0;
}

void ull_conn_iso_cis_established(struct ll_conn_iso_stream *cis)
{
#if defined(CONFIG_BT_LL_SW_LLCP_LEGACY)
	struct node_rx_conn_iso_estab *est;
	struct node_rx_pdu *node_rx;

	node_rx = ull_pdu_rx_alloc();
	if (!node_rx) {
		/* No node available - try again later */
		return;
	}

	/* TODO: Send CIS_ESTABLISHED with status != 0 in error scenarios */
	node_rx->hdr.type = NODE_RX_TYPE_CIS_ESTABLISHED;
	node_rx->hdr.handle = 0xFFFF;
	node_rx->hdr.rx_ftr.param = cis;

	est = (void *)node_rx->pdu;
	est->status = 0;
	est->cis_handle = cis->lll.handle;

	ll_rx_put(node_rx->hdr.link, node_rx);
	ll_rx_sched();
#endif /* defined(CONFIG_BT_LL_SW_LLCP_LEGACY) */

	cis->established = 1;
}

void ull_conn_iso_done(struct node_rx_event_done *done)
{
	struct lll_conn_iso_group *lll;
	struct ll_conn_iso_group *cig;
	struct ll_conn_iso_stream *cis;
	uint32_t ticks_drift_minus;
	uint32_t ticks_drift_plus;
	uint16_t handle_iter;
	uint8_t cis_idx;

	/* Get reference to ULL context */
	cig = CONTAINER_OF(done->param, struct ll_conn_iso_group, ull);
	lll = &cig->lll;

	/* Skip if CIG terminated by local host */
	if (unlikely(lll->handle == 0xFFFF)) {
		return;
	}

	ticks_drift_plus  = 0;
	ticks_drift_minus = 0;
	handle_iter = UINT16_MAX;
	cis = NULL;

	/* Check all CISes for supervison/establishment timeout */
	for (cis_idx = 0; cis_idx < cig->lll.num_cis; cis_idx++) {
		cis = ll_conn_iso_stream_get_by_group(cig, &handle_iter);
		LL_ASSERT(cis);

		if (cis->lll.handle != LLL_HANDLE_INVALID) {
			/* CIS was setup and is now expected to be going */
			if (!(done->extra.trx_performed_mask &
			      (1U << LL_CIS_IDX_FROM_HANDLE(cis->lll.handle)))) {
				/* We did NOT have successful transaction on established CIS,
				 * or CIS was not yet established, so handle timeout
				 */
				if (!cis->event_expire) {
					struct ll_conn *conn = ll_conn_get(cis->lll.acl_handle);

					cis->event_expire =
						RADIO_CONN_EVENTS(
							conn->supervision_timeout * 10U * 1000U,
							cig->iso_interval * CONN_INT_UNIT_US) + 1;
				}

				if (--cis->event_expire == 0) {
					/* Stop CIS and defer cleanup to after teardown. */
					ull_conn_iso_cis_stop(cis, NULL,
							      cis->established ?
							      BT_HCI_ERR_CONN_TIMEOUT :
							      BT_HCI_ERR_CONN_FAIL_TO_ESTAB);

				}
			} else {
				cis->event_expire = 0;
			}
		}
	}

	if (done->extra.trx_performed_mask &&
	    IS_ENABLED(CONFIG_BT_CTLR_PERIPHERAL_ISO) && lll->role) {
		ull_drift_ticks_get(done, &ticks_drift_plus,
				    &ticks_drift_minus);
	}

	/* Update CIG ticker to compensate for drift.
	 * Since all CISes in a CIG 'belong to' the same ACL,
	 * any CIS found in the above for-loop will do to dereference the ACL
	 */
	if (cis && (ticks_drift_plus || ticks_drift_minus)) {
		uint8_t ticker_id = TICKER_ID_CONN_ISO_BASE +
				    ll_conn_iso_group_handle_get(cig);
		struct ll_conn *conn = ll_connected_get(cis->lll.acl_handle);
		uint32_t ticker_status;

		ticker_status = ticker_update(TICKER_INSTANCE_ID_CTLR,
					      TICKER_USER_ID_ULL_HIGH,
					      ticker_id,
					      ticks_drift_plus,
					      ticks_drift_minus, 0, 0,
					      TICKER_NULL_LAZY, 0,
					      ticker_update_cig_op_cb,
					      cig);

		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY) ||
			  ((void *)conn == ull_disable_mark_get()));
	}
}

/**
 * @brief Stop and tear down a connected ISO stream
 * This function may be called to tear down a CIS. When the CIS teardown
 * has completed and the stream is released and callback is provided, the
 * cis_released_cb callback is invoked.
 *
 * @param cis             Pointer to connected ISO stream to stop
 * @param cis_released_cb Callback to invoke when the CIS has been released.
 *                        NULL to ignore.
 * @param reason          Termination reason
 */
void ull_conn_iso_cis_stop(struct ll_conn_iso_stream *cis,
			   ll_iso_stream_released_cb_t cis_released_cb,
			   uint8_t reason)
{
	struct ll_conn_iso_group *cig;
	struct ull_hdr *hdr;

	if (cis->teardown) {
		/* Teardown already started */
		return;
	}
	cis->teardown = 1;
	cis->released_cb = cis_released_cb;
	cis->terminate_reason = reason;

	/* Check ref count to determine if any pending LLL events in pipeline */
	cig = cis->group;
	hdr = &cig->ull;
	if (ull_ref_get(hdr)) {
		static memq_link_t link;
		static struct mayfly mfy = {0, 0, &link, NULL, lll_disable};
		uint32_t ret;

		mfy.param = &cig->lll;

		/* Setup disabled callback to be called when ref count
		 * returns to zero.
		 */
		/* Event is active (prepare/done ongoing) - wait for done and
		 * continue CIS teardown from there. The disabled_cb cannot be
		 * reserved for other use.
		 */
		LL_ASSERT(!hdr->disabled_cb ||
			  (hdr->disabled_cb == cis_disabled_cb));
		hdr->disabled_param = mfy.param;
		hdr->disabled_cb = cis_disabled_cb;

		/* Trigger LLL disable */
		ret = mayfly_enqueue(TICKER_USER_ID_ULL_HIGH,
				     TICKER_USER_ID_LLL, 0, &mfy);
		LL_ASSERT(!ret);
	} else {
		/* No pending LLL events */

		/* Tear down CIS now in ULL_HIGH context. Ignore enqueue
		 * error (already enqueued) as all CISes marked for teardown
		 * will be handled in cis_disabled_cb. Use mayfly chaining to
		 * prevent recursive stop calls.
		 */
		cis_disabled_cb(&cig->lll);
	}
}

void ull_conn_iso_resume_ticker_start(struct lll_event *resume_event,
				      uint16_t cis_handle,
				      uint32_t ticks_anchor,
				      uint32_t resume_timeout)
{
	struct lll_conn_iso_group *cig;
	uint32_t ready_delay_us;
	uint32_t resume_delay_us;
	int32_t resume_offset_us;
	uint8_t ticker_id;
	uint32_t ret;

	cig = resume_event->prepare_param.param;
	ticker_id = TICKER_ID_CONN_ISO_RESUME_BASE + cig->handle;

	if (cig->resume_cis != LLL_HANDLE_INVALID) {
		/* Restarting resume ticker - must be stopped first */
		(void)ticker_stop(TICKER_INSTANCE_ID_CTLR, TICKER_USER_ID_LLL,
				  ticker_id, NULL, NULL);
	}
	cig->resume_cis = cis_handle;

	if (0) {
#if defined(CONFIG_BT_CTLR_PHY)
	} else {
		struct ll_conn_iso_stream *cis;
		struct ll_conn *acl;

		cis = ll_conn_iso_stream_get(cis_handle);
		acl = ll_conn_get(cis->lll.acl_handle);

		ready_delay_us = lll_radio_rx_ready_delay_get(acl->lll.phy_rx, 1);
#else
	} else {
		ready_delay_us = lll_radio_rx_ready_delay_get(0, 0);
#endif /* CONFIG_BT_CTLR_PHY */
	}

	resume_delay_us  = EVENT_OVERHEAD_START_US;
	resume_delay_us += EVENT_TICKER_RES_MARGIN_US;
	resume_delay_us += EVENT_JITTER_US;
	resume_delay_us += ready_delay_us;

	resume_offset_us = (int32_t)(resume_timeout - resume_delay_us);
	LL_ASSERT(resume_offset_us >= 0);

	/* Setup resume timeout as single-shot */
	ret = ticker_start(TICKER_INSTANCE_ID_CTLR,
			   TICKER_USER_ID_LLL,
			   ticker_id,
			   ticks_anchor,
			   HAL_TICKER_US_TO_TICKS(resume_offset_us),
			   TICKER_NULL_PERIOD,
			   TICKER_NULL_REMAINDER,
			   TICKER_NULL_LAZY,
			   TICKER_NULL_SLOT,
			   ticker_resume_cb, resume_event,
			   ticker_resume_op_cb, NULL);

	LL_ASSERT((ret == TICKER_STATUS_SUCCESS) ||
		  (ret == TICKER_STATUS_BUSY));
}

int ull_conn_iso_init(void)
{
	return init_reset();
}

int ull_conn_iso_reset(void)
{
	return init_reset();
}

static int init_reset(void)
{
	struct ll_conn_iso_stream *cis;
	struct ll_conn_iso_group *cig;
	uint16_t handle;
	int err;

	/* Disable all active CIGs (uses blocking ull_ticker_stop_with_mark) */
	for (handle = 0U; handle < CONFIG_BT_CTLR_CONN_ISO_GROUPS; handle++) {
		disable(handle);
	}

	/* Initialize CIS pool */
	mem_init(cis_pool, sizeof(struct ll_conn_iso_stream),
		 sizeof(cis_pool) / sizeof(struct ll_conn_iso_stream),
		 &cis_free);

	/* Initialize CIG pool */
	mem_init(cig_pool, sizeof(struct ll_conn_iso_group),
		 sizeof(cig_pool) / sizeof(struct ll_conn_iso_group),
		 &cig_free);

	for (handle = 0; handle < CONFIG_BT_CTLR_CONN_ISO_GROUPS; handle++) {
		cig = ll_conn_iso_group_get(handle);
		cig->cig_id  = 0xFF;
		cig->started = 0;
		cig->lll.num_cis = 0;
	}

	for (handle = LL_CIS_HANDLE_BASE; handle <= LAST_VALID_CIS_HANDLE; handle++) {
		cis = ll_conn_iso_stream_get(handle);
		cis->cis_id = 0;
		cis->group  = NULL;
	}

	conn_accept_timeout = CONN_ACCEPT_TIMEOUT_DEFAULT;

	/* Initialize LLL */
	err = lll_conn_iso_init();
	if (err) {
		return err;
	}

	return 0;
}

void ull_conn_iso_ticker_cb(uint32_t ticks_at_expire, uint32_t ticks_drift,
			    uint32_t remainder, uint16_t lazy, uint8_t force,
			    void *param)
{
	static memq_link_t link;
	static struct mayfly mfy = { 0, 0, &link, NULL, NULL };
	static struct lll_prepare_param p;
	struct ll_conn_iso_group *cig;
	struct ll_conn_iso_stream *cis;
	uint64_t leading_event_count;
	uint16_t handle_iter;
	uint32_t err;
	uint8_t ref;

	cig = param;
	leading_event_count = 0;

	/* Check if stopping ticker (on disconnection, race with ticker expiry)
	 */
	if (unlikely(cig->lll.handle == 0xFFFF)) {
		return;
	}

	handle_iter = UINT16_MAX;

	/* Increment CIS event counters */
	for (int i = 0; i < cig->lll.num_cis; i++)  {
		cis = ll_conn_iso_stream_get_by_group(cig, &handle_iter);
		LL_ASSERT(cis);

		/* New CIS may become available by creation prior to the CIG
		 * event in which it has event_count == 0. Don't increment
		 * event count until its handle is validated in
		 * ull_conn_iso_start, which means that its ACL instant
		 * has been reached, and offset calculated.
		 */
		if (cis->lll.handle != 0xFFFF && cis->lll.active) {
			cis->lll.event_count++;

			leading_event_count = MAX(leading_event_count,
						cis->lll.event_count);

			ull_iso_lll_event_prepare(cis->lll.handle, cis->lll.event_count);
		}

		/* Latch datapath validity entering event */
		cis->lll.datapath_ready_rx = cis->hdr.datapath_out != NULL;
	}

	/* Update the CIG reference point for this event. Event 0 for the
	 * leading CIS in the CIG would have had it's reference point set in
	 * ull_conn_iso_start(). The reference point should only be
	 * updated from event 1 onwards. Although the cig reference point set
	 * this way is not accurate, it is the best possible until the anchor
	 * point for the leading CIS is available for this event.
	 */
	if (leading_event_count > 0) {
		cig->cig_ref_point += (cig->iso_interval * CONN_INT_UNIT_US);
	}

	/* Increment prepare reference count */
	ref = ull_ref_inc(&cig->ull);
	LL_ASSERT(ref);

	/* Append timing parameters */
	p.ticks_at_expire = ticks_at_expire;
	p.remainder = remainder;
	p.lazy = lazy;
	p.param = &cig->lll;
	mfy.param = &p;

#if !defined(CONFIG_BT_CTLR_PERIPHERAL_ISO)
	mfy.fp = lll_central_iso_prepare;
#elif !defined(CONFIG_BT_CTLR_CENTRAL_ISO)
	mfy.fp = lll_peripheral_iso_prepare;
#else
	mfy.fp = (cig->lll.role == BT_HCI_ROLE_PERIPHERAL) ? lll_peripheral_iso_prepare :
							     lll_central_iso_prepare;
#endif

	if (IS_PERIPHERAL(cig) && cig->sca_update) {
		/* CIG/ACL affilaition established */
		uint32_t iso_interval_us_frac =
			EVENT_US_TO_US_FRAC(cig->iso_interval * CONN_INT_UNIT_US);
		cig->lll.window_widening_periodic_us_frac =
			ceiling_fraction(((lll_clock_ppm_local_get() +
					   lll_clock_ppm_get(cig->sca_update - 1)) *
					  iso_interval_us_frac),
					 1000000U);
		iso_interval_us_frac -= cig->lll.window_widening_periodic_us_frac;

		ull_peripheral_iso_update_ticker(cig, ticks_at_expire, iso_interval_us_frac);
		cig->sca_update = 0;
	}

	/* Kick LLL prepare */
	err = mayfly_enqueue(TICKER_USER_ID_ULL_HIGH, TICKER_USER_ID_LLL, 0, &mfy);
	LL_ASSERT(!err);

	/* Handle ISO Transmit Test for this CIG */
	ull_conn_iso_transmit_test_cig_interval(cig->lll.handle, ticks_at_expire);
}

static void ticker_op_cb(uint32_t status, void *param)
{
	ARG_UNUSED(param);

	LL_ASSERT(status == TICKER_STATUS_SUCCESS);
}

void ull_conn_iso_start(struct ll_conn *acl, uint32_t ticks_at_expire, uint16_t cis_handle)
{
	struct ll_conn_iso_group *cig;
	struct ll_conn_iso_stream *cis;
	uint32_t acl_to_cig_ref_point;
	uint32_t cis_offs_to_cig_ref;
	uint32_t ticks_remainder;
	uint32_t ticks_periodic;
	uint32_t ready_delay_us;
	uint32_t ticker_status;
	int32_t cig_offset_us;
	uint8_t ticker_id;

	cis = ll_conn_iso_stream_get(cis_handle);
	cig = cis->group;

	cis_offs_to_cig_ref = cig->sync_delay - cis->sync_delay;

	cis->lll.offset = cis_offs_to_cig_ref;
	cis->lll.handle = cis_handle;
	cis->lll.active = 1U;

	/* Connection establishment timeout */
	cis->event_expire = CONN_ESTAB_COUNTDOWN;

	/* Check if another CIS was already started and CIG ticker is
	 * running. If so, we just return with updated offset and
	 * validated handle.
	 */
	if (cig->started) {
		/* We're done */
		return;
	}

	ticker_id = TICKER_ID_CONN_ISO_BASE + ll_conn_iso_group_handle_get(cig);

	/* Establish the CIG reference point by adjusting ACL-to-CIS offset
	 * (cis->offset) by the difference between CIG- and CIS sync delays.
	 */
	acl_to_cig_ref_point = cis->offset - cis_offs_to_cig_ref;

#if defined(CONFIG_BT_CTLR_PHY)
	ready_delay_us = lll_radio_rx_ready_delay_get(acl->lll.phy_rx, 1);
#else
	ready_delay_us = lll_radio_rx_ready_delay_get(0, 0);
#endif

	/* Calculate initial ticker offset */
	cig_offset_us  = acl_to_cig_ref_point;

	/* Calculate the CIG reference point of first CIG event. This
	 * calculation is inaccurate. However it is the best estimate available
	 * until the first anchor point for the leading CIS is available.
	 */
	cig->cig_ref_point = HAL_TICKER_TICKS_TO_US(ticks_at_expire);
	cig->cig_ref_point += EVENT_OVERHEAD_START_US;
	cig->cig_ref_point += acl_to_cig_ref_point;

	if (IS_PERIPHERAL(cig)) {
		uint32_t iso_interval_us_frac;

		/* Calculate interval in fractional microseconds for highest precision when
		 * accumulating the window widening window size. Ticker interval is set lopsided,
		 * with natural drift towards earlier timeout.
		 */
		iso_interval_us_frac = EVENT_US_TO_US_FRAC(cig->iso_interval * ISO_INT_UNIT_US) -
				       cig->lll.window_widening_periodic_us_frac;
		ticks_periodic  = EVENT_US_FRAC_TO_TICKS(iso_interval_us_frac);
		ticks_remainder = EVENT_US_FRAC_TO_REMAINDER(iso_interval_us_frac);

		/* Adjust CIG offset and reference point ahead one interval */
		cig_offset_us -= EVENT_TICKER_RES_MARGIN_US;
		cig_offset_us -= EVENT_JITTER_US;
		cig_offset_us -= ready_delay_us;
		cig_offset_us += (acl->lll.interval * CONN_INT_UNIT_US);

		cig->cig_ref_point += (acl->lll.interval * CONN_INT_UNIT_US);
	} else {
		uint32_t iso_interval_us;

		iso_interval_us = cig->iso_interval * ISO_INT_UNIT_US;
		ticks_periodic  = HAL_TICKER_US_TO_TICKS(iso_interval_us);
		ticks_remainder = HAL_TICKER_REMAINDER(iso_interval_us);
	}

	/* Make sure we have time to service first subevent. TODO: Improve
	 * by skipping <n> interval(s) and incrementing event_count.
	 */
	LL_ASSERT(cig_offset_us > 0);

	/* Start CIS peripheral CIG ticker */
	ticker_status = ticker_start(TICKER_INSTANCE_ID_CTLR,
				     TICKER_USER_ID_ULL_HIGH,
				     ticker_id,
				     ticks_at_expire,
				     HAL_TICKER_US_TO_TICKS(cig_offset_us),
				     ticks_periodic,
				     ticks_remainder,
				     TICKER_NULL_LAZY,
				     0,
				     ull_conn_iso_ticker_cb, cig,
				     ticker_op_cb, NULL);

	LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
		  (ticker_status == TICKER_STATUS_BUSY));

	cig->started = 1;
}

static void ticker_update_cig_op_cb(uint32_t status, void *param)
{
	/* CIG drift compensation succeeds, or it fails in a race condition
	 * when disconnecting (race between ticker_update and ticker_stop
	 * calls). TODO: Are the race-checks needed?
	 */
	LL_ASSERT(status == TICKER_STATUS_SUCCESS ||
		  param == ull_update_mark_get() ||
		  param == ull_disable_mark_get());
}

static void ticker_resume_op_cb(uint32_t status, void *param)
{
	ARG_UNUSED(param);

	LL_ASSERT(status == TICKER_STATUS_SUCCESS);
}

static void ticker_resume_cb(uint32_t ticks_at_expire, uint32_t ticks_drift,
			     uint32_t remainder, uint16_t lazy, uint8_t force,
			     void *param)
{
	static memq_link_t link;
	static struct mayfly mfy = {0, 0, &link, NULL, lll_resume};
	struct lll_conn_iso_group *cig;
	struct lll_event *resume_event;
	uint32_t ret;

	ARG_UNUSED(ticks_drift);
	LL_ASSERT(lazy == 0);

	resume_event = param;

	/* Append timing parameters */
	resume_event->prepare_param.ticks_at_expire = ticks_at_expire;
	resume_event->prepare_param.remainder = remainder;
	resume_event->prepare_param.lazy = 0;
	resume_event->prepare_param.force = force;
	mfy.param = resume_event;

	/* Mark resume as done */
	cig = resume_event->prepare_param.param;
	cig->resume_cis = LLL_HANDLE_INVALID;

	/* Kick LLL resume */
	ret = mayfly_enqueue(TICKER_USER_ID_ULL_HIGH, TICKER_USER_ID_LLL,
			     0, &mfy);

	LL_ASSERT(!ret);
}

static void cis_disabled_cb(void *param)
{
	struct ll_conn_iso_group *cig;
	struct ll_conn_iso_stream *cis;
	uint32_t ticker_status;
	uint16_t handle_iter;
	uint8_t is_last_cis;
	uint8_t cis_idx;

	cig = HDR_LLL2ULL(param);
	is_last_cis = cig->lll.num_cis == 1;
	handle_iter = UINT16_MAX;

	/* Remove all CISes marked for teardown */
	for (cis_idx = 0; cis_idx < cig->lll.num_cis; cis_idx++) {
		cis = ll_conn_iso_stream_get_by_group(cig, &handle_iter);
		LL_ASSERT(cis);

		if (cis->lll.flushed) {
			ll_iso_stream_released_cb_t cis_released_cb;
			struct ll_conn *conn;

			conn = ll_conn_get(cis->lll.acl_handle);
			cis_released_cb = cis->released_cb;

			/* Remove data path and ISOAL sink/source associated with this CIS
			 * for both directions.
			 */
			ll_remove_iso_path(cis->lll.handle, BT_HCI_DATAPATH_DIR_CTLR_TO_HOST);
			ll_remove_iso_path(cis->lll.handle, BT_HCI_DATAPATH_DIR_HOST_TO_CTLR);

			ll_conn_iso_stream_release(cis);
			cig->lll.num_cis--;

#if !defined(CONFIG_BT_LL_SW_LLCP_LEGACY)
			/* CIS terminated, triggers completion of CIS_TERMINATE_IND procedure */
			/* Only used by local procedure, ignored for remote procedure */
			conn->llcp.cis.terminate_ack = 1U;
#endif /* defined(CONFIG_BT_LL_SW_LLCP_LEGACY) */

			/* Check if removed CIS has an ACL disassociation callback. Invoke
			 * the callback to allow cleanup.
			 */
			if (cis_released_cb) {
				/* CIS removed - notify caller */
				cis_released_cb(conn);
			}
		} else if (cis->teardown) {
			DECLARE_MAYFLY_ARRAY(mfys, cis_tx_lll_flush,
				CONFIG_BT_CTLR_CONN_ISO_GROUPS);
			struct node_rx_pdu *node_terminate;
			uint32_t ret;

			/* Create and enqueue termination node. This shall prevent
			 * further enqueuing of TX nodes for terminating CIS.
			 */
			node_terminate = ull_pdu_rx_alloc();
			LL_ASSERT(node_terminate);
			node_terminate->hdr.handle = cis->lll.handle;
			node_terminate->hdr.type = NODE_RX_TYPE_TERMINATE;
			*((uint8_t *)node_terminate->pdu) = cis->terminate_reason;

			ll_rx_put(node_terminate->hdr.link, node_terminate);
			ll_rx_sched();

			if (cig->lll.resume_cis == cis->lll.handle) {
				/* Resume pending for terminating CIS - stop ticker */
				(void)ticker_stop(TICKER_INSTANCE_ID_CTLR,
						  TICKER_USER_ID_ULL_HIGH,
						  TICKER_ID_CONN_ISO_RESUME_BASE +
						  ll_conn_iso_group_handle_get(cig),
						  NULL, NULL);

				cig->lll.resume_cis = LLL_HANDLE_INVALID;
			}

			/* We need to flush TX nodes in LLL before releasing the stream.
			 * More than one CIG may be terminating at the same time, so
			 * enqueue a mayfly instance for this CIG.
			 */
			mfys[cig->lll.handle].param = &cis->lll;
			ret = mayfly_enqueue(TICKER_USER_ID_ULL_HIGH,
					     TICKER_USER_ID_LLL, 1, &mfys[cig->lll.handle]);
			LL_ASSERT(!ret);

			return;
		}
	}

	if (is_last_cis && cig->lll.num_cis == 0) {
		/* This was the last CIS of the CIG. Initiate CIG teardown by
		 * stopping ticker.
		 */
		ticker_status = ticker_stop(TICKER_INSTANCE_ID_CTLR,
					    TICKER_USER_ID_ULL_HIGH,
					    TICKER_ID_CONN_ISO_BASE +
					    ll_conn_iso_group_handle_get(cig),
					    ticker_stop_op_cb,
					    cig);

		LL_ASSERT((ticker_status == TICKER_STATUS_SUCCESS) ||
			  (ticker_status == TICKER_STATUS_BUSY));
	}
}

static void cis_tx_lll_flush(void *param)
{
	DECLARE_MAYFLY_ARRAY(mfys, cis_disabled_cb, CONFIG_BT_CTLR_CONN_ISO_GROUPS);

	struct lll_conn_iso_stream *lll;
	struct ll_conn_iso_stream *cis;
	struct ll_conn_iso_group *cig;
	struct node_tx *tx;
	memq_link_t *link;
	uint32_t ret;

	lll = param;
	lll->flushed = 1U;
	lll->active = 0U;

	cis = ll_conn_iso_stream_get(lll->handle);
	cig = cis->group;

	/* Flush in LLL - may return TX nodes to ack queue */
	lll_conn_iso_flush(lll->handle, lll);

	link = memq_dequeue(lll->memq_tx.tail, &lll->memq_tx.head, (void **)&tx);
	while (link) {
		/* Create instant NACK */
		ll_tx_ack_put(lll->handle, tx);
		link->next = tx->next;
		tx->next = link;

		link = memq_dequeue(lll->memq_tx.tail, &lll->memq_tx.head,
				    (void **)&tx);
	}

	/* Resume CIS teardown in ULL_HIGH context */
	mfys[cig->lll.handle].param = &cig->lll;
	ret = mayfly_enqueue(TICKER_USER_ID_LLL,
			     TICKER_USER_ID_ULL_HIGH, 1, &mfys[cig->lll.handle]);
	LL_ASSERT(!ret);
}

static void ticker_stop_op_cb(uint32_t status, void *param)
{
	static memq_link_t link;
	static struct mayfly mfy = {0, 0, &link, NULL, cig_disable};
	uint32_t ret;

	/* Assert if race between thread and ULL */
	LL_ASSERT(status == TICKER_STATUS_SUCCESS);

	/* Check if any pending LLL events that need to be aborted */
	mfy.param = param;
	ret = mayfly_enqueue(TICKER_USER_ID_ULL_LOW,
			     TICKER_USER_ID_ULL_HIGH, 0, &mfy);
	LL_ASSERT(!ret);
}

static void cig_disable(void *param)
{
	struct ll_conn_iso_group *cig;
	struct ull_hdr *hdr;

	/* Check ref count to determine if any pending LLL events in pipeline */
	cig = param;
	hdr = &cig->ull;
	if (ull_ref_get(hdr)) {
		static memq_link_t link;
		static struct mayfly mfy = {0, 0, &link, NULL, lll_disable};
		uint32_t ret;

		mfy.param = &cig->lll;

		/* Setup disabled callback to be called when ref count
		 * returns to zero.
		 */
		LL_ASSERT(!hdr->disabled_cb);
		hdr->disabled_param = mfy.param;
		hdr->disabled_cb = cig_disabled_cb;

		/* Trigger LLL disable */
		ret = mayfly_enqueue(TICKER_USER_ID_ULL_HIGH,
				     TICKER_USER_ID_LLL, 0, &mfy);
		LL_ASSERT(!ret);
	} else {
		/* No pending LLL events */
		cig_disabled_cb(&cig->lll);
	}
}

static void cig_disabled_cb(void *param)
{
	struct ll_conn_iso_group *cig;

	cig = HDR_LLL2ULL(param);

	if (IS_PERIPHERAL(cig) || cig->cis_count == 0) {
		ll_conn_iso_group_release(cig);
	} else {
		/* CIG shall be released by ll_cig_remove */
		cig->started = 0;
	}
}

static void disable(uint16_t handle)
{
	struct ll_conn_iso_group *cig;
	int err;

	cig = ll_conn_iso_group_get(handle);

	(void)ticker_stop(TICKER_INSTANCE_ID_CTLR, TICKER_USER_ID_THREAD,
			  TICKER_ID_CONN_ISO_RESUME_BASE + handle, NULL,
			  NULL);

	err = ull_ticker_stop_with_mark(TICKER_ID_CONN_ISO_BASE + handle,
					cig, &cig->lll);

	LL_ASSERT(err == 0 || err == -EALREADY);

	cig->lll.handle = LLL_HANDLE_INVALID;
	cig->lll.resume_cis = LLL_HANDLE_INVALID;
}

/* An ISO interval has elapsed for a Connected Isochronous Group */
void ull_conn_iso_transmit_test_cig_interval(uint16_t handle, uint32_t ticks_at_expire)
{
	struct ll_conn_iso_stream *cis;
	struct ll_conn_iso_group *cig;
	uint32_t sdu_interval;
	uint32_t iso_interval;
	uint16_t handle_iter;
	uint64_t sdu_counter;
	uint8_t tx_sdu_count;

	cig = ll_conn_iso_group_get(handle);
	LL_ASSERT(cig);

	handle_iter = UINT16_MAX;

	if (cig->lll.role) {
		/* Peripheral */
		sdu_interval = cig->p_sdu_interval;
	} else {
		/* Central */
		sdu_interval = cig->c_sdu_interval;
	}

	iso_interval = cig->iso_interval * PERIODIC_INT_UNIT_US;

	/* Handle ISO Transmit Test for all active CISes in the group */
	for (uint8_t i = 0; i < cig->lll.num_cis; i++)  {
		cis = ll_conn_iso_stream_get_by_group(cig, &handle_iter);
		LL_ASSERT(cis);

		if (!cis->hdr.test_mode.tx_enabled || cis->lll.handle == LLL_HANDLE_INVALID) {
			continue;
		}

		/* Calculate number of SDUs to transmit in the next ISO event. Ensure no overflow
		 * on 64-bit sdu_counter:
		 *   (39 bits x 22 bits (4x10^6 us) = 61 bits / 8 bits (255 us) = 53 bits)
		 */
		sdu_counter = ceiling_fraction((cis->lll.event_count + 1U) * iso_interval,
					       sdu_interval);

		if (cis->hdr.test_mode.tx_sdu_counter == 0U) {
			/* First ISO event. Align SDU counter for next event */
			cis->hdr.test_mode.tx_sdu_counter = sdu_counter;
			tx_sdu_count = 0U;
		} else {
			/* Calculate number of SDUs to produce for next ISO event */
			tx_sdu_count = sdu_counter - cis->hdr.test_mode.tx_sdu_counter;
		}

		/* Now process all SDUs due for next ISO event */
		for (uint8_t sdu = 0; sdu < tx_sdu_count; sdu++) {
			ll_iso_transmit_test_send_sdu(cis->lll.handle, ticks_at_expire);
		}
	}
}
