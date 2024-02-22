/*
 * Copyright (c) 2018,2021-2023 Hewlett Packard Enterprise Development LP
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include <ofi_list.h>
#include <ofi.h>
#include <fasthash.h>

#include "cxip.h"

#define FC_SW_LE_MSG_FATAL "LE exhaustion during flow control, "\
	"FI_CXI_RX_MATCH_MODE=[hybrid|software] is required\n"
#define FC_SW_ONLOAD_MSG_FATAL "LE resources not recovered during "\
	"flow control. FI_CXI_RX_MATCH_MODE=[hybrid|software] is required\n"
#define FC_OFLOW_NO_MATCH_MSG "Flow control overflow no match, increasing "\
	"FI_CXI_OFLOW_BUF_SIZE (current is %ldB) may reduce occurrence\n"
#define FC_REQ_FULL_MSG "Flow control request list full, increasing"\
	" FI_CXI_REQ_BUF_SIZE value (current is %ldB) may reduce occurrence\n"
#define FC_DROP_COUNT_MSG "Re-enable Drop count mismatch, re-enable will "\
	"be retried on notify\n"

#define WARN_RESTRICTED_DISABLED "Insufficient resources for %s "\
	"protocol, switching to %s protocol\n"

/* Defines the posted receive interval for checking LE allocation if
 * in hybrid RX match mode and preemptive transitions to software
 * managed EP are requested.
 */
#define CXIP_HYBRID_RECV_CHECK_INTERVAL	(64-1)

static int cxip_recv_cb(struct cxip_req *req, const union c_event *event);
static void cxip_ux_onload_complete(struct cxip_req *req);
static int cxip_ux_onload(struct cxip_rxc *rxc);
static int cxip_recv_req_queue(struct cxip_req *req, bool restart_seq);
static int cxip_recv_req_dropped(struct cxip_req *req);
static ssize_t _cxip_recv_req(struct cxip_req *req, bool restart_seq);

static int cxip_send_req_dropped(struct cxip_txc *txc, struct cxip_req *req);
static int cxip_send_req_dequeue(struct cxip_txc *txc, struct cxip_req *req);

static void cxip_fc_progress_ctrl(struct cxip_rxc *rxc);
static void cxip_send_buf_fini(struct cxip_req *req);

/*
 * match_put_event() - Find/add a matching event.
 *
 * For every Put Overflow event there is a matching Put event. These events can
 * be generated in any order. Both events must be received before progress can
 * be made.
 *
 * If the matching event exists in the mapping, matched is set to true and
 * the deferred event is returned. If a match was not found, matched is set to
 * false and the event is added to the deferred event mapping.
 *
 * The deferred match event is returned; unless it must be added to the
 * deferred mapping and memory is insufficient.
 *
 * Caller must hold ep_obj->lock.
 */
static struct cxip_deferred_event *
match_put_event(struct cxip_rxc *rxc, struct cxip_req *req,
		const union c_event *event, bool *matched)
{
	union cxip_def_event_key key = {};
	struct cxip_deferred_event *def_ev;
	union cxip_match_bits mb;
	int bucket;
	enum c_event_type match_type =
		event->tgt_long.event_type == C_EVENT_PUT ? C_EVENT_PUT_OVERFLOW : C_EVENT_PUT;

	if (event->tgt_long.rendezvous) {
		key.initiator = event->tgt_long.initiator.initiator.process;
		mb.raw = event->tgt_long.match_bits;
		key.rdzv_id = (mb.rdzv_id_hi << CXIP_RDZV_ID_CMD_WIDTH) |
			      event->tgt_long.rendezvous_id;
		key.rdzv = 1;
	} else {
		key.start_addr = event->tgt_long.start;
	}

	bucket = fasthash64(&key.raw, sizeof(key.raw), 0) %
			    CXIP_DEF_EVENT_HT_BUCKETS;
	dlist_foreach_container(&rxc->deferred_events.bh[bucket],
				struct cxip_deferred_event, def_ev,
				rxc_entry) {
		if (def_ev->key.raw == key.raw &&
		    def_ev->ev.tgt_long.event_type == match_type &&
		    def_ev->ev.tgt_long.return_code == event->tgt_long.return_code &&
		    def_ev->ev.tgt_long.initiator.initiator.process == event->tgt_long.initiator.initiator.process &&
		    def_ev->ev.tgt_long.match_bits == event->tgt_long.match_bits) {
			*matched = true;
			return def_ev;
		}
	}

	/* Not found, add mapping to hash bucket */
	*matched = false;

	def_ev = calloc(1, sizeof(*def_ev));
	if (!def_ev) {
		RXC_WARN(rxc, "Failed allocate to memory\n");
		return NULL;
	}

	def_ev->key.raw	= key.raw;
	def_ev->req = req;
	def_ev->ev = *event;

	dlist_insert_tail(&def_ev->rxc_entry, &rxc->deferred_events.bh[bucket]);

	return def_ev;
}

/*
 * free_put_event() - Free a deferred put event.
 *
 * Free an event previously allocated added with match_put_event().
 *
 * Caller must hold ep_obj->lock.
 */
static void free_put_event(struct cxip_rxc *rxc,
			   struct cxip_deferred_event *def_ev)
{
	dlist_remove(&def_ev->rxc_entry);
	free(def_ev);
}

/*
 * recv_req_src_addr() - Translate request source address to FI address.
 */
static fi_addr_t recv_req_src_addr(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;

	/* If the FI_SOURCE capability is enabled, convert the initiator's
	 * address to an FI address to be reported in a CQ event. If
	 * application AVs are symmetric, the match_id in the EQ event is
	 * logical and translation is not needed. Otherwise, translate the
	 * physical address in the EQ event to logical FI address.
	 */
	if (rxc->attr.caps & FI_SOURCE) {
		struct cxip_addr addr = {};

		if (rxc->ep_obj->av->symmetric)
			return CXI_MATCH_ID_EP(rxc->pid_bits,
					       req->recv.initiator);

		addr.nic = CXI_MATCH_ID_EP(rxc->pid_bits, req->recv.initiator);
		addr.pid = CXI_MATCH_ID_PID(rxc->pid_bits, req->recv.initiator);
		addr.vni = req->recv.vni;

		return cxip_av_lookup_fi_addr(rxc->ep_obj->av, &addr);
	}

	return FI_ADDR_NOTAVAIL;
}

/*
 * cxip_recv_req_alloc() - Allocate a request handle for a receive,
 * mapping the associated buffer if required.
 *
 * Caller must hold ep->ep_obj->lock.
 */
static int cxip_recv_req_alloc(struct cxip_rxc *rxc, void *buf, size_t len,
			       struct cxip_req **cxip_req)
{
	struct cxip_domain *dom = rxc->domain;
	struct cxip_req *req;
	struct cxip_md *recv_md = NULL;
	int ret;

	/* Software EP only mode receives are not posted to hardware
	 * and are not constrained by hardware buffer ID limits.
	 *
	 * Note: cxip_evtq_req_alloc() zeros the req.
	 */
	req = cxip_evtq_req_alloc(&rxc->rx_evtq, !rxc->sw_ep_only, rxc);
	if (!req) {
		RXC_INFO(rxc, "Recv request unavailable: -FI_EAGAIN\n");
		return -FI_EAGAIN;
	}

	if (len) {
		ret = cxip_map(dom, (void *)buf, len, 0, &recv_md);
		if (ret) {
			RXC_WARN(rxc, "Map of recv buffer failed: %d, %s\n",
				 ret, fi_strerror(-ret));
			goto err_free_request;
		}
	}

	/* Initialize common receive request attributes. */
	req->type = CXIP_REQ_RECV;
	req->cb = cxip_recv_cb;
	req->recv.rxc = rxc;
	req->recv.recv_buf = buf;
	req->recv.recv_md = recv_md;
	req->recv.ulen = len;
	dlist_init(&req->recv.children);
	dlist_init(&req->recv.rxc_entry);

	ofi_atomic_inc32(&rxc->orx_reqs);
	*cxip_req = req;

	return FI_SUCCESS;

err_free_request:
	cxip_evtq_req_free(req);

	return ret;
}

static void cxip_recv_req_free(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;

	assert(req->type == CXIP_REQ_RECV);
	assert(dlist_empty(&req->recv.children));
	assert(dlist_empty(&req->recv.rxc_entry));

	ofi_atomic_dec32(&rxc->orx_reqs);

	if (req->recv.recv_md)
		cxip_unmap(req->recv.recv_md);

	cxip_evtq_req_free(req);
}

/*
 * recv_req_event_success() - Generate successful receive event completions.
 */
static inline int recv_req_event_success(struct cxip_rxc *rxc,
					 struct cxip_req *req)
{
	int ret;
	fi_addr_t src_addr;
	struct cxip_addr *addr;

	if (req->recv.rxc->attr.caps & FI_SOURCE) {
		src_addr = recv_req_src_addr(req);
		if (src_addr != FI_ADDR_NOTAVAIL ||
		    !(rxc->attr.caps & FI_SOURCE_ERR))
			return cxip_cq_req_complete_addr(req, src_addr);

		addr = calloc(1, sizeof(*addr));
		if (!addr)
			return -FI_ENOMEM;

		addr->nic = CXI_MATCH_ID_EP(rxc->pid_bits, req->recv.initiator);
		addr->pid = CXI_MATCH_ID_PID(rxc->pid_bits,
					     req->recv.initiator);

		src_addr = cxip_av_lookup_auth_key_fi_addr(rxc->ep_obj->av,
							   req->recv.vni);

		ret = cxip_cq_req_error(req, 0, FI_EADDRNOTAVAIL, req->recv.rc,
					addr, sizeof(*addr), src_addr);

		free(addr);
	} else {
		ret = cxip_cq_req_complete(req);
	}

	return ret;
}

/*
 * recv_req_report() - Report the completion of a receive operation.
 */
static void recv_req_report(struct cxip_req *req)
{
	int ret;
	int err;
	int success_event = (req->flags & FI_COMPLETION);
	struct cxip_rxc *rxc = req->recv.rxc;
	ssize_t truncated = req->recv.rlen - req->data_len;

	/* data_len (i.e. mlength) should NEVER be greater than rlength. */
	assert(truncated >= 0);

	req->flags &= (FI_MSG | FI_TAGGED | FI_RECV | FI_REMOTE_CQ_DATA);

	if (req->recv.parent) {
		struct cxip_req *parent = req->recv.parent;
		bool unlinked = false;

		parent->recv.mrecv_bytes += req->data_len;
		RXC_DBG(rxc,
			"Putting %lu mrecv bytes (req: %p consumed: %lu auto_unlinked: %u unlink_bytes: %lu addr: %#lx ulen=%u min_free=%lu hw_offloaded=%u)\n",
			req->data_len, parent, parent->recv.mrecv_bytes,
			parent->recv.auto_unlinked, parent->recv.mrecv_unlink_bytes,
			req->buf, parent->recv.ulen, rxc->min_multi_recv,
			parent->recv.hw_offloaded);

		/* Handle mrecv edge case. If all unexpected headers were
		 * onloaded, the entire mrecv buffer may be matched against the
		 * sw_ux_list list before being offloaded to HW. Detect this
		 * case.
		 */
		if (parent->recv.hw_offloaded) {
			if (parent->recv.auto_unlinked &&
			    parent->recv.mrecv_bytes == parent->recv.mrecv_unlink_bytes)
				unlinked = true;
		} else {
			if ((parent->recv.ulen - parent->recv.mrecv_bytes) < rxc->min_multi_recv)
				unlinked = true;
		}

		if (unlinked) {
			RXC_DBG(rxc, "Freeing parent: %p\n", req->recv.parent);
			cxip_recv_req_free(req->recv.parent);

			req->flags |= FI_MULTI_RECV;
		}
	}

	if (req->recv.rc == C_RC_OK && !truncated) {
		RXC_DBG(rxc, "Request success: %p\n", req);

		if (success_event) {
			ret = recv_req_event_success(rxc, req);
			if (ret != FI_SUCCESS)
				RXC_WARN(rxc,
					 "Failed to report completion: %d\n",
					 ret);
		}

		if (req->recv.cntr) {
			ret = cxip_cntr_mod(req->recv.cntr, 1, false, false);
			if (ret)
				RXC_WARN(rxc, "cxip_cntr_mod returned: %d\n",
					 ret);
		}
	} else {
		if (req->recv.unlinked) {
			err = FI_ECANCELED;
			if (req->recv.multi_recv)
				req->flags |= FI_MULTI_RECV;
			RXC_DBG(rxc, "Request canceled: %p (err: %d)\n", req,
				err);
		} else if (truncated) {
			err = FI_ETRUNC;
			RXC_DBG(rxc, "Request truncated: %p (err: %d)\n", req,
				err);
		} else if (req->recv.flags & FI_PEEK) {
			req->data_len = 0;
			err = FI_ENOMSG;
			RXC_DBG(rxc, "Peek request not found: %p (err: %d)\n",
				req, err);
		} else {
			err = proverr2errno(req->recv.rc);

			RXC_WARN(rxc, "Request error: %p (err: %d, %s)\n", req,
				 err, cxi_rc_to_str(req->recv.rc));
		}

		ret = cxip_cq_req_error(req, truncated, err, req->recv.rc,
					NULL, 0, FI_ADDR_UNSPEC);
		if (ret != FI_SUCCESS)
			RXC_WARN(rxc, "Failed to report error: %d\n", ret);

		if (req->recv.cntr) {
			ret = cxip_cntr_mod(req->recv.cntr, 1, false, true);
			if (ret)
				RXC_WARN(rxc, "cxip_cntr_mod returned: %d\n",
					 ret);
		}
	}
}

/*
 * recv_req_tgt_event() - Update common receive request fields
 *
 * Populate a receive request with information found in all receive event
 * types.
 */
static void
recv_req_tgt_event(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	union cxip_match_bits mb = {
		.raw = event->tgt_long.match_bits
	};
	uint32_t init = event->tgt_long.initiator.initiator.process;

	assert(event->hdr.event_type == C_EVENT_PUT ||
	       event->hdr.event_type == C_EVENT_PUT_OVERFLOW ||
	       event->hdr.event_type == C_EVENT_RENDEZVOUS ||
	       event->hdr.event_type == C_EVENT_SEARCH);

	/* Rendezvous events contain the wrong match bits and do not provide
	 * initiator context for symmetric AVs.
	 */
	if (event->hdr.event_type != C_EVENT_RENDEZVOUS) {
		req->tag = mb.tag;
		req->recv.initiator = init;

		if (mb.cq_data)
			req->flags |= FI_REMOTE_CQ_DATA;
	}

	/* remote_offset is not provided in Overflow events. */
	if (event->hdr.event_type != C_EVENT_PUT_OVERFLOW)
		req->recv.src_offset = event->tgt_long.remote_offset;

	/* For rendezvous, initiator is the RGet DFA. */
	if (event->hdr.event_type == C_EVENT_RENDEZVOUS) {
		init = cxi_dfa_to_init(init, rxc->pid_bits);
		req->recv.rget_nic = CXI_MATCH_ID_EP(rxc->pid_bits, init);
		req->recv.rget_pid = CXI_MATCH_ID_PID(rxc->pid_bits, init);
	}

	/* Only need one event to set remaining fields. */
	if (req->recv.tgt_event)
		return;
	req->recv.tgt_event = true;

	/* VNI is needed to support FI_AV_AUTH_KEY. */
	req->recv.vni = event->tgt_long.vni;

	/* rlen is used to detect truncation. */
	req->recv.rlen = event->tgt_long.rlength;

	/* RC is used when generating completion events. */
	req->recv.rc = cxi_tgt_event_rc(event);

	/* Header data is provided in all completion events. */
	req->data = event->tgt_long.header_data;

	/* rdzv_id is used to correlate Put and Put Overflow events when using
	 * offloaded RPut. Otherwise, Overflow buffer start address is used to
	 * correlate events.
	 */
	if (event->tgt_long.rendezvous)
		req->recv.rdzv_id = (mb.rdzv_id_hi << CXIP_RDZV_ID_CMD_WIDTH) |
				    event->tgt_long.rendezvous_id;
	else
		req->recv.oflow_start = event->tgt_long.start;

	req->recv.rdzv_lac = mb.rdzv_lac;
	req->recv.rdzv_proto = mb.rdzv_proto;
	req->recv.rdzv_mlen = event->tgt_long.mlength;

	/* data_len must be set uniquely for each protocol! */
}

/*
 * rdzv_mrecv_req_lookup() - Search for a matching rendezvous, multi-receive
 * child request.
 */
static int rdzv_mrecv_req_lookup(struct cxip_req *req,
				 const union c_event *event,
				 uint32_t *initiator, uint32_t *rdzv_id,
				 bool perform_event_checks,
				 struct cxip_req **req_out)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	struct cxip_req *child_req;
	union cxip_match_bits mb;
	uint32_t ev_init;
	uint32_t ev_rdzv_id;
	struct cxip_addr caddr;
	int ret;
	int i;

	if (event->hdr.event_type == C_EVENT_REPLY) {
		struct cxi_rdzv_user_ptr *user_ptr;

		/* Events for software-issued operations will return a
		 * reference to the correct request.
		 */
		if (!event->init_short.rendezvous) {
			*req_out = req;
			return FI_SUCCESS;
		}

		user_ptr = (struct cxi_rdzv_user_ptr *)
				&event->init_short.user_ptr;

		ev_init = CXI_MATCH_ID(rxc->pid_bits, user_ptr->src_pid,
					user_ptr->src_nid);
		ev_rdzv_id = user_ptr->rendezvous_id;
	} else if (event->hdr.event_type == C_EVENT_RENDEZVOUS) {
		struct cxip_rxc *rxc = req->recv.rxc;
		uint32_t dfa = event->tgt_long.initiator.initiator.process;

		ev_init = cxi_dfa_to_init(dfa, rxc->pid_bits);
		mb.raw = event->tgt_long.match_bits;

		ev_rdzv_id = (mb.rdzv_id_hi << CXIP_RDZV_ID_CMD_WIDTH) |
			     event->tgt_long.rendezvous_id;
	} else {
		ev_init = event->tgt_long.initiator.initiator.process;
		mb.raw = event->tgt_long.match_bits;

		ev_rdzv_id = (mb.rdzv_id_hi << CXIP_RDZV_ID_CMD_WIDTH) |
			     event->tgt_long.rendezvous_id;
	}

	if ((event->hdr.event_type == C_EVENT_PUT_OVERFLOW ||
	     event->hdr.event_type == C_EVENT_PUT)  &&
	    rxc->ep_obj->av->symmetric) {
		ret = cxip_av_lookup_addr(rxc->ep_obj->av,
					  CXI_MATCH_ID_EP(rxc->pid_bits, ev_init),
					  &caddr);
		if (ret != FI_SUCCESS)
			RXC_FATAL(rxc, "Lookup of FI addr 0x%x: failed %d\n",
				  ev_init, ret);

		ev_init = CXI_MATCH_ID(rxc->pid_bits,
				       CXI_MATCH_ID_PID(rxc->pid_bits, ev_init),
				       caddr.nic);
	}

	*initiator = ev_init;
	*rdzv_id = ev_rdzv_id;

	/* Events for hardware-issued operations will return a rendezvous_id
	 * and initiator data. Use these fields to find a matching child
	 * request.
	 */
	dlist_foreach_container(&req->recv.children,
				struct cxip_req, child_req,
				recv.children) {
		if (child_req->recv.rdzv_id == ev_rdzv_id &&
		    child_req->recv.rdzv_initiator == ev_init) {

			if (perform_event_checks) {
				/* There is an edge case where source may reuse the
				 * same rendezvous ID before the target has had time to
				 * process the C_EVENT_REPLY. If this is the case, an
				 * incorrect child_req match would occur. To prevent
				 * this, the events seen are stored with the child_req.
				 * If a redundant event is seen, this is a sign
				 * C_EVENT_REPLY needs to be process. Thus, return
				 * -FI_EAGAIN to process TX EQ.
				 */
				for (i = 0; i < child_req->recv.rdzv_events; i++) {
					if (child_req->recv.rdzv_event_types[i] == event->hdr.event_type) {
						assert(event->hdr.event_type != C_EVENT_REPLY);
						return -FI_EAGAIN;
					}
				}
			}

			*req_out = child_req;
			return FI_SUCCESS;
		}
	}

	return -FI_ENOMSG;
}

/*
 * mrecv_req_dup() - Create a new request using an event targeting a
 * multi-recv buffer.
 *
 * @mrecv_req: A previously posted multi-recv buffer request.
 */
static struct cxip_req *mrecv_req_dup(struct cxip_req *mrecv_req)
{
	struct cxip_rxc *rxc = mrecv_req->recv.rxc;
	struct cxip_req *req;

	req = cxip_evtq_req_alloc(&rxc->rx_evtq, 0, rxc);
	if (!req)
		return NULL;

	/* Duplicate the parent request. */
	req->cb = mrecv_req->cb;
	req->context = mrecv_req->context;
	req->flags = mrecv_req->flags;
	req->type = mrecv_req->type;
	req->recv = mrecv_req->recv;

	/* Update fields specific to this Send */
	req->recv.parent = mrecv_req;

	/* Start pointer and data_len must be set elsewhere! */

	return req;
}

/*
 * rdzv_mrecv_req_event() - Look up a multi-recieve child request using an
 * event and multi-recv request.
 *
 * Each rendezvous Put transaction targeting a multi-receive buffer is tracked
 * using a separate child request. A child request is uniquely identified by
 * rendezvous ID and source address. Return a reference to a child request
 * which matches the event. Allocate a new child request, if necessary.
 */
static struct cxip_req *
rdzv_mrecv_req_event(struct cxip_req *mrecv_req, const union c_event *event)
{
	uint32_t ev_init;
	uint32_t ev_rdzv_id;
	struct cxip_req *req;
	struct cxip_rxc *rxc __attribute__((unused)) = mrecv_req->recv.rxc;
	int ret;

	assert(event->hdr.event_type == C_EVENT_REPLY ||
	       event->hdr.event_type == C_EVENT_PUT ||
	       event->hdr.event_type == C_EVENT_PUT_OVERFLOW ||
	       event->hdr.event_type == C_EVENT_RENDEZVOUS);

	ret = rdzv_mrecv_req_lookup(mrecv_req, event, &ev_init, &ev_rdzv_id,
				    true, &req);
	switch (ret) {
	case -FI_EAGAIN:
		return NULL;

	case -FI_ENOMSG:
		req = mrecv_req_dup(mrecv_req);
		if (!req)
			return NULL;

		/* Store event initiator and rdzv_id for matching. */
		req->recv.rdzv_id = ev_rdzv_id;
		req->recv.rdzv_initiator = ev_init;

		dlist_insert_tail(&req->recv.children,
				  &mrecv_req->recv.children);

		RXC_DBG(rxc, "New child: %p parent: %p event: %s\n", req,
			mrecv_req, cxi_event_to_str(event));
		return req;

	case FI_SUCCESS:
		RXC_DBG(rxc, "Found child: %p parent: %p event: %s\n", req,
			mrecv_req, cxi_event_to_str(event));
		return req;

	default:
		RXC_FATAL(rxc, "Unhandled rdzv_mrecv_req_lookup %d\n", ret);
	}
}

/*
 * rdzv_recv_req_event() - Count a rendezvous event.
 *
 * Call for each target rendezvous event generated on a user receive buffer.
 * After three events, a rendezvous receive is complete. The three events could
 * be either:
 *   -Put, Rendezvous, Reply -- or
 *   -Put Overflow, Rendezvous, Reply
 *
 * For a restricted Get there is a fourth event, the ACK of the notify.
 *
 * In either case, the events could be generated in any order. As soon as the
 * events expected are processed, the request is complete.
 */
static void rdzv_recv_req_event(struct cxip_req *req, enum c_event_type type)
{
	int total_events = req->recv.done_notify ? 4 : 3;

	req->recv.rdzv_event_types[req->recv.rdzv_events] = type;

	if (++req->recv.rdzv_events == total_events) {
		if (req->recv.multi_recv) {
			dlist_remove(&req->recv.children);
			recv_req_report(req);
			cxip_evtq_req_free(req);
		} else {
			recv_req_report(req);
			cxip_recv_req_free(req);
		}
	}
}

/*
 * oflow_req_put_bytes() - Consume bytes in the Overflow buffer.
 *
 * An Overflow buffer is freed when all bytes are consumed by the NIC.
 *
 * Caller must hold ep_obj->lock.
 */
static void oflow_req_put_bytes(struct cxip_req *req, size_t bytes)
{
	struct cxip_ptelist_buf *oflow_buf = req->req_ctx;

	/* Non-zero length UX messages with 0 eager portion do not
	 * have a dependency on the oflow buffer.
	 */
	if (bytes == 0)
		return;

	oflow_buf->cur_offset += bytes;

	RXC_DBG(oflow_buf->rxc, "Putting %lu bytes (%lu/%lu): %p\n", bytes,
		oflow_buf->cur_offset, oflow_buf->unlink_length, req);

	if (oflow_buf->cur_offset == oflow_buf->unlink_length)
		cxip_ptelist_buf_consumed(oflow_buf);
}

/*
 * issue_rdzv_get() - Perform a Get to pull source data from the Initiator of a
 * Send operation.
 */
static int issue_rdzv_get(struct cxip_req *req)
{
	struct c_full_dma_cmd cmd = {};
	uint64_t local_addr;
	uint64_t rem_offset;
	uint32_t align_bytes;
	uint32_t mlen;
	struct cxip_rxc *rxc = req->recv.rxc;
	uint32_t pid_idx = rxc->domain->iface->dev->info.rdzv_get_idx;
	uint8_t idx_ext;
	union cxip_match_bits mb = {};
	int ret;
	union c_fab_addr dfa;

	if (req->recv.rdzv_proto == CXIP_RDZV_PROTO_ALT_WRITE)
		RXC_WARN_ONCE(rxc, "Rendezvous protocol: %s not implemented\n",
			      cxip_rdzv_proto_to_str(req->recv.rdzv_proto));

	cmd.command.cmd_type = C_CMD_TYPE_DMA;
	cmd.command.opcode = C_CMD_GET;
	cmd.lac = req->recv.recv_md->md->lac;
	cmd.event_send_disable = 1;

	/* Must deliver to TX event queue */
	cmd.eq = cxip_evtq_eqn(&rxc->ep_obj->txc.tx_evtq);

	if (req->recv.rdzv_proto == CXIP_RDZV_PROTO_ALT_READ) {
		pid_idx = CXIP_PTL_IDX_RDZV_RESTRICTED(req->recv.rdzv_lac);
		cmd.restricted = 1;
		req->recv.done_notify = true;
	} else {
		pid_idx = rxc->domain->iface->dev->info.rdzv_get_idx;
		mb.rdzv_lac = req->recv.rdzv_lac;
		mb.rdzv_id_lo = req->recv.rdzv_id;
		mb.rdzv_id_hi = req->recv.rdzv_id >> CXIP_RDZV_ID_CMD_WIDTH;
	}
	cmd.match_bits = mb.raw;

	cmd.user_ptr = (uint64_t)req;
	cxi_build_dfa(req->recv.rget_nic, req->recv.rget_pid, rxc->pid_bits,
		      pid_idx, &dfa, &idx_ext);
	cmd.dfa = dfa;
	cmd.index_ext = idx_ext;

	local_addr = CXI_VA_TO_IOVA(req->recv.recv_md->md,
				    req->recv.recv_buf);
	local_addr += req->recv.rdzv_mlen;

	rem_offset = req->recv.src_offset;
	mlen = req->recv.rdzv_mlen;

	RXC_DBG(rxc, "SW RGet addr: 0x%" PRIx64 " len %" PRId64
		" rem_off: %" PRId64 " restricted: %d\n", local_addr,
		req->data_len - req->recv.rdzv_mlen, rem_offset,
		cmd.restricted);

	/* Align mask will be non-zero if local DMA address cache-line
	 * alignment is desired.
	 */
	if (mlen >= rxc->rget_align_mask) {
		align_bytes = local_addr & rxc->rget_align_mask;
		local_addr -= align_bytes;
		rem_offset -= align_bytes;
		mlen -= align_bytes;
	}

	if (req->data_len < mlen)
		cmd.request_len = 0;
	else
		cmd.request_len = req->data_len - mlen;

	cmd.local_addr = local_addr;
	cmd.remote_offset = rem_offset;

	RXC_DBG(rxc, "Aligned addr: 0x%" PRIx64 " len %d rem_off %" PRId64 "\n",
		(uint64_t)cmd.local_addr, cmd.request_len,
		(uint64_t)cmd.remote_offset);

	/* Issue Rendezvous Get command */
	ret = cxi_cq_emit_dma(rxc->tx_cmdq->dev_cmdq, &cmd);
	if (ret) {
		RXC_DBG(rxc, "Failed to queue GET command: %d\n", ret);
		return -FI_EAGAIN;
	}

	cxi_cq_ring(rxc->tx_cmdq->dev_cmdq);

	return FI_SUCCESS;
}

/*
 * cxip_notify_match_cb() - Callback function for match complete notifiction
 * Ack events.
 */
static int
cxip_notify_match_cb(struct cxip_req *req, const union c_event *event)
{
	RXC_DBG(req->recv.rxc, "Match complete: %p\n", req);

	recv_req_report(req);

	if (req->recv.multi_recv)
		cxip_evtq_req_free(req);
	else
		cxip_recv_req_free(req);

	return FI_SUCCESS;
}

/*
 * cxip_notify_match() - Notify the initiator of a Send that the match is
 * complete at the target.
 *
 * A transaction ID corresponding to the matched Send request is sent back to
 * the initiator in the match_bits field of a zero-byte Put.
 */
static int cxip_notify_match(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	uint32_t pid_idx = rxc->domain->iface->dev->info.rdzv_get_idx;
	uint32_t init = event->tgt_long.initiator.initiator.process;
	uint32_t nic = CXI_MATCH_ID_EP(rxc->pid_bits, init);
	uint32_t pid = CXI_MATCH_ID_PID(rxc->pid_bits, init);
	union c_fab_addr dfa;
	uint8_t idx_ext;
	union cxip_match_bits mb = {
		.le_type = CXIP_LE_TYPE_ZBP,
	};
	union cxip_match_bits event_mb;
	union c_cmdu cmd = {};
	int ret;

	event_mb.raw = event->tgt_long.match_bits;
	mb.tx_id = event_mb.tx_id;

	cxi_build_dfa(nic, pid, rxc->pid_bits, pid_idx, &dfa, &idx_ext);

	cmd.c_state.event_send_disable = 1;
	cmd.c_state.index_ext = idx_ext;
	cmd.c_state.eq = cxip_evtq_eqn(&rxc->ep_obj->txc.tx_evtq);

	ret = cxip_cmdq_emit_c_state(rxc->tx_cmdq, &cmd.c_state);
	if (ret) {
		RXC_DBG(rxc, "Failed to issue C_STATE command: %d\n", ret);
		return ret;
	}

	memset(&cmd.idc_msg, 0, sizeof(cmd.idc_msg));
	cmd.idc_msg.dfa = dfa;
	cmd.idc_msg.match_bits = mb.raw;

	cmd.idc_msg.user_ptr = (uint64_t)req;

	ret = cxi_cq_emit_idc_msg(rxc->tx_cmdq->dev_cmdq, &cmd.idc_msg,
				  NULL, 0);
	if (ret) {
		RXC_DBG(rxc, "Failed to write IDC: %d\n", ret);

		/* Return error according to Domain Resource Management
		 */
		return -FI_EAGAIN;
	}

	req->cb = cxip_notify_match_cb;

	cxi_cq_ring(rxc->tx_cmdq->dev_cmdq);

	RXC_DBG(rxc, "Queued match completion message: %p\n", req);

	return FI_SUCCESS;
}

/*
 * mrecv_req_oflow_event() - Set start and length uniquely for an unexpected
 * mrecv request.
 *
 * Overflow buffer events contain a start address representing the offset into
 * the Overflow buffer where data was written. When a unexpected header is
 * later matched to a multi-receive buffer in the priority list, The Put
 * Overflow event does not contain the offset into the Priority list buffer
 * where data should be copied. Software must track the the Priority list
 * buffer offset using ordered Put Overflow events.
 */
static int mrecv_req_put_bytes(struct cxip_req *req, uint32_t rlen)
{
	uintptr_t mrecv_head;
	uintptr_t mrecv_tail;
	size_t mrecv_bytes_remaining;

	mrecv_head = (uintptr_t)req->recv.recv_buf + req->recv.start_offset;
	mrecv_tail = (uintptr_t)req->recv.recv_buf + req->recv.ulen;
	mrecv_bytes_remaining = mrecv_tail - mrecv_head;

	rlen = MIN(mrecv_bytes_remaining, rlen);
	req->recv.start_offset += rlen;

	return rlen;
}

/* cxip_recv_req_set_rget_info() - Set RGet NIC and PID fields. Used for
 * messages where a rendezvous event will not be generated. Current usages are
 * for the eager long protocol and rendezvous operations which have unexpected
 * headers onloaded due to flow control.
 */
static void cxip_recv_req_set_rget_info(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	int ret;

	if (rxc->ep_obj->av->symmetric) {
		struct cxip_addr caddr;

		RXC_DBG(rxc, "Translating initiator: %x, req: %p\n",
			req->recv.initiator, req);

		ret = cxip_av_lookup_addr(rxc->ep_obj->av,
				          CXI_MATCH_ID_EP(rxc->pid_bits, req->recv.initiator),
					  &caddr);
		if (ret != FI_SUCCESS)
			RXC_FATAL(rxc, "Failed to look up FI addr: %d\n", ret);

		req->recv.rget_nic = caddr.nic;
	} else {
		req->recv.rget_nic = CXI_MATCH_ID_EP(rxc->pid_bits,
						     req->recv.initiator);
	}

	req->recv.rget_pid = CXI_MATCH_ID_PID(rxc->pid_bits,
					      req->recv.initiator);
}

/*
 * cxip_ux_send() - Progress an unexpected Send after receiving matching Put
 * and Put and Put Overflow events.
 *
 * Caller must hold ep_obj->lock.
 */
static int cxip_ux_send(struct cxip_req *match_req, struct cxip_req *oflow_req,
			const union c_event *put_event, uint64_t mrecv_start,
			uint32_t mrecv_len, bool remove_recv_entry)
{
	struct cxip_ptelist_buf *buf;
	void *oflow_va;
	size_t oflow_bytes;
	union cxip_match_bits mb;
	ssize_t ret;
	struct cxip_req *parent_req = match_req;

	assert(match_req->type == CXIP_REQ_RECV);

	if (match_req->recv.multi_recv) {
		if (put_event->tgt_long.rendezvous)
			match_req = rdzv_mrecv_req_event(match_req, put_event);
		else
			match_req = mrecv_req_dup(match_req);
		if (!match_req)
			return -FI_EAGAIN;

		/* Set start and length uniquely for an unexpected
		 * mrecv request.
		 */
		match_req->recv.recv_buf = (uint8_t *)
				match_req->recv.parent->recv.recv_buf +
				mrecv_start;
		match_req->buf = (uint64_t)match_req->recv.recv_buf;
		match_req->data_len = mrecv_len;
	} else {
		match_req->data_len = put_event->tgt_long.rlength;
		if (match_req->data_len > match_req->recv.ulen)
			match_req->data_len = match_req->recv.ulen;
	}

	recv_req_tgt_event(match_req, put_event);
	buf = oflow_req->req_ctx;
	oflow_va = (void *)CXI_IOVA_TO_VA(buf->md->md,
					  put_event->tgt_long.start);

	/* Copy data out of overflow buffer. */
	oflow_bytes = MIN(put_event->tgt_long.mlength, match_req->data_len);
	cxip_copy_to_md(match_req->recv.recv_md, match_req->recv.recv_buf,
			oflow_va, oflow_bytes);

	if (oflow_req->type == CXIP_REQ_OFLOW)
		oflow_req_put_bytes(oflow_req, put_event->tgt_long.mlength);

	/* Remaining unexpected rendezvous processing is deferred until RGet
	 * completes.
	 */
	if (put_event->tgt_long.rendezvous) {
		if (remove_recv_entry)
			dlist_remove_init(&parent_req->recv.rxc_entry);

		rdzv_recv_req_event(match_req, put_event->hdr.event_type);
		return FI_SUCCESS;
	}

	mb.raw = put_event->tgt_long.match_bits;

	/* Check if the initiator requires match completion guarantees.
	 * If so, notify the initiator that the match is now complete.
	 * Delay the Receive event until the notification is complete.
	 */
	if (mb.match_comp) {
		ret = cxip_notify_match(match_req, put_event);
		if (ret != FI_SUCCESS) {
			if (match_req->recv.multi_recv)
				cxip_evtq_req_free(match_req);

			return -FI_EAGAIN;
		}

		if (remove_recv_entry)
			dlist_remove_init(&parent_req->recv.rxc_entry);

		return FI_SUCCESS;
	}

	if (remove_recv_entry)
		dlist_remove_init(&parent_req->recv.rxc_entry);

	recv_req_report(match_req);

	if (match_req->recv.multi_recv)
		cxip_evtq_req_free(match_req);
	else
		cxip_recv_req_free(match_req);

	return FI_SUCCESS;
}

/*
 * cxip_ux_send_zb() - Progress an unexpected zero-byte Send after receiving
 * a Put Overflow event.
 *
 * Zero-byte Put events for unexpected Sends are discarded. Progress the Send
 * using only the Overflow event. There is no Send data to be copied out.
 */
static int cxip_ux_send_zb(struct cxip_req *match_req,
			   const union c_event *oflow_event,
			   uint64_t mrecv_start, bool remove_recv_entry)
{
	union cxip_match_bits mb;
	int ret;
	struct cxip_req *parent_req = match_req;

	assert(!oflow_event->tgt_long.rlength);

	if (match_req->recv.multi_recv) {
		match_req = mrecv_req_dup(match_req);
		if (!match_req)
			return -FI_EAGAIN;

		match_req->buf = (uint64_t)
				match_req->recv.parent->recv.recv_buf +
				mrecv_start;
	}

	recv_req_tgt_event(match_req, oflow_event);

	match_req->data_len = 0;

	mb.raw = oflow_event->tgt_long.match_bits;

	/* Check if the initiator requires match completion guarantees.
	 * If so, notify the initiator that the match is now complete.
	 * Delay the Receive event until the notification is complete.
	 */
	if (mb.match_comp) {
		ret = cxip_notify_match(match_req, oflow_event);
		if (ret != FI_SUCCESS) {
			if (match_req->recv.multi_recv)
				cxip_evtq_req_free(match_req);

			return -FI_EAGAIN;
		}

		if (remove_recv_entry)
			dlist_remove_init(&parent_req->recv.rxc_entry);

		return FI_SUCCESS;
	}

	if (remove_recv_entry)
		dlist_remove_init(&parent_req->recv.rxc_entry);

	recv_req_report(match_req);

	if (match_req->recv.multi_recv)
		cxip_evtq_req_free(match_req);
	else
		cxip_recv_req_free(match_req);

	return FI_SUCCESS;
}

static bool cxip_ux_is_onload_complete(struct cxip_req *req)
{
	return !req->search.puts_pending && req->search.complete;
}

/*
 * recv_req_peek_complete - FI_PEEK operation completed
 */
static void recv_req_peek_complete(struct cxip_req *req,
				   struct cxip_ux_send *ux_send)
{
	/* If no unexpected message match we need to return original
	 * tag in the completion.
	 */
	if (req->recv.rc != C_RC_OK)
		req->tag = req->recv.tag;
	else if (req->recv.flags & FI_CLAIM)
		((struct fi_context *)req->context)->internal[0] = ux_send;

	/* Avoid truncation processing, peek does not receive data */
	req->data_len = req->recv.rlen;

	recv_req_report(req);

	cxip_recv_req_free(req);
}

/* Caller must hold ep_obj->lock. */
static int cxip_oflow_process_put_event(struct cxip_rxc *rxc,
					struct cxip_req *req,
					const union c_event *event)
{
	int ret;
	struct cxip_deferred_event *def_ev;
	struct cxip_req *save_req;
	bool matched;

	def_ev = match_put_event(rxc, req, event, &matched);
	if (!matched)
		return !def_ev ? -FI_EAGAIN : FI_SUCCESS;

	RXC_DBG(rxc, "Overflow beat Put event: %p\n", def_ev->req);

	if (def_ev->ux_send) {
		/* UX Send was onloaded for one of these reasons:
		 * 1) Flow control
		 * 2) ULE was claimed by a FI_CLAIM
		 */
		save_req = def_ev->req;
		def_ev->ux_send->req = req;
		def_ev->ux_send->put_ev = *event;

		if (def_ev->ux_send->claimed) {
			recv_req_tgt_event(save_req, &def_ev->ux_send->put_ev);
			recv_req_peek_complete(save_req, def_ev->ux_send);
			RXC_DBG(rxc, "FI_CLAIM put complete: %p, ux_send %p\n",
				save_req, def_ev->ux_send);
			goto done;
		} else {
			def_ev->req->search.puts_pending--;
			RXC_DBG(rxc, "put complete: %p\n", def_ev->req);
		}

		if (cxip_ux_is_onload_complete(def_ev->req))
			cxip_ux_onload_complete(def_ev->req);

	} else {
		ret = cxip_ux_send(def_ev->req, req, event, def_ev->mrecv_start,
				   def_ev->mrecv_len, false);
		if (ret != FI_SUCCESS)
			return -FI_EAGAIN;
	}

done:
	free_put_event(rxc, def_ev);

	return FI_SUCCESS;
}

/* Caller must hold ep_obj->lock */
static int cxip_recv_pending_ptlte_disable(struct cxip_rxc *rxc,
					   bool check_fc)
{
	int ret;

	assert(rxc->state == RXC_ENABLED ||
	       rxc->state == RXC_ONLOAD_FLOW_CONTROL ||
	       rxc->state == RXC_ONLOAD_FLOW_CONTROL_REENABLE ||
	       rxc->state == RXC_FLOW_CONTROL ||
	       rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED ||
	       rxc->state == RXC_PENDING_PTLTE_DISABLE);

	/* Having flow control triggered while in flow control is a sign of LE
	 * exhaustion. Software endpoint mode is required to scale past hardware
	 * LE limit.
	 */
	if (check_fc && rxc->state == RXC_FLOW_CONTROL)
		RXC_FATAL(rxc, FC_SW_LE_MSG_FATAL);

	if (rxc->state != RXC_ENABLED)
		return FI_SUCCESS;

	RXC_DBG(rxc, "Manual request PTLTE_DISABLED\n");

	ret = cxip_pte_set_state(rxc->rx_pte, rxc->rx_cmdq, C_PTLTE_DISABLED,
				 0);
	if (ret == FI_SUCCESS)
		rxc->state = RXC_PENDING_PTLTE_DISABLE;

	return ret;
}

/* cxip_rxp_check_le_usage_hybrid_preempt() - Examines LE Pool usage and forces
 * a preemptive hardware to software transition if needed.
 *
 * In cases where the LE pool entry reservation is insufficient to meet request
 * list buffers (due to multiple EP sharing an LE Pool or insufficient LE Pool
 * reservation value), then enabling the periodic checking of LE allocations
 * can be used to force preemptive transitions to software match mode before
 * resources are exhausted or so depleted they are starve software managed
 * endpoint. The lpe_stat_2 is set to the number of LE pool entries allocated
 * to the LE pool and lpe_stat_1 is the current allocation. Skid is required
 * as stats are relative to hardware processing, not software processing of
 * the event.
 *
 * Caller should hold ep_obj->lock.
 */
static inline bool
cxip_rxp_check_le_usage_hybrid_preempt(struct cxip_rxc *rxc,
				       const union c_event *event)
{
	if (event->tgt_long.lpe_stat_1 > (event->tgt_long.lpe_stat_2 >> 1) &&
	    rxc->state == RXC_ENABLED) {
		if (cxip_recv_pending_ptlte_disable(rxc, false))
			RXC_WARN(rxc, "Force FC failed\n");
		return true;
	}
	return false;
}

static int cxip_rxc_check_ule_hybrid_preempt(struct cxip_rxc *rxc)
{
	int ret;
	int count;

	if (cxip_env.rx_match_mode == CXIP_PTLTE_HYBRID_MODE &&
	    cxip_env.hybrid_unexpected_msg_preemptive == 1) {
		count = ofi_atomic_get32(&rxc->orx_hw_ule_cnt);

		if (rxc->state == RXC_ENABLED && count > rxc->attr.size) {
			ret = cxip_recv_pending_ptlte_disable(rxc, false);
			if (ret == FI_SUCCESS) {
				RXC_WARN(rxc,
					 "Transitioning to SW EP due to too many unexpected messages: posted_count=%u request_size=%lu\n",
					 ret, rxc->attr.size);
			} else {
				assert(ret == -FI_EAGAIN);
				RXC_WARN(rxc,
					 "Failed to transition to SW EP: %d\n",
					 ret);
			}

			return ret;
		}
	}

	return FI_SUCCESS;
}

/*
 * cxip_oflow_cb() - Process an Overflow buffer event.
 *
 * Overflow buffers are used to land unexpected Send data. Link, Unlink
 * and Put events are expected from Overflow buffers. However, Link
 * events will only be requested when running in hybrid RX match mode
 * with FI_CXI_HYBRID_PREEMPTIVE=1.
 *
 * An Unlink event indicates that buffer space was exhausted. Overflow buffers
 * are configured to use locally managed LEs. When enough Puts match in an
 * Overflow buffer, consuming its space, the NIC automatically unlinks the LE.
 * An automatic Unlink event is generated before the final Put which caused
 * buffer space to become exhausted.
 *
 * An Unlink event is generated by an Unlink command. Overflow buffers are
 * manually unlinked in this way during teardown. When an LE is manually
 * unlinked the auto_unlinked field in the corresponding event is zero. In this
 * case, the request is freed immediately.
 *
 * A Put event is generated for each Put that matches the Overflow buffer LE.
 * This event indicates that data is available in the Overflow buffer. This
 * event must be correlated to a Put Overflow event from a user receive buffer
 * LE. The Put Overflow event may arrive before or after the Put event.
 *
 * When each Put event arrives, check for the existence of a previously posted
 * receive buffer which generated a matching Put Overflow event. If such a
 * buffer exists, copy data from the Overflow buffer to the user receive
 * buffer. Otherwise, store a record of the Put event for matching once a user
 * posts a new buffer that matches the unexpected Put.
 *
 * If data will remain in the Overflow buffer, take a reference to it to
 * prevent it from being freed. If an Unlink-Put event is detected, drop a
 * reference to the Overflow buffer so it is automatically freed once all user
 * data is copied out.
 */
static int cxip_oflow_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_ptelist_buf *oflow_buf = req->req_ctx;
	struct cxip_rxc *rxc = oflow_buf->rxc;
	int ret = FI_SUCCESS;

	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		/* Success events only used with hybrid preemptive */
		if (cxi_event_rc(event) == C_RC_OK) {

			if (!cxip_env.hybrid_preemptive)
				return FI_SUCCESS;

			/* Check for possible hybrid mode preemptive
			 * transitions to software managed mode.
			 */
			if (cxip_rxp_check_le_usage_hybrid_preempt(rxc, event))
				RXC_WARN(rxc,
					 "Force preemptive switch to SW EP\n");
			return FI_SUCCESS;
		}

		assert(cxi_event_rc(event) == C_RC_NO_SPACE);

		RXC_DBG(rxc, "Oflow LE append failed\n");

		ret = cxip_recv_pending_ptlte_disable(rxc, true);
		if (ret != FI_SUCCESS)
			RXC_WARN(rxc, "Force disable failed %d %s\n",
				 ret, fi_strerror(-ret));
		cxip_ptelist_buf_link_err(oflow_buf, cxi_event_rc(event));
		return ret;
	case C_EVENT_UNLINK:
		assert(!event->tgt_long.auto_unlinked);

		cxip_ptelist_buf_unlink(oflow_buf);
		return FI_SUCCESS;
	case C_EVENT_PUT:
		/* Put event handling is complicated. Handle below. */
		break;
	default:
		RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}

	ofi_atomic_inc32(&rxc->orx_hw_ule_cnt);

	if (event->tgt_long.auto_unlinked) {

		oflow_buf->unlink_length = event->tgt_long.start -
			CXI_VA_TO_IOVA(oflow_buf->md->md, oflow_buf->data)
			+ event->tgt_long.mlength;

		ofi_atomic_dec32(&oflow_buf->pool->bufs_linked);

		RXC_DBG(rxc, "Oflow auto unlink buf %p, linked %u\n", oflow_buf,
			ofi_atomic_get32(&oflow_buf->pool->bufs_linked));

		/* Replace the eager overflow buffer. */
		cxip_ptelist_buf_replenish(rxc->oflow_list_bufpool, false);
	}

	ret = cxip_rxc_check_ule_hybrid_preempt(rxc);
	if (ret)
		goto err_dec_ule;

	/* Drop all unexpected 0-byte Put events. */
	if (!event->tgt_long.rlength)
		return FI_SUCCESS;

	/* Handle Put events */
	ret = cxip_oflow_process_put_event(rxc, req, event);
	if (ret)
		goto err_dec_ule;

	return FI_SUCCESS;

err_dec_ule:
	ofi_atomic_dec32(&rxc->orx_hw_ule_cnt);

	return ret;
}

static void report_send_completion(struct cxip_req *req, bool sw_cntr);
static void rdzv_send_req_event(struct cxip_req *req);

/*
 * cxip_rdzv_pte_zbp_cb() - Process zero-byte Put events.
 *
 * Zero-byte Puts (ZBP) are used to transfer small messages without consuming
 * buffers outside of the EQ. ZBPs are currently only used for match complete
 * messages.
 */
int cxip_rdzv_pte_zbp_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rdzv_pte *rdzv_pte = req->req_ctx;
	struct cxip_txc *txc = rdzv_pte->txc;
	struct cxip_req *put_req;
	union cxip_match_bits mb;
	int event_rc = cxi_event_rc(event);
	int rdzv_id;
	int ret;

	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		if (event_rc == C_RC_OK)
			ofi_atomic_inc32(&rdzv_pte->le_linked_success_count);
		else
			ofi_atomic_inc32(&rdzv_pte->le_linked_failure_count);
		return FI_SUCCESS;

	case C_EVENT_PUT:
		mb.raw = event->tgt_long.match_bits;

		if (mb.rdzv_done) {
			rdzv_id = (mb.rdzv_id_hi << CXIP_RDZV_ID_CMD_WIDTH) |
				mb.rdzv_id_lo;
			put_req = cxip_rdzv_id_lookup(txc, rdzv_id);
			if (!put_req) {
				TXC_WARN(txc, "Failed to find RDZV ID: %d\n",
					 rdzv_id);
				return FI_SUCCESS;
			}

			if (event_rc != C_RC_OK)
				TXC_WARN(txc, "RDZV Done error: %p rc: %s\n",
					 put_req, cxi_rc_to_str(event_rc));
			else
				TXC_DBG(txc, "RDZV Done ACK: %p rc: %s\n",
					put_req, cxi_rc_to_str(event_rc));

			put_req->send.rc = event_rc;
			rdzv_send_req_event(put_req);

			return FI_SUCCESS;
		}

		/* Match complete */
		put_req = cxip_tx_id_lookup(txc, mb.tx_id);
		if (!put_req) {
			TXC_WARN(txc, "Failed to find TX ID: %d\n", mb.tx_id);
			return FI_SUCCESS;
		}

		event_rc = cxi_tgt_event_rc(event);
		if (event_rc != C_RC_OK)
			TXC_WARN(txc, "ZBP error: %p rc: %s\n", put_req,
				 cxi_rc_to_str(event_rc));
		else
			TXC_DBG(txc, "ZBP received: %p rc: %s\n", put_req,
				cxi_rc_to_str(event_rc));

		ret = cxip_send_req_dequeue(put_req->send.txc, put_req);
		if (ret != FI_SUCCESS)
			return ret;

		cxip_tx_id_free(txc, mb.tx_id);

		/* The unexpected message has been matched. Generate a
		 * completion event. The ZBP event is guaranteed to arrive
		 * after the eager Send Ack, so the transfer is always done at
		 * this point.
		 *
		 * If MATCH_COMPLETE was requested, software must manage
		 * counters.
		 */
		report_send_completion(put_req, true);

		ofi_atomic_dec32(&put_req->send.txc->otx_reqs);
		cxip_evtq_req_free(put_req);

		return FI_SUCCESS;

	default:
		TXC_FATAL(txc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}
}

/*
 * cxip_oflow_bufpool_fini() - Finalize overflow buffers used for messaging.
 *
 * Must be called with the RX PtlTE disabled.
 */
void cxip_oflow_bufpool_fini(struct cxip_rxc *rxc)
{
	struct cxip_deferred_event *def_ev = NULL;
	struct cxip_ptelist_buf *oflow_buf;
	struct dlist_entry *tmp;
	int i;
	int def_events = 0;

	/* Clean up unexpected Put records. The PtlTE is disabled, so no more
	 * events can be expected.
	 */
	for (i = 0; i < CXIP_DEF_EVENT_HT_BUCKETS; i++) {
		dlist_foreach_container_safe(&rxc->deferred_events.bh[i],
					     struct cxip_deferred_event,
					     def_ev, rxc_entry, tmp) {
			/* Dropping the last reference will cause the
			 * oflow_buf to be removed from the RXC list and
			 * freed.
			 */
			oflow_buf = def_ev->req->req_ctx;

			if (oflow_buf->le_type == CXIP_LE_TYPE_RX)
				oflow_req_put_bytes(def_ev->req,
					    def_ev->ev.tgt_long.mlength);

			free_put_event(rxc, def_ev);
			def_events++;
		}
	}

	if (def_events)
		RXC_DBG(rxc, "Freed %d deferred event(s)\n", def_events);

	cxip_ptelist_bufpool_fini(rxc->oflow_list_bufpool);
}

int cxip_oflow_bufpool_init(struct cxip_rxc *rxc)
{
	struct cxip_ptelist_bufpool_attr attr = {
		.list_type = C_PTL_LIST_OVERFLOW,
		.ptelist_cb = cxip_oflow_cb,
		.buf_size = cxip_env.oflow_buf_size,
		.min_posted = cxip_env.oflow_buf_min_posted,
		.max_posted = cxip_env.oflow_buf_min_posted, /* min == max */
		.max_cached = cxip_env.oflow_buf_max_cached,
		.min_space_avail = rxc->max_eager_size,
	};

	return cxip_ptelist_bufpool_init(rxc, &rxc->oflow_list_bufpool, &attr);
}

/*
 * cxip_rdzv_done_notify() - Sends a rendezvous complete from target to source
 *
 * Sends a zero byte matching notification to the source of rendezvous
 * indicating completion of a rendezvous. This is used when restricted get
 * DMA (CXIP_RDZV_PROTO_ALT_READ) is used to transfer non-eager data.
 */
static int cxip_rdzv_done_notify(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	union c_fab_addr dfa;
	uint32_t pid_idx = CXIP_PTL_IDX_RDZV_DEST;
	uint32_t match_id;
	struct c_full_dma_cmd cmd = {};
	union cxip_match_bits mb = {};
	int ret;
	uint8_t idx_ext;

	mb.rdzv_id_lo = req->recv.rdzv_id;
	mb.rdzv_id_hi = req->recv.rdzv_id >> CXIP_RDZV_ID_CMD_WIDTH;
	mb.rdzv_done = 1;
	mb.le_type = CXIP_LE_TYPE_ZBP;

	cxi_build_dfa(req->recv.rget_nic, req->recv.rget_pid, rxc->pid_bits,
		      pid_idx, &dfa, &idx_ext);
	match_id = CXI_MATCH_ID(rxc->pid_bits, rxc->ep_obj->src_addr.pid,
				rxc->ep_obj->src_addr.nic);

	cmd.command.cmd_type = C_CMD_TYPE_DMA;
	cmd.command.opcode = C_CMD_PUT;
	cmd.index_ext = idx_ext;
	cmd.event_send_disable = 1;
	cmd.dfa = dfa;
	cmd.eq = cxip_evtq_eqn(&rxc->ep_obj->txc.tx_evtq);
	cmd.user_ptr = (uint64_t)req;
	cmd.initiator = match_id;
	cmd.match_bits = mb.raw;

	ret = cxi_cq_emit_dma(rxc->tx_cmdq->dev_cmdq, &cmd);
	if (ret != FI_SUCCESS) {
		RXC_DBG(rxc, "Faile to write notify IDC: %d %s\n",
			ret, fi_strerror(-ret));
		return -FI_EAGAIN;
	}

	cxi_cq_ring(rxc->tx_cmdq->dev_cmdq);

	RXC_DBG(rxc, "RDZV done notify send RDZV ID: %d\n",
		req->recv.rdzv_id);

	return FI_SUCCESS;
}

static int cxip_recv_rdzv_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	struct cxip_deferred_event *def_ev;
	int event_rc;
	int ret;
	bool matched;

	switch (event->hdr.event_type) {
	/* When errors happen, send events can occur before the put/get event.
	 * These events should just be dropped.
	 */
	case C_EVENT_SEND:
		RXC_WARN(rxc, CXIP_UNEXPECTED_EVENT,
			 cxi_event_to_str(event),
			 cxi_rc_to_str(cxi_event_rc(event)));
		return FI_SUCCESS;

	case C_EVENT_PUT_OVERFLOW:
		/* We matched an unexpected header */
		/* Check for a previously received unexpected Put event,
		 * if not found defer until it arrives.
		 */
		def_ev = match_put_event(rxc, req, event, &matched);
		if (!def_ev)
			return -FI_EAGAIN;

		/* For multi-recv, management of start_offset requires events
		 * manage_local related events to arrive in order.
		 * Only C_EVENT_PUT_OVERFLOW events meet this criteria.
		 */
		def_ev->mrecv_start = req->recv.start_offset;
		def_ev->mrecv_len =
			mrecv_req_put_bytes(req, event->tgt_long.rlength);

		if (req->recv.multi_recv && event->tgt_long.auto_unlinked) {
			/* If a C_EVENT_PUT_OVERFLOW unlinks a multi-recv
			 * buffer, mrecv_start contains the number of bytes
			 * consumed before this C_EVENT_PUT_OVERFLOW. Adding in
			 * mrecv_len gets the total bytes consumed.
			 */
			req->recv.auto_unlinked = true;
			req->recv.mrecv_unlink_bytes =
				def_ev->mrecv_start + def_ev->mrecv_len;
		}

		ofi_atomic_dec32(&rxc->orx_hw_ule_cnt);

		if (!matched)
			return FI_SUCCESS;

		RXC_DBG(rxc, "Matched deferred event: %p\n", def_ev);

		ret = cxip_ux_send(req, def_ev->req, &def_ev->ev,
				   def_ev->mrecv_start, def_ev->mrecv_len,
				   false);
		if (ret == FI_SUCCESS) {
			free_put_event(rxc, def_ev);
		} else {
			/* undo mrecv_req_put_bytes() and orx_hw_ule_cnt dec */
			req->recv.start_offset -= def_ev->mrecv_len;
			ofi_atomic_inc32(&rxc->orx_hw_ule_cnt);
		}

		return ret;
	case C_EVENT_PUT:
		/* Eager data was delivered directly to the user buffer. */
		if (req->recv.multi_recv) {
			if (event->tgt_long.auto_unlinked) {
				uintptr_t mrecv_head;
				uintptr_t mrecv_tail;
				size_t mrecv_bytes_remaining;
				size_t rlen;

				/* For C_EVENT_PUT, need to calculate how much
				 * of the multi-recv buffer was consumed while
				 * factoring in any truncation.
				 */
				mrecv_head =
					CXI_IOVA_TO_VA(req->recv.recv_md->md,
						       event->tgt_long.start);
				mrecv_tail = (uintptr_t)req->recv.recv_buf +
					req->recv.ulen;
				mrecv_bytes_remaining = mrecv_tail - mrecv_head;
				rlen = MIN(mrecv_bytes_remaining,
					   event->tgt_long.rlength);

				req->recv.auto_unlinked = true;
				req->recv.mrecv_unlink_bytes =
					mrecv_head -
					(uintptr_t)req->recv.recv_buf + rlen;
			}

			req = rdzv_mrecv_req_event(req, event);
			if (!req)
				return -FI_EAGAIN;

			/* Set start pointer and data_len using Rendezvous or
			 * Put Overflow event (depending on if message was
			 * unexpected).
			 */
		}

		recv_req_tgt_event(req, event);

		/* Count the rendezvous event. */
		rdzv_recv_req_event(req, event->hdr.event_type);
		return FI_SUCCESS;
	case C_EVENT_RENDEZVOUS:
		if (req->recv.multi_recv) {
			req = rdzv_mrecv_req_event(req, event);
			if (!req)
				return -FI_EAGAIN;

			/* Use Rendezvous event to set start pointer and
			 * data_len for expected Sends.
			 */
			struct cxip_req *parent = req->recv.parent;
			size_t mrecv_bytes_remaining;

			req->buf = CXI_IOVA_TO_VA(
					parent->recv.recv_md->md,
					event->tgt_long.start) -
					event->tgt_long.mlength;
			req->recv.recv_buf = (void *)req->buf;

			mrecv_bytes_remaining =
				(uint64_t)parent->recv.recv_buf +
				parent->recv.ulen -
				(uint64_t)req->recv.recv_buf;
			req->data_len = MIN(mrecv_bytes_remaining,
					    event->tgt_long.rlength);
		} else {
			req->data_len = MIN(req->recv.ulen, event->tgt_long.rlength);
		}

		recv_req_tgt_event(req, event);

		if (!event->tgt_long.get_issued) {
			if (ofi_atomic_inc32(&rxc->orx_tx_reqs) >
			    rxc->max_tx || issue_rdzv_get(req)) {

				/* Could not issue get */
				ofi_atomic_dec32(&rxc->orx_tx_reqs);

				/* Undo multi-recv event processing. */
				if (req->recv.multi_recv &&
				    !req->recv.rdzv_events) {
					dlist_remove(&req->recv.children);
					cxip_evtq_req_free(req);
				}
				return -FI_EAGAIN;
			}

			RXC_DBG(rxc, "Software issued Get, req: %p\n", req);
		}

		/* Count the rendezvous event. */
		rdzv_recv_req_event(req, event->hdr.event_type);
		return FI_SUCCESS;
	case C_EVENT_REPLY:
		/* If mrecv, look up the correct child request. */
		if (req->recv.multi_recv) {
			req = rdzv_mrecv_req_event(req, event);
			if (!req)
				return -FI_EAGAIN;
		}

		/* If a rendezvous operation requires a done notification
		 * send it. Must wait for the ACK from the notify to be returned
		 * before completing the target operation.
		 */
		if (req->recv.done_notify) {
			if (ofi_atomic_inc32(&rxc->orx_tx_reqs) > rxc->max_tx ||
			    cxip_rdzv_done_notify(req)) {

				/* Could not issue notify, will be retried */
				ofi_atomic_dec32(&rxc->orx_tx_reqs);
				return -FI_EAGAIN;
			}
		}

		/* Rendezvous Get completed, update event counts and
		 * complete if using unrestricted get protocol.
		 */
		req->recv.rc = cxi_init_event_rc(event);
		rdzv_recv_req_event(req, event->hdr.event_type);

		/* If RGet initiated by software return the TX credit */
		if (!event->init_short.rendezvous) {
			ofi_atomic_dec32(&rxc->orx_tx_reqs);
			assert(ofi_atomic_get32(&rxc->orx_tx_reqs) >= 0);
		}

		return FI_SUCCESS;

	case C_EVENT_ACK:
		event_rc = cxi_init_event_rc(event);
		if (event_rc != C_RC_OK)
			RXC_WARN(rxc, "%#x:%u Bad RDZV notify ACK status %s\n",
				 req->recv.rget_nic, req->recv.rget_pid,
				 cxi_rc_to_str(event_rc));

		/* Special case of the ZBP destination EQ being full and ZBP
		 * could not complete. This must be retried, we use the TX
		 * credit already allocated.
		 */
		if (event_rc == C_RC_ENTRY_NOT_FOUND) {
			usleep(CXIP_DONE_NOTIFY_RETRY_DELAY_US);

			if (cxip_rdzv_done_notify(req))
				return -FI_EAGAIN;

			return FI_SUCCESS;
		}

		/* Reflect the completion status of the ACK in the target
		 * side completion so that a failure will not go undetected.
		 */
		req->recv.rc = event_rc;
		ofi_atomic_dec32(&req->recv.rxc->orx_tx_reqs);
		rdzv_recv_req_event(req, event->hdr.event_type);

		return FI_SUCCESS;

	default:
		RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}
}

static void cxip_rxc_record_req_stat(struct cxip_rxc *rxc, enum c_ptl_list list,
				     size_t rlength, struct cxip_req *req)
{
	enum fi_hmem_iface iface = rlength ? req->recv.recv_md->info.iface : FI_HMEM_SYSTEM;

	cxip_msg_counters_msg_record(&rxc->cntrs, list, iface, rlength);
}

/*
 * cxip_recv_cb() - Process a user receive buffer event.
 *
 * A user receive buffer is described by an LE linked to the Priority list.
 * Link, Unlink, Put, Put Overflow, and Reply events are expected from a user
 * receive buffer.
 *
 * A Link event indicates that a new user buffer has been linked to the
 * priority list. Successful Link events may be suppressed.
 *
 * An Unlink event indicates that a user buffer has been unlinked. Normally, a
 * receive is used once and unlinked when it is matched with a Send. In this
 * case, a successful Unlink event may be suppressed.
 *
 * For expected, eager Sends, a Put will be matched to a user receive buffer by
 * the NIC. Send data is copied directly to the user buffer. A Put event is
 * generated describing the match.
 *
 * For unexpected, eager Sends, a Put will first match a buffer in the Overflow
 * list. See cxip_oflow_cb() for details on Overflow event handling. Once a
 * matching user receive buffer is appended to the Priority list, a Put
 * Overflow event is generated. Put and Put Overflow events for an unexpected,
 * eager Send must be correlated. These events may arrive in any order. Once
 * both events are accounted, data is copied from the Overflow buffer to the
 * user receive buffer.
 *
 * Unexpected, eager Sends that are longer than the eager threshold have their
 * data truncated to zero. This is to avoid long messages consuming too much
 * Overflow buffer space at the target. Once a match is made with a user
 * receive buffer, data is re-read from the initiator using a Get.
 *
 * Rendezvous receive events are handled by cxip_recv_rdzv_cb().
 */
static int cxip_recv_cb(struct cxip_req *req, const union c_event *event)
{
	int ret;
	struct cxip_rxc *rxc = req->recv.rxc;
	struct cxip_deferred_event *def_ev;
	bool rdzv = false;
	bool matched;

	/* Common processing for rendezvous and non-rendezvous events.
	 * TODO: Avoid having two switch statements for event_type.
	 */
	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		/* In cases where the LE pool entry reservation is insufficient
		 * to meet priority list buffers (due to multiple EP sharing an
		 * LE Pool or insufficient LE Pool reservation value), then
		 * enabling the periodic checking of LE allocations can be
		 * used to force preemptive transitions to software match mode.
		 */
		if (cxi_tgt_event_rc(event) == C_RC_OK) {

			if (!cxip_env.hybrid_recv_preemptive)
				return FI_SUCCESS;

			/* Check for possible hybrid mode preemptive
			 * transitions to software managed mode.
			 */
			if (cxip_rxp_check_le_usage_hybrid_preempt(rxc, event))
				RXC_WARN(rxc,
					 "Force preemptive switch to SW EP\n");

			return FI_SUCCESS;
		}

		/* If endpoint has been disabled and an append fails, free the
		 * user request without reporting any event.
		 */
		if (rxc->state == RXC_DISABLED) {
			cxip_recv_req_free(req);
			return FI_SUCCESS;
		}

		/* Save append to repost, NIC will initiate transition to
		 * software managed EP.
		 */
		if (cxi_tgt_event_rc(event) == C_RC_PTLTE_SW_MANAGED) {
			RXC_WARN(rxc, "Append err, transitioning to SW\n");
			cxip_recv_req_dropped(req);

			return FI_SUCCESS;
		}

		/* Transition into onload and flow control if an append
		 * fails.
		 */
		if (cxi_tgt_event_rc(event) != C_RC_NO_SPACE)
			RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT_STS,
				  cxi_event_to_str(event),
				  cxi_rc_to_str(cxi_tgt_event_rc(event)));

		RXC_WARN(rxc, "Append err, priority LE exhaustion\n");

		/* Manually transition to DISABLED to initiate flow control
		 * and onload  instead of waiting for eventual NIC no match
		 * transition.
		 */
		ret = cxip_recv_pending_ptlte_disable(rxc, true);
		if (ret != FI_SUCCESS)
			RXC_WARN(rxc, "Force disable failed %d %s\n",
				 ret, fi_strerror(-ret));

		ret = FI_SUCCESS;
		cxip_recv_req_dropped(req);

		return ret;

	case C_EVENT_UNLINK:
		assert(!event->tgt_long.auto_unlinked);

		/* TODO: This is broken with multi-recv. The multi-recv request
		 * may be freed with pending child requests.
		 */
		req->recv.unlinked = true;
		recv_req_report(req);
		cxip_recv_req_free(req);

		return FI_SUCCESS;

	case C_EVENT_PUT_OVERFLOW:
		cxip_rxc_record_req_stat(rxc, C_PTL_LIST_OVERFLOW,
					 event->tgt_long.rlength, req);

		/* ULE freed. Update RXC state to signal that the RXC should
		 * be reenabled.
		 */
		/* TODO: this is not atomic, there must be a better way */
		if (rxc->state == RXC_ONLOAD_FLOW_CONTROL)
			rxc->state = RXC_ONLOAD_FLOW_CONTROL_REENABLE;
		break;

	case C_EVENT_PUT:
		cxip_rxc_record_req_stat(rxc, C_PTL_LIST_PRIORITY,
					 event->tgt_long.rlength, req);
		break;
	default:
		break;
	}

	/* All events related to an offloaded rendezvous receive will be
	 * handled by cxip_recv_rdzv_cb(). Those events are identified by the
	 * event rendezvous field. Two exceptions are a Reply event generated
	 * from a SW-issued Get, and a Ack for a software done notification
	 * when using restricted eager get. When such an event is generated,
	 * the request will have already processed a Rendezvous event. If the
	 * rendezvous field is not set, but the rdzv_events count is elevated,
	 * this must be a SW-issued Reply or Ack event.
	 */
	if (event->hdr.event_type == C_EVENT_REPLY ||
	    event->hdr.event_type == C_EVENT_ACK)
		rdzv = (event->init_short.rendezvous || req->recv.rdzv_events);
	else
		rdzv = event->tgt_long.rendezvous;

	if (rdzv)
		return cxip_recv_rdzv_cb(req, event);

	switch (event->hdr.event_type) {
	case C_EVENT_SEND:
		/* TODO Handle Send event errors. */
		assert(cxi_event_rc(event) == C_RC_OK);
		return FI_SUCCESS;
	case C_EVENT_PUT_OVERFLOW:
		/* We matched an unexpected header */
		/* Unexpected 0-byte Put events are dropped. Skip matching. */
		if (!event->tgt_long.rlength) {
			ret = cxip_ux_send_zb(req, event,
					      req->recv.start_offset, false);
			if (ret == FI_SUCCESS)
				ofi_atomic_dec32(&rxc->orx_hw_ule_cnt);

			return ret;
		}

		/* Check for a previously received unexpected Put event,
		 * if not found defer until it arrives.
		 */
		def_ev = match_put_event(rxc, req, event, &matched);
		if (!def_ev)
			return -FI_EAGAIN;

		/* For multi-recv, management of start_offset requires events
		 * manage_local related events to arrive in order.
		 * Only C_EVENT_PUT_OVERFLOW events meet this criteria.
		 */
		def_ev->mrecv_start = req->recv.start_offset;
		def_ev->mrecv_len =
			mrecv_req_put_bytes(req, event->tgt_long.rlength);

		if (req->recv.multi_recv && event->tgt_long.auto_unlinked) {
			/* If a C_EVENT_PUT_OVERFLOW unlinks a multi-recv
			 * buffer, mrecv_start contains the number of bytes
			 * consumed before this C_EVENT_PUT_OVERFLOW. Adding in
			 * mrecv_len gets the total bytes consumed.
			 */
			req->recv.auto_unlinked = true;
			req->recv.mrecv_unlink_bytes =
				def_ev->mrecv_start + def_ev->mrecv_len;
		}

		ofi_atomic_dec32(&rxc->orx_hw_ule_cnt);

		if (!matched)
			return FI_SUCCESS;

		ret = cxip_ux_send(req, def_ev->req, &def_ev->ev,
				   def_ev->mrecv_start, def_ev->mrecv_len,
				   false);
		if (ret == FI_SUCCESS) {
			free_put_event(rxc, def_ev);
		} else {
			/* undo mrecv_req_put_bytes() and orx_hw_ule_cnt dec */
			req->recv.start_offset -= def_ev->mrecv_len;
			ofi_atomic_inc32(&rxc->orx_hw_ule_cnt);
		}

		return ret;
	case C_EVENT_PUT:
		/* Data was delivered directly to the user buffer. Complete the
		 * request.
		 */
		if (req->recv.multi_recv) {
			if (event->tgt_long.auto_unlinked) {
				uintptr_t mrecv_head;

				/* For C_EVENT_PUT, need to calculate how much
				 * of the multi-recv buffer was consumed while
				 * factoring in any truncation.
				 */
				mrecv_head =
					CXI_IOVA_TO_VA(req->recv.recv_md->md,
						       event->tgt_long.start);

				req->recv.auto_unlinked = true;
				req->recv.mrecv_unlink_bytes =
					mrecv_head -
					(uintptr_t)req->recv.recv_buf +
					event->tgt_long.mlength;
			}

			req = mrecv_req_dup(req);
			if (!req)
				return -FI_EAGAIN;
			recv_req_tgt_event(req, event);

			req->buf = (uint64_t)(CXI_IOVA_TO_VA(
					req->recv.recv_md->md,
					event->tgt_long.start));
			req->data_len = event->tgt_long.mlength;

			recv_req_report(req);
			cxip_evtq_req_free(req);
		} else {
			req->data_len = event->tgt_long.mlength;
			recv_req_tgt_event(req, event);
			recv_req_report(req);
			cxip_recv_req_free(req);
		}
		return FI_SUCCESS;

	case C_EVENT_REPLY:
		/* Long-send Get completed. Complete the request. */
		req->recv.rc = cxi_init_event_rc(event);

		recv_req_report(req);
		if (req->recv.multi_recv)
			cxip_evtq_req_free(req);
		else
			cxip_recv_req_free(req);

		return FI_SUCCESS;
	default:
		RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}
}

/*
 * cxip_recv_cancel() - Cancel outstanding receive request.
 */
int cxip_recv_cancel(struct cxip_req *req)
{
	int ret = FI_SUCCESS;
	struct cxip_rxc *rxc = req->recv.rxc;

	/* In hybrid mode requests could be on priority list
	 * or software receive list.
	 */
	if (req->recv.software_list) {
		dlist_remove_init(&req->recv.rxc_entry);
		req->recv.canceled = true;
		req->recv.unlinked = true;
		recv_req_report(req);
		cxip_recv_req_free(req);
	} else {
		ret = cxip_pte_unlink(rxc->rx_pte, C_PTL_LIST_PRIORITY,
				req->req_id, rxc->rx_cmdq);
		if (ret == FI_SUCCESS)
			req->recv.canceled = true;
	}
	return ret;
}

/*
 * cxip_recv_reenable() - Attempt to re-enable the RX queue.
 *
 * Called by disabled EP ready to re-enable.
 *
 * Determine if the RX queue can be re-enabled and perform a state change
 * command if necessary. The Endpoint must receive dropped Send notifications
 * from all peers who experienced drops before re-enabling the RX queue.
 *
 * Caller must hold ep_obj->lock.
 */
int cxip_recv_reenable(struct cxip_rxc *rxc)
{
	struct cxi_pte_status pte_status = {};
	int ret __attribute__((unused));

	if (rxc->drop_count == -1) {
		RXC_WARN(rxc, "Waiting for pending FC_NOTIFY messages\n");
		return -FI_EAGAIN;
	}

	ret = cxil_pte_status(rxc->rx_pte->pte, &pte_status);
	assert(!ret);

	if (rxc->drop_count != pte_status.drop_count) {
		RXC_DBG(rxc, "Processed %d/%d drops\n",
			rxc->drop_count, pte_status.drop_count);
		return -FI_EAGAIN;
	}

	RXC_WARN(rxc, "Re-enabling PTE, drop_count %d\n",
		 rxc->drop_count);

	do {
		ret = cxip_rxc_msg_enable(rxc, rxc->drop_count);
		if (ret == -FI_EAGAIN &&
		    rxc->new_state == RXC_ENABLED_SOFTWARE) {
			RXC_WARN(rxc,
				 "PTE disable->sm drop mismatch, will retry\n");
			break;
		}
	} while (ret == -FI_EAGAIN);

	if (ret != FI_SUCCESS && ret != -FI_EAGAIN)
		RXC_FATAL(rxc, "cxip_rxc_msg_enable failed: %d\n", ret);

	return ret;
}

/*
 * cxip_fc_resume_cb() - Process FC resume completion events.
 */
int cxip_fc_resume_cb(struct cxip_ctrl_req *req, const union c_event *event)
{
	struct cxip_fc_drops *fc_drops = container_of(req,
			struct cxip_fc_drops, req);
	struct cxip_rxc *rxc = fc_drops->rxc;
	int ret = FI_SUCCESS;

	switch (event->hdr.event_type) {
	case C_EVENT_ACK:
		switch (cxi_event_rc(event)) {
		case C_RC_OK:
			RXC_DBG(rxc,
				"FC_RESUME to %#x:%u successfully sent: retry_count=%u\n",
				fc_drops->nic_addr, fc_drops->pid,
				fc_drops->retry_count);
			free(fc_drops);
			break;

		/* This error occurs when the target's control event queue has
		 * run out of space. Since the target should be processing the
		 * event queue, it is safe to replay messages until C_RC_OK is
		 * returned.
		 */
		case C_RC_ENTRY_NOT_FOUND:
			fc_drops->retry_count++;
			RXC_WARN(rxc,
				 "%#x:%u dropped FC message: retry_delay_usecs=%d retry_count=%u\n",
				 fc_drops->nic_addr, fc_drops->pid,
				 cxip_env.fc_retry_usec_delay,
				 fc_drops->retry_count);
			usleep(cxip_env.fc_retry_usec_delay);
			ret = cxip_ctrl_msg_send(req);
			break;
		default:
			RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT_STS,
				  cxi_event_to_str(event),
				  cxi_rc_to_str(cxi_event_rc(event)));
		}
		break;
	default:
		RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}

	return ret;
}

/*
 * cxip_fc_process_drops() - Process a dropped Send notification from a peer.
 *
 * Called by disabled EP waiting to re-enable.
 *
 * When a peer detects dropped Sends it follows up by sending a message to the
 * disabled Endpoint indicating the number of drops experienced. The disabled
 * Endpoint peer must count all drops before re-enabling its RX queue.
 */
int cxip_fc_process_drops(struct cxip_ep_obj *ep_obj, uint32_t nic_addr,
			  uint32_t pid, uint16_t drops)
{
	struct cxip_rxc *rxc = &ep_obj->rxc;
	struct cxip_fc_drops *fc_drops;
	int ret __attribute__((unused));

	fc_drops = calloc(1, sizeof(*fc_drops));
	if (!fc_drops) {
		RXC_WARN(rxc, "Failed to allocate drops\n");
		return -FI_ENOMEM;
	}

	/* TODO: Cleanup cxip_fc_drops fields. Many of the fields are redundant
	 * with the req structure.
	 */
	fc_drops->rxc = rxc;
	fc_drops->nic_addr = nic_addr;
	fc_drops->pid = pid;
	fc_drops->drops = drops;

	fc_drops->req.send.nic_addr = nic_addr;
	fc_drops->req.send.pid = pid;
	fc_drops->req.send.mb.drops = drops;

	fc_drops->req.send.mb.ctrl_le_type = CXIP_CTRL_LE_TYPE_CTRL_MSG;
	fc_drops->req.send.mb.ctrl_msg_type = CXIP_CTRL_MSG_FC_RESUME;
	fc_drops->req.cb = cxip_fc_resume_cb;
	fc_drops->req.ep_obj = rxc->ep_obj;

	dlist_insert_tail(&fc_drops->rxc_entry, &rxc->fc_drops);

	RXC_DBG(rxc, "Processed drops: %d NIC: %#x PID: %d\n",
		drops, nic_addr, pid);

	rxc->drop_count += drops;

	/* Wait until search and delete completes before attempting to
	 * re-enable.
	 */
	if (rxc->state == RXC_FLOW_CONTROL) {
		ret = cxip_recv_reenable(rxc);
		assert(ret == FI_SUCCESS || ret == -FI_EAGAIN);

		/* Disable to software managed transition is synchronous
		 * in order to handle drop count mismatches correctly. If
		 * successful the H/W transition completed, otherwise it
		 * will be retried when notified and count matches.
		 */
		if (rxc->new_state == RXC_ENABLED_SOFTWARE &&
		    ret == FI_SUCCESS) {
			cxip_fc_progress_ctrl(rxc);
			rxc->state = RXC_ENABLED_SOFTWARE;
			RXC_WARN(rxc, "Now in RXC_ENABLED_SOFTWARE\n");
		}
	}

	return FI_SUCCESS;
}

/*
 * cxip_recv_replay() - Replay dropped Receive requests.
 *
 * When no LE is available while processing an Append command, the command is
 * dropped and future appends are disabled. After all outstanding commands are
 * dropped and resources are recovered, replayed all Receive requests in order.
 *
 * Caller must hold ep_obj->lock.
 */
static int cxip_recv_replay(struct cxip_rxc *rxc)
{
	struct cxip_req *req;
	struct dlist_entry *tmp;
	bool restart_seq = true;
	int ret;

	dlist_foreach_container_safe(&rxc->replay_queue,
				     struct cxip_req, req,
				     recv.rxc_entry, tmp) {
		dlist_remove_init(&req->recv.rxc_entry);

		/* Since the RXC and PtlTE are in a controlled state and no new
		 * user receives are being posted, it is safe to ignore the RXC
		 * state when replaying failed user posted receives.
		 */
		ret = cxip_recv_req_queue(req, restart_seq);

		/* Match made in software? */
		if (ret == -FI_EALREADY)
			continue;

		/* TODO: Low memory or full CQ during SW matching would cause
		 * -FI_EAGAIN to be seen here.
		 */
		assert(ret == FI_SUCCESS);

		restart_seq = false;
	}

	return FI_SUCCESS;
}

/*
 * cxip_recv_resume() - Send a resume message to all peers who reported dropped
 * Sends.
 *
 * Called by disabled EP after re-enable.
 *
 * After counting all dropped sends targeting a disabled RX queue and
 * re-enabling the queue, notify all peers who experienced dropped Sends so
 * they can be replayed.
 *
 * Caller must hold ep_obj->lock.
 */
int cxip_recv_resume(struct cxip_rxc *rxc)
{
	struct cxip_fc_drops *fc_drops;
	struct dlist_entry *tmp;
	int ret;

	dlist_foreach_container_safe(&rxc->fc_drops,
				     struct cxip_fc_drops, fc_drops,
				     rxc_entry, tmp) {
		ret = cxip_ctrl_msg_send(&fc_drops->req);
		if (ret)
			return ret;

		dlist_remove(&fc_drops->rxc_entry);
	}

	return FI_SUCCESS;
}

/*
 * cxip_fc_progress_ctrl() - Progress the control EP until all resume
 * control messages can be queued.
 *
 * Caller must hold ep_obj->lock.
 */
static void cxip_fc_progress_ctrl(struct cxip_rxc *rxc)
{
	int ret __attribute__((unused));

	assert(rxc->state == RXC_FLOW_CONTROL);

	/* Successful transition from disabled occurred, reset
	 * drop count.
	 */
	rxc->drop_count = rxc->ep_obj->asic_ver < CASSINI_2_0 ? -1 : 0;

	while ((ret = cxip_recv_resume(rxc)) == -FI_EAGAIN)
		cxip_ep_tx_ctrl_progress_locked(rxc->ep_obj);

	assert(ret == FI_SUCCESS);
}

/*
 * cxip_post_ux_onload_sw() - Nic HW-to-SW EP post UX onload processing.
 *
 * PTE transitioned from enabled to software managed. Onloading
 * was done and appends that failed need to be replayed.
 */
static void cxip_post_ux_onload_sw(struct cxip_rxc *rxc)
{
	int ret;

	assert(cxip_env.rx_match_mode == CXIP_PTLTE_HYBRID_MODE);
	assert(rxc->prev_state == RXC_ENABLED);
	assert(rxc->new_state == RXC_ENABLED_SOFTWARE);

	ret = cxip_ptelist_buf_replenish(rxc->req_list_bufpool,
					 true);
	if (ret != FI_SUCCESS)
		RXC_WARN(rxc, "Request list replenish failed %d %s\n",
			 ret, fi_strerror(-ret));

	/* Priority list appends that failed during the transition can
	 * now be replayed.
	 */
	ret = cxip_recv_replay(rxc);
	assert(ret == FI_SUCCESS || ret == -FI_EAGAIN);

	if (rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED) {
		/* Transition from enabled to software managed is complete.
		 * Allow posting of receive operations.
		 */
		RXC_WARN(rxc, "Now in RXC_ENABLED_SOFTWARE\n");
		rxc->state = RXC_ENABLED_SOFTWARE;
	}
}

/*
 * cxip_post_ux_onload_fc() - Flow control onload complete processing.
 *
 * PTE transitioned to disabled and UX onload has completed.
 */
static void cxip_post_ux_onload_fc(struct cxip_rxc *rxc)
{
	int ret;

	/* Disable RX matching offload if transitioning to
	 * software enabled EP.
	 */
	if (rxc->new_state == RXC_ENABLED_SOFTWARE) {
		RXC_DBG(rxc, "Transitioning to SW EP\n");
		rxc->msg_offload = 0;
	}

	if (rxc->fc_reason == C_SC_FC_EQ_FULL)
		goto replay;

	if (rxc->new_state == RXC_ENABLED_SOFTWARE)
		ret = cxip_ptelist_buf_replenish(rxc->req_list_bufpool,
						 true);
	else
		ret = cxip_ptelist_buf_replenish(rxc->oflow_list_bufpool,
						 true);
	if (ret != FI_SUCCESS)
		RXC_WARN(rxc, "%s buffer replenish failed %d %s\n",
			 rxc->new_state == RXC_ENABLED_SOFTWARE ?
			 "Request" : "Overflow", ret, fi_strerror(-ret));

replay:
	/* Any priority list appends that failed during the transition
	 * can now be replayed.
	 */
	if (rxc->new_state == RXC_ENABLED)
		rxc->msg_offload = 1;

	ret = cxip_recv_replay(rxc);
	RXC_DBG(rxc, "Replay of failed receives ret: %d %s\n",
		ret, fi_strerror(-ret));
	assert(ret == FI_SUCCESS || ret == -FI_EAGAIN);

	if (rxc->state != RXC_ONLOAD_FLOW_CONTROL_REENABLE &&
	    rxc->new_state != RXC_ENABLED_SOFTWARE)
		RXC_FATAL(rxc, FC_SW_ONLOAD_MSG_FATAL);

	rxc->state = RXC_FLOW_CONTROL;
	ret = cxip_recv_reenable(rxc);
	assert(ret == FI_SUCCESS || ret == -FI_EAGAIN);
	RXC_WARN(rxc, "Now in RXC_FLOW_CONTROL\n");

	/* Disable to software managed transition is synchronous in order to
	 * handle drop count mismatches correctly. If successful the H/W
	 * transition completed, otherwise the transition will occur when
	 * additional drop notifies are received.
	 */
	if (rxc->new_state == RXC_ENABLED_SOFTWARE && ret == FI_SUCCESS) {
		cxip_fc_progress_ctrl(rxc);
		rxc->state = RXC_ENABLED_SOFTWARE;
		RXC_WARN(rxc, "Now in RXC_ENABLED_SOFTWARE\n");
	}
}

/*
 * cxip_ux_onload_complete() - Unexpected list entry onload complete.
 *
 * All unexpected message headers have been onloaded from hardware.
 */
static void cxip_ux_onload_complete(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->search.rxc;

	assert(rxc->state == RXC_ONLOAD_FLOW_CONTROL_REENABLE ||
	       rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED);

	free(rxc->ule_offsets);
	rxc->ule_offsets = 0;

	/* During a transition to software managed PtlTE, received
	 * request list entries resulting from hardware not matching
	 * the priority list on an incoming packet were added to a
	 * pending unexpected message list. We merge the two
	 * expected list here.
	 */
	RXC_DBG(rxc, "Req pending %d UX entries, SW list %d UX entries\n",
		rxc->sw_pending_ux_list_len, rxc->sw_ux_list_len);

	dlist_splice_tail(&rxc->sw_ux_list, &rxc->sw_pending_ux_list);
	rxc->sw_ux_list_len += rxc->sw_pending_ux_list_len;
	rxc->sw_pending_ux_list_len = 0;

	RXC_WARN(rxc, "Software UX list updated, %d SW UX entries\n",
		 rxc->sw_ux_list_len);

	if (rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED)
		cxip_post_ux_onload_sw(rxc);
	else
		cxip_post_ux_onload_fc(rxc);

	ofi_atomic_dec32(&rxc->orx_reqs);
	cxip_evtq_req_free(req);
}

/*
 * cxip_get_ule_offsets() - Initialize an in-order array of ULE offsets
 *
 * If snapshot is requested, no more than two passes at getting offsets
 * will be made. This is intended to be used with FI_CLAIM processing,
 * where the PtlTE is enabled.
 */
static int cxip_get_ule_offsets(struct cxip_rxc *rxc, uint64_t **ule_offsets,
				unsigned int *num_ule_offsets, bool snapshot)
{
	struct cxi_pte_status pte_status = {
		.ule_count = 512
	};
	size_t cur_ule_count = 0;
	int ret;
	int calls = 0;

	/* Get all the unexpected header remote offsets. */
	*ule_offsets = NULL;
	*num_ule_offsets = 0;

	do {
		cur_ule_count = pte_status.ule_count;
		*ule_offsets = reallocarray(*ule_offsets, cur_ule_count,
					    sizeof(*ule_offsets));
		if (*ule_offsets == NULL) {
			RXC_WARN(rxc, "Failed allocate ule offset memory\n");
			ret = -FI_ENOMEM;
			goto err;
		}

		pte_status.ule_offsets = (void *)*ule_offsets;
		ret = cxil_pte_status(rxc->rx_pte->pte, &pte_status);
		assert(!ret);
	} while (cur_ule_count < pte_status.ule_count &&
		 !(snapshot && ++calls > 1));

	*num_ule_offsets = pte_status.ule_count;

	return FI_SUCCESS;
err:
	free(*ule_offsets);

	return ret;
}

/*
 * cxip_ux_onload_cb() - Process SEARCH_AND_DELETE command events.
 */
static int cxip_ux_onload_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->search.rxc;
	struct cxip_deferred_event *def_ev;
	struct cxip_ux_send *ux_send;
	bool matched;

	assert(rxc->state == RXC_ONLOAD_FLOW_CONTROL ||
	       rxc->state == RXC_ONLOAD_FLOW_CONTROL_REENABLE ||
	       rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED);

	switch (event->hdr.event_type) {
	case C_EVENT_PUT_OVERFLOW:
		assert(cxi_event_rc(event) == C_RC_OK);

		ux_send = calloc(1, sizeof(*ux_send));
		if (!ux_send) {
			RXC_WARN(rxc, "Failed allocate to memory\n");
			return -FI_EAGAIN;
		}

		/* Zero-byte unexpected onloads require special handling since
		 * no deferred structure would be allocated.
		 */
		if (event->tgt_long.rlength) {

			def_ev = match_put_event(rxc, req, event, &matched);
			if (!matched) {
				if (!def_ev) {
					free(ux_send);
					return -FI_EAGAIN;
				}

				/* Gather Put events later */
				def_ev->ux_send = ux_send;
				req->search.puts_pending++;
			} else {
				ux_send->req = def_ev->req;
				ux_send->put_ev = def_ev->ev;

				free_put_event(rxc, def_ev);
			}
		} else {
			ux_send->put_ev = *event;
		}

		/* For flow control transition if a ULE is freed, then
		 * set state so that re-enable will be attempted.
		 */
		if (rxc->state == RXC_ONLOAD_FLOW_CONTROL)
			rxc->state = RXC_ONLOAD_FLOW_CONTROL_REENABLE;

		/* Fixup event with the expected remote offset for an RGet. */
		if (event->tgt_long.rlength) {
			ux_send->put_ev.tgt_long.remote_offset =
				rxc->ule_offsets[rxc->cur_ule_offsets] +
				event->tgt_long.mlength;
		}
		rxc->cur_ule_offsets++;

		dlist_insert_tail(&ux_send->rxc_entry, &rxc->sw_ux_list);
		rxc->sw_ux_list_len++;

		RXC_DBG(rxc, "Onloaded Send: %p\n", ux_send);

		ofi_atomic_dec32(&rxc->orx_hw_ule_cnt);

		break;
	case C_EVENT_SEARCH:
		if (rxc->new_state == RXC_ENABLED_SOFTWARE &&
		    rxc->state == RXC_ONLOAD_FLOW_CONTROL)
			rxc->state = RXC_ONLOAD_FLOW_CONTROL_REENABLE;

		if (rxc->state == RXC_ONLOAD_FLOW_CONTROL)
			RXC_FATAL(rxc, FC_SW_ONLOAD_MSG_FATAL);

		req->search.complete = true;
		rxc->rx_evtq.ack_batch_size = rxc->rx_evtq.cq->ack_batch_size;

		RXC_DBG(rxc, "UX Onload Search done\n");

		if (cxip_ux_is_onload_complete(req))
			cxip_ux_onload_complete(req);

		break;
	default:
		RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}

	return FI_SUCCESS;
}

/*
 * cxip_ux_onload() - Issue SEARCH_AND_DELETE command to on-load unexpected
 * Send headers queued on the RXC message queue.
 *
 * Caller must hold ep_obj->lock.
 */
static int cxip_ux_onload(struct cxip_rxc *rxc)
{
	struct cxip_req *req;
	union c_cmdu cmd = {};
	int ret;

	assert(rxc->state == RXC_ONLOAD_FLOW_CONTROL ||
	       rxc->state == RXC_ONLOAD_FLOW_CONTROL_REENABLE ||
	       rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED);

	RXC_DBG(rxc, "Initiate hardware UX list onload\n");

	/* Get all the unexpected header remote offsets. */
	rxc->ule_offsets = NULL;
	rxc->num_ule_offsets = 0;
	rxc->cur_ule_offsets = 0;

	ret = cxip_get_ule_offsets(rxc, &rxc->ule_offsets,
				   &rxc->num_ule_offsets, false);
	if (ret) {
		RXC_WARN(rxc, "Failed to read UX remote offsets: %d %s\n",
			 ret, fi_strerror(-ret));
		goto err;
	}

	/* Populate request */
	req = cxip_evtq_req_alloc(&rxc->rx_evtq, 1, NULL);
	if (!req) {
		RXC_DBG(rxc, "Failed to allocate request\n");
		ret = -FI_EAGAIN;
		goto err_free_onload_offset;
	}
	ofi_atomic_inc32(&rxc->orx_reqs);

	req->cb = cxip_ux_onload_cb;
	req->type = CXIP_REQ_SEARCH;
	req->search.rxc = rxc;

	cmd.command.opcode = C_CMD_TGT_SEARCH_AND_DELETE;
	cmd.target.ptl_list = C_PTL_LIST_UNEXPECTED;
	cmd.target.ptlte_index = rxc->rx_pte->pte->ptn;
	cmd.target.buffer_id = req->req_id;
	cmd.target.length = -1U;
	cmd.target.ignore_bits = -1UL;
	cmd.target.match_id = CXI_MATCH_ID_ANY;

	ret = cxi_cq_emit_target(rxc->rx_cmdq->dev_cmdq, &cmd);
	if (ret) {
		RXC_WARN(rxc, "Failed to write Search command: %d\n", ret);
		ret = -FI_EAGAIN;
		goto err_dec_free_cq_req;
	}

	cxi_cq_ring(rxc->rx_cmdq->dev_cmdq);

	return FI_SUCCESS;

err_dec_free_cq_req:
	ofi_atomic_dec32(&rxc->orx_reqs);
	cxip_evtq_req_free(req);
err_free_onload_offset:
	free(rxc->ule_offsets);
err:
	RXC_WARN(rxc, "Hardware UX list onload initiation error, ret: %d\n",
		 ret);
	return ret;
}

static int cxip_flush_appends_cb(struct cxip_req *req,
				 const union c_event *event)
{
	struct cxip_rxc *rxc = req->req_ctx;
	int ret;

	assert(rxc->state == RXC_ONLOAD_FLOW_CONTROL ||
	       rxc->state == RXC_ONLOAD_FLOW_CONTROL_REENABLE ||
	       rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED);

	assert(event->hdr.event_type == C_EVENT_SEARCH);
	assert(cxi_event_rc(event) == C_RC_NO_MATCH);

	ret = cxip_ux_onload(rxc);
	if (ret == FI_SUCCESS) {
		ofi_atomic_dec32(&rxc->orx_reqs);
		cxip_evtq_req_free(req);
	}

	return ret;
}

/*
 * cxip_flush_appends() - Flush all user appends for a RXC.
 *
 * Before cxip_ux_onload() can be called, all user appends in the command queue
 * must be flushed. If not, this can cause cxip_ux_onload() to read incorrect
 * remote offsets from cxil_pte_status(). The flush is implemented by issuing
 * a search command which will match zero ULEs. When the search event is
 * processed, all pending user appends will have been processed. Since the RXC
 * is not enabled, new appends cannot occur during this time.
 *
 * Caller must hold ep_obj->lock.
 */
static int cxip_flush_appends(struct cxip_rxc *rxc)
{
	struct cxip_req *req;
	union c_cmdu cmd = {};
	int ret;

	assert(rxc->state == RXC_ONLOAD_FLOW_CONTROL ||
	       rxc->state == RXC_ONLOAD_FLOW_CONTROL_REENABLE ||
	       rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED);

	/* Populate request */
	req = cxip_evtq_req_alloc(&rxc->rx_evtq, 1, rxc);
	if (!req) {
		RXC_DBG(rxc, "Failed to allocate request\n");
		ret = -FI_EAGAIN;
		goto err;
	}
	ofi_atomic_inc32(&rxc->orx_reqs);

	rxc->rx_evtq.ack_batch_size = 1;

	req->cb = cxip_flush_appends_cb;
	req->type = CXIP_REQ_SEARCH;

	/* Search command which should match nothing. */
	cmd.command.opcode = C_CMD_TGT_SEARCH;
	cmd.target.ptl_list = C_PTL_LIST_UNEXPECTED;
	cmd.target.ptlte_index = rxc->rx_pte->pte->ptn;
	cmd.target.buffer_id = req->req_id;
	cmd.target.match_bits = -1UL;
	cmd.target.length = 0;

	ret = cxi_cq_emit_target(rxc->rx_cmdq->dev_cmdq, &cmd);
	if (ret) {
		RXC_WARN(rxc, "Failed to write Search command: %d\n", ret);
		ret = -FI_EAGAIN;
		goto err_dec_free_cq_req;
	}

	cxi_cq_ring(rxc->rx_cmdq->dev_cmdq);

	return FI_SUCCESS;

err_dec_free_cq_req:
	ofi_atomic_dec32(&rxc->orx_reqs);
	cxip_evtq_req_free(req);
err:
	return ret;
}

/*
 * cxip_recv_pte_cb() - Process receive PTE state change events.
 */
void cxip_recv_pte_cb(struct cxip_pte *pte, const union c_event *event)
{
	struct cxip_rxc *rxc = (struct cxip_rxc *)pte->ctx;
	int fc_reason = cxip_fc_reason(event);
	int ret __attribute__((unused));

	switch (pte->state) {
	case C_PTLTE_ENABLED:
		assert(rxc->state == RXC_FLOW_CONTROL ||
		       rxc->state == RXC_DISABLED ||
		       rxc->state == RXC_PENDING_PTLTE_HARDWARE);

		/* Queue any flow control resume messages */
		if (rxc->state == RXC_FLOW_CONTROL) {
			cxip_fc_progress_ctrl(rxc);
			RXC_WARN(rxc, "Now in RXC_ENABLED\n");
		}

		rxc->state = RXC_ENABLED;
		break;

	case C_PTLTE_DISABLED:
		if (rxc->state == RXC_DISABLED)
			break;

		if (fc_reason == C_SC_DIS_UNCOR)
			RXC_FATAL(rxc, "Disabled, LE uncorrectable err\n");

		/* An incorrect drop count was used during PTE enable.
		 * Another attempt will be made when a peer sends a side-band
		 * drop message.
		 */
		if (cxi_event_rc(event) == C_RC_NO_MATCH) {
			assert(rxc->state == RXC_FLOW_CONTROL ||
			       rxc->state == RXC_ONLOAD_FLOW_CONTROL ||
			       rxc->state == RXC_ONLOAD_FLOW_CONTROL_REENABLE ||
			       rxc->state ==
			       RXC_PENDING_PTLTE_SOFTWARE_MANAGED);
			RXC_WARN(rxc, FC_DROP_COUNT_MSG);
			break;
		}

		/* Flow control occurred while transitioning from HW to SW
		 * managed PTE. Since onloading of all UX entries will have
		 * been initiated (i.e. no new ones will be added) and the
		 * PTE state change from RXC_PENDING_PTLTE_SOFTWARE_MANAGED
		 * to RXC_ENABLED_SOFTWARE following onload complete is
		 * protected by the ep_obj->lock, it is safe to indicate that
		 * SW managed EP must be re-enabled on onload complete.
		 * The request list will have been replenished.
		 */
		if (rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED) {
			RXC_WARN(rxc,
				 "Flow control during HW to SW transition\n");
			rxc->state = RXC_ONLOAD_FLOW_CONTROL_REENABLE;
			break;
		}

		/* Check for flow control during flow control */
		if (rxc->state != RXC_ENABLED &&
		    rxc->state != RXC_ENABLED_SOFTWARE &&
		    rxc->state != RXC_PENDING_PTLTE_DISABLE) {

			/* There is race between SW disable on priority list
			 * and HW initiated LE flow control which can be
			 * ignored; otherwise it is a fatal error.
			 */
			if (fc_reason == CXIP_FC_SOFTWARE_INITIATED)
				break;
			RXC_FATAL(rxc, FC_SW_LE_MSG_FATAL);
		}

		/* Starting flow control processing. The default is for
		 * flow control should re-enable in the previous
		 * hardware/software managed state.
		 */
		rxc->prev_state = rxc->state;
		rxc->new_state = rxc->state;
		rxc->state = RXC_ONLOAD_FLOW_CONTROL;

		RXC_DBG(rxc, "Flow control detected, H/W: %d reason: %d\n",
			event->tgt_long.initiator.state_change.sc_nic_auto,
			fc_reason);

		switch (fc_reason) {
		case CXIP_FC_SOFTWARE_INITIATED:
			/* Software initiated state change, drop count
			 * needs to start at zero instead of -1. Add 1 to
			 * account for this. Note this is only initiated
			 * from an hardware enabled PTE state.
			 */
			RXC_WARN(rxc, "SW initiated flow control\n");
			if (rxc->ep_obj->asic_ver < CASSINI_2_0)
				rxc->drop_count++;

			/* If running in hybrid mode, resume operation as a
			 * software managed EP to reduce LE resource load.
			 */
			if (cxip_env.rx_match_mode == CXIP_PTLTE_HYBRID_MODE)
				rxc->new_state = RXC_ENABLED_SOFTWARE;

			rxc->num_fc_append_fail++;
			break;

		case C_SC_FC_EQ_FULL:
			/* EQ full does not require LE resources be recovered
			 * to re-enable.
			 */
			RXC_WARN(rxc, "Flow control EQ full\n");
			rxc->state = RXC_ONLOAD_FLOW_CONTROL_REENABLE;
			rxc->num_fc_eq_full++;
			break;

		case C_SC_FC_NO_MATCH:
			/* Overflow list buffers were full/could not be matched
			 * against. Must replenish buffers, but does not in
			 * itself require resources be recovered.
			 */
			RXC_WARN(rxc, FC_OFLOW_NO_MATCH_MSG,
				 cxip_env.oflow_buf_size);

			rxc->state = RXC_ONLOAD_FLOW_CONTROL_REENABLE;
			rxc->num_fc_no_match++;
			break;

		case C_SC_FC_UNEXPECTED_FAIL:
			/* Hybrid mode is not enabled and overflow matches, but
			 * LE resources prevent unexpected message allocation.
			 */
			RXC_WARN(rxc, "Flow control UX LE resources\n");
			rxc->num_fc_unexp++;
			break;

		case C_SC_FC_REQUEST_FULL:
			/* Running as software managed EP and request list
			 * buffers were full/could not be matched against.
			 * Must replenish buffers, but does not require that
			 * LE resources are recovered.
			 */
			RXC_WARN(rxc, FC_REQ_FULL_MSG, cxip_env.req_buf_size);
			rxc->state = RXC_ONLOAD_FLOW_CONTROL_REENABLE;
			rxc->num_fc_req_full++;
			break;

		case C_SC_SM_APPEND_FAIL:
		case C_SC_SM_UNEXPECTED_FAIL:
		default:
			RXC_FATAL(rxc, "Invalid disable PTE c_sc_reason: %d\n",
				  fc_reason);
		}
		rxc->fc_reason = fc_reason;

		do {
			ret = cxip_flush_appends(rxc);
		} while (ret == -FI_EAGAIN);

		if (ret != FI_SUCCESS)
			RXC_FATAL(rxc, "cxip_flush_appends failed: %d\n", ret);

		break;

	case C_PTLTE_SOFTWARE_MANAGED:
		/* There is an inherent race between hardware and software
		 * in setting the PtlTE state. If software requested to
		 * disable the PtlTE after hardware started a HW to SW
		 * transition; just wait for the disable event.
		 */
		if (rxc->state == RXC_PENDING_PTLTE_DISABLE)
			break;

		RXC_DBG(rxc, "SW Managed: nic auto: %d, reason: %d\n",
			event->tgt_long.initiator.state_change.sc_nic_auto,
			event->tgt_long.initiator.state_change.sc_nic_auto ?
			event->tgt_long.initiator.state_change.sc_reason : -1);

		/* We should not get a bad drop count status since the
		 * transition is synchronous but we will need this in
		 * the future.
		 */
		if (cxi_event_rc(event) == C_RC_NO_MATCH) {
			RXC_WARN(rxc, "Bad drop count, ignored\n");
			break;
		}

		/* Sanity check */
		if (rxc->state == RXC_FLOW_CONTROL)
			RXC_FATAL(rxc, "FC to SW EP should be synchronous\n");

		assert(rxc->state == RXC_DISABLED ||
		       rxc->state == RXC_ENABLED ||
		       rxc->state == RXC_PENDING_PTLTE_SOFTWARE_MANAGED);

		/* Hardware should only generate PTE software managed events
		 * in two cases:
		 * 1. Initial start in software mode: disabled->software.
		 * 2. NIC initiated software transition: enabled->software.
		 */
		switch (fc_reason) {
		case CXIP_FC_SOFTWARE_INITIATED:
			/* If messaging was initially offloaded then this
			 * state transition can only happen if the RXC has
			 * been disabled; it is safe to ignore this change.
			 */
			assert(rxc->state == RXC_DISABLED);
			if (!cxip_env.msg_offload) {
				RXC_WARN(rxc, "Software managed EP enabled\n");
				rxc->state = RXC_ENABLED_SOFTWARE;
			}
			break;

		case C_SC_SM_APPEND_FAIL:
		case C_SC_SM_UNEXPECTED_FAIL:
			/* The NIC initiated the transition; priority list
			 * appends that are in flight will fail and be added
			 * to the receive replay list. Update state so that
			 * no additional appends will be attempted until
			 * onload completes and the failed appends are
			 * replayed.
			 */
			RXC_WARN(rxc,
				 "NIC transition to SW EP, c_sc_reason: %d\n",
				 fc_reason);
			rxc->fc_reason = fc_reason;
			rxc->prev_state = rxc->state;
			rxc->new_state = RXC_ENABLED_SOFTWARE;

			if (rxc->fc_reason == C_SC_SM_UNEXPECTED_FAIL)
				rxc->num_sc_nic_hw2sw_unexp++;
			else if (rxc->fc_reason == C_SC_SM_APPEND_FAIL)
				rxc->num_sc_nic_hw2sw_append_fail++;

			rxc->msg_offload = 0;
			rxc->state = RXC_PENDING_PTLTE_SOFTWARE_MANAGED;
			do {
				/* Flush and kick-off onloading of UX list */
				ret = cxip_flush_appends(rxc);
			} while (ret == -FI_EAGAIN);
			if (ret != FI_SUCCESS)
				RXC_WARN(rxc, "Flush/UX onload err: %d\n", ret);
			break;
		default:
			RXC_FATAL(rxc, "Invalid PTE c_sc_reason: %d\n",
				  fc_reason);
		}

		break;
	default:
		RXC_FATAL(rxc, "Unexpected state received: %u\n", pte->state);
	}
}

/*
 * tag_match() - Compare UX Send tag and Receive tags in SW.
 */
static bool tag_match(uint64_t init_mb, uint64_t mb, uint64_t ib)
{
	return !((init_mb ^ mb) & ~ib);
}

/*
 * tag_match() - Compare UX Send initiator and Receive initiator in SW.
 */
static bool init_match(struct cxip_rxc *rxc, uint32_t init, uint32_t match_id)
{
	if (match_id == CXI_MATCH_ID_ANY)
		return true;

	if (rxc->ep_obj->av->symmetric) {
		init = CXI_MATCH_ID_EP(rxc->pid_bits, init);
		match_id = CXI_MATCH_ID_EP(rxc->pid_bits, match_id);
	}

	return init == match_id;
}

/*
 * cxip_claim_onload_cb() - Process SEARCH and DELETE of claimed UX message.
 */
static int cxip_claim_onload_cb(struct cxip_req *req,
				const union c_event *evt)
{
	struct cxip_rxc *rxc = req->req_ctx;
	struct cxip_deferred_event *def_ev;
	struct cxip_ux_send *ux_send;
	bool matched = false;

	if (evt->hdr.event_type != C_EVENT_PUT_OVERFLOW)
		RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(evt),
			  cxi_rc_to_str(cxi_event_rc(evt)));

	/* Failed to onload UX message, return ENOMSG */
	if (cxi_event_rc(evt) != C_RC_OK) {
		RXC_WARN(rxc, "FI_CLAIM HW onload failed: %d\n",
			 cxi_event_rc(evt));
		recv_req_peek_complete(req, NULL);

		return FI_SUCCESS;
	}

	ofi_atomic_dec32(&rxc->orx_hw_ule_cnt);

	/* FI_CLAIM UX message onloaded from hardware */
	ux_send = calloc(1, sizeof(*ux_send));
	if (!ux_send) {
		RXC_WARN(rxc, "Failed allocate UX memory\n");
		return -FI_EAGAIN;
	}
	ux_send->claimed = true;

	/* Zero-byte unexpected onloads require special handling
	 * since no deferred structure would be allocated.
	 */
	if (evt->tgt_long.rlength) {
		def_ev = match_put_event(rxc, req, evt, &matched);
		if (!matched) {
			/* The EVENT_PUT to the overflow list has not been
			 * processed. The FI_CLAIM operation will be completed
			 * when the matching put is received.
			 */
			if (!def_ev) {
				free(ux_send);
				return -FI_EAGAIN;
			}
			def_ev->ux_send = ux_send;
		} else {
			ux_send->req = def_ev->req;
			ux_send->put_ev = def_ev->ev;
			free_put_event(rxc, def_ev);
		}

		/* Fixup event remote offset for an RGet. */
		if (evt->tgt_long.rlength)
			ux_send->put_ev.tgt_long.remote_offset =
				req->recv.ule_offset + evt->tgt_long.mlength;

	} else {
		matched = true;
		ux_send->put_ev = *evt;
	}

	/* Add to the sw UX list as a claimed entry, it will be ignored in
	 * recieve matching of UX list entries. Its order no longer matters.
	 */
	dlist_insert_tail(&ux_send->rxc_entry, &rxc->sw_ux_list);
	rxc->sw_ux_list_len++;

	RXC_DBG(rxc, "FI_CLAIM Onload req: %p ux_send %p\n", req, ux_send);
	recv_req_tgt_event(req, &ux_send->put_ev);

	/* Put was already received, return FI_CLAIM completion */
	if (matched) {
		recv_req_peek_complete(req, ux_send);
		RXC_DBG(rxc, "FI_CLAIM onload complete, req %p, ux_send %p\n",
			req, ux_send);
	}

	ofi_atomic_dec32(&rxc->orx_hw_ule_cnt);

	return FI_SUCCESS;
}

/*
 * cxip_claim_ux_onload() - Initiate SEARCH and DELETE of FI_CLAIM ux entry.
 */
static int cxip_claim_ux_onload(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->req_ctx;
	int ret = FI_SUCCESS;
	union c_cmdu cmd = {};
	union cxip_match_bits mb = {};
	union cxip_match_bits ib = {};

	if (rxc->state != RXC_ENABLED) {
		RXC_DBG(rxc, "FC inprogress, fail claim req %p\n", req);
		goto err;
	}

	/* Initiate a search to get the remote offset for the
	 * unexpected list entry we matched.
	 */
	req->cb = cxip_claim_onload_cb;
	mb.tag = req->recv.tag;
	mb.tagged = 1;
	ib.tx_id = ~0;
	ib.cq_data = ~0;
	ib.match_comp = ~0;
	ib.rdzv_done = ~0;
	ib.le_type = ~0;
	ib.tag = req->recv.ignore;

	cmd.command.opcode = C_CMD_TGT_SEARCH_AND_DELETE;

	cmd.target.ptl_list = C_PTL_LIST_UNEXPECTED;
	cmd.target.ptlte_index = rxc->rx_pte->pte->ptn;
	cmd.target.buffer_id = req->req_id;
	cmd.target.length = -1U;
	cmd.target.ignore_bits = ib.raw;
	cmd.target.match_bits =  mb.raw;
	cmd.target.match_id = req->recv.match_id;
	/* Delete first match */
	cmd.target.use_once = 1;

	ret = cxi_cq_emit_target(rxc->rx_cmdq->dev_cmdq, &cmd);
	if (ret) {
		/* This condition should clear */
		RXC_WARN(rxc,
			 "Cannot emit of UX delete cmd, return -FI_EAGAIN\n");
		return -FI_EAGAIN;
	}

	cxi_cq_ring(rxc->rx_cmdq->dev_cmdq);

	/* Hardware handles the race between subsequent priority list
	 * appends to the search and delete command. Re-enable.
	 */
	rxc->hw_claim_in_progress = false;
	RXC_DBG(rxc, "FI_CLAIM Search and Delete of UX entry initiated\n");

	return FI_SUCCESS;

err:
	/* Unable to initiate FI_CLAIM, report as ENOMSG */
	rxc->hw_claim_in_progress = false;
	recv_req_peek_complete(req, NULL);

	return FI_SUCCESS;
}

/*
 * cxip_hw_claim_offset_cb() - Process SEARCH command events to get remote
 * offset of entry to be deleted.
 */
static int cxip_hw_claim_offset_cb(struct cxip_req *req,
				   const union c_event *evt)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	union cxip_match_bits ux_mb;
	uint32_t ux_init;
	int ret;

	switch (evt->hdr.event_type) {
	case C_EVENT_SEARCH:
		if (cxi_event_rc(evt) == C_RC_OK) {
			RXC_DBG(rxc, "Claim UX offset search entry, req: %p\n",
				req);

			if (req->recv.offset_found)
				break;

			req->recv.cur_ule_offsets++;

			/* Not found in range of the offsets we have */
			if (req->recv.cur_ule_offsets >
			    req->recv.num_ule_offsets) {
				RXC_DBG(rxc, "Claim UX offsets exceeded\n");
				break;
			}

			/* Check for a match against the FI_PEEK */
			ux_mb.raw = evt->tgt_long.match_bits;
			ux_init = evt->tgt_long.initiator.initiator.process;

			if (req->recv.tagged != ux_mb.tagged)
				break;
			if (ux_mb.tagged
			    && !tag_match(ux_mb.tag, req->recv.tag,
					  req->recv.ignore))
				break;
			if (!init_match(rxc, ux_init, req->recv.match_id))
				break;

			/* Matched, update to ignore any future events */
			req->recv.offset_found = true;
			req->recv.ule_offset =
				req->recv.ule_offsets[req->recv.cur_ule_offsets - 1];

			RXC_DBG(rxc, "Found offset for claim %p, %d : 0x%lX\n",
				req, req->recv.cur_ule_offsets - 1,
				req->recv.ule_offset);
			break;
		}

		assert(cxi_event_rc(evt) == C_RC_NO_MATCH);

		RXC_DBG(rxc, "FI_CLAIM remote offset search done, status %d\n",
			cxi_event_rc(evt));

		if (!req->recv.offset_found) {
			RXC_DBG(rxc, "Req %p, FI_CLAIM UX not found\n", req);
			goto err_not_found;
		}

		ret = cxip_claim_ux_onload(req);
		if (ret) {
			/* Unable to initiate SEARCH and DELETE, this
			 * should clear. All other errors return ENOMSG.
			 */
			if (ret == -FI_EAGAIN)
				return ret;

			RXC_WARN(rxc, "claim_ux_onload failed %d\n", ret);
			goto err_not_found;
		}

		RXC_DBG(rxc, "FI_CLAIM req %p remote offset 0x%lX\n",
			req, req->recv.ule_offset);
		break;
	default:
		RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(evt),
			  cxi_rc_to_str(cxi_event_rc(evt)));
	}

	return FI_SUCCESS;

err_not_found:
	/* Terminate FI_PEEK with FI_CLAIM with ENOMSG */
	rxc->hw_claim_in_progress = false;
	free(req->recv.ule_offsets);
	req->recv.ule_offsets = NULL;
	recv_req_peek_complete(req, NULL);

	return FI_SUCCESS;
}

/*
 * cxip_initiate_hw_claim() - Onload the specified peek, claiming it.
 */
static int cxip_initiate_hw_claim(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->req_ctx;
	union c_cmdu cmd = {};
	int ret = FI_SUCCESS;

	if (rxc->state != RXC_ENABLED) {
		RXC_DBG(rxc, "FC inprogress, unable to claim req %p\n", req);
		goto err;
	}

	/* UX entry exists in hardware, the initial search acts as a flush of
	 * the event queue for priority list appends. Get remote offset for
	 * the associated unexpected list entry.
	 */
	req->recv.cur_ule_offsets = 0;
	ret = cxip_get_ule_offsets(rxc, &req->recv.ule_offsets,
				   &req->recv.num_ule_offsets, true);
	if (ret) {
		RXC_WARN(rxc, "Unable to get FI_CLAIM  UX offsets\n");
		goto err;
	}

	RXC_DBG(rxc, "ule_offsets %p, num offsets %d\n",
		req->recv.ule_offsets, req->recv.num_ule_offsets);

	/* Initiate a search to get the remote offset for the
	 * unexpected list entry we matched. This requires going
	 * through the list.
	 */
	req->cb = cxip_hw_claim_offset_cb;

	cmd.command.opcode = C_CMD_TGT_SEARCH;
	cmd.target.ptl_list = C_PTL_LIST_UNEXPECTED;
	cmd.target.ptlte_index = rxc->rx_pte->pte->ptn;
	cmd.target.buffer_id = req->req_id;
	cmd.target.length = -1U;
	cmd.target.ignore_bits = -1UL;
	cmd.target.match_id = CXI_MATCH_ID_ANY;

	ret = cxi_cq_emit_target(rxc->rx_cmdq->dev_cmdq, &cmd);
	if (ret) {
		RXC_WARN(rxc, "Failed to write Search command: %d\n", ret);
		goto err_free_offsets;
	}

	cxi_cq_ring(rxc->rx_cmdq->dev_cmdq);

	RXC_DBG(rxc, "Search for remote offsets initiated, req %p\n", req);

	return FI_SUCCESS;

err_free_offsets:
	free(req->recv.ule_offsets);
	req->recv.ule_offsets = NULL;
err:
	/* Unable to initiate FI_CLAIM, report as ENOMSG */
	rxc->hw_claim_in_progress = false;
	recv_req_peek_complete(req, NULL);

	return FI_SUCCESS;
}

/*
 * cxip_ux_peek_cb() - Process UX list SEARCH command events.
 */
static int cxip_ux_peek_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->req_ctx;

	assert(req->recv.flags & FI_PEEK);

	switch (event->hdr.event_type) {
	case C_EVENT_SEARCH:
		/* Will receive event for only first match or failure */
		if (cxi_event_rc(event) == C_RC_OK) {
			RXC_DBG(rxc, "Peek UX search req: %p matched\n", req);
			if (req->recv.flags & FI_CLAIM) {
				RXC_DBG(rxc, "req: %p UX must be claimed\n",
					req);
				return cxip_initiate_hw_claim(req);
			}

			/* FI_PEEK only was found */
			recv_req_tgt_event(req, event);
		} else {
			RXC_DBG(rxc, "Peek UX search req: %p no match\n", req);
		}

		recv_req_peek_complete(req, NULL);
		break;

	default:
		RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}

	return FI_SUCCESS;
}

/*
 * cxip_ux_peek() - Issue a SEARCH command to peek for a matching send
 * on the RXC offloaded unexpected message list.
 *
 * Caller must hold ep_obj->lock.
 */
static int cxip_ux_peek(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->req_ctx;
	union c_cmdu cmd = {};
	union cxip_match_bits mb = {};
	union cxip_match_bits ib = {};
	int ret;

	assert(req->recv.flags & FI_PEEK);

	req->cb = cxip_ux_peek_cb;

	mb.tag = req->recv.tag;
	mb.tagged = 1;
	ib.tx_id = ~0;
	ib.cq_data = ~0;
	ib.match_comp = ~0;
	ib.rdzv_done = ~0;
	ib.le_type = ~0;
	ib.tag = req->recv.ignore;

	cmd.command.opcode = C_CMD_TGT_SEARCH;
	cmd.target.ptl_list = C_PTL_LIST_UNEXPECTED;
	cmd.target.ptlte_index = rxc->rx_pte->pte->ptn;
	cmd.target.buffer_id = req->req_id;
	cmd.target.length = -1U;
	cmd.target.ignore_bits = ib.raw;
	cmd.target.match_bits =  mb.raw;
	cmd.target.match_id = req->recv.match_id;
	/* First match only */
	cmd.target.use_once = 1;

	if (cxip_evtq_saturated(&rxc->rx_evtq)) {
		RXC_DBG(rxc, "Target HW EQ saturated\n");
		return -FI_EAGAIN;
	}

	RXC_DBG(rxc, "Peek UX search req: %p mb.raw: 0x%" PRIx64 " match_id: 0x%x ignore: 0x%" PRIx64 "\n",
		req, mb.raw, req->recv.match_id, req->recv.ignore);

	ret = cxi_cq_emit_target(rxc->rx_cmdq->dev_cmdq, &cmd);
	if (ret) {
		RXC_WARN(rxc, "Failed to write Search command: %d\n", ret);
		return -FI_EAGAIN;
	}

	cxi_cq_ring(rxc->rx_cmdq->dev_cmdq);

	/* If FI_CLAIM, we disable priority list appends so the
	 * search acts as a flush of outstanding appends.
	 */
	if (req->flags & FI_CLAIM)
		rxc->hw_claim_in_progress = true;

	return FI_SUCCESS;
}

/* cxip_set_ux_dump_entry() - initialize a CQ entry structure
 * and/or source address with UX message info.
 */
static void cxip_set_ux_dump_entry(struct cxip_req *req,
				   const union c_event *evt)
{
	struct cxip_ux_dump_state *ux_dump = req->recv.ux_dump;
	union cxip_match_bits mb;
	struct fi_cq_tagged_entry *cq_entry = NULL;
	fi_addr_t *src_addr = NULL;

	ux_dump->ux_count++;

	/* If exceeding caller provided space updating the total
	 * available UX message count is all that is required.
	 */
	if (ux_dump->ret_count >= ux_dump->max_count)
		return;

	if (ux_dump->entry)
		cq_entry = &ux_dump->entry[ux_dump->ret_count];
	if (ux_dump->src_addr)
		src_addr = &ux_dump->src_addr[ux_dump->ret_count];

	if (cq_entry || src_addr) {
		ux_dump->ret_count++;

		req->recv.tgt_event = false;
		req->flags = 0;
		recv_req_tgt_event(req, evt);

		if (cq_entry) {
			/* Need to add FI_TAGGED or FI_MSG directly */
			mb.raw = evt->tgt_long.match_bits;
			if (mb.tagged)
				req->flags |= FI_TAGGED;
			else
				req->flags |= FI_MSG;
			cq_entry->op_context = NULL;
			cq_entry->flags = req->flags;
			cq_entry->len = req->recv.rlen;
			cq_entry->buf = NULL;
			cq_entry->data = req->data;
			cq_entry->tag = req->tag;
		}

		if (src_addr && req->recv.rxc->attr.caps & FI_SOURCE)
			*src_addr = recv_req_src_addr(req);
	}
}

/*
 * cxip_unexp_msg_dump_cb() - Process search command dumping H/W UX entries.
 */
static int cxip_unexp_msg_dump_cb(struct cxip_req *req,
				  const union c_event *evt)
{
	struct cxip_rxc *rxc = req->recv.rxc;

	if (evt->hdr.event_type != C_EVENT_SEARCH)
		RXC_FATAL(rxc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(evt),
			  cxi_rc_to_str(cxi_event_rc(evt)));

	if (cxi_event_rc(evt) == C_RC_NO_MATCH) {
		req->recv.ux_dump->done = true;
		return FI_SUCCESS;
	}
	assert(cxi_event_rc(evt) == C_RC_OK);

	cxip_set_ux_dump_entry(req, evt);

	return FI_SUCCESS;
}

/*
 * cxip_build_debug_ux_entry_info() - Initialize UX info array from ULE.
 *
 * It is expected that a debugger is utilizing this interface and is
 * expecting synchronous behavior.
 *
 * Caller should hold ep_obj->lock.
 */
int cxip_build_ux_entry_info(struct cxip_ep *ep,
			     struct fi_cq_tagged_entry *entry, size_t count,
			     fi_addr_t *src_addr, size_t *ux_count)
{
	struct cxip_rxc *rxc = &ep->ep_obj->rxc;
	struct cxip_ux_dump_state *ux_dump;
	struct cxip_ux_send *ux_send;
	struct dlist_entry *tmp;
	struct cxip_req *req = NULL;
	union c_cmdu cmd = {};
	int ret_count;
	int ret;

	ret = cxip_recv_req_alloc(rxc, NULL, 0, &req);
	if (ret)
		return ret;

	ux_dump = calloc(1, sizeof(struct cxip_ux_dump_state));
	if (!ux_dump) {
		RXC_WARN(rxc, "ENOMEM on allocate of UX state buffer\n");
		ret_count = -FI_ENOMEM;
		goto done;
	}

	ux_dump->max_count = count;
	ux_dump->entry = entry;
	ux_dump->src_addr = src_addr;
	req->recv.ux_dump = ux_dump;

	/* Get entries from software UX list first */
	dlist_foreach_container_safe(&rxc->sw_ux_list, struct cxip_ux_send,
				     ux_send, rxc_entry, tmp)
		cxip_set_ux_dump_entry(req, &ux_send->put_ev);

	if (!rxc->msg_offload)
		goto done;

	/* Read H/W UX list processing the request events synchronously
	 * until we set "Done" in the request callback.
	 */
	req->cb = cxip_unexp_msg_dump_cb;
	cmd.command.opcode = C_CMD_TGT_SEARCH;
	cmd.target.ptl_list = C_PTL_LIST_UNEXPECTED;
	cmd.target.ptlte_index = rxc->rx_pte->pte->ptn;
	cmd.target.buffer_id = req->req_id;
	cmd.target.length = -1U;
	cmd.target.ignore_bits = -1UL;
	cmd.target.match_id = CXI_MATCH_ID_ANY;

	ret = cxi_cq_emit_target(rxc->rx_cmdq->dev_cmdq, &cmd);
	if (ret) {
		RXC_WARN(rxc, "Failed to write ULE Search command: %d\n", ret);
		ret_count = ret;
		goto done;
	}
	cxi_cq_ring(rxc->rx_cmdq->dev_cmdq);

	RXC_DBG(rxc, "Search for ULE dump initiated, req %p\n", req);
	do {
		cxip_evtq_progress(&rxc->rx_evtq);
		sched_yield();
	} while (!ux_dump->done);

	RXC_DBG(rxc, "Search ULE dump done, req %p, count %ld\n",
		req, ux_dump->ret_count);
done:
	ret_count = ux_dump->ret_count;
	*ux_count = ux_dump->ux_count;

	free(ux_dump);
	cxip_recv_req_free(req);

	return ret_count;
}

/*
 * cxip_recv_sw_matched() - Progress the SW Receive match.
 *
 * Progress the operation which matched in SW.
 */
static int cxip_recv_sw_matched(struct cxip_req *req,
				struct cxip_ux_send *ux_send)
{
	int ret;
	uint64_t mrecv_start;
	uint32_t mrecv_len;
	bool req_done = true;
	uint32_t ev_init;
	uint32_t ev_rdzv_id;
	struct cxip_req *rdzv_req;
	struct cxip_rxc *rxc = req->recv.rxc;

	assert(req->type == CXIP_REQ_RECV);

	mrecv_start = req->recv.start_offset;
	mrecv_len = mrecv_req_put_bytes(req, ux_send->put_ev.tgt_long.rlength);

	if (req->recv.multi_recv &&
	    (req->recv.ulen - req->recv.start_offset) >=
	     req->recv.rxc->min_multi_recv)
		req_done = false;

	if (ux_send->put_ev.tgt_long.rendezvous) {

		/* Make sure we can issue the RGet; if not we stall
		 * and TX event queue progress will free up credits.
		 */
		if (ofi_atomic_inc32(&rxc->orx_tx_reqs) > rxc->max_tx) {
			ofi_atomic_dec32(&rxc->orx_tx_reqs);
			return -FI_EAGAIN;
		}

		ret = cxip_ux_send(req, ux_send->req, &ux_send->put_ev,
				   mrecv_start, mrecv_len, req_done);
		if (ret != FI_SUCCESS) {
			req->recv.start_offset -= mrecv_len;
			ofi_atomic_dec32(&rxc->orx_tx_reqs);

			return ret;
		}

		/* If multi-recv, a child request was created from
		 * cxip_ux_send(). Need to lookup this request.
		 *
		 * NOTE: Since the same event will be used, the evenet checks
		 * must be NOT be performed. The event checks are only needed
		 * when hardware is generating put and put overflow events for
		 * an mrecv buffer. If we have reached here, we know a put
		 * overflow event will never occur since the mrecv buffer has
		 * not been offloaded to hardware.
		 */
		if (req->recv.multi_recv) {
			ret = rdzv_mrecv_req_lookup(req, &ux_send->put_ev,
						    &ev_init, &ev_rdzv_id,
						    false, &rdzv_req);

			/* If the previous cxip_ux_send() returns FI_SUCCESS,
			 * a matching rdzv mrecv req will always exist.
			 */
			assert(ret == FI_SUCCESS);
		} else {
			rdzv_req = req;
		}

		/* Rendezvous event will not happen. So ack rendezvous event
		 * now.
		 */
		rdzv_recv_req_event(rdzv_req, ux_send->put_ev.hdr.event_type);

		cxip_recv_req_set_rget_info(rdzv_req);


		/* A TX credit has been reserved and user receive request may
		 * have been removed from the ordered SW queue. If the command
		 * queue is backed up the condition will clear and the rget
		 * must get sent out, so wait for it.
		 */
		do {
			ret = issue_rdzv_get(rdzv_req);
		} while (ret == -FI_EAGAIN);
		assert(ret == FI_SUCCESS);
	} else {
		if (ux_send->put_ev.tgt_long.rlength)
			ret = cxip_ux_send(req, ux_send->req, &ux_send->put_ev,
					   mrecv_start, mrecv_len, req_done);
		else
			ret = cxip_ux_send_zb(req, &ux_send->put_ev,
					      mrecv_start, req_done);

		if (ret != FI_SUCCESS) {
			/* undo mrecv_req_put_bytes() */
			req->recv.start_offset -= mrecv_len;
			return ret;
		}
	}

	/* If this is a multi-receive request and there is still space, return
	 * a special code to indicate SW should keep matching messages to it.
	 */
	if (ret == FI_SUCCESS && !req_done)
		return -FI_EINPROGRESS;

	return ret;
}

static bool cxip_match_recv_sw(struct cxip_rxc *rxc, struct cxip_req *req,
			       struct cxip_ux_send *ux, bool claimed)
{
	union cxip_match_bits ux_mb;
	uint32_t ux_init;

	if (claimed != ux->claimed)
		return false;

	ux_mb.raw = ux->put_ev.tgt_long.match_bits;
	ux_init = ux->put_ev.tgt_long.initiator.initiator.process;

	if (req->recv.tagged != ux_mb.tagged)
		return false;

	if (ux_mb.tagged &&
	    !tag_match(ux_mb.tag, req->recv.tag, req->recv.ignore))
		return false;

	if (!init_match(rxc, ux_init, req->recv.match_id))
		return false;

	return true;
}

static int cxip_recv_sw_matcher(struct cxip_rxc *rxc, struct cxip_req *req,
				struct cxip_ux_send *ux, bool claimed)
{
	int ret;

	if (!cxip_match_recv_sw(rxc, req, ux, claimed))
		return -FI_ENOMSG;

	ret = cxip_recv_sw_matched(req, ux);
	if (ret == -FI_EAGAIN)
		return -FI_EAGAIN;

	/* FI_EINPROGRESS is return for a multi-recv match. */
	assert(ret == FI_SUCCESS || ret == -FI_EINPROGRESS);

	/* TODO: Manage freeing of UX entries better. */
	dlist_remove(&ux->rxc_entry);
	if (ux->req && ux->req->type == CXIP_REQ_RBUF) {
		cxip_req_buf_ux_free(ux);
		rxc->sw_ux_list_len--;
	} else {
		free(ux);
		rxc->sw_ux_list_len--;
	}

	RXC_DBG(rxc,
		"Software match, req: %p ux_send: %p (sw_ux_list_len: %u)\n",
		req, ux, req->recv.rxc->sw_ux_list_len);

	return ret;
}

/*
 * cxip_recv_ux_sw_matcher() - Attempt to match an unexpected message to a user
 * posted receive.
 *
 * User must hold the ep_obj->lock.
 */
int cxip_recv_ux_sw_matcher(struct cxip_ux_send *ux)
{
	struct cxip_ptelist_buf *rbuf = ux->req->req_ctx;
	struct cxip_rxc *rxc = rbuf->rxc;
	struct cxip_req *req;
	struct dlist_entry *tmp;
	int ret;

	if (dlist_empty(&rxc->sw_recv_queue))
		return -FI_ENOMSG;

	dlist_foreach_container_safe(&rxc->sw_recv_queue, struct cxip_req, req,
				     recv.rxc_entry, tmp) {
		/* Only matches against unclaimed UX messages */
		ret = cxip_recv_sw_matcher(rxc, req, ux, false);

		/* Unexpected message found match but unable to progress */
		if (ret == -FI_EAGAIN)
			return ret;

		/* Unexpected message found a match. */
		if (ret == FI_SUCCESS || ret == -FI_EINPROGRESS)
			return FI_SUCCESS;
	}

	return -FI_ENOMSG;
}

/*
 * cxip_recv_req_sw_matcher() - Attempt to match the receive request in SW.
 *
 * Loop through all onloaded UX Sends looking for a match for the Receive
 * request. If a match is found, progress the operation.
 *
 * Caller must hold ep_obj->lock.
 */
int cxip_recv_req_sw_matcher(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	struct cxip_ux_send *ux_send;
	struct dlist_entry *tmp;
	int ret;

	if (dlist_empty(&rxc->sw_ux_list))
		return -FI_ENOMSG;

	dlist_foreach_container_safe(&rxc->sw_ux_list, struct cxip_ux_send,
				     ux_send, rxc_entry, tmp) {
		/* Only match against unclaimed UX messages */
		ret = cxip_recv_sw_matcher(rxc, req, ux_send, false);
		switch (ret) {
		/* On successful multi-recv or no match, keep matching. */
		case -FI_EINPROGRESS:
		case -FI_ENOMSG:
			break;

		/* Stop matching. */
		default:
			return ret;
		}
	}

	return -FI_ENOMSG;
}

/*
 * cxip_recv_req_dropped() - Mark the Received request dropped.
 *
 * If HW does not have sufficient LEs to perform an append, the command is
 * dropped. Queue the request for replay. When all outstanding append commands
 * complete, replay all Receives.
 *
 * Caller must hold ep_obj->lock
 */
static int cxip_recv_req_dropped(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	int ret __attribute__((unused));

	assert(dlist_empty(&req->recv.rxc_entry));
	dlist_insert_tail(&req->recv.rxc_entry, &rxc->replay_queue);

	RXC_DBG(rxc, "Receive dropped: %p\n", req);

	return FI_SUCCESS;
}

/*
 * cxip_recv_req_peek() - Peek for matching unexpected message on RXC.
 *
 * Examine onloaded UX sends, if not found there and HW offload is enabled,
 * initiate check of HW UX list. In either case the operation will not
 * consume the UX send, but only report the results of the peek to the CQ.
 *
 * Caller must hold the ep_obj->lock.
 */
static int cxip_recv_req_peek(struct cxip_req *req, bool check_rxc_state)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	struct cxip_ux_send *ux_send;
	struct dlist_entry *tmp;
	int ret;

	if (check_rxc_state && rxc->state != RXC_ENABLED &&
	    rxc->state != RXC_ENABLED_SOFTWARE)
		return -FI_EAGAIN;

	/* Attempt to match the onloaded UX list first */
	dlist_foreach_container_safe(&rxc->sw_ux_list, struct cxip_ux_send,
				     ux_send, rxc_entry, tmp) {
		if (cxip_match_recv_sw(rxc, req, ux_send, false)) {
			if (req->recv.flags & FI_CLAIM)
				ux_send->claimed = true;

			recv_req_tgt_event(req, &ux_send->put_ev);
			recv_req_peek_complete(req, ux_send);
			return FI_SUCCESS;
		}
	}

	if (rxc->msg_offload) {
		/* Must serialize H/W FI_CLAIM due to getting remote offsets */
		if (rxc->hw_claim_in_progress)
			return -FI_EAGAIN;

		ret = cxip_ux_peek(req);
	} else {
		req->recv.rc = C_RC_NO_MATCH;
		recv_req_peek_complete(req, NULL);
		ret = FI_SUCCESS;
	}

	return ret;
}

/*
 * cxip_recv_req_queue() - Queue Receive request on RXC.
 *
 * Before appending a new Receive request to a HW list, attempt to match the
 * Receive to any onloaded UX Sends.
 *
 * Caller must hold the RXC lock and ensure correct RXC state if required.
 */
static int cxip_recv_req_queue(struct cxip_req *req, bool restart_seq)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	int ret;

	/* Try to match against onloaded Sends first. */
	ret = cxip_recv_req_sw_matcher(req);
	if (ret == FI_SUCCESS)
		return -FI_EALREADY;
	else if (ret == -FI_EAGAIN)
		return -FI_EAGAIN;
	else if (ret != -FI_ENOMSG)
		RXC_FATAL(rxc, "SW matching failed: %d\n", ret);

	if (rxc->msg_offload) {
		/* Can not append to priority list if claimng UX */
		if (rxc->hw_claim_in_progress)
			goto err_dequeue_req;

		ret = _cxip_recv_req(req, restart_seq);
		if (ret)
			goto err_dequeue_req;
	} else {

		req->recv.software_list = true;
		dlist_insert_tail(&req->recv.rxc_entry, &rxc->sw_recv_queue);
	}

	return FI_SUCCESS;

err_dequeue_req:
	dlist_remove_init(&req->recv.rxc_entry);

	return -FI_EAGAIN;
}

static int cxip_rxc_check_recv_count_hybrid_preempt(struct cxip_rxc *rxc)
{
	int ret;
	int count;

	if (cxip_env.rx_match_mode == CXIP_PTLTE_HYBRID_MODE &&
	    cxip_env.hybrid_posted_recv_preemptive == 1) {
		count = ofi_atomic_get32(&rxc->orx_reqs);

		if (count > rxc->attr.size) {
			assert(rxc->state == RXC_ENABLED);

			/* On success, need to return -FI_EAGAIN which will
			 * propagate back to the user. In addition, RXC state
			 * will have transitioned to RXC_PENDING_PTLTE_DISABLE.
			 */
			ret = cxip_recv_pending_ptlte_disable(rxc, false);
			if (ret == FI_SUCCESS) {
				RXC_WARN(rxc,
					 "Transitioning to SW EP due to too many posted recvs: posted_count=%u request_size=%lu\n",
					 ret, rxc->attr.size);
				return -FI_EAGAIN;
			}

			RXC_WARN(rxc, "Failed to transition to SW EP: %d\n",
				 ret);
			return ret;
		}
	}

	return FI_SUCCESS;
}

/*
 * _cxip_recv_req() - Submit Receive request to hardware.
 */
static ssize_t _cxip_recv_req(struct cxip_req *req, bool restart_seq)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	uint32_t le_flags = 0;
	union cxip_match_bits mb = {};
	union cxip_match_bits ib = {
		.tx_id = ~0,
		.match_comp = 1,
		.cq_data = 1,
		.rdzv_done = 1,
		.le_type = ~0,
	};
	int ret;
	struct cxip_md *recv_md = req->recv.recv_md;
	uint64_t recv_iova = 0;

	ret = cxip_rxc_check_recv_count_hybrid_preempt(rxc);
	if (ret != FI_SUCCESS)
		return ret;

	if (req->recv.tagged) {
		mb.tagged = 1;
		mb.tag = req->recv.tag;
		ib.tag = req->recv.ignore;
	}

	/* For poorly written applications a periodic check LE pool
	 * resources can be requested to force transitions to software mode.
	 * For this to occur, the code must be executing in hybrid mode,
	 * still matching in hardware, and FI_CXI_HYBRID_RECV_PREEMPTIVE
	 * explicitly set by the application.
	 */
	if (cxip_env.rx_match_mode != CXIP_PTLTE_HYBRID_MODE ||
	    ++rxc->recv_appends & CXIP_HYBRID_RECV_CHECK_INTERVAL)
		le_flags = C_LE_EVENT_LINK_DISABLE;

	/* Always set manage_local in Receive LEs. This makes Cassini ignore
	 * initiator remote_offset in all Puts. With this, remote_offset in Put
	 * events can be used by the initiator for protocol data. The behavior
	 * of use_once is not impacted by manage_local.
	 */
	le_flags |= C_LE_EVENT_UNLINK_DISABLE | C_LE_MANAGE_LOCAL |
		    C_LE_UNRESTRICTED_BODY_RO | C_LE_UNRESTRICTED_END_RO |
		    C_LE_OP_PUT;

	if (!req->recv.multi_recv)
		le_flags |= C_LE_USE_ONCE;
	if (restart_seq)
		le_flags |= C_LE_RESTART_SEQ;

	if (recv_md)
		recv_iova = CXI_VA_TO_IOVA(recv_md->md,
					   (uint64_t)req->recv.recv_buf +
					   req->recv.start_offset);

	req->recv.hw_offloaded = true;

	/* Issue Append command */
	ret = cxip_pte_append(rxc->rx_pte, recv_iova,
			      req->recv.ulen - req->recv.start_offset,
			      recv_md ? recv_md->md->lac : 0,
			      C_PTL_LIST_PRIORITY, req->req_id,
			      mb.raw, ib.raw, req->recv.match_id,
			      req->recv.multi_recv ? rxc->min_multi_recv : 0,
			      le_flags, NULL, rxc->rx_cmdq,
			      !(req->recv.flags & FI_MORE));
	if (ret != FI_SUCCESS) {
		RXC_WARN(rxc, "Failed to write Append command: %d\n", ret);
		return ret;
	}

	return FI_SUCCESS;
}

/*
 * cxip_recv_common() - Common message receive function. Used for tagged and
 * untagged sends of all sizes.
 */
ssize_t cxip_recv_common(struct cxip_rxc *rxc, void *buf, size_t len,
			 void *desc, fi_addr_t src_addr, uint64_t tag,
			 uint64_t ignore, void *context, uint64_t flags,
			 bool tagged, struct cxip_cntr *comp_cntr)
{
	int ret;
	struct cxip_req *req;
	struct cxip_addr caddr;
	struct cxip_ux_send *ux_msg;
	uint32_t match_id;

	if (len && !buf)
		return -FI_EINVAL;

	if (rxc->state == RXC_DISABLED)
		return -FI_EOPBADSTATE;

	/* HW to SW PtlTE transition, ensure progress is made */
	if (rxc->state != RXC_ENABLED && rxc->state != RXC_ENABLED_SOFTWARE) {
		cxip_cq_progress(rxc->recv_cq);
		return -FI_EAGAIN;
	}

	if (tagged) {
		if (tag & ~CXIP_TAG_MASK || ignore & ~CXIP_TAG_MASK) {
			RXC_WARN(rxc,
				 "Invalid tag: %#018lx ignore: %#018lx (%#018lx)\n",
				 tag, ignore, CXIP_TAG_MASK);
			return -FI_EINVAL;
		}
		flags &= ~FI_MULTI_RECV;
	}

	/* If FI_DIRECTED_RECV and a src_addr is specified, encode the address
	 * in the LE for matching. If application AVs are symmetric, use
	 * logical FI address for matching. Otherwise, use physical address.
	 */
	if (rxc->attr.caps & FI_DIRECTED_RECV &&
	    src_addr != FI_ADDR_UNSPEC) {
		if (rxc->ep_obj->av->symmetric) {
			/* PID is not used for matching */
			match_id = CXI_MATCH_ID(rxc->pid_bits, C_PID_ANY,
						src_addr);
		} else {
			ret = cxip_av_lookup_addr(rxc->ep_obj->av, src_addr,
						  &caddr);
			if (ret != FI_SUCCESS) {
				RXC_WARN(rxc, "Failed to look up FI addr: %d\n",
					 ret);
				return -FI_EINVAL;
			}

			match_id = CXI_MATCH_ID(rxc->pid_bits, caddr.pid,
						caddr.nic);
		}
	} else {
		match_id = CXI_MATCH_ID_ANY;
	}

	ofi_genlock_lock(&rxc->ep_obj->lock);
	ret = cxip_recv_req_alloc(rxc, buf, len, &req);
	if (ret)
		goto err;

	/* req->data_len, req->tag, req->data must be set later. req->buf may
	 * be overwritten later.
	 */
	req->context = (uint64_t)context;

	req->flags = FI_RECV | (flags & FI_COMPLETION);
	if (tagged)
		req->flags |= FI_TAGGED;
	else
		req->flags |= FI_MSG;

	req->recv.cntr = comp_cntr ? comp_cntr : rxc->recv_cntr;
	req->recv.match_id = match_id;
	req->recv.tag = tag;
	req->recv.ignore = ignore;
	req->recv.flags = flags;
	req->recv.tagged = tagged;
	req->recv.multi_recv = (flags & FI_MULTI_RECV ? true : false);

	if (rxc->state != RXC_ENABLED && rxc->state != RXC_ENABLED_SOFTWARE) {
		ret = -FI_EAGAIN;
		goto err_free_request;
	}

	if (!(req->recv.flags & (FI_PEEK | FI_CLAIM))) {

		ret = cxip_recv_req_queue(req, false);
		/* Match made in software? */
		if (ret == -FI_EALREADY) {
			ofi_genlock_unlock(&rxc->ep_obj->lock);

			return FI_SUCCESS;
		}

		/* RXC busy (onloading Sends or full CQ)? */
		if (ret != FI_SUCCESS)
			goto err_free_request;

		ofi_genlock_unlock(&rxc->ep_obj->lock);

		RXC_DBG(rxc,
			"req: %p buf: %p len: %lu src_addr: %ld tag(%c):"
			" 0x%lx ignore: 0x%lx context: %p\n",
			req, buf, len, src_addr, tagged ? '*' : '-', tag,
			ignore, context);

		return FI_SUCCESS;
	}

	/* FI_PEEK with/without FI_CLAIM */
	if (req->recv.flags & FI_PEEK) {
		if (req->recv.flags & FI_CLAIM && !req->context) {
			RXC_WARN(rxc, "FI_CLAIM requires fi_context\n");
			ret = -FI_EINVAL;
			goto err_free_request;
		}
		ret = cxip_recv_req_peek(req, true);
		if (ret == FI_SUCCESS) {
			ofi_genlock_unlock(&rxc->ep_obj->lock);

			return ret;
		}

		goto err_free_request;
	}

	/* FI_CLAIM without FI_PEEK */
	ux_msg = ((struct fi_context *)req->context)->internal[0];
	if (!ux_msg->claimed) {
		RXC_WARN(rxc, "Bad fi_context specified with FI_CLAIM\n");
		ret = -FI_EINVAL;
		goto err_free_request;
	}

	RXC_DBG(rxc, "FI_CLAIM invoke sw matcher %p\n", ux_msg);
	ret = cxip_recv_sw_matcher(rxc, req, ux_msg, true);
	if (ret == FI_SUCCESS || ret == -FI_EINPROGRESS) {
		ofi_genlock_unlock(&rxc->ep_obj->lock);

		return FI_SUCCESS;
	}

err_free_request:
	cxip_recv_req_free(req);
err:
	ofi_genlock_unlock(&rxc->ep_obj->lock);

	return ret;
}

/*
 * cxip_txc_fi_addr() - Return the FI address of the TXC.
 */
static fi_addr_t _txc_fi_addr(struct cxip_txc *txc)
{
	if (txc->ep_obj->fi_addr == FI_ADDR_NOTAVAIL) {
		txc->ep_obj->fi_addr =
				cxip_av_lookup_fi_addr(txc->ep_obj->av,
						       &txc->ep_obj->src_addr);
		TXC_DBG(txc, "Found EP FI Addr: %lu\n", txc->ep_obj->fi_addr);
	}

	return txc->ep_obj->fi_addr;
}

/*
 * cxip_msg_match_id() - Return the TXC's initiator address used to transmit a
 * message.
 *
 * By default, the physical address of the TXC is returned. This address is
 * sent along with message data and is used for source address matching at the
 * target. When the target receives a message, the physical ID is translated to
 * a logical FI address. Translation adds overhead to the receive path.
 *
 * As an optimization, if rendezvous offload is not being used and the process
 * is part of a job with symmetric AVs, a logical FI address is returned. This
 * way, there is no source address translation overhead involved in the
 * receive.
 */
static uint32_t cxip_msg_match_id(struct cxip_txc *txc)
{
	/* PID is not used for logical matching, but is used for rendezvous. */
	if (txc->ep_obj->av->symmetric)
		return CXI_MATCH_ID(txc->pid_bits, txc->ep_obj->src_addr.pid,
				    _txc_fi_addr(txc));

	return CXI_MATCH_ID(txc->pid_bits, txc->ep_obj->src_addr.pid,
			    txc->ep_obj->src_addr.nic);
}

/*
 * report_send_completion() - Report the completion of a send operation.
 */
static void report_send_completion(struct cxip_req *req, bool sw_cntr)
{
	int ret;
	int ret_err;
	int success_event = (req->flags & FI_COMPLETION);
	struct cxip_txc *txc = req->send.txc;

	req->flags &= (FI_MSG | FI_TAGGED | FI_SEND);

	if (req->send.rc == C_RC_OK) {
		TXC_DBG(txc, "Request success: %p\n", req);

		if (success_event) {
			ret = cxip_cq_req_complete(req);
			if (ret != FI_SUCCESS)
				TXC_WARN(txc,
					 "Failed to report completion: %d\n",
					 ret);
		}

		if (sw_cntr && req->send.cntr) {
			ret = cxip_cntr_mod(req->send.cntr, 1, false, false);
			if (ret)
				TXC_WARN(txc, "cxip_cntr_mod returned: %d\n",
					 ret);
		}
	} else {
		ret_err = proverr2errno(req->send.rc);
		TXC_WARN(txc, "Request dest_addr: %ld caddr.nic: %#X caddr.pid: %u error: %p (err: %d, %s)\n",
			 req->send.dest_addr, req->send.caddr.nic,
			 req->send.caddr.pid, req, ret_err,
			 cxi_rc_to_str(req->send.rc));

		ret = cxip_cq_req_error(req, 0, ret_err,
					req->send.rc, NULL, 0,
					FI_ADDR_UNSPEC);
		if (ret != FI_SUCCESS)
			TXC_WARN(txc, "Failed to report error: %d\n", ret);

		if (sw_cntr && req->send.cntr) {
			ret = cxip_cntr_mod(req->send.cntr, 1, false, true);
			if (ret)
				TXC_WARN(txc, "cxip_cntr_mod returned: %d\n",
					 ret);
		}
	}
}

/*
 * rdzv_send_req_complete() - Complete long send request.
 */
static void rdzv_send_req_complete(struct cxip_req *req)
{
	cxip_rdzv_id_free(req->send.txc, req->send.rdzv_id);

	cxip_send_buf_fini(req);

	report_send_completion(req, true);

	ofi_atomic_dec32(&req->send.txc->otx_reqs);
	cxip_evtq_req_free(req);
}

/*
 * rdzv_send_req_event() - Count a rendezvous send event.
 *
 * Call for each initiator event. The events could be generated in any order.
 * Once all expected events are received, complete the request.
 *
 * A successful rendezvous Send generates two events: Ack and Get.
 */
static void rdzv_send_req_event(struct cxip_req *req)
{
	if (++req->send.rdzv_send_events == 2)
		rdzv_send_req_complete(req);
}

/*
 * cxip_send_rdzv_put_cb() - Long send callback.
 *
 * Progress a long send operation to completion.
 */
static int cxip_send_rdzv_put_cb(struct cxip_req *req,
				 const union c_event *event)
{
	int event_rc;
	int ret;
	struct cxip_txc *txc = req->send.txc;

	switch (event->hdr.event_type) {
	case C_EVENT_ACK:
		/* The source Put completed. */
		event_rc = cxi_init_event_rc(event);

		TXC_DBG(txc, "Acked: %p (rc: %s list: %s)\n", req,
			cxi_rc_to_str(event_rc),
			cxi_ptl_list_to_str(event->init_short.ptl_list));

		/* If the message was dropped, mark the peer as disabled. Do
		 * not generate a completion. Free associated resources. Do not
		 * free the request (it will be used to replay the Send).
		 */
		if (event_rc == C_RC_PT_DISABLED) {
			ret = cxip_send_req_dropped(req->send.txc, req);
			if (ret == FI_SUCCESS)
				cxip_rdzv_id_free(req->send.txc,
						  req->send.rdzv_id);
			else
				ret = -FI_EAGAIN;

			return ret;
		}

		/* Message was accepted by the peer. Match order is preserved.
		 * The request can be dequeued from the SW message queue. This
		 * allows flow-control recovery to be performed before
		 * outstanding long Send operations have completed.
		 */
		ret = cxip_send_req_dequeue(req->send.txc, req);
		if (ret != FI_SUCCESS)
			return ret;

		/* The transaction is complete if the put failed */
		if (event_rc != C_RC_OK) {
			req->send.rc = event_rc;
			rdzv_send_req_complete(req);
		} else {
			/* Count the event, another may be expected. */
			rdzv_send_req_event(req);
		}
		return FI_SUCCESS;

	/* When errors happen, send events can occur before the put/get event.
	 * These events should just be dropped.
	 */
	case C_EVENT_SEND:
	{
		struct cxi_md *md = req->send.send_md->md;

		TXC_WARN(txc, "Unexpected %s event: rc:%s buf:%p len:0x%lx iova:0x%llx md.va:0x%llx lac:%d\n",
			 cxi_event_to_str(event),
			 cxi_rc_to_str(cxi_event_rc(event)), req->send.buf,
			 req->send.len, CXI_VA_TO_IOVA(md, req->send.buf),
			 md->iova, md->lac);
	}
		return FI_SUCCESS;

	default:
		TXC_FATAL(txc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}
}

/*
 * cxip_rdzv_pte_src_cb() - Process rendezvous source buffer events.
 *
 * A Get event is generated for each rendezvous Send indicating Send completion.
 */
int cxip_rdzv_pte_src_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rdzv_pte *rdzv_pte = req->req_ctx;
	struct cxip_txc *txc = rdzv_pte->txc;
	struct cxip_req *get_req;
	union cxip_match_bits mb;
	int event_rc = cxi_event_rc(event);
	int rdzv_id;

	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		if (event_rc == C_RC_OK)
			ofi_atomic_inc32(&rdzv_pte->le_linked_success_count);
		else
			ofi_atomic_inc32(&rdzv_pte->le_linked_failure_count);
		return FI_SUCCESS;

	case C_EVENT_GET:
		mb.raw = event->tgt_long.match_bits;
		rdzv_id = (mb.rdzv_id_hi << CXIP_RDZV_ID_CMD_WIDTH) |
			  mb.rdzv_id_lo;
		get_req = cxip_rdzv_id_lookup(txc, rdzv_id);
		if (!get_req) {
			TXC_WARN(txc, "Failed to find RDZV ID: %d\n",
				 mb.rdzv_id_lo);
			return FI_SUCCESS;
		}

		if (event_rc != C_RC_OK)
			TXC_WARN(txc, "Get error: %p rc: %s\n", get_req,
				 cxi_rc_to_str(event_rc));
		else
			TXC_DBG(txc, "Get received: %p rc: %s\n", get_req,
				cxi_rc_to_str(event_rc));

		get_req->send.rc = event_rc;

		/* Count the event, another may be expected. */
		rdzv_send_req_event(get_req);

		return FI_SUCCESS;
	default:
		TXC_FATAL(txc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}
}

static inline int cxip_send_prep_cmdq(struct cxip_cmdq *cmdq,
				      struct cxip_req *req,
				      uint32_t tclass)
{
	struct cxip_txc *txc = req->send.txc;
	int ret;
	uint16_t vni;

	if (!req->triggered) {
		if (txc->ep_obj->av_auth_key)
			vni = req->send.caddr.vni;
		else
			vni = txc->ep_obj->auth_key.vni;

		ret = cxip_txq_cp_set(cmdq, vni,
				      cxip_ofi_to_cxi_tc(txc->tclass),
				      CXI_TC_TYPE_DEFAULT);
		if (ret != FI_SUCCESS)
			return ret;
	}

	if (req->send.flags & FI_FENCE) {
		ret = cxi_cq_emit_cq_cmd(cmdq->dev_cmdq, C_CMD_CQ_FENCE);
		if (ret) {
			TXC_DBG(txc, "Failed to issue CQ_FENCE command: %d\n",
				ret);
			return -FI_EAGAIN;
		}
	}

	return FI_SUCCESS;
}

/*
 * _cxip_send_rdzv_put() - Initiate a send rendezvous put operation.
 *
 * The rendezvous protocol works as follows:
 *
 * 1. The Initiator performs a Rendezvous Put command which includes a portion
 *    of the source buffer data.
 * 2. Once the Put is matched to a user receive buffer (in the Priority list),
 *    a Get of the remaining source data is performed.
 */
static ssize_t _cxip_send_rdzv_put(struct cxip_req *req)
{
	struct cxip_txc *txc = req->send.txc;
	union c_fab_addr dfa;
	uint8_t idx_ext;
	struct c_full_dma_cmd cmd = {};
	union cxip_match_bits put_mb = {};
	int rdzv_id;
	int lac = req->send.send_md->md->lac;
	int ret;
	struct cxip_cmdq *cmdq =
		req->triggered ? txc->domain->trig_cmdq : txc->tx_cmdq;

	/* Zero length rendezvous not supported. */
	assert(req->send.send_md);
	assert(req->send.len);

	/* Allocate rendezvous ID */
	rdzv_id = cxip_rdzv_id_alloc(txc, req);
	if (rdzv_id < 0)
		return -FI_EAGAIN;

	/* Calculate DFA */
	cxi_build_dfa(req->send.caddr.nic, req->send.caddr.pid, txc->pid_bits,
		      CXIP_PTL_IDX_RXQ, &dfa, &idx_ext);

	/* Allocate a source request for the given LAC. This makes the source
	 * memory accessible for rendezvous.
	 */
	ret = cxip_rdzv_pte_src_req_alloc(txc->rdzv_pte, lac);
	if (ret) {
		TXC_WARN(txc, "Failed to prepare source window: %d\n", ret);
		goto err_free_rdzv_id;
	}


	/* Allocate restricted source window. If resources can not be allocated
	 * discontinue use of the restricted protocol, falling back
	 * to unrestricted. TODO: keep track and only switch for LAC that
	 * failed.
	 */
	if (txc->rdzv_proto == CXIP_RDZV_PROTO_ALT_READ &&
	    !txc->rdzv_nomatch_pte[lac]) {
		TXC_DBG(txc, "allocate restricted PTE lac %d\n", lac);

		ret = cxip_rdzv_nomatch_pte_alloc(txc, lac,
						  &txc->rdzv_nomatch_pte[lac]);
		if (ret) {
			TXC_WARN(txc, WARN_RESTRICTED_DISABLED,
				 cxip_rdzv_proto_to_str(txc->rdzv_proto),
				 cxip_rdzv_proto_to_str(CXIP_RDZV_PROTO_DEFAULT));
			txc->rdzv_proto = CXIP_RDZV_PROTO_DEFAULT;
		}
	}

	/* Build match bits */
	if (req->send.tagged) {
		put_mb.tagged = 1;
		put_mb.tag = req->send.tag;
	}

	if (req->send.flags & FI_REMOTE_CQ_DATA)
		put_mb.cq_data = 1;

	put_mb.rdzv_proto = txc->rdzv_proto;

	req->send.rdzv_id = rdzv_id;
	req->cb = cxip_send_rdzv_put_cb;
	req->send.rdzv_send_events = 0;

	/* Build Put command descriptor */
	cmd.command.cmd_type = C_CMD_TYPE_DMA;
	cmd.index_ext = idx_ext;
	cmd.lac = req->send.send_md->md->lac;
	cmd.event_send_disable = 1;
	cmd.restricted = 0;
	cmd.dfa = dfa;
	cmd.local_addr = CXI_VA_TO_IOVA(req->send.send_md->md, req->send.buf);
	cmd.request_len = req->send.len;
	cmd.eq = cxip_evtq_eqn(&txc->tx_evtq);
	cmd.user_ptr = (uint64_t)req;
	cmd.initiator = cxip_msg_match_id(txc);
	cmd.header_data = req->send.data;
	cmd.remote_offset =
		CXI_VA_TO_IOVA(req->send.send_md->md, req->send.buf);
	cmd.command.opcode = C_CMD_RENDEZVOUS_PUT;
	cmd.eager_length = txc->rdzv_eager_size;
	cmd.use_offset_for_get = 1;

	put_mb.rdzv_id_hi = rdzv_id >> CXIP_RDZV_ID_CMD_WIDTH;
	put_mb.rdzv_lac = req->send.send_md->md->lac;
	put_mb.le_type = CXIP_LE_TYPE_RX;
	cmd.match_bits = put_mb.raw;
	cmd.rendezvous_id = rdzv_id;

	if (req->triggered) {
		const struct c_ct_cmd ct_cmd = {
			.trig_ct = req->trig_cntr->ct->ctn,
			.threshold = req->trig_thresh,
		};

		/* Triggered command queue is domain resource, lock. */
		ofi_genlock_lock(&txc->domain->trig_cmdq_lock);

		ret = cxip_send_prep_cmdq(cmdq, req, req->send.tclass);
		if (ret) {
			ofi_genlock_unlock(&txc->domain->trig_cmdq_lock);
			goto err_free_rdzv_id;
		}

		/* Clear the triggered flag to prevent retrying of operation,
		 * due to flow control, from using the triggered path.
		 */
		req->triggered = false;

		ret = cxi_cq_emit_trig_full_dma(cmdq->dev_cmdq, &ct_cmd,
						&cmd);
		if (ret) {
			ofi_genlock_unlock(&txc->domain->trig_cmdq_lock);
			goto err_enqueue;
		}

		cxip_txq_ring(cmdq, !!(req->send.flags & FI_MORE),
			      ofi_atomic_get32(&req->send.txc->otx_reqs) - 1);
		ofi_genlock_unlock(&txc->domain->trig_cmdq_lock);
	} else {

		ret = cxip_send_prep_cmdq(cmdq, req, req->send.tclass);
		if (ret)
			goto err_free_rdzv_id;

		ret = cxi_cq_emit_dma(cmdq->dev_cmdq, &cmd);
		if (ret)
			goto err_enqueue;

		cxip_txq_ring(cmdq, !!(req->send.flags & FI_MORE),
			      ofi_atomic_get32(&req->send.txc->otx_reqs) - 1);
	}

	return FI_SUCCESS;

err_enqueue:
	TXC_DBG(txc, "Failed to enqueue Put: %d, return -FI_EAGAIN\n", ret);
err_free_rdzv_id:
	cxip_rdzv_id_free(txc, rdzv_id);

	return -FI_EAGAIN;
}

/*
 * cxip_send_eager_cb() - Eager send callback. Used for both tagged and
 * untagged messages.
 */
static int cxip_send_eager_cb(struct cxip_req *req,
			      const union c_event *event)
{
	int match_complete = req->flags & FI_MATCH_COMPLETE;
	int ret;

	/* When errors happen, send events can occur before the put/get event.
	 * These events should just be dropped.
	 */
	if (event->hdr.event_type == C_EVENT_SEND) {
		TXC_WARN(req->send.txc, CXIP_UNEXPECTED_EVENT,
			 cxi_event_to_str(event),
			 cxi_rc_to_str(cxi_event_rc(event)));
		return FI_SUCCESS;
	}

	assert(event->hdr.event_type == C_EVENT_ACK);

	req->send.rc = cxi_init_event_rc(event);

	/* If the message was dropped, mark the peer as disabled. Do not
	 * generate a completion. Free associated resources. Do not free the
	 * request (it will be used to replay the Send).
	 */
	if (req->send.rc == C_RC_PT_DISABLED) {

		ret = cxip_send_req_dropped(req->send.txc, req);
		if (ret != FI_SUCCESS)
			return -FI_EAGAIN;

		if (match_complete)
			cxip_tx_id_free(req->send.txc, req->send.tx_id);

		return FI_SUCCESS;
	}

	ret = cxip_send_req_dequeue(req->send.txc, req);
	if (ret != FI_SUCCESS)
		return ret;

	cxip_send_buf_fini(req);

	/* If MATCH_COMPLETE was requested and the the Put did not match a user
	 * buffer, do not generate a completion event until the target notifies
	 * the initiator that the match is complete.
	 */
	if (match_complete) {
		if (req->send.rc == C_RC_OK &&
		    event->init_short.ptl_list != C_PTL_LIST_PRIORITY) {
			TXC_DBG(req->send.txc,
				"Waiting for match complete: %p\n", req);
			return FI_SUCCESS;
		}

		TXC_DBG(req->send.txc, "Match complete with Ack: %p\n", req);
		cxip_tx_id_free(req->send.txc, req->send.tx_id);
	}

	/* If MATCH_COMPLETE was requested, software must manage counters. */
	report_send_completion(req, match_complete);

	ofi_atomic_dec32(&req->send.txc->otx_reqs);
	cxip_evtq_req_free(req);

	return FI_SUCCESS;
}

static inline int cxip_set_eager_mb(struct cxip_req *req,
				    union cxip_match_bits *mb)
{
	int tx_id;

	mb->raw = 0;
	mb->le_type = CXIP_LE_TYPE_RX;
	mb->tagged = req->send.tagged;
	mb->tag = req->send.tag;
	mb->cq_data = !!(req->send.flags & FI_REMOTE_CQ_DATA);

	/* Allocate a TX ID if match completion guarantees are required */
	if (req->send.flags & FI_MATCH_COMPLETE) {

		tx_id = cxip_tx_id_alloc(req->send.txc, req);
		if (tx_id < 0) {
			TXC_DBG(req->send.txc,
				"Failed to allocate TX ID: %d\n", tx_id);
			return -FI_EAGAIN;
		}

		req->send.tx_id = tx_id;
		mb->match_comp = 1;
		mb->tx_id = tx_id;
	}

	return FI_SUCCESS;
}

/*
 * _cxip_send_eager_idc() - Enqueue eager IDC message
 */
static ssize_t _cxip_send_eager_idc(struct cxip_req *req)
{
	struct cxip_txc *txc = req->send.txc;
	union c_fab_addr dfa;
	uint8_t idx_ext;
	union cxip_match_bits mb;
	ssize_t ret;
	struct cxip_cmdq *cmdq = txc->tx_cmdq;
	const void *buf;
	struct c_cstate_cmd cstate_cmd = {};
	struct c_idc_msg_hdr idc_cmd;

	assert(req->send.len > 0);

#if ENABLE_DEBUG
	if (req->send.flags & FI_INJECT)
		assert(req->send.ibuf);

	/* ibuf and send_md are mutually exclusive. */
	if (req->send.ibuf) {
		assert(req->send.send_md == NULL);
	} else if (req->send.send_md) {
		assert(req->send.ibuf == NULL);

		/* All non FI_HMEM_SYSTEM buffers require an ibuf. */
		assert(req->send.send_md->info.iface == FI_HMEM_SYSTEM);
	}
#endif

	/* Calculate DFA */
	cxi_build_dfa(req->send.caddr.nic, req->send.caddr.pid, txc->pid_bits,
		      CXIP_PTL_IDX_RXQ, &dfa, &idx_ext);

	/* Favor bounce buffer if allocated. */
	if (req->send.ibuf)
		buf = req->send.ibuf;
	else
		buf = req->send.buf;

	ret = cxip_set_eager_mb(req, &mb);
	if (ret)
		goto err;

	req->cb = cxip_send_eager_cb;

	/* Build commands before taking lock */
	cstate_cmd.event_send_disable = 1;
	cstate_cmd.index_ext = idx_ext;
	cstate_cmd.eq = cxip_evtq_eqn(&txc->tx_evtq);
	cstate_cmd.initiator = cxip_msg_match_id(txc);

	/* If MATCH_COMPLETE was requested, software must manage
	 * counters.
	 */
	if (req->send.cntr && !mb.match_comp) {
		cstate_cmd.event_ct_ack = 1;
		cstate_cmd.ct = req->send.cntr->ct->ctn;
	}

	/* Note: IDC command completely filled in */
	idc_cmd.unused_0 = 0;
	idc_cmd.dfa = dfa;
	idc_cmd.match_bits = mb.raw;
	idc_cmd.header_data = req->send.data;
	idc_cmd.user_ptr = (uint64_t)req;

	/* Submit command */
	ret = cxip_send_prep_cmdq(cmdq, req, req->send.tclass);
	if (ret)
		goto err_cleanup;

	ret = cxip_cmdq_emit_c_state(cmdq, &cstate_cmd);
	if (ret) {
		TXC_DBG(txc, "Failed to issue C_STATE command: %ld\n", ret);
		goto err_cleanup;
	}

	ret = cxi_cq_emit_idc_msg(cmdq->dev_cmdq, &idc_cmd, buf, req->send.len);
	if (ret) {
		TXC_DBG(txc, "Failed to write IDC: %ld\n", ret);

		/* Return error according to Domain Resource Management */
		ret = -FI_EAGAIN;
		goto err_cleanup;
	}

	cxip_txq_ring(cmdq, !!(req->send.flags & FI_MORE),
		      ofi_atomic_get32(&req->send.txc->otx_reqs) - 1);

	return FI_SUCCESS;

err_cleanup:
	if (mb.match_comp)
		cxip_tx_id_free(txc, req->send.tx_id);
err:
	return ret;
}

/*
 * _cxip_send_eager() - Enqueue eager send command.
 */
static ssize_t _cxip_send_eager(struct cxip_req *req)
{
	struct cxip_txc *txc = req->send.txc;
	union c_fab_addr dfa;
	uint8_t idx_ext;
	union cxip_match_bits mb;
	ssize_t ret;
	struct cxip_cmdq *cmdq =
		req->triggered ? txc->domain->trig_cmdq : txc->tx_cmdq;
	bool trig = req->triggered;
	struct c_full_dma_cmd cmd = {};

	/* Calculate DFA */
	cxi_build_dfa(req->send.caddr.nic, req->send.caddr.pid, txc->pid_bits,
		      CXIP_PTL_IDX_RXQ, &dfa, &idx_ext);

	ret = cxip_set_eager_mb(req, &mb);
	if (ret)
		goto err;

	req->cb = cxip_send_eager_cb;

	cmd.command.cmd_type = C_CMD_TYPE_DMA;
	cmd.command.opcode = C_CMD_PUT;
	cmd.index_ext = idx_ext;
	cmd.event_send_disable = 1;
	cmd.dfa = dfa;
	cmd.eq = cxip_evtq_eqn(&txc->tx_evtq);
	cmd.user_ptr = (uint64_t)req;
	cmd.initiator = cxip_msg_match_id(txc);
	cmd.match_bits = mb.raw;
	cmd.header_data = req->send.data;

	/* Triggered ops could result in 0 length DMA */
	if (req->send.send_md) {
		cmd.lac = req->send.send_md->md->lac;
		cmd.local_addr = CXI_VA_TO_IOVA(req->send.send_md->md,
						req->send.buf);
		cmd.request_len = req->send.len;
	}

	/* If MATCH_COMPLETE was requested, software must manage
	 * counters.
	 */
	if (req->send.cntr && !mb.match_comp) {
		cmd.event_ct_ack = 1;
		cmd.ct = req->send.cntr->ct->ctn;
	}

	/* Issue Eager Put command */
	if (trig) {
		const struct c_ct_cmd ct_cmd = {
			.trig_ct = req->trig_cntr->ct->ctn,
			.threshold = req->trig_thresh,
		};

		/* Triggered command queue is domain resource, lock. */
		ofi_genlock_lock(&txc->domain->trig_cmdq_lock);
		ret = cxip_send_prep_cmdq(cmdq, req, req->send.tclass);
		if (ret) {
			ofi_genlock_unlock(&txc->domain->trig_cmdq_lock);
			goto err;
		}

		/* Clear the triggered flag to prevent retrying of
		 * operation, due to flow control, from using the
		 * triggered path.
		 */
		req->triggered = false;

		ret = cxi_cq_emit_trig_full_dma(cmdq->dev_cmdq, &ct_cmd,
						&cmd);
		if (ret) {
			ofi_genlock_unlock(&txc->domain->trig_cmdq_lock);
			goto err_enqueue;
		}
		cxip_txq_ring(cmdq, !!(req->send.flags & FI_MORE),
			      ofi_atomic_get32(&req->send.txc->otx_reqs) - 1);
		ofi_genlock_unlock(&txc->domain->trig_cmdq_lock);

	} else {
		ret = cxip_send_prep_cmdq(cmdq, req, req->send.tclass);
		if (ret)
			goto err;

		ret = cxi_cq_emit_dma(cmdq->dev_cmdq, &cmd);
		if (ret)
			goto err_enqueue;

		cxip_txq_ring(cmdq, !!(req->send.flags & FI_MORE),
			      ofi_atomic_get32(&req->send.txc->otx_reqs) - 1);
	}

	return FI_SUCCESS;

err_enqueue:
	TXC_DBG(txc, "Failed to write DMA command: %ld\n", ret);
	ret = -FI_EAGAIN;

	if (mb.match_comp)
		cxip_tx_id_free(txc, req->send.tx_id);
err:
	return ret;
}

static bool cxip_send_eager_idc(struct cxip_req *req)
{
	return (req->send.len <= CXIP_INJECT_SIZE) &&
		!cxip_env.disable_non_inject_msg_idc;
}

static ssize_t _cxip_send_req(struct cxip_req *req)
{
	/* Force all zero-byte operations to use the eager path. This utilizes
	 * a smaller command format.
	 */
	if (req->send.len == 0)
		return _cxip_send_eager(req);

	/* IDC commands are not supported with triggered operations. */
	if (!req->triggered &&
	    ((req->send.flags & FI_INJECT) || cxip_send_eager_idc(req)))
		return _cxip_send_eager_idc(req);

	if (req->send.len <= req->send.txc->max_eager_size)
		return _cxip_send_eager(req);

	return _cxip_send_rdzv_put(req);
}

/*
 * cxip_fc_peer_lookup() - Check if a peer is disabled.
 *
 * Look up disabled peer state and return it, if available.
 *
 * Caller must hold ep_obj->lock.
 */
static struct cxip_fc_peer *cxip_fc_peer_lookup(struct cxip_txc *txc,
						struct cxip_addr caddr)
{
	struct cxip_fc_peer *peer;

	dlist_foreach_container(&txc->fc_peers, struct cxip_fc_peer,
				peer, txc_entry) {
		if (CXIP_ADDR_EQUAL(peer->caddr, caddr))
			return peer;
	}

	return NULL;
}

/*
 * cxip_fc_peer_put() - Account for completion of an outstanding Send targeting
 * a disabled peer.
 *
 * Drop a reference to a disabled peer. When the last reference is dropped,
 * attempt flow-control recovery.
 *
 * Caller must hold ep_obj->lock.
 */
static int cxip_fc_peer_put(struct cxip_fc_peer *peer)
{
	int ret;

	assert(peer->pending > 0);

	/* Account for the completed Send */
	if (!--peer->pending) {
		peer->req.send.mb.drops = peer->dropped;

		ret = cxip_ctrl_msg_send(&peer->req);
		if (ret != FI_SUCCESS) {
			peer->pending++;
			return ret;
		}

		peer->pending_acks++;

		TXC_DBG(peer->txc,
			"Notified disabled peer NIC: %#x PID: %u dropped: %u\n",
			peer->caddr.nic, peer->caddr.pid, peer->dropped);
	}

	return FI_SUCCESS;
}

/*
 * cxip_fc_peer_fini() - Remove disabled peer state.
 *
 * Caller must hold ep_obj->lock.
 */
static void cxip_fc_peer_fini(struct cxip_fc_peer *peer)
{
	assert(dlist_empty(&peer->msg_queue));
	dlist_remove(&peer->txc_entry);
	free(peer);
}

/*
 * cxip_fc_notify_cb() - Process FC notify completion events.
 */
int cxip_fc_notify_cb(struct cxip_ctrl_req *req, const union c_event *event)
{
	struct cxip_fc_peer *peer = container_of(req, struct cxip_fc_peer, req);
	struct cxip_txc *txc = peer->txc;

	switch (event->hdr.event_type) {
	case C_EVENT_ACK:
		switch (cxi_event_rc(event)) {
		case C_RC_OK:
			TXC_DBG(txc,
				"FC_NOTIFY to %#x:%u successfully sent: retry_count=%u\n",
				peer->caddr.nic, peer->caddr.pid,
				peer->retry_count);

			/* Peer flow control structure can only be freed if
			 * replay is complete and all acks accounted for.
			 */
			peer->pending_acks--;
			if (!peer->pending_acks && peer->replayed)
				cxip_fc_peer_fini(peer);

			return FI_SUCCESS;

		/* This error occurs when the target's control event queue has
		 * run out of space. Since the target should be processing the
		 * event queue, it is safe to replay messages until C_RC_OK is
		 * returned.
		 */
		case C_RC_ENTRY_NOT_FOUND:
			peer->retry_count++;
			TXC_WARN(txc,
				 "%#x:%u dropped FC message: retry_delay_usecs=%d retry_count=%u\n",
				 peer->caddr.nic, peer->caddr.pid,
				 cxip_env.fc_retry_usec_delay,
				 peer->retry_count);
			usleep(cxip_env.fc_retry_usec_delay);
			return cxip_ctrl_msg_send(req);
		default:
			TXC_FATAL(txc, CXIP_UNEXPECTED_EVENT_STS,
				  cxi_event_to_str(event),
				  cxi_rc_to_str(cxi_event_rc(event)));
		}
	default:
		TXC_FATAL(txc, CXIP_UNEXPECTED_EVENT,
			  cxi_event_to_str(event),
			  cxi_rc_to_str(cxi_event_rc(event)));
	}
}

/*
 * cxip_fc_peer_init() - Mark a peer as disabled.
 *
 * Called by sending EP after experiencing first dropped Send to a peer.
 *
 * Allocate state to track the disabled peer. Locate all outstanding Sends
 * targeting the peer.
 *
 * Caller must hold ep_obj->lock.
 */
static int cxip_fc_peer_init(struct cxip_txc *txc, struct cxip_addr caddr,
			     struct cxip_fc_peer **peer)
{
	struct cxip_fc_peer *p;
	struct cxip_req *req;
	struct dlist_entry *tmp;

	p = calloc(1, sizeof(*p));
	if (!p) {
		TXC_WARN(txc, "Failed to allocate FC Peer\n");
		return -FI_ENOMEM;
	}

	p->caddr = caddr;
	p->txc = txc;
	dlist_init(&p->msg_queue);
	dlist_insert_tail(&p->txc_entry, &txc->fc_peers);

	p->req.send.nic_addr = caddr.nic;
	p->req.send.pid = caddr.pid;
	/* TODO: remove */
	p->req.send.mb.txc_id = 0;
	p->req.send.mb.rxc_id = 0;

	p->req.send.mb.ctrl_le_type = CXIP_CTRL_LE_TYPE_CTRL_MSG;
	p->req.send.mb.ctrl_msg_type = CXIP_CTRL_MSG_FC_NOTIFY;
	p->req.cb = cxip_fc_notify_cb;
	p->req.ep_obj = txc->ep_obj;

	/* Queue all Sends to the FC'ed peer */
	dlist_foreach_container_safe(&txc->msg_queue, struct cxip_req,
				     req, send.txc_entry, tmp) {
		if (CXIP_ADDR_EQUAL(req->send.caddr, caddr)) {
			dlist_remove(&req->send.txc_entry);
			dlist_insert_tail(&req->send.txc_entry, &p->msg_queue);
			p->pending++;
			req->send.fc_peer = p;
		}
	}

	*peer = p;

	return FI_SUCCESS;
}

/*
 * cxip_fc_resume() - Replay dropped Sends.
 *
 * Called by sending EP after being notified disabled peer was re-enabled.
 *
 * Replay all dropped Sends in order.
 */
int cxip_fc_resume(struct cxip_ep_obj *ep_obj, uint32_t nic_addr, uint32_t pid)
{
	struct cxip_txc *txc = &ep_obj->txc;
	struct cxip_fc_peer *peer;
	struct cxip_addr caddr = {
		.nic = nic_addr,
		.pid = pid,
	};
	struct cxip_req *req;
	struct dlist_entry *tmp;
	int ret __attribute__((unused));

	peer = cxip_fc_peer_lookup(txc, caddr);
	if (!peer)
		TXC_FATAL(txc, "Fatal, FC peer not found: NIC: %#x PID: %d\n",
			  nic_addr, pid);

	TXC_DBG(txc, "Replaying dropped sends, NIC: %#x PID: %d\n",
		nic_addr, pid);

	dlist_foreach_container_safe(&peer->msg_queue, struct cxip_req,
				     req, send.txc_entry, tmp) {
		/* -FI_EAGAIN can be return if the command queue is full. Loop
		 * until this goes through.
		 */
		do {
			ret = _cxip_send_req(req);
		} while (ret == -FI_EAGAIN);
		assert(ret == FI_SUCCESS);

		/* Move request back to the message queue. */
		dlist_remove(&req->send.txc_entry);
		req->send.fc_peer = NULL;
		dlist_insert_tail(&req->send.txc_entry, &txc->msg_queue);

		TXC_DBG(txc, "Replayed %p\n", req);
	}

	/* Peer flow control structure can only be freed if replay is complete
	 * and all acks accounted for.
	 */
	if (!peer->pending_acks)
		cxip_fc_peer_fini(peer);
	else
		peer->replayed = true;

	return FI_SUCCESS;
}

/*
 * cxip_send_req_dropped() - Mark the Send request dropped.
 *
 * Mark the Send request dropped. Mark the target peer as disabled. Track all
 * outstanding Sends targeting the disabled peer. When all outstanding Sends
 * are completed, recovery will be performed.
 */
static int cxip_send_req_dropped(struct cxip_txc *txc, struct cxip_req *req)
{
	struct cxip_fc_peer *peer;
	int ret;

	/* Check if peer is already disabled */
	peer = cxip_fc_peer_lookup(txc, req->send.caddr);
	if (!peer) {
		ret = cxip_fc_peer_init(txc, req->send.caddr, &peer);
		if (ret != FI_SUCCESS)
			return ret;

		TXC_DBG(txc,
			"Disabled peer detected, NIC: %#x PID: %u pending: %u\n",
			peer->caddr.nic, peer->caddr.pid, peer->pending);
	}

	/* Account for the dropped message. */
	peer->dropped++;
	ret = cxip_fc_peer_put(peer);
	if (ret)
		peer->dropped--;
	else
		TXC_DBG(txc,
			"Send dropped, req: %p NIC: %#x PID: %u pending: %u dropped: %u\n",
			req, peer->caddr.nic, peer->caddr.pid, peer->pending,
			peer->dropped);

	return ret;
}

/*
 * cxip_send_req_queue() - Queue Send request on TXC.
 *
 * Place the Send request in an ordered SW queue. Return error if the target
 * peer is disabled.
 */
static int cxip_send_req_queue(struct cxip_txc *txc, struct cxip_req *req)
{
	struct cxip_fc_peer *peer;

	if (!dlist_empty(&txc->fc_peers)) {
		peer = cxip_fc_peer_lookup(txc, req->send.caddr);
		if (peer) {
			/* Peer is disabled. Progress control EQs so future
			 * cxip_send_req_queue() may succeed.
			 */
			cxip_ep_ctrl_progress_locked(txc->ep_obj);

			return -FI_EAGAIN;
		}
	}

	dlist_insert_tail(&req->send.txc_entry, &txc->msg_queue);

	return FI_SUCCESS;
}

/*
 * cxip_send_req_dequeue() - Dequeue Send request from TXC.
 *
 * Remove the Send requst from the ordered message queue. Update peer
 * flow-control state, if necessary.
 */
static int cxip_send_req_dequeue(struct cxip_txc *txc, struct cxip_req *req)
{
	int ret;

	if (req->send.fc_peer) {
		/* The peer was disabled after this message arrived. */
		TXC_DBG(txc,
			"Send not dropped, req: %p NIC: %#x PID: %u pending: %u dropped: %u\n",
			req, req->send.fc_peer->caddr.nic,
			req->send.fc_peer->caddr.pid,
			req->send.fc_peer->pending, req->send.fc_peer->dropped);

		ret = cxip_fc_peer_put(req->send.fc_peer);
		if (ret != FI_SUCCESS)
			return ret;

		req->send.fc_peer = NULL;
	}

	dlist_remove(&req->send.txc_entry);

	return FI_SUCCESS;
}

static void cxip_send_buf_fini(struct cxip_req *req)
{
	if (req->send.send_md)
		cxip_unmap(req->send.send_md);
	if (req->send.ibuf)
		cxip_txc_ibuf_free(req->send.txc, req->send.ibuf);
}

static int cxip_send_buf_init(struct cxip_req *req)
{
	struct cxip_txc *txc = req->send.txc;
	int ret;

	/* Nothing to do for zero byte sends. */
	if (!req->send.len)
		return FI_SUCCESS;

	/* Triggered operation always requires memory registration. */
	if (req->triggered)
		return cxip_map(txc->domain, req->send.buf, req->send.len, 0,
			       &req->send.send_md);

	/* FI_INJECT operations always require an internal bounce buffer. This
	 * is needed to replay FI_INJECT operations which may experience flow
	 * control.
	 */
	if (req->send.flags & FI_INJECT) {

		req->send.ibuf = cxip_txc_ibuf_alloc(txc);
		if (!req->send.ibuf)
			return -FI_EAGAIN;

		if (txc->hmem) {
			ret = cxip_txc_copy_from_hmem(txc, NULL, req->send.ibuf,
						      req->send.buf,
						      req->send.len);
			if (ret) {
				TXC_WARN(txc,
					 "cxip_txc_copy_from_hmem failed: %d:%s\n",
					 ret, fi_strerror(-ret));
				goto err_buf_fini;
			}

			return FI_SUCCESS;
		}

		memcpy(req->send.ibuf, req->send.buf, req->send.len);
		return FI_SUCCESS;
	}

	/* If message is going to be sent as an IDC, a bounce buffer is needed
	 * if FI_HMEM is being used. This is due to the buffer type being
	 * unknown.
	 */
	if (cxip_send_eager_idc(req)) {
		if (txc->hmem) {

			req->send.ibuf = cxip_txc_ibuf_alloc(txc);
			if (!req->send.ibuf) {
				ret = -FI_EAGAIN;
				goto err_buf_fini;
			}

			ret = cxip_txc_copy_from_hmem(txc, NULL, req->send.ibuf,
						      req->send.buf,
						      req->send.len);
			if (ret) {
				TXC_WARN(txc,
					 "cxip_txc_copy_from_hmem failed: %d:%s\n",
					 ret, fi_strerror(-ret));
				goto err_buf_fini;
			}
		}

		return FI_SUCCESS;
	}

	/* Everything else requires memory registeration. */
	return cxip_map(txc->domain, req->send.buf, req->send.len, 0,
			&req->send.send_md);

err_buf_fini:
	cxip_send_buf_fini(req);

	return ret;
}

/*
 * cxip_send_common() - Common message send function. Used for tagged and
 * untagged sends of all sizes. This includes triggered operations.
 */
ssize_t cxip_send_common(struct cxip_txc *txc, uint32_t tclass, const void *buf,
			 size_t len, void *desc, uint64_t data,
			 fi_addr_t dest_addr, uint64_t tag, void *context,
			 uint64_t flags, bool tagged, bool triggered,
			 uint64_t trig_thresh, struct cxip_cntr *trig_cntr,
			 struct cxip_cntr *comp_cntr)
{
	struct cxip_req *req;
	struct cxip_addr caddr;
	int ret;

	if (len && !buf)
		return -FI_EINVAL;

	if (len > CXIP_EP_MAX_MSG_SZ)
		return -FI_EMSGSIZE;

	if (tagged && tag & ~CXIP_TAG_MASK) {
		TXC_WARN(txc, "Invalid tag: %#018lx (%#018lx)\n",
			 tag, CXIP_TAG_MASK);
		return -FI_EINVAL;
	}

	if (flags & FI_INJECT && len > CXIP_INJECT_SIZE) {
		TXC_WARN(txc, "Invalid inject length: %lu\n", len);
		return -FI_EMSGSIZE;
	}

	ofi_genlock_lock(&txc->ep_obj->lock);

	req = cxip_evtq_req_alloc(&txc->tx_evtq, false, txc);
	if (!req) {
		TXC_DBG(txc, "Failed to allocate request, return -FI_EAGAIN\n");
		ret = -FI_EAGAIN;
		goto unlock;
	}

	/* Restrict outstanding success event requests to queue size */
	if (ofi_atomic_inc32(&txc->otx_reqs) > txc->attr.size) {
		ret = -FI_EAGAIN;
		goto err_req_free;
	}

	req->triggered = triggered;
	req->trig_thresh = trig_thresh;
	req->trig_cntr = trig_cntr;

	/* Save Send parameters to replay */
	req->type = CXIP_REQ_SEND;
	req->send.txc = txc;
	req->send.tclass = tclass;

	req->send.cntr = triggered ? comp_cntr : txc->send_cntr;
	req->send.buf = buf;
	req->send.len = len;
	req->send.data = data;
	req->send.flags = flags;

	/* Set completion parameters */
	req->context = (uint64_t)context;
	req->flags = FI_SEND | (flags & (FI_COMPLETION | FI_MATCH_COMPLETE));
	if (tagged) {
		req->send.tagged = tagged;
		req->send.tag = tag;
		req->flags |= FI_TAGGED;
	} else {
		req->flags |= FI_MSG;
	}

	ret = cxip_send_buf_init(req);
	if (ret) {
		TXC_WARN(txc, "cxip_send_buf_init failed: %d:%s\n", ret,
			 fi_strerror(-ret));
		goto err_req_free;
	}

	/* Look up target CXI address */
	ret = cxip_av_lookup_addr(txc->ep_obj->av, dest_addr, &caddr);
	if (ret != FI_SUCCESS) {
		TXC_WARN(txc, "Failed to look up FI addr: %d\n", ret);
		goto err_req_buf_fini;
	}

	req->send.caddr = caddr;
	req->send.dest_addr = dest_addr;

	if (cxip_evtq_saturated(&txc->tx_evtq)) {
		TXC_DBG(txc, "TX HW EQ saturated\n");
		ret = -FI_EAGAIN;
		goto err_req_buf_fini;
	}

	/* Check if target peer is disabled */
	ret = cxip_send_req_queue(req->send.txc, req);
	if (ret != FI_SUCCESS) {
		TXC_DBG(txc, "Target peer disabled\n");
		goto err_req_buf_fini;
	}

	/* Try Send */
	ret = _cxip_send_req(req);
	if (ret != FI_SUCCESS)
		goto err_req_dequeue;

	ofi_genlock_unlock(&txc->ep_obj->lock);

	TXC_DBG(txc,
		"req: %p buf: %p len: %lu dest_addr: 0x%lX nic: %d pid: %d tag(%c): 0x%lx context %#lx\n",
		req, req->send.buf, req->send.len, dest_addr, caddr.nic,
		caddr.pid, req->send.tagged ? '*' : '-', req->send.tag,
		req->context);

	return FI_SUCCESS;

err_req_dequeue:
	cxip_send_req_dequeue(req->send.txc, req);
err_req_buf_fini:
	cxip_send_buf_fini(req);
err_req_free:
	ofi_atomic_dec32(&txc->otx_reqs);
	cxip_evtq_req_free(req);
unlock:
	ofi_genlock_unlock(&txc->ep_obj->lock);

	return ret;
}

/*
 * Libfabric APIs
 */
static ssize_t cxip_trecv(struct fid_ep *fid_ep, void *buf, size_t len,
			  void *desc, fi_addr_t src_addr, uint64_t tag,
			  uint64_t ignore, void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_recv_common(&ep->ep_obj->rxc, buf, len, desc, src_addr,
				tag, ignore, context, ep->rx_attr.op_flags,
				true, NULL);
}

static ssize_t cxip_trecvv(struct fid_ep *fid_ep, const struct iovec *iov,
			   void **desc, size_t count, fi_addr_t src_addr,
			   uint64_t tag, uint64_t ignore, void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);
	size_t len;
	void *buf;
	void *mr_desc;

	if (count == 0) {
		len = 0;
		buf = NULL;
		mr_desc = NULL;
	} else if (iov && count == 1) {
		len = iov[0].iov_len;
		buf = iov[0].iov_base;
		mr_desc = desc ? desc[0] : NULL;
	} else {
		RXC_WARN(&ep->ep_obj->rxc, "Invalid IOV\n");
		return -FI_EINVAL;
	}

	return cxip_recv_common(&ep->ep_obj->rxc, buf, len, mr_desc, src_addr,
				tag, ignore, context, ep->rx_attr.op_flags,
				true, NULL);
}

static ssize_t cxip_trecvmsg(struct fid_ep *fid_ep,
			     const struct fi_msg_tagged *msg, uint64_t flags)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);
	size_t len;
	void *buf;
	void *mr_desc;

	if (flags & ~(CXIP_RX_OP_FLAGS | CXIP_RX_IGNORE_OP_FLAGS |
		      FI_PEEK | FI_CLAIM))
		return -FI_EBADFLAGS;

	if (!msg) {
		RXC_WARN(&ep->ep_obj->rxc, "NULL msg not supported\n");
		return -FI_EINVAL;
	}

	/* If selective completion is not requested, always generate
	 * completions.
	 */
	if (!ep->ep_obj->rxc.selective_completion)
		flags |= FI_COMPLETION;

	if (!(flags & FI_PEEK)) {
		if (msg->iov_count == 0) {
			len = 0;
			buf = NULL;
			mr_desc = NULL;
		} else if (msg->msg_iov && msg->iov_count == 1) {
			len = msg->msg_iov[0].iov_len;
			buf = msg->msg_iov[0].iov_base;
			mr_desc = msg->desc ? msg->desc[0] : NULL;
		} else {
			RXC_WARN(&ep->ep_obj->rxc, "Invalid IOV\n");
			return -FI_EINVAL;
		}

		return cxip_recv_common(&ep->ep_obj->rxc, buf, len, mr_desc,
					msg->addr, msg->tag, msg->ignore,
					msg->context, flags, true, NULL);
	}

	/* FI_PEEK does not post a recv or return message payload */
	return cxip_recv_common(&ep->ep_obj->rxc, NULL, 0UL, NULL, msg->addr,
				msg->tag, msg->ignore, msg->context, flags,
				true, NULL);
}

static ssize_t cxip_tsend(struct fid_ep *fid_ep, const void *buf, size_t len,
			  void *desc, fi_addr_t dest_addr, uint64_t tag,
			  void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass, buf, len,
				desc, 0, dest_addr, tag, context,
				ep->tx_attr.op_flags, true, false, 0,
				NULL, NULL);
}

static ssize_t cxip_tsendv(struct fid_ep *fid_ep, const struct iovec *iov,
			   void **desc, size_t count, fi_addr_t dest_addr,
			   uint64_t tag, void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);
	size_t len;
	const void *buf;
	void *mr_desc;

	if (count == 0) {
		len = 0;
		buf = NULL;
		mr_desc = NULL;
	} else if (iov && count == 1) {
		len = iov[0].iov_len;
		buf = iov[0].iov_base;
		mr_desc = desc ? desc[0] : NULL;
	} else {
		TXC_WARN(&ep->ep_obj->txc, "Invalid IOV\n");
		return -FI_EINVAL;
	}

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass, buf, len,
				mr_desc, 0, dest_addr, tag, context,
				ep->tx_attr.op_flags, true, false, 0, NULL,
				NULL);
}

static ssize_t cxip_tsendmsg(struct fid_ep *fid_ep,
			     const struct fi_msg_tagged *msg, uint64_t flags)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);
	struct cxip_txc *txc = &ep->ep_obj->txc;
	size_t len;
	const void *buf;
	void *mr_desc;

	if (!msg) {
		TXC_WARN(&ep->ep_obj->txc, "NULL msg not supported\n");
		return -FI_EINVAL;
	}

	if (msg->iov_count == 0) {
		len = 0;
		buf = NULL;
		mr_desc = NULL;
	} else if (msg->msg_iov && msg->iov_count == 1) {
		len = msg->msg_iov[0].iov_len;
		buf = msg->msg_iov[0].iov_base;
		mr_desc = msg->desc ? msg->desc[0] : NULL;
	} else {
		TXC_WARN(&ep->ep_obj->txc, "Invalid IOV\n");
		return -FI_EINVAL;
	}

	if (flags & ~CXIP_TX_OP_FLAGS)
		return -FI_EBADFLAGS;

	if (flags & FI_FENCE && !(txc->attr.caps & FI_FENCE))
		return -FI_EINVAL;

	/* If selective completion is not requested, always generate
	 * completions.
	 */
	if (!txc->selective_completion)
		flags |= FI_COMPLETION;

	return cxip_send_common(txc, ep->tx_attr.tclass, buf, len, mr_desc,
				msg->data, msg->addr, msg->tag, msg->context,
				flags, true, false, 0, NULL, NULL);
}

static ssize_t cxip_tinject(struct fid_ep *fid_ep, const void *buf, size_t len,
			    fi_addr_t dest_addr, uint64_t tag)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass, buf, len,
				NULL, 0, dest_addr, tag, NULL, FI_INJECT,
				true, false, 0, NULL, NULL);
}

static ssize_t cxip_tsenddata(struct fid_ep *fid_ep, const void *buf,
			      size_t len, void *desc, uint64_t data,
			      fi_addr_t dest_addr, uint64_t tag, void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass, buf, len,
				desc, data, dest_addr, tag, context,
				ep->tx_attr.op_flags | FI_REMOTE_CQ_DATA,
				true, false, 0, NULL, NULL);
}

static ssize_t cxip_tinjectdata(struct fid_ep *fid_ep, const void *buf,
				size_t len, uint64_t data, fi_addr_t dest_addr,
				uint64_t tag)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass, buf,
				len, NULL, data, dest_addr, tag, NULL,
				FI_INJECT | FI_REMOTE_CQ_DATA,
				true, false, 0, NULL, NULL);
}

struct fi_ops_tagged cxip_ep_tagged_no_ops = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = fi_no_tagged_recv,
	.recvv = fi_no_tagged_recvv,
	.recvmsg = fi_no_tagged_recvmsg,
	.send = fi_no_tagged_send,
	.sendv = fi_no_tagged_sendv,
	.sendmsg = fi_no_tagged_sendmsg,
	.inject = fi_no_tagged_inject,
	.senddata = fi_no_tagged_senddata,
	.injectdata = fi_no_tagged_injectdata,
};

struct fi_ops_tagged cxip_ep_tagged_ops = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = cxip_trecv,
	.recvv = cxip_trecvv,
	.recvmsg = cxip_trecvmsg,
	.send = cxip_tsend,
	.sendv = cxip_tsendv,
	.sendmsg = cxip_tsendmsg,
	.inject = cxip_tinject,
	.senddata = cxip_tsenddata,
	.injectdata = cxip_tinjectdata,
};

struct fi_ops_tagged cxip_ep_tagged_no_tx_ops = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = cxip_trecv,
	.recvv = cxip_trecvv,
	.recvmsg = cxip_trecvmsg,
	.send = fi_no_tagged_send,
	.sendv = fi_no_tagged_sendv,
	.sendmsg = fi_no_tagged_sendmsg,
	.inject = fi_no_tagged_inject,
	.senddata = fi_no_tagged_senddata,
	.injectdata = fi_no_tagged_injectdata,
};

struct fi_ops_tagged cxip_ep_tagged_no_rx_ops = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = fi_no_tagged_recv,
	.recvv = fi_no_tagged_recvv,
	.recvmsg = fi_no_tagged_recvmsg,
	.send = cxip_tsend,
	.sendv = cxip_tsendv,
	.sendmsg = cxip_tsendmsg,
	.inject = cxip_tinject,
	.senddata = cxip_tsenddata,
	.injectdata = cxip_tinjectdata,
};

static ssize_t cxip_recv(struct fid_ep *fid_ep, void *buf, size_t len,
			 void *desc, fi_addr_t src_addr, void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_recv_common(&ep->ep_obj->rxc, buf, len, desc, src_addr, 0,
				0, context, ep->rx_attr.op_flags, false, NULL);
}

static ssize_t cxip_recvv(struct fid_ep *fid_ep, const struct iovec *iov,
			  void **desc, size_t count, fi_addr_t src_addr,
			  void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);
	size_t len;
	void *buf;
	void *mr_desc;

	if (count == 0) {
		len = 0;
		buf = NULL;
		mr_desc = NULL;
	} else if (iov && count == 1) {
		len = iov[0].iov_len;
		buf = iov[0].iov_base;
		mr_desc = desc ? desc[0] : NULL;
	} else {
		RXC_WARN(&ep->ep_obj->rxc, "Invalid IOV\n");
		return -FI_EINVAL;
	}

	return cxip_recv_common(&ep->ep_obj->rxc, buf, len, mr_desc, src_addr,
				0, 0, context, ep->rx_attr.op_flags, false,
				NULL);
}

static ssize_t cxip_recvmsg(struct fid_ep *fid_ep, const struct fi_msg *msg,
			    uint64_t flags)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);
	struct cxip_rxc *rxc = &ep->ep_obj->rxc;
	size_t len;
	void *buf;
	void *mr_desc;

	if (flags & ~(CXIP_RX_OP_FLAGS | CXIP_RX_IGNORE_OP_FLAGS))
		return -FI_EBADFLAGS;

	if (!msg) {
		RXC_WARN(rxc, "NULL msg not supported\n");
		return -FI_EINVAL;
	}

	if (msg->iov_count == 0) {
		len = 0;
		buf = NULL;
		mr_desc = NULL;
	} else if (msg->msg_iov && msg->iov_count == 1) {
		len = msg->msg_iov[0].iov_len;
		buf = msg->msg_iov[0].iov_base;
		mr_desc = msg->desc ? msg->desc[0] : NULL;
	} else {
		RXC_WARN(&ep->ep_obj->rxc, "Invalid IOV\n");
		return -FI_EINVAL;
	}

	/* If selective completion is not requested, always generate
	 * completions.
	 */
	if (!rxc->selective_completion)
		flags |= FI_COMPLETION;

	return cxip_recv_common(rxc, buf, len, mr_desc, msg->addr, 0, 0,
				msg->context, flags, false, NULL);
}

static ssize_t cxip_send(struct fid_ep *fid_ep, const void *buf, size_t len,
			 void *desc, fi_addr_t dest_addr, void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass,
				buf, len, desc, 0, dest_addr, 0, context,
				ep->tx_attr.op_flags, false, false, 0,
				NULL, NULL);
}

static ssize_t cxip_sendv(struct fid_ep *fid_ep, const struct iovec *iov,
			  void **desc, size_t count, fi_addr_t dest_addr,
			  void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);
	size_t len;
	const void *buf;
	void *mr_desc;

	if (count == 0) {
		len = 0;
		buf = NULL;
		mr_desc = NULL;
	} else if (iov && count == 1) {
		len = iov[0].iov_len;
		buf = iov[0].iov_base;
		mr_desc = desc ? desc[0] : NULL;
	} else {
		TXC_WARN(&ep->ep_obj->txc, "Invalid IOV\n");
		return -FI_EINVAL;
	}

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass,
				buf, len, mr_desc, 0, dest_addr, 0, context,
				ep->tx_attr.op_flags, false, false, 0,
				NULL, NULL);
}

static ssize_t cxip_sendmsg(struct fid_ep *fid_ep, const struct fi_msg *msg,
			    uint64_t flags)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);
	struct cxip_txc *txc = &ep->ep_obj->txc;
	size_t len;
	const void *buf;
	void *mr_desc;

	if (!msg) {
		TXC_WARN(&ep->ep_obj->txc, "NULL msg not supported\n");
		return -FI_EINVAL;
	}

	if (msg->iov_count == 0) {
		len = 0;
		buf = NULL;
		mr_desc = NULL;
	} else if (msg->msg_iov && msg->iov_count == 1) {
		len = msg->msg_iov[0].iov_len;
		buf = msg->msg_iov[0].iov_base;
		mr_desc = msg->desc ? msg->desc[0] : NULL;
	} else {
		TXC_WARN(&ep->ep_obj->txc, "Invalid IOV\n");
		return -FI_EINVAL;
	}

	if (flags & ~CXIP_TX_OP_FLAGS)
		return -FI_EBADFLAGS;

	if (flags & FI_FENCE && !(txc->attr.caps & FI_FENCE))
		return -FI_EINVAL;

	/* If selective completion is not requested, always generate
	 * completions.
	 */
	if (!txc->selective_completion)
		flags |= FI_COMPLETION;

	return cxip_send_common(txc, ep->tx_attr.tclass, buf, len, mr_desc,
				msg->data, msg->addr, 0, msg->context, flags,
				false, false, 0, NULL, NULL);
}

static ssize_t cxip_inject(struct fid_ep *fid_ep, const void *buf, size_t len,
			   fi_addr_t dest_addr)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass, buf,
				len, NULL, 0, dest_addr, 0, NULL, FI_INJECT,
				false, false, 0, NULL, NULL);
}

static ssize_t cxip_senddata(struct fid_ep *fid_ep, const void *buf, size_t len,
			     void *desc, uint64_t data, fi_addr_t dest_addr,
			     void *context)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass, buf,
				len, desc, data, dest_addr, 0, context,
				ep->tx_attr.op_flags | FI_REMOTE_CQ_DATA,
				false, false, 0, NULL, NULL);
}

static ssize_t cxip_injectdata(struct fid_ep *fid_ep, const void *buf,
			       size_t len, uint64_t data, fi_addr_t dest_addr)
{
	struct cxip_ep *ep = container_of(fid_ep, struct cxip_ep, ep);

	return cxip_send_common(&ep->ep_obj->txc, ep->tx_attr.tclass, buf,
				len, NULL, data, dest_addr, 0, NULL,
				FI_INJECT | FI_REMOTE_CQ_DATA,
				false, false, 0, NULL, NULL);
}

struct fi_ops_msg cxip_ep_msg_no_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = fi_no_msg_recv,
	.recvv = fi_no_msg_recvv,
	.recvmsg = fi_no_msg_recvmsg,
	.send = fi_no_msg_send,
	.sendv = fi_no_msg_sendv,
	.sendmsg = fi_no_msg_sendmsg,
	.inject = fi_no_msg_inject,
	.senddata = fi_no_msg_senddata,
	.injectdata = fi_no_msg_injectdata,
};

struct fi_ops_msg cxip_ep_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = cxip_recv,
	.recvv = cxip_recvv,
	.recvmsg = cxip_recvmsg,
	.send = cxip_send,
	.sendv = cxip_sendv,
	.sendmsg = cxip_sendmsg,
	.inject = cxip_inject,
	.senddata = cxip_senddata,
	.injectdata = cxip_injectdata,
};

struct fi_ops_msg cxip_ep_msg_no_tx_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = cxip_recv,
	.recvv = cxip_recvv,
	.recvmsg = cxip_recvmsg,
	.send = fi_no_msg_send,
	.sendv = fi_no_msg_sendv,
	.sendmsg = fi_no_msg_sendmsg,
	.inject = fi_no_msg_inject,
	.senddata = fi_no_msg_senddata,
	.injectdata = fi_no_msg_injectdata,
};

struct fi_ops_msg cxip_ep_msg_no_rx_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = fi_no_msg_recv,
	.recvv = fi_no_msg_recvv,
	.recvmsg = fi_no_msg_recvmsg,
	.send = cxip_send,
	.sendv = cxip_sendv,
	.sendmsg = cxip_sendmsg,
	.inject = cxip_inject,
	.senddata = cxip_senddata,
	.injectdata = cxip_injectdata,
};
