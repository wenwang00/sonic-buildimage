/*
 * Zebra dataplane plugin for Forwarding Plane Manager (FPM) using netlink.
 *
 * Copyright (C) 2019 Network Device Education Foundation, Inc. ("NetDEF")
 *                    Rafael Zalamena
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* Include this explicitly */
#endif

#include <arpa/inet.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>
#include <string.h>

#include "lib/zebra.h"
#include "lib/json.h"
#include "lib/libfrr.h"
#include "lib/frratomic.h"
#include "lib/command.h"
#include "lib/memory.h"
#include "lib/network.h"
#include "lib/ns.h"
#include "lib/frr_pthread.h"
#include "zebra/debug.h"
#include "zebra/interface.h"
#include "zebra/zebra_dplane.h"
#include "zebra/zebra_mpls.h"
#include "zebra/zebra_router.h"
#include "zebra/zebra_evpn.h"
#include "zebra/zebra_evpn_mac.h"
#include "zebra/zebra_vxlan_private.h"
#include "zebra/kernel_netlink.h"
#include "zebra/rt_netlink.h"
#include "zebra/debug.h"
#include "zebra/zebra_srv6.h"
#include "fpm/fpm.h"

#define SOUTHBOUND_DEFAULT_ADDR INADDR_LOOPBACK
#define SOUTHBOUND_DEFAULT_PORT 2620

/**
 * FPM header:
 * {
 *   version: 1 byte (always 1),
 *   type: 1 byte (1 for netlink, 2 protobuf),
 *   len: 2 bytes (network order),
 * }
 *
 * This header is used with any format to tell the users how many bytes to
 * expect.
 */
#define FPM_HEADER_SIZE 4

/**
 * Custom Netlink TLVs
*/

/* Custom Netlink message types */
enum custom_nlmsg_types {
	RTM_NEWSRV6LOCALSID		= 1000,
	RTM_DELSRV6LOCALSID		= 1001,
};

/* Custom Netlink attribute types */
enum custom_rtattr_encap {
	FPM_ROUTE_ENCAP_SRV6		= 101,
};

enum custom_rtattr_srv6_localsid {
	FPM_SRV6_LOCALSID_UNSPEC			= 0,
	FPM_SRV6_LOCALSID_SID_VALUE			= 1,
	FPM_SRV6_LOCALSID_FORMAT			= 2,
	FPM_SRV6_LOCALSID_ACTION			= 3,
	FPM_SRV6_LOCALSID_VRFNAME			= 4,
	FPM_SRV6_LOCALSID_NH6				= 5,
	FPM_SRV6_LOCALSID_NH4				= 6,
	FPM_SRV6_LOCALSID_IIF				= 7,
	FPM_SRV6_LOCALSID_OIF				= 8,
	FPM_SRV6_LOCALSID_BPF				= 9,
	FPM_SRV6_LOCALSID_SIDLIST			= 10,
	FPM_SRV6_LOCALSID_ENCAP_SRC_ADDR	= 11,
};

enum custom_rtattr_encap_srv6 {
	FPM_ROUTE_ENCAP_SRV6_ENCAP_UNSPEC		= 0,
	FPM_ROUTE_ENCAP_SRV6_VPN_SID			= 1,
	FPM_ROUTE_ENCAP_SRV6_ENCAP_SRC_ADDR		= 2,
};

enum custom_rtattr_srv6_localsid_format {
	FPM_SRV6_LOCALSID_FORMAT_UNSPEC			= 0,
	FPM_SRV6_LOCALSID_FORMAT_BLOCK_LEN		= 1,
	FPM_SRV6_LOCALSID_FORMAT_NODE_LEN		= 2,
	FPM_SRV6_LOCALSID_FORMAT_FUNC_LEN		= 3,
	FPM_SRV6_LOCALSID_FORMAT_ARG_LEN		= 4,
};

enum custom_rtattr_srv6_localsid_action {
	FPM_SRV6_LOCALSID_ACTION_UNSPEC				= 0,
	FPM_SRV6_LOCALSID_ACTION_END				= 1,
	FPM_SRV6_LOCALSID_ACTION_END_X				= 2,
	FPM_SRV6_LOCALSID_ACTION_END_T				= 3,
	FPM_SRV6_LOCALSID_ACTION_END_DX2			= 4,
	FPM_SRV6_LOCALSID_ACTION_END_DX6			= 5,
	FPM_SRV6_LOCALSID_ACTION_END_DX4			= 6,
	FPM_SRV6_LOCALSID_ACTION_END_DT6			= 7,
	FPM_SRV6_LOCALSID_ACTION_END_DT4			= 8,
	FPM_SRV6_LOCALSID_ACTION_END_DT46			= 9,
	FPM_SRV6_LOCALSID_ACTION_B6_ENCAPS			= 10,
	FPM_SRV6_LOCALSID_ACTION_B6_ENCAPS_RED		= 11,
	FPM_SRV6_LOCALSID_ACTION_B6_INSERT			= 12,
	FPM_SRV6_LOCALSID_ACTION_B6_INSERT_RED		= 13,
	FPM_SRV6_LOCALSID_ACTION_UN					= 14,
	FPM_SRV6_LOCALSID_ACTION_UA					= 15,
	FPM_SRV6_LOCALSID_ACTION_UDX2				= 16,
	FPM_SRV6_LOCALSID_ACTION_UDX6				= 17,
	FPM_SRV6_LOCALSID_ACTION_UDX4				= 18,
	FPM_SRV6_LOCALSID_ACTION_UDT6				= 19,
	FPM_SRV6_LOCALSID_ACTION_UDT4				= 20,
	FPM_SRV6_LOCALSID_ACTION_UDT46				= 21,
};

static const char *prov_name = "dplane_fpm_sonic";

struct fpm_nl_ctx {
	/* data plane connection. */
	int socket;
	bool disabled;
	bool connecting;
	bool use_nhg;
	struct sockaddr_storage addr;

	/* data plane buffers. */
	struct stream *ibuf;
	struct stream *obuf;
	pthread_mutex_t obuf_mutex;

	/*
	 * data plane context queue:
	 * When a FPM server connection becomes a bottleneck, we must keep the
	 * data plane contexts until we get a chance to process them.
	 */
	struct dplane_ctx_list_head ctxqueue;
	pthread_mutex_t ctxqueue_mutex;

	/* data plane events. */
	struct zebra_dplane_provider *prov;
	struct frr_pthread *fthread;
	struct thread *t_connect;
	struct thread *t_read;
	struct thread *t_write;
	struct thread *t_event;
	struct thread *t_nhg;
	struct thread *t_dequeue;

	/* zebra events. */
	struct thread *t_lspreset;
	struct thread *t_lspwalk;
	struct thread *t_nhgreset;
	struct thread *t_nhgwalk;
	struct thread *t_ribreset;
	struct thread *t_ribwalk;
	struct thread *t_rmacreset;
	struct thread *t_rmacwalk;

	/* Statistic counters. */
	struct {
		/* Amount of bytes read into ibuf. */
		_Atomic uint32_t bytes_read;
		/* Amount of bytes written from obuf. */
		_Atomic uint32_t bytes_sent;
		/* Output buffer current usage. */
		_Atomic uint32_t obuf_bytes;
		/* Output buffer peak usage. */
		_Atomic uint32_t obuf_peak;

		/* Amount of connection closes. */
		_Atomic uint32_t connection_closes;
		/* Amount of connection errors. */
		_Atomic uint32_t connection_errors;

		/* Amount of user configurations: FNE_RECONNECT. */
		_Atomic uint32_t user_configures;
		/* Amount of user disable requests: FNE_DISABLE. */
		_Atomic uint32_t user_disables;

		/* Amount of data plane context processed. */
		_Atomic uint32_t dplane_contexts;
		/* Amount of data plane contexts enqueued. */
		_Atomic uint32_t ctxqueue_len;
		/* Peak amount of data plane contexts enqueued. */
		_Atomic uint32_t ctxqueue_len_peak;

		/* Amount of buffer full events. */
		_Atomic uint32_t buffer_full;
	} counters;
} *gfnc;

enum fpm_nl_events {
	/* Ask for FPM to reconnect the external server. */
	FNE_RECONNECT,
	/* Disable FPM. */
	FNE_DISABLE,
	/* Reset counters. */
	FNE_RESET_COUNTERS,
	/* Toggle next hop group feature. */
	FNE_TOGGLE_NHG,
	/* Reconnect request by our own code to avoid races. */
	FNE_INTERNAL_RECONNECT,

	/* LSP walk finished. */
	FNE_LSP_FINISHED,
	/* Next hop groups walk finished. */
	FNE_NHG_FINISHED,
	/* RIB walk finished. */
	FNE_RIB_FINISHED,
	/* RMAC walk finished. */
	FNE_RMAC_FINISHED,
};

#define FPM_RECONNECT(fnc)                                                     \
	thread_add_event((fnc)->fthread->master, fpm_process_event, (fnc),     \
			 FNE_INTERNAL_RECONNECT, &(fnc)->t_event)

#define WALK_FINISH(fnc, ev)                                                   \
	thread_add_event((fnc)->fthread->master, fpm_process_event, (fnc),     \
			 (ev), NULL)

/*
 * Prototypes.
 */
static void fpm_process_event(struct thread *t);
static int fpm_nl_enqueue(struct fpm_nl_ctx *fnc, struct zebra_dplane_ctx *ctx);
static void fpm_lsp_send(struct thread *t);
static void fpm_lsp_reset(struct thread *t);
static void fpm_nhg_send(struct thread *t);
static void fpm_nhg_reset(struct thread *t);
static void fpm_rib_send(struct thread *t);
static void fpm_rib_reset(struct thread *t);
static void fpm_rmac_send(struct thread *t);
static void fpm_rmac_reset(struct thread *t);

/*
 * CLI.
 */
#define FPM_STR "Forwarding Plane Manager configuration\n"

DEFUN(fpm_set_address, fpm_set_address_cmd,
      "fpm address <A.B.C.D|X:X::X:X> [port (1-65535)]",
      FPM_STR
      "FPM remote listening server address\n"
      "Remote IPv4 FPM server\n"
      "Remote IPv6 FPM server\n"
      "FPM remote listening server port\n"
      "Remote FPM server port\n")
{
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;
	uint16_t port = 0;
	uint8_t naddr[INET6_BUFSIZ];

	if (argc == 5)
		port = strtol(argv[4]->arg, NULL, 10);

	/* Handle IPv4 addresses. */
	if (inet_pton(AF_INET, argv[2]->arg, naddr) == 1) {
		sin = (struct sockaddr_in *)&gfnc->addr;

		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		sin->sin_port =
			port ? htons(port) : htons(SOUTHBOUND_DEFAULT_PORT);
#ifdef HAVE_STRUCT_SOCKADDR_SA_LEN
		sin->sin_len = sizeof(*sin);
#endif /* HAVE_STRUCT_SOCKADDR_SA_LEN */
		memcpy(&sin->sin_addr, naddr, sizeof(sin->sin_addr));

		goto ask_reconnect;
	}

	/* Handle IPv6 addresses. */
	if (inet_pton(AF_INET6, argv[2]->arg, naddr) != 1) {
		vty_out(vty, "%% Invalid address: %s\n", argv[2]->arg);
		return CMD_WARNING;
	}

	sin6 = (struct sockaddr_in6 *)&gfnc->addr;
	memset(sin6, 0, sizeof(*sin6));
	sin6->sin6_family = AF_INET6;
	sin6->sin6_port = port ? htons(port) : htons(SOUTHBOUND_DEFAULT_PORT);
#ifdef HAVE_STRUCT_SOCKADDR_SA_LEN
	sin6->sin6_len = sizeof(*sin6);
#endif /* HAVE_STRUCT_SOCKADDR_SA_LEN */
	memcpy(&sin6->sin6_addr, naddr, sizeof(sin6->sin6_addr));

ask_reconnect:
	thread_add_event(gfnc->fthread->master, fpm_process_event, gfnc,
			 FNE_RECONNECT, &gfnc->t_event);
	return CMD_SUCCESS;
}

DEFUN(no_fpm_set_address, no_fpm_set_address_cmd,
      "no fpm address [<A.B.C.D|X:X::X:X> [port <1-65535>]]",
      NO_STR
      FPM_STR
      "FPM remote listening server address\n"
      "Remote IPv4 FPM server\n"
      "Remote IPv6 FPM server\n"
      "FPM remote listening server port\n"
      "Remote FPM server port\n")
{
	thread_add_event(gfnc->fthread->master, fpm_process_event, gfnc,
			 FNE_DISABLE, &gfnc->t_event);
	return CMD_SUCCESS;
}

DEFUN(fpm_use_nhg, fpm_use_nhg_cmd,
      "fpm use-next-hop-groups",
      FPM_STR
      "Use netlink next hop groups feature.\n")
{
	/* Already enabled. */
	if (gfnc->use_nhg)
		return CMD_SUCCESS;

	thread_add_event(gfnc->fthread->master, fpm_process_event, gfnc,
			 FNE_TOGGLE_NHG, &gfnc->t_nhg);

	return CMD_SUCCESS;
}

DEFUN(no_fpm_use_nhg, no_fpm_use_nhg_cmd,
      "no fpm use-next-hop-groups",
      NO_STR
      FPM_STR
      "Use netlink next hop groups feature.\n")
{
	/* Already disabled. */
	if (!gfnc->use_nhg)
		return CMD_SUCCESS;

	thread_add_event(gfnc->fthread->master, fpm_process_event, gfnc,
			 FNE_TOGGLE_NHG, &gfnc->t_nhg);

	return CMD_SUCCESS;
}

DEFUN(fpm_reset_counters, fpm_reset_counters_cmd,
      "clear fpm counters",
      CLEAR_STR
      FPM_STR
      "FPM statistic counters\n")
{
	thread_add_event(gfnc->fthread->master, fpm_process_event, gfnc,
			 FNE_RESET_COUNTERS, &gfnc->t_event);
	return CMD_SUCCESS;
}

DEFUN(fpm_show_counters, fpm_show_counters_cmd,
      "show fpm counters",
      SHOW_STR
      FPM_STR
      "FPM statistic counters\n")
{
	vty_out(vty, "%30s\n%30s\n", "FPM counters", "============");

#define SHOW_COUNTER(label, counter) \
	vty_out(vty, "%28s: %u\n", (label), (counter))

	SHOW_COUNTER("Input bytes", gfnc->counters.bytes_read);
	SHOW_COUNTER("Output bytes", gfnc->counters.bytes_sent);
	SHOW_COUNTER("Output buffer current size", gfnc->counters.obuf_bytes);
	SHOW_COUNTER("Output buffer peak size", gfnc->counters.obuf_peak);
	SHOW_COUNTER("Connection closes", gfnc->counters.connection_closes);
	SHOW_COUNTER("Connection errors", gfnc->counters.connection_errors);
	SHOW_COUNTER("Data plane items processed",
		     gfnc->counters.dplane_contexts);
	SHOW_COUNTER("Data plane items enqueued",
		     gfnc->counters.ctxqueue_len);
	SHOW_COUNTER("Data plane items queue peak",
		     gfnc->counters.ctxqueue_len_peak);
	SHOW_COUNTER("Buffer full hits", gfnc->counters.buffer_full);
	SHOW_COUNTER("User FPM configurations", gfnc->counters.user_configures);
	SHOW_COUNTER("User FPM disable requests", gfnc->counters.user_disables);

#undef SHOW_COUNTER

	return CMD_SUCCESS;
}

DEFUN(fpm_show_counters_json, fpm_show_counters_json_cmd,
      "show fpm counters json",
      SHOW_STR
      FPM_STR
      "FPM statistic counters\n"
      JSON_STR)
{
	struct json_object *jo;

	jo = json_object_new_object();
	json_object_int_add(jo, "bytes-read", gfnc->counters.bytes_read);
	json_object_int_add(jo, "bytes-sent", gfnc->counters.bytes_sent);
	json_object_int_add(jo, "obuf-bytes", gfnc->counters.obuf_bytes);
	json_object_int_add(jo, "obuf-bytes-peak", gfnc->counters.obuf_peak);
	json_object_int_add(jo, "connection-closes",
			    gfnc->counters.connection_closes);
	json_object_int_add(jo, "connection-errors",
			    gfnc->counters.connection_errors);
	json_object_int_add(jo, "data-plane-contexts",
			    gfnc->counters.dplane_contexts);
	json_object_int_add(jo, "data-plane-contexts-queue",
			    gfnc->counters.ctxqueue_len);
	json_object_int_add(jo, "data-plane-contexts-queue-peak",
			    gfnc->counters.ctxqueue_len_peak);
	json_object_int_add(jo, "buffer-full-hits", gfnc->counters.buffer_full);
	json_object_int_add(jo, "user-configures",
			    gfnc->counters.user_configures);
	json_object_int_add(jo, "user-disables", gfnc->counters.user_disables);
	vty_json(vty, jo);

	return CMD_SUCCESS;
}

static int fpm_write_config(struct vty *vty)
{
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;
	int written = 0;

	if (gfnc->disabled)
		return written;

	switch (gfnc->addr.ss_family) {
	case AF_INET:
		written = 1;
		sin = (struct sockaddr_in *)&gfnc->addr;
		vty_out(vty, "fpm address %pI4", &sin->sin_addr);
		if (sin->sin_port != htons(SOUTHBOUND_DEFAULT_PORT))
			vty_out(vty, " port %d", ntohs(sin->sin_port));

		vty_out(vty, "\n");
		break;
	case AF_INET6:
		written = 1;
		sin6 = (struct sockaddr_in6 *)&gfnc->addr;
		vty_out(vty, "fpm address %pI6", &sin6->sin6_addr);
		if (sin6->sin6_port != htons(SOUTHBOUND_DEFAULT_PORT))
			vty_out(vty, " port %d", ntohs(sin6->sin6_port));

		vty_out(vty, "\n");
		break;

	default:
		break;
	}

	if (!gfnc->use_nhg) {
		vty_out(vty, "no fpm use-next-hop-groups\n");
		written = 1;
	}

	return written;
}

static struct cmd_node fpm_node = {
	.name = "fpm",
	.node = FPM_NODE,
	.prompt = "",
	.config_write = fpm_write_config,
};

/*
 * FPM functions.
 */
static void fpm_connect(struct thread *t);

static void fpm_reconnect(struct fpm_nl_ctx *fnc)
{
	/* Cancel all zebra threads first. */
	thread_cancel_async(zrouter.master, &fnc->t_lspreset, NULL);
	thread_cancel_async(zrouter.master, &fnc->t_lspwalk, NULL);
	thread_cancel_async(zrouter.master, &fnc->t_nhgreset, NULL);
	thread_cancel_async(zrouter.master, &fnc->t_nhgwalk, NULL);
	thread_cancel_async(zrouter.master, &fnc->t_ribreset, NULL);
	thread_cancel_async(zrouter.master, &fnc->t_ribwalk, NULL);
	thread_cancel_async(zrouter.master, &fnc->t_rmacreset, NULL);
	thread_cancel_async(zrouter.master, &fnc->t_rmacwalk, NULL);

	/*
	 * Grab the lock to empty the streams (data plane might try to
	 * enqueue updates while we are closing).
	 */
	frr_mutex_lock_autounlock(&fnc->obuf_mutex);

	/* Avoid calling close on `-1`. */
	if (fnc->socket != -1) {
		close(fnc->socket);
		fnc->socket = -1;
	}

	stream_reset(fnc->ibuf);
	stream_reset(fnc->obuf);
	THREAD_OFF(fnc->t_read);
	THREAD_OFF(fnc->t_write);

	/* FPM is disabled, don't attempt to connect. */
	if (fnc->disabled)
		return;

	thread_add_timer(fnc->fthread->master, fpm_connect, fnc, 3,
			 &fnc->t_connect);
}

static void fpm_read(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	fpm_msg_hdr_t fpm;
	ssize_t rv;
	char buf[65535];
	struct nlmsghdr *hdr;
	struct zebra_dplane_ctx *ctx;
	size_t available_bytes;
	size_t hdr_available_bytes;

	/* Let's ignore the input at the moment. */
	rv = stream_read_try(fnc->ibuf, fnc->socket,
			     STREAM_WRITEABLE(fnc->ibuf));
	if (rv == 0) {
		atomic_fetch_add_explicit(&fnc->counters.connection_closes, 1,
					  memory_order_relaxed);

		if (IS_ZEBRA_DEBUG_FPM)
			zlog_debug("%s: connection closed", __func__);

		FPM_RECONNECT(fnc);
		return;
	}
	if (rv == -1) {
		atomic_fetch_add_explicit(&fnc->counters.connection_errors, 1,
					  memory_order_relaxed);
		zlog_warn("%s: connection failure: %s", __func__,
			  strerror(errno));
		FPM_RECONNECT(fnc);
		return;
	}

	/* Schedule the next read */
	thread_add_read(fnc->fthread->master, fpm_read, fnc, fnc->socket,
			&fnc->t_read);

	/* We've got an interruption. */
	if (rv == -2)
		return;


	/* Account all bytes read. */
	atomic_fetch_add_explicit(&fnc->counters.bytes_read, rv,
				  memory_order_relaxed);

	available_bytes = STREAM_READABLE(fnc->ibuf);
	while (available_bytes) {
		if (available_bytes < (ssize_t)FPM_MSG_HDR_LEN) {
			stream_pulldown(fnc->ibuf);
			return;
		}

		fpm.version = stream_getc(fnc->ibuf);
		fpm.msg_type = stream_getc(fnc->ibuf);
		fpm.msg_len = stream_getw(fnc->ibuf);

		if (fpm.version != FPM_PROTO_VERSION &&
		    fpm.msg_type != FPM_MSG_TYPE_NETLINK) {
			stream_reset(fnc->ibuf);
			zlog_warn(
				"%s: Received version/msg_type %u/%u, expected 1/1",
				__func__, fpm.version, fpm.msg_type);

			FPM_RECONNECT(fnc);
			return;
		}

		/*
		 * If the passed in length doesn't even fill in the header
		 * something is wrong and reset.
		 */
		if (fpm.msg_len < FPM_MSG_HDR_LEN) {
			zlog_warn(
				"%s: Received message length: %u that does not even fill the FPM header",
				__func__, fpm.msg_len);
			FPM_RECONNECT(fnc);
			return;
		}

		/*
		 * If we have not received the whole payload, reset the stream
		 * back to the beginning of the header and move it to the
		 * top.
		 */
		if (fpm.msg_len > available_bytes) {
			stream_rewind_getp(fnc->ibuf, FPM_MSG_HDR_LEN);
			stream_pulldown(fnc->ibuf);
			return;
		}

		available_bytes -= FPM_MSG_HDR_LEN;

		/*
		 * Place the data from the stream into a buffer
		 */
		hdr = (struct nlmsghdr *)buf;
		stream_get(buf, fnc->ibuf, fpm.msg_len - FPM_MSG_HDR_LEN);
		hdr_available_bytes = fpm.msg_len - FPM_MSG_HDR_LEN;
		available_bytes -= hdr_available_bytes;

		/* Sanity check: must be at least header size. */
		if (hdr->nlmsg_len < sizeof(*hdr)) {
			zlog_warn(
				"%s: [seq=%u] invalid message length %u (< %zu)",
				__func__, hdr->nlmsg_seq, hdr->nlmsg_len,
				sizeof(*hdr));
			continue;
		}
		if (hdr->nlmsg_len > fpm.msg_len) {
			zlog_warn(
				"%s: Received a inner header length of %u that is greater than the fpm total length of %u",
				__func__, hdr->nlmsg_len, fpm.msg_len);
			FPM_RECONNECT(fnc);
		}
		/* Not enough bytes available. */
		if (hdr->nlmsg_len > hdr_available_bytes) {
			zlog_warn(
				"%s: [seq=%u] invalid message length %u (> %zu)",
				__func__, hdr->nlmsg_seq, hdr->nlmsg_len,
				available_bytes);
			continue;
		}

		if (!(hdr->nlmsg_flags & NLM_F_REQUEST)) {
			if (IS_ZEBRA_DEBUG_FPM)
				zlog_debug(
					"%s: [seq=%u] not a request, skipping",
					__func__, hdr->nlmsg_seq);

			/*
			 * This request is a bust, go to the next one
			 */
			continue;
		}

		switch (hdr->nlmsg_type) {
		case RTM_NEWROUTE:
			ctx = dplane_ctx_alloc();
			dplane_ctx_route_init(ctx, DPLANE_OP_ROUTE_NOTIFY, NULL,
 					      NULL);
			if (netlink_route_change_read_unicast_internal(
				    hdr, 0, false, ctx) != 1) {
				dplane_ctx_fini(&ctx);
				stream_pulldown(fnc->ibuf);
				/*
				 * Let's continue to read other messages
				 * Even if we ignore this one.
				 */
			}
			break;
		default:
			if (IS_ZEBRA_DEBUG_FPM)
				zlog_debug(
					"%s: Received message type %u which is not currently handled",
					__func__, hdr->nlmsg_type);
			break;
		}
	}

	stream_reset(fnc->ibuf);
}

static void fpm_write(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	socklen_t statuslen;
	ssize_t bwritten;
	int rv, status;
	size_t btotal;

	if (fnc->connecting == true) {
		status = 0;
		statuslen = sizeof(status);

		rv = getsockopt(fnc->socket, SOL_SOCKET, SO_ERROR, &status,
				&statuslen);
		if (rv == -1 || status != 0) {
			if (rv != -1)
				zlog_warn("%s: connection failed: %s", __func__,
					  strerror(status));
			else
				zlog_warn("%s: SO_ERROR failed: %s", __func__,
					  strerror(status));

			atomic_fetch_add_explicit(
				&fnc->counters.connection_errors, 1,
				memory_order_relaxed);

			FPM_RECONNECT(fnc);
			return;
		}

		fnc->connecting = false;

		/*
		 * Starting with LSPs walk all FPM objects, marking them
		 * as unsent and then replaying them.
		 */
		thread_add_timer(zrouter.master, fpm_lsp_reset, fnc, 0,
				 &fnc->t_lspreset);

		/* Permit receiving messages now. */
		thread_add_read(fnc->fthread->master, fpm_read, fnc,
				fnc->socket, &fnc->t_read);
	}

	frr_mutex_lock_autounlock(&fnc->obuf_mutex);

	while (true) {
		/* Stream is empty: reset pointers and return. */
		if (STREAM_READABLE(fnc->obuf) == 0) {
			stream_reset(fnc->obuf);
			break;
		}

		/* Try to write all at once. */
		btotal = stream_get_endp(fnc->obuf) -
			stream_get_getp(fnc->obuf);
		bwritten = write(fnc->socket, stream_pnt(fnc->obuf), btotal);
		if (bwritten == 0) {
			atomic_fetch_add_explicit(
				&fnc->counters.connection_closes, 1,
				memory_order_relaxed);

			if (IS_ZEBRA_DEBUG_FPM)
				zlog_debug("%s: connection closed", __func__);
			break;
		}
		if (bwritten == -1) {
			/* Attempt to continue if blocked by a signal. */
			if (errno == EINTR)
				continue;
			/* Receiver is probably slow, lets give it some time. */
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			atomic_fetch_add_explicit(
				&fnc->counters.connection_errors, 1,
				memory_order_relaxed);
			zlog_warn("%s: connection failure: %s", __func__,
				  strerror(errno));

			FPM_RECONNECT(fnc);
			return;
		}

		/* Account all bytes sent. */
		atomic_fetch_add_explicit(&fnc->counters.bytes_sent, bwritten,
					  memory_order_relaxed);

		/* Account number of bytes free. */
		atomic_fetch_sub_explicit(&fnc->counters.obuf_bytes, bwritten,
					  memory_order_relaxed);

		stream_forward_getp(fnc->obuf, (size_t)bwritten);
	}

	/* Stream is not empty yet, we must schedule more writes. */
	if (STREAM_READABLE(fnc->obuf)) {
		stream_pulldown(fnc->obuf);
		thread_add_write(fnc->fthread->master, fpm_write, fnc,
				 fnc->socket, &fnc->t_write);
		return;
	}
}

static void fpm_connect(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	struct sockaddr_in *sin = (struct sockaddr_in *)&fnc->addr;
	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&fnc->addr;
	socklen_t slen;
	int rv, sock;
	char addrstr[INET6_ADDRSTRLEN];

	sock = socket(fnc->addr.ss_family, SOCK_STREAM, 0);
	if (sock == -1) {
		zlog_err("%s: fpm socket failed: %s", __func__,
			 strerror(errno));
		thread_add_timer(fnc->fthread->master, fpm_connect, fnc, 3,
				 &fnc->t_connect);
		return;
	}

	set_nonblocking(sock);

	if (fnc->addr.ss_family == AF_INET) {
		inet_ntop(AF_INET, &sin->sin_addr, addrstr, sizeof(addrstr));
		slen = sizeof(*sin);
	} else {
		inet_ntop(AF_INET6, &sin6->sin6_addr, addrstr, sizeof(addrstr));
		slen = sizeof(*sin6);
	}

	if (IS_ZEBRA_DEBUG_FPM)
		zlog_debug("%s: attempting to connect to %s:%d", __func__,
			   addrstr, ntohs(sin->sin_port));

	rv = connect(sock, (struct sockaddr *)&fnc->addr, slen);
	if (rv == -1 && errno != EINPROGRESS) {
		atomic_fetch_add_explicit(&fnc->counters.connection_errors, 1,
					  memory_order_relaxed);
		close(sock);
		zlog_warn("%s: fpm connection failed: %s", __func__,
			  strerror(errno));
		thread_add_timer(fnc->fthread->master, fpm_connect, fnc, 3,
				 &fnc->t_connect);
		return;
	}

	fnc->connecting = (errno == EINPROGRESS);
	fnc->socket = sock;
	if (!fnc->connecting)
		thread_add_read(fnc->fthread->master, fpm_read, fnc, sock,
				&fnc->t_read);
	thread_add_write(fnc->fthread->master, fpm_write, fnc, sock,
			 &fnc->t_write);

	/*
	 * Starting with LSPs walk all FPM objects, marking them
	 * as unsent and then replaying them.
	 *
	 * If we are not connected, then delay the objects reset/send.
	 */
	if (!fnc->connecting)
		thread_add_timer(zrouter.master, fpm_lsp_reset, fnc, 0,
				 &fnc->t_lspreset);
}

static struct zebra_vrf *vrf_lookup_by_table_id(uint32_t table_id)
{
 	struct vrf *vrf;
 	struct zebra_vrf *zvrf;

 	RB_FOREACH (vrf, vrf_id_head, &vrfs_by_id) {
 		zvrf = vrf->info;
 		if (zvrf == NULL)
 			continue;
 		/* case vrf with netns : match the netnsid */
 		if (vrf_is_backend_netns()) {
 			return NULL;
 		} else {
 			/* VRF is VRF_BACKEND_VRF_LITE */
 			if (zvrf->table_id != table_id)
 				continue;
 			return zvrf;
 		}
 	}

 	return NULL;
 }

/**
 * Resets the SRv6 routes FPM flags so we send all SRv6 routes again.
 */
static void fpm_srv6_route_reset(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	rib_dest_t *dest;
	struct route_node *rn;
	struct route_entry *re;
	struct route_table *rt;
	struct nexthop *nexthop;
	rib_tables_iter_t rt_iter;

	rt_iter.state = RIB_TABLES_ITER_S_INIT;
	while ((rt = rib_tables_iter_next(&rt_iter))) {
		for (rn = route_top(rt); rn; rn = srcdest_route_next(rn)) {
			dest = rib_dest_from_rnode(rn);
			/* Skip bad route entries. */
			if (dest == NULL)
				continue;

			re = dest->selected_fib;
			if (re == NULL)
				continue;

			nexthop = re->nhe->nhg.nexthop;
			if (nexthop && nexthop->nh_srv6 &&
					!sid_zero(&nexthop->nh_srv6->seg6_segs))
				/* Unset FPM installation flag so it gets installed again. */
				UNSET_FLAG(dest->flags, RIB_DEST_UPDATE_FPM);
		}
	}

	/* Schedule next step: send RIB routes. */
	thread_add_event(zrouter.master, fpm_rib_send, fnc, 0, &fnc->t_ribwalk);
}

/*
 * SRv6 localsid change via netlink interface, using a dataplane context object
 *
 * Returns -1 on failure, 0 when the msg doesn't fit entirely in the buffer
 * otherwise the number of bytes written to buf.
 */
static ssize_t netlink_srv6_localsid_msg_encode(int cmd,
					   struct zebra_dplane_ctx *ctx,
					   uint8_t *data, size_t datalen,
					   bool fpm, bool force_nhg)
{
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct zebra_vrf *zvrf;
	struct srv6_locator *l, *locator = NULL;
	struct listnode *node;
	struct rtattr *nest;
	const struct seg6local_context *seg6local_ctx;
	struct nexthop *nexthop;
	const struct prefix *p;
	struct nlsock *nl;
	int bytelen;
	vrf_id_t vrf_id;
	uint32_t table_id;
	uint32_t action;

	struct {
		struct nlmsghdr n;
		struct rtmsg r;
		char buf[];
	} *req = (void *)data;

	nexthop = dplane_ctx_get_ng(ctx)->nexthop;
	if (!nexthop || !nexthop->nh_srv6 || nexthop->nh_srv6->seg6local_action == ZEBRA_SEG6_LOCAL_ACTION_UNSPEC)
		return -1;

	p = dplane_ctx_get_dest(ctx);

	if (datalen < sizeof(*req))
		return 0;

	nl = kernel_netlink_nlsock_lookup(dplane_ctx_get_ns_sock(ctx));

	memset(req, 0, sizeof(*req));

	if (p->family != AF_INET6) {
		zlog_err("%s: invalid family: expected %u, got %u", __func__, AF_INET6, p->family);
		return -1;
	}

	bytelen = IPV6_MAX_BYTELEN;

	req->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req->n.nlmsg_flags = NLM_F_CREATE | NLM_F_REQUEST;

	if ((cmd == RTM_NEWSRV6LOCALSID) &&
		(v6_rr_semantics))
		req->n.nlmsg_flags |= NLM_F_REPLACE;

	req->n.nlmsg_type = cmd;

	req->n.nlmsg_pid = nl->snl.nl_pid;

	req->r.rtm_family = p->family;
	req->r.rtm_dst_len = p->prefixlen;
	req->r.rtm_scope = RT_SCOPE_UNIVERSE;

	if (cmd == RTM_DELSRV6LOCALSID)
		req->r.rtm_protocol = zebra2proto(dplane_ctx_get_old_type(ctx));
	else
		req->r.rtm_protocol = zebra2proto(dplane_ctx_get_type(ctx));

	if (!nl_attr_put(&req->n, datalen, FPM_SRV6_LOCALSID_SID_VALUE, &p->u.prefix, bytelen))
		return 0;

	/* Table corresponding to this route. */
	table_id = dplane_ctx_get_table(ctx);
	if (!fpm) {
		if (table_id < 256)
			req->r.rtm_table = table_id;
		else {
			req->r.rtm_table = RT_TABLE_UNSPEC;
			if (!nl_attr_put32(&req->n, datalen, RTA_TABLE, table_id))
				return 0;
		}
	} else {
		/* Put vrf if_index instead of table id */
		vrf_id = dplane_ctx_get_vrf(ctx);
		if (vrf_id < 256)
			req->r.rtm_table = vrf_id;
		else {
			req->r.rtm_table = RT_TABLE_UNSPEC;
			if (!nl_attr_put32(&req->n, datalen, RTA_TABLE, vrf_id))
				return 0;
		}
	}

	if (IS_ZEBRA_DEBUG_FPM)
		zlog_debug(
			"%s: %s %pFX vrf %u(%u)", __func__,
			(cmd == RTM_NEWSRV6LOCALSID) ? "RTM_NEWSRV6LOCALSID" : "RTM_DELSRV6LOCALSID", p, dplane_ctx_get_vrf(ctx),
			table_id);

	for (ALL_LIST_ELEMENTS_RO(srv6->locators, node, l)) {
		if (prefix_match(&l->prefix, p)) {
			locator = l;
			break;
		}
	}

	if (locator) {
		nest =
			nl_attr_nest(&req->n, datalen, 
						FPM_SRV6_LOCALSID_FORMAT);

		if (locator->block_bits_length)
			if (!nl_attr_put8(
					&req->n, datalen, 
					FPM_SRV6_LOCALSID_FORMAT_BLOCK_LEN,
					locator->block_bits_length))
				return -1;

		if (locator->node_bits_length)
			if (!nl_attr_put8(
					&req->n, datalen, 
					FPM_SRV6_LOCALSID_FORMAT_NODE_LEN,
					locator->node_bits_length))
				return -1;

		if (locator->function_bits_length)
			if (!nl_attr_put8(
					&req->n, datalen, 
					FPM_SRV6_LOCALSID_FORMAT_FUNC_LEN,
					locator->function_bits_length))
				return -1;

		if (locator->argument_bits_length)
			if (!nl_attr_put8(
					&req->n, datalen, 
					FPM_SRV6_LOCALSID_FORMAT_ARG_LEN,
					locator->argument_bits_length))
				return -1;

		nl_attr_nest_end(&req->n, nest);
	}

	if (cmd == RTM_DELSRV6LOCALSID)
		return NLMSG_ALIGN(req->n.nlmsg_len);

	seg6local_ctx = &nexthop->nh_srv6->seg6local_ctx;

	switch (nexthop->nh_srv6->seg6local_action) {
	case ZEBRA_SEG6_LOCAL_ACTION_END:
		action = (locator && CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID)) ? FPM_SRV6_LOCALSID_ACTION_UN : FPM_SRV6_LOCALSID_ACTION_END;
		if (!nl_attr_put32(&req->n, datalen, 
					FPM_SRV6_LOCALSID_ACTION,
					action))
			return -1;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_X:
		action = (locator && CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID)) ? FPM_SRV6_LOCALSID_ACTION_UA : FPM_SRV6_LOCALSID_ACTION_END_X;
		if (!nl_attr_put32(&req->n, datalen, 
					FPM_SRV6_LOCALSID_ACTION,
					action))
			return -1;
		if (!nl_attr_put(&req->n, datalen, 
					FPM_SRV6_LOCALSID_NH6, &seg6local_ctx->nh6,
					sizeof(struct in6_addr)))
			return -1;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_T:
		zvrf = vrf_lookup_by_table_id(seg6local_ctx->table);
		if (!zvrf)
			return false;

		if (!nl_attr_put32(&req->n, datalen, 
					FPM_SRV6_LOCALSID_ACTION,
					FPM_SRV6_LOCALSID_ACTION_END_T))
			return -1;
		if (!nl_attr_put(&req->n, datalen, 
					FPM_SRV6_LOCALSID_VRFNAME,
					zvrf->vrf->name,
					strlen(zvrf->vrf->name) + 1))
			return -1;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_DX6:
		action = (locator && CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID)) ? FPM_SRV6_LOCALSID_ACTION_UDX6 : FPM_SRV6_LOCALSID_ACTION_END_DX6;
		if (!nl_attr_put32(&req->n, datalen, 
					FPM_SRV6_LOCALSID_ACTION,
					action))
			return -1;
		if (!nl_attr_put(&req->n, datalen, 
					FPM_SRV6_LOCALSID_NH6, &seg6local_ctx->nh6,
					sizeof(struct in6_addr)))
			return -1;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_DX4:
		action = (locator && CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID)) ? FPM_SRV6_LOCALSID_ACTION_UDX4 : FPM_SRV6_LOCALSID_ACTION_END_DX4;
		if (!nl_attr_put32(&req->n, datalen, 
					FPM_SRV6_LOCALSID_ACTION,
					action))
			return -1;
		if (!nl_attr_put(&req->n, datalen, 
					FPM_SRV6_LOCALSID_NH4, &seg6local_ctx->nh4,
					sizeof(struct in_addr)))
			return -1;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_DT6:
		zvrf = vrf_lookup_by_table_id(seg6local_ctx->table);
		if (!zvrf)
			return false;

		action = (locator && CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID)) ? FPM_SRV6_LOCALSID_ACTION_UDT6 : FPM_SRV6_LOCALSID_ACTION_END_DT6;
		if (!nl_attr_put32(&req->n, datalen, 
					FPM_SRV6_LOCALSID_ACTION,
					action))
			return -1;
		if (!nl_attr_put(&req->n, datalen, 
					FPM_SRV6_LOCALSID_VRFNAME,
					zvrf->vrf->name,
					strlen(zvrf->vrf->name) + 1))
			return -1;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_DT4:
		zvrf = vrf_lookup_by_table_id(seg6local_ctx->table);
		if (!zvrf)
			return false;

		action = (locator && CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID)) ? FPM_SRV6_LOCALSID_ACTION_UDT4 : FPM_SRV6_LOCALSID_ACTION_END_DT4;
		if (!nl_attr_put32(&req->n, datalen, 
					FPM_SRV6_LOCALSID_ACTION,
					action))
			return -1;
		if (!nl_attr_put(&req->n, datalen, 
					FPM_SRV6_LOCALSID_VRFNAME,
					zvrf->vrf->name,
					strlen(zvrf->vrf->name) + 1))
			return -1;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_DT46:
		zvrf = vrf_lookup_by_table_id(seg6local_ctx->table);
		if (!zvrf)
			return false;

		action = (locator && CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID)) ? FPM_SRV6_LOCALSID_ACTION_UDT46 : FPM_SRV6_LOCALSID_ACTION_END_DT46;
		if (!nl_attr_put32(&req->n, datalen, 
					FPM_SRV6_LOCALSID_ACTION,
					action))
			return -1;
		if (!nl_attr_put(&req->n, datalen, 
					FPM_SRV6_LOCALSID_VRFNAME,
					zvrf->vrf->name,
					strlen(zvrf->vrf->name) + 1))
			return -1;
		break;
	default:
		zlog_err("%s: unsupport seg6local behaviour action=%u",
				__func__,
				nexthop->nh_srv6->seg6local_action);
		return -1;
	}

	return NLMSG_ALIGN(req->n.nlmsg_len);
}

/*
 * SRv6 VPN route change via netlink interface, using a dataplane context object
 *
 * Returns -1 on failure, 0 when the msg doesn't fit entirely in the buffer
 * otherwise the number of bytes written to buf.
 */
static ssize_t netlink_srv6_vpn_route_msg_encode(int cmd,
					   struct zebra_dplane_ctx *ctx,
					   uint8_t *data, size_t datalen,
					   bool fpm, bool force_nhg)
{
	struct rtattr *nest;
	struct nexthop *nexthop;
	const struct prefix *p;
	struct nlsock *nl;
	int bytelen;
	vrf_id_t vrf_id;
	uint32_t table_id;
	struct interface *ifp;
	struct in6_addr encap_src_addr = {};
	struct listnode *node;
	struct connected *connected;

	struct {
		struct nlmsghdr n;
		struct rtmsg r;
		char buf[];
	} *req = (void *)data;

	nexthop = dplane_ctx_get_ng(ctx)->nexthop;
	if (!nexthop || !nexthop->nh_srv6 || sid_zero(&nexthop->nh_srv6->seg6_segs))
		return -1;

	p = dplane_ctx_get_dest(ctx);

	if (datalen < sizeof(*req))
		return 0;

	nl = kernel_netlink_nlsock_lookup(dplane_ctx_get_ns_sock(ctx));

	memset(req, 0, sizeof(*req));

	bytelen = (p->family == AF_INET ? IPV4_MAX_BYTELEN : IPV6_MAX_BYTELEN);

	req->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req->n.nlmsg_flags = NLM_F_CREATE | NLM_F_REQUEST;

	if ((cmd == RTM_NEWROUTE) &&
	    ((p->family == AF_INET) || v6_rr_semantics))
		req->n.nlmsg_flags |= NLM_F_REPLACE;

	req->n.nlmsg_type = cmd;

	req->n.nlmsg_pid = nl->snl.nl_pid;

	req->r.rtm_family = p->family;
	req->r.rtm_dst_len = p->prefixlen;
	req->r.rtm_scope = RT_SCOPE_UNIVERSE;

	if (cmd == RTM_DELROUTE)
		req->r.rtm_protocol = zebra2proto(dplane_ctx_get_old_type(ctx));
	else
		req->r.rtm_protocol = zebra2proto(dplane_ctx_get_type(ctx));

	req->r.rtm_type = RTN_UNICAST;

	if (!nl_attr_put(&req->n, datalen, RTA_DST, &p->u.prefix, bytelen))
		return 0;

	/* Table corresponding to this route. */
	table_id = dplane_ctx_get_table(ctx);
	if (!fpm) {
		if (table_id < 256)
			req->r.rtm_table = table_id;
		else {
			req->r.rtm_table = RT_TABLE_UNSPEC;
			if (!nl_attr_put32(&req->n, datalen, RTA_TABLE, table_id))
				return 0;
		}
	} else {
		/* Put vrf if_index instead of table id */
		vrf_id = dplane_ctx_get_vrf(ctx);
		if (vrf_id < 256)
			req->r.rtm_table = vrf_id;
		else {
			req->r.rtm_table = RT_TABLE_UNSPEC;
			if (!nl_attr_put32(&req->n, datalen, RTA_TABLE, vrf_id))
				return 0;
		}
	}

	if (IS_ZEBRA_DEBUG_FPM)
		zlog_debug(
			"%s: %s %pFX vrf %u(%u)", __func__,
			nl_msg_type_to_str(cmd), p, dplane_ctx_get_vrf(ctx),
			table_id);

	if (!nl_attr_put16(&req->n, datalen, RTA_ENCAP_TYPE,
				FPM_ROUTE_ENCAP_SRV6))
		return false;
	nest = nl_attr_nest(&req->n, datalen, RTA_ENCAP);
	if (!nest)
		return false;

	/*
	 * by default, we use the loopback address as encap source address,
	 * if it is valid
	 */
	ifp = if_lookup_by_name("lo", VRF_DEFAULT);
	if (ifp) {
		FOR_ALL_INTERFACES_ADDRESSES(ifp, connected, node) {
			if (connected->address->family == AF_INET6 &&
					!IN6_IS_ADDR_LOOPBACK(&connected->address->u.prefix6) &&
					!IN6_IS_ADDR_LINKLOCAL(&connected->address->u.prefix6)) {
				encap_src_addr = connected->address->u.prefix6;
				break;
			}
		}
	}

	if (!nl_attr_put(
			&req->n, datalen, FPM_ROUTE_ENCAP_SRV6_ENCAP_SRC_ADDR,
			&encap_src_addr, IPV6_MAX_BYTELEN))
		return false;
	if (!nl_attr_put(&req->n, datalen, FPM_ROUTE_ENCAP_SRV6_VPN_SID,
				&nexthop->nh_srv6->seg6_segs,
				IPV6_MAX_BYTELEN))
		return false;
	nl_attr_nest_end(&req->n, nest);

	return NLMSG_ALIGN(req->n.nlmsg_len);
}

/*
 * SRv6 change via netlink interface, using a dataplane context object
 *
 * Returns -1 on failure, 0 when the msg doesn't fit entirely in the buffer
 * otherwise the number of bytes written to buf.
 */
static ssize_t netlink_srv6_msg_encode(int cmd,
					   struct zebra_dplane_ctx *ctx,
					   uint8_t *data, size_t datalen,
					   bool fpm, bool force_nhg)
{
	struct nexthop *nexthop = NULL;

	struct {
		struct nlmsghdr n;
		struct rtmsg r;
		char buf[];
	} *req = (void *)data;

	nexthop = dplane_ctx_get_ng(ctx)->nexthop;
	if (!nexthop || !nexthop->nh_srv6)
		return -1;

	if (nexthop->nh_srv6->seg6local_action !=
 		    ZEBRA_SEG6_LOCAL_ACTION_UNSPEC) {
		if (cmd == RTM_NEWROUTE)
			cmd = RTM_NEWSRV6LOCALSID;
		else if (cmd == RTM_DELROUTE)
			cmd = RTM_DELSRV6LOCALSID;

		if (!netlink_srv6_localsid_msg_encode(
				cmd, ctx, data, datalen, fpm, force_nhg))
			return 0;
	} else if (!sid_zero(&nexthop->nh_srv6->seg6_segs)) {
		if (!netlink_srv6_vpn_route_msg_encode(
				cmd, ctx, data, datalen, fpm, force_nhg))
			return 0;
	} else {
		zlog_err(
			"%s: invalid srv6 nexthop", __func__);
		return -1;
	}

	return NLMSG_ALIGN(req->n.nlmsg_len);
}

/**
 * Encode data plane operation context into netlink and enqueue it in the FPM
 * output buffer.
 *
 * @param fnc the netlink FPM context.
 * @param ctx the data plane operation context data.
 * @return 0 on success or -1 on not enough space.
 */
static int fpm_nl_enqueue(struct fpm_nl_ctx *fnc, struct zebra_dplane_ctx *ctx)
{
	uint8_t nl_buf[NL_PKT_BUF_SIZE];
	size_t nl_buf_len;
	ssize_t rv;
	uint64_t obytes, obytes_peak;
	enum dplane_op_e op = dplane_ctx_get_op(ctx);
	struct nexthop *nexthop;

	/*
	 * If we were configured to not use next hop groups, then quit as soon
	 * as possible.
	 */
	if ((!fnc->use_nhg)
	    && (op == DPLANE_OP_NH_DELETE || op == DPLANE_OP_NH_INSTALL
		|| op == DPLANE_OP_NH_UPDATE))
		return 0;
 
	/*
	 * Ignore route from default table, because when mgmt port goes down,
	 * zebra will remove the default route and causing ASIC to blackhole IO.
	 */
	if (dplane_ctx_get_table(ctx) == RT_TABLE_DEFAULT) {
		zlog_debug("%s: discard default table route", __func__);
		return 0;
	}

	nl_buf_len = 0;

	frr_mutex_lock_autounlock(&fnc->obuf_mutex);

	switch (op) {
	case DPLANE_OP_ROUTE_UPDATE:
	case DPLANE_OP_ROUTE_DELETE:
		nexthop = dplane_ctx_get_ng(ctx)->nexthop;
		if (nexthop && nexthop->nh_srv6) {
			rv = netlink_srv6_msg_encode(RTM_DELROUTE, ctx,
								nl_buf, sizeof(nl_buf),
								true, fnc->use_nhg);
			if (rv <= 0) {
				zlog_err(
					"%s: netlink_srv6_msg_encode failed",
					__func__);
				return 0;
			}
		} else {
			rv = netlink_route_multipath_msg_encode(RTM_DELROUTE, ctx,
								nl_buf, sizeof(nl_buf),
								true, fnc->use_nhg);
			if (rv <= 0) {
				zlog_err(
					"%s: netlink_route_multipath_msg_encode failed",
					__func__);
				return 0;
			}
		}

		nl_buf_len = (size_t)rv;

		/* UPDATE operations need a INSTALL, otherwise just quit. */
		if (op == DPLANE_OP_ROUTE_DELETE)
			break;

		/* FALL THROUGH */
	case DPLANE_OP_ROUTE_INSTALL:
		nexthop = dplane_ctx_get_ng(ctx)->nexthop;
		if (nexthop && nexthop->nh_srv6) {
			rv = netlink_srv6_msg_encode(
				RTM_NEWROUTE, ctx, &nl_buf[nl_buf_len],
				sizeof(nl_buf) - nl_buf_len, true, fnc->use_nhg);
			if (rv <= 0) {
				zlog_err(
					"%s: netlink_srv6_msg_encode failed",
					__func__);
				return 0;
			}
		} else {
			rv = netlink_route_multipath_msg_encode(
				RTM_NEWROUTE, ctx, &nl_buf[nl_buf_len],
				sizeof(nl_buf) - nl_buf_len, true, fnc->use_nhg);
			if (rv <= 0) {
				zlog_err(
					"%s: netlink_route_multipath_msg_encode failed",
					__func__);
				return 0;
			}
		}

		nl_buf_len += (size_t)rv;

		break;

	case DPLANE_OP_MAC_INSTALL:
	case DPLANE_OP_MAC_DELETE:
		rv = netlink_macfdb_update_ctx(ctx, nl_buf, sizeof(nl_buf));
		if (rv <= 0) {
			zlog_err("%s: netlink_macfdb_update_ctx failed",
				 __func__);
			return 0;
		}

		nl_buf_len = (size_t)rv;
		break;

	case DPLANE_OP_NH_DELETE:
		rv = netlink_nexthop_msg_encode(RTM_DELNEXTHOP, ctx, nl_buf,
						sizeof(nl_buf), true);
		if (rv <= 0) {
			zlog_err("%s: netlink_nexthop_msg_encode failed",
				 __func__);
			return 0;
		}

		nl_buf_len = (size_t)rv;
		break;
	case DPLANE_OP_NH_INSTALL:
	case DPLANE_OP_NH_UPDATE:
		rv = netlink_nexthop_msg_encode(RTM_NEWNEXTHOP, ctx, nl_buf,
						sizeof(nl_buf), true);
		if (rv <= 0) {
			zlog_err("%s: netlink_nexthop_msg_encode failed",
				 __func__);
			return 0;
		}

		nl_buf_len = (size_t)rv;
		break;

	case DPLANE_OP_LSP_INSTALL:
	case DPLANE_OP_LSP_UPDATE:
	case DPLANE_OP_LSP_DELETE:
		rv = netlink_lsp_msg_encoder(ctx, nl_buf, sizeof(nl_buf));
		if (rv <= 0) {
			zlog_err("%s: netlink_lsp_msg_encoder failed",
				 __func__);
			return 0;
		}

		nl_buf_len += (size_t)rv;
		break;

	case DPLANE_OP_ADDR_INSTALL:
	case DPLANE_OP_ADDR_UNINSTALL:
		if (strmatch(dplane_ctx_get_ifname(ctx), "lo"))
			thread_add_timer(fnc->fthread->master, fpm_srv6_route_reset,
				 fnc, 0, &fnc->t_ribreset);
		break;

	/* Un-handled by FPM at this time. */
	case DPLANE_OP_PW_INSTALL:
	case DPLANE_OP_PW_UNINSTALL:
	case DPLANE_OP_NEIGH_INSTALL:
	case DPLANE_OP_NEIGH_UPDATE:
	case DPLANE_OP_NEIGH_DELETE:
	case DPLANE_OP_VTEP_ADD:
	case DPLANE_OP_VTEP_DELETE:
	case DPLANE_OP_SYS_ROUTE_ADD:
	case DPLANE_OP_SYS_ROUTE_DELETE:
	case DPLANE_OP_ROUTE_NOTIFY:
	case DPLANE_OP_LSP_NOTIFY:
	case DPLANE_OP_RULE_ADD:
	case DPLANE_OP_RULE_DELETE:
	case DPLANE_OP_RULE_UPDATE:
	case DPLANE_OP_NEIGH_DISCOVER:
	case DPLANE_OP_BR_PORT_UPDATE:
	case DPLANE_OP_IPTABLE_ADD:
	case DPLANE_OP_IPTABLE_DELETE:
	case DPLANE_OP_IPSET_ADD:
	case DPLANE_OP_IPSET_DELETE:
	case DPLANE_OP_IPSET_ENTRY_ADD:
	case DPLANE_OP_IPSET_ENTRY_DELETE:
	case DPLANE_OP_NEIGH_IP_INSTALL:
	case DPLANE_OP_NEIGH_IP_DELETE:
	case DPLANE_OP_NEIGH_TABLE_UPDATE:
	case DPLANE_OP_GRE_SET:
	case DPLANE_OP_INTF_ADDR_ADD:
	case DPLANE_OP_INTF_ADDR_DEL:
	case DPLANE_OP_INTF_NETCONFIG:
	case DPLANE_OP_INTF_INSTALL:
	case DPLANE_OP_INTF_UPDATE:
	case DPLANE_OP_INTF_DELETE:
	case DPLANE_OP_TC_QDISC_INSTALL:
	case DPLANE_OP_TC_QDISC_UNINSTALL:
	case DPLANE_OP_TC_CLASS_ADD:
	case DPLANE_OP_TC_CLASS_DELETE:
	case DPLANE_OP_TC_CLASS_UPDATE:
	case DPLANE_OP_TC_FILTER_ADD:
	case DPLANE_OP_TC_FILTER_DELETE:
	case DPLANE_OP_TC_FILTER_UPDATE:
	case DPLANE_OP_NONE:
	case DPLANE_OP_STARTUP_STAGE:
		break;

	}

	/* Skip empty enqueues. */
	if (nl_buf_len == 0)
		return 0;

	/* We must know if someday a message goes beyond 65KiB. */
	assert((nl_buf_len + FPM_HEADER_SIZE) <= UINT16_MAX);

	/* Check if we have enough buffer space. */
	if (STREAM_WRITEABLE(fnc->obuf) < (nl_buf_len + FPM_HEADER_SIZE)) {
		atomic_fetch_add_explicit(&fnc->counters.buffer_full, 1,
					  memory_order_relaxed);

		if (IS_ZEBRA_DEBUG_FPM)
			zlog_debug(
				"%s: buffer full: wants to write %zu but has %zu",
				__func__, nl_buf_len + FPM_HEADER_SIZE,
				STREAM_WRITEABLE(fnc->obuf));

		return -1;
	}

	/*
	 * Fill in the FPM header information.
	 *
	 * See FPM_HEADER_SIZE definition for more information.
	 */
	stream_putc(fnc->obuf, 1);
	stream_putc(fnc->obuf, 1);
	stream_putw(fnc->obuf, nl_buf_len + FPM_HEADER_SIZE);

	/* Write current data. */
	stream_write(fnc->obuf, nl_buf, (size_t)nl_buf_len);

	/* Account number of bytes waiting to be written. */
	atomic_fetch_add_explicit(&fnc->counters.obuf_bytes,
				  nl_buf_len + FPM_HEADER_SIZE,
				  memory_order_relaxed);
	obytes = atomic_load_explicit(&fnc->counters.obuf_bytes,
				      memory_order_relaxed);
	obytes_peak = atomic_load_explicit(&fnc->counters.obuf_peak,
					   memory_order_relaxed);
	if (obytes_peak < obytes)
		atomic_store_explicit(&fnc->counters.obuf_peak, obytes,
				      memory_order_relaxed);

	/* Tell the thread to start writing. */
	thread_add_write(fnc->fthread->master, fpm_write, fnc, fnc->socket,
			 &fnc->t_write);

	return 0;
}

/*
 * LSP walk/send functions
 */
struct fpm_lsp_arg {
	struct zebra_dplane_ctx *ctx;
	struct fpm_nl_ctx *fnc;
	bool complete;
};

static int fpm_lsp_send_cb(struct hash_bucket *bucket, void *arg)
{
	struct zebra_lsp *lsp = bucket->data;
	struct fpm_lsp_arg *fla = arg;

	/* Skip entries which have already been sent */
	if (CHECK_FLAG(lsp->flags, LSP_FLAG_FPM))
		return HASHWALK_CONTINUE;

	dplane_ctx_reset(fla->ctx);
	dplane_ctx_lsp_init(fla->ctx, DPLANE_OP_LSP_INSTALL, lsp);

	if (fpm_nl_enqueue(fla->fnc, fla->ctx) == -1) {
		fla->complete = false;
		return HASHWALK_ABORT;
	}

	/* Mark entry as sent */
	SET_FLAG(lsp->flags, LSP_FLAG_FPM);
	return HASHWALK_CONTINUE;
}

static void fpm_lsp_send(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	struct zebra_vrf *zvrf = vrf_info_lookup(VRF_DEFAULT);
	struct fpm_lsp_arg fla;

	fla.fnc = fnc;
	fla.ctx = dplane_ctx_alloc();
	fla.complete = true;

	hash_walk(zvrf->lsp_table, fpm_lsp_send_cb, &fla);

	dplane_ctx_fini(&fla.ctx);

	if (fla.complete) {
		WALK_FINISH(fnc, FNE_LSP_FINISHED);

		/* Now move onto routes */
		thread_add_timer(zrouter.master, fpm_nhg_reset, fnc, 0,
				 &fnc->t_nhgreset);
	} else {
		/* Didn't finish - reschedule LSP walk */
		thread_add_timer(zrouter.master, fpm_lsp_send, fnc, 0,
				 &fnc->t_lspwalk);
	}
}

/*
 * Next hop walk/send functions.
 */
struct fpm_nhg_arg {
	struct zebra_dplane_ctx *ctx;
	struct fpm_nl_ctx *fnc;
	bool complete;
};

static int fpm_nhg_send_cb(struct hash_bucket *bucket, void *arg)
{
	struct nhg_hash_entry *nhe = bucket->data;
	struct fpm_nhg_arg *fna = arg;

	/* This entry was already sent, skip it. */
	if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_FPM))
		return HASHWALK_CONTINUE;

	/* Reset ctx to reuse allocated memory, take a snapshot and send it. */
	dplane_ctx_reset(fna->ctx);
	dplane_ctx_nexthop_init(fna->ctx, DPLANE_OP_NH_INSTALL, nhe);
	if (fpm_nl_enqueue(fna->fnc, fna->ctx) == -1) {
		/* Our buffers are full, lets give it some cycles. */
		fna->complete = false;
		return HASHWALK_ABORT;
	}

	/* Mark group as sent, so it doesn't get sent again. */
	SET_FLAG(nhe->flags, NEXTHOP_GROUP_FPM);

	return HASHWALK_CONTINUE;
}

static void fpm_nhg_send(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	struct fpm_nhg_arg fna;

	fna.fnc = fnc;
	fna.ctx = dplane_ctx_alloc();
	fna.complete = true;

	/* Send next hops. */
	if (fnc->use_nhg)
		hash_walk(zrouter.nhgs_id, fpm_nhg_send_cb, &fna);

	/* `free()` allocated memory. */
	dplane_ctx_fini(&fna.ctx);

	/* We are done sending next hops, lets install the routes now. */
	if (fna.complete) {
		WALK_FINISH(fnc, FNE_NHG_FINISHED);
		thread_add_timer(zrouter.master, fpm_rib_reset, fnc, 0,
				 &fnc->t_ribreset);
	} else /* Otherwise reschedule next hop group again. */
		thread_add_timer(zrouter.master, fpm_nhg_send, fnc, 0,
				 &fnc->t_nhgwalk);
}

/**
 * Send all RIB installed routes to the connected data plane.
 */
static void fpm_rib_send(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	rib_dest_t *dest;
	struct route_node *rn;
	struct route_table *rt;
	struct zebra_dplane_ctx *ctx;
	rib_tables_iter_t rt_iter;

	/* Allocate temporary context for all transactions. */
	ctx = dplane_ctx_alloc();

	rt_iter.state = RIB_TABLES_ITER_S_INIT;
	while ((rt = rib_tables_iter_next(&rt_iter))) {
		for (rn = route_top(rt); rn; rn = srcdest_route_next(rn)) {
			dest = rib_dest_from_rnode(rn);
			/* Skip bad route entries. */
			if (dest == NULL || dest->selected_fib == NULL)
				continue;

			/* Check for already sent routes. */
			if (CHECK_FLAG(dest->flags, RIB_DEST_UPDATE_FPM))
				continue;

			/* Enqueue route install. */
			dplane_ctx_reset(ctx);
			dplane_ctx_route_init(ctx, DPLANE_OP_ROUTE_INSTALL, rn,
					      dest->selected_fib);
			if (fpm_nl_enqueue(fnc, ctx) == -1) {
				/* Free the temporary allocated context. */
				dplane_ctx_fini(&ctx);

				thread_add_timer(zrouter.master, fpm_rib_send,
						 fnc, 1, &fnc->t_ribwalk);
				return;
			}

			/* Mark as sent. */
			SET_FLAG(dest->flags, RIB_DEST_UPDATE_FPM);
		}
	}

	/* Free the temporary allocated context. */
	dplane_ctx_fini(&ctx);

	/* All RIB routes sent! */
	WALK_FINISH(fnc, FNE_RIB_FINISHED);

	/* Schedule next event: RMAC reset. */
	thread_add_event(zrouter.master, fpm_rmac_reset, fnc, 0,
			 &fnc->t_rmacreset);
}

/*
 * The next three functions will handle RMAC enqueue.
 */
struct fpm_rmac_arg {
	struct zebra_dplane_ctx *ctx;
	struct fpm_nl_ctx *fnc;
	struct zebra_l3vni *zl3vni;
	bool complete;
};

static void fpm_enqueue_rmac_table(struct hash_bucket *bucket, void *arg)
{
	struct fpm_rmac_arg *fra = arg;
	struct zebra_mac *zrmac = bucket->data;
	struct zebra_if *zif = fra->zl3vni->vxlan_if->info;
	const struct zebra_l2info_vxlan *vxl = &zif->l2info.vxl;
	struct zebra_if *br_zif;
	vlanid_t vid;
	bool sticky;

	/* Entry already sent. */
	if (CHECK_FLAG(zrmac->flags, ZEBRA_MAC_FPM_SENT) || !fra->complete)
		return;

	sticky = !!CHECK_FLAG(zrmac->flags,
			      (ZEBRA_MAC_STICKY | ZEBRA_MAC_REMOTE_DEF_GW));
	br_zif = (struct zebra_if *)(zif->brslave_info.br_if->info);
	vid = IS_ZEBRA_IF_BRIDGE_VLAN_AWARE(br_zif) ? vxl->access_vlan : 0;

	dplane_ctx_reset(fra->ctx);
	dplane_ctx_set_op(fra->ctx, DPLANE_OP_MAC_INSTALL);
	dplane_mac_init(fra->ctx, fra->zl3vni->vxlan_if,
			zif->brslave_info.br_if, vid,
			&zrmac->macaddr, zrmac->fwd_info.r_vtep_ip, sticky,
			0 /*nhg*/, 0 /*update_flags*/);
	if (fpm_nl_enqueue(fra->fnc, fra->ctx) == -1) {
		thread_add_timer(zrouter.master, fpm_rmac_send,
				 fra->fnc, 1, &fra->fnc->t_rmacwalk);
		fra->complete = false;
	}
}

static void fpm_enqueue_l3vni_table(struct hash_bucket *bucket, void *arg)
{
	struct fpm_rmac_arg *fra = arg;
	struct zebra_l3vni *zl3vni = bucket->data;

	fra->zl3vni = zl3vni;
	hash_iterate(zl3vni->rmac_table, fpm_enqueue_rmac_table, zl3vni);
}

static void fpm_rmac_send(struct thread *t)
{
	struct fpm_rmac_arg fra;

	fra.fnc = THREAD_ARG(t);
	fra.ctx = dplane_ctx_alloc();
	fra.complete = true;
	hash_iterate(zrouter.l3vni_table, fpm_enqueue_l3vni_table, &fra);
	dplane_ctx_fini(&fra.ctx);

	/* RMAC walk completed. */
	if (fra.complete)
		WALK_FINISH(fra.fnc, FNE_RMAC_FINISHED);
}

/*
 * Resets the next hop FPM flags so we send all next hops again.
 */
static void fpm_nhg_reset_cb(struct hash_bucket *bucket, void *arg)
{
	struct nhg_hash_entry *nhe = bucket->data;

	/* Unset FPM installation flag so it gets installed again. */
	UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_FPM);
}

static void fpm_nhg_reset(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);

	hash_iterate(zrouter.nhgs_id, fpm_nhg_reset_cb, NULL);

	/* Schedule next step: send next hop groups. */
	thread_add_event(zrouter.master, fpm_nhg_send, fnc, 0, &fnc->t_nhgwalk);
}

/*
 * Resets the LSP FPM flag so we send all LSPs again.
 */
static void fpm_lsp_reset_cb(struct hash_bucket *bucket, void *arg)
{
	struct zebra_lsp *lsp = bucket->data;

	UNSET_FLAG(lsp->flags, LSP_FLAG_FPM);
}

static void fpm_lsp_reset(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	struct zebra_vrf *zvrf = vrf_info_lookup(VRF_DEFAULT);

	hash_iterate(zvrf->lsp_table, fpm_lsp_reset_cb, NULL);

	/* Schedule next step: send LSPs */
	thread_add_event(zrouter.master, fpm_lsp_send, fnc, 0, &fnc->t_lspwalk);
}

/**
 * Resets the RIB FPM flags so we send all routes again.
 */
static void fpm_rib_reset(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	rib_dest_t *dest;
	struct route_node *rn;
	struct route_table *rt;
	rib_tables_iter_t rt_iter;

	rt_iter.state = RIB_TABLES_ITER_S_INIT;
	while ((rt = rib_tables_iter_next(&rt_iter))) {
		for (rn = route_top(rt); rn; rn = srcdest_route_next(rn)) {
			dest = rib_dest_from_rnode(rn);
			/* Skip bad route entries. */
			if (dest == NULL)
				continue;

			UNSET_FLAG(dest->flags, RIB_DEST_UPDATE_FPM);
		}
	}

	/* Schedule next step: send RIB routes. */
	thread_add_event(zrouter.master, fpm_rib_send, fnc, 0, &fnc->t_ribwalk);
}

/*
 * The next three function will handle RMAC table reset.
 */
static void fpm_unset_rmac_table(struct hash_bucket *bucket, void *arg)
{
	struct zebra_mac *zrmac = bucket->data;

	UNSET_FLAG(zrmac->flags, ZEBRA_MAC_FPM_SENT);
}

static void fpm_unset_l3vni_table(struct hash_bucket *bucket, void *arg)
{
	struct zebra_l3vni *zl3vni = bucket->data;

	hash_iterate(zl3vni->rmac_table, fpm_unset_rmac_table, zl3vni);
}

static void fpm_rmac_reset(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);

	hash_iterate(zrouter.l3vni_table, fpm_unset_l3vni_table, NULL);

	/* Schedule next event: send RMAC entries. */
	thread_add_event(zrouter.master, fpm_rmac_send, fnc, 0,
			 &fnc->t_rmacwalk);
}

static void fpm_process_queue(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	struct zebra_dplane_ctx *ctx;
	bool no_bufs = false;
	uint64_t processed_contexts = 0;

	while (true) {
		/* No space available yet. */
		if (STREAM_WRITEABLE(fnc->obuf) < NL_PKT_BUF_SIZE) {
			no_bufs = true;
			break;
		}

		/* Dequeue next item or quit processing. */
		frr_with_mutex (&fnc->ctxqueue_mutex) {
			ctx = dplane_ctx_dequeue(&fnc->ctxqueue);
		}
		if (ctx == NULL)
			break;

		/*
		 * Intentionally ignoring the return value
		 * as that we are ensuring that we can write to
		 * the output data in the STREAM_WRITEABLE
		 * check above, so we can ignore the return
		 */
		if (fnc->socket != -1)
			(void)fpm_nl_enqueue(fnc, ctx);

		/* Account the processed entries. */
		processed_contexts++;
		atomic_fetch_sub_explicit(&fnc->counters.ctxqueue_len, 1,
					  memory_order_relaxed);

		dplane_ctx_set_status(ctx, ZEBRA_DPLANE_REQUEST_SUCCESS);
		dplane_provider_enqueue_out_ctx(fnc->prov, ctx);
	}

	/* Update count of processed contexts */
	atomic_fetch_add_explicit(&fnc->counters.dplane_contexts,
				  processed_contexts, memory_order_relaxed);

	/* Re-schedule if we ran out of buffer space */
	if (no_bufs)
		thread_add_timer(fnc->fthread->master, fpm_process_queue,
				 fnc, 0, &fnc->t_dequeue);

	/*
	 * Let the dataplane thread know if there are items in the
	 * output queue to be processed. Otherwise they may sit
	 * until the dataplane thread gets scheduled for new,
	 * unrelated work.
	 */
	if (dplane_provider_out_ctx_queue_len(fnc->prov) > 0)
		dplane_provider_work_ready();
}

/**
 * Handles external (e.g. CLI, data plane or others) events.
 */
static void fpm_process_event(struct thread *t)
{
	struct fpm_nl_ctx *fnc = THREAD_ARG(t);
	enum fpm_nl_events event = THREAD_VAL(t);

	switch (event) {
	case FNE_DISABLE:
		zlog_info("%s: manual FPM disable event", __func__);
		fnc->disabled = true;
		atomic_fetch_add_explicit(&fnc->counters.user_disables, 1,
					  memory_order_relaxed);

		/* Call reconnect to disable timers and clean up context. */
		fpm_reconnect(fnc);
		break;

	case FNE_RECONNECT:
		zlog_info("%s: manual FPM reconnect event", __func__);
		fnc->disabled = false;
		atomic_fetch_add_explicit(&fnc->counters.user_configures, 1,
					  memory_order_relaxed);
		fpm_reconnect(fnc);
		break;

	case FNE_RESET_COUNTERS:
		zlog_info("%s: manual FPM counters reset event", __func__);
		memset(&fnc->counters, 0, sizeof(fnc->counters));
		break;

	case FNE_TOGGLE_NHG:
		zlog_info("%s: toggle next hop groups support", __func__);
		fnc->use_nhg = !fnc->use_nhg;
		fpm_reconnect(fnc);
		break;

	case FNE_INTERNAL_RECONNECT:
		fpm_reconnect(fnc);
		break;

	case FNE_NHG_FINISHED:
		if (IS_ZEBRA_DEBUG_FPM)
			zlog_debug("%s: next hop groups walk finished",
				   __func__);
		break;
	case FNE_RIB_FINISHED:
		if (IS_ZEBRA_DEBUG_FPM)
			zlog_debug("%s: RIB walk finished", __func__);
		break;
	case FNE_RMAC_FINISHED:
		if (IS_ZEBRA_DEBUG_FPM)
			zlog_debug("%s: RMAC walk finished", __func__);
		break;
	case FNE_LSP_FINISHED:
		if (IS_ZEBRA_DEBUG_FPM)
			zlog_debug("%s: LSP walk finished", __func__);
		break;
	}
}

/*
 * Data plane functions.
 */
static int fpm_nl_start(struct zebra_dplane_provider *prov)
{
	struct fpm_nl_ctx *fnc;

	fnc = dplane_provider_get_data(prov);
	fnc->fthread = frr_pthread_new(NULL, prov_name, prov_name);
	assert(frr_pthread_run(fnc->fthread, NULL) == 0);
	fnc->ibuf = stream_new(NL_PKT_BUF_SIZE);
	fnc->obuf = stream_new(NL_PKT_BUF_SIZE * 128);
	pthread_mutex_init(&fnc->obuf_mutex, NULL);
	fnc->socket = -1;
	fnc->disabled = true;
	fnc->prov = prov;
	dplane_ctx_q_init(&fnc->ctxqueue);
	pthread_mutex_init(&fnc->ctxqueue_mutex, NULL);

	/* Set default values. */
	fnc->use_nhg = true;

	return 0;
}

static int fpm_nl_finish_early(struct fpm_nl_ctx *fnc)
{
	/* Disable all events and close socket. */
	THREAD_OFF(fnc->t_lspreset);
	THREAD_OFF(fnc->t_lspwalk);
	THREAD_OFF(fnc->t_nhgreset);
	THREAD_OFF(fnc->t_nhgwalk);
	THREAD_OFF(fnc->t_ribreset);
	THREAD_OFF(fnc->t_ribwalk);
	THREAD_OFF(fnc->t_rmacreset);
	THREAD_OFF(fnc->t_rmacwalk);
	THREAD_OFF(fnc->t_event);
	THREAD_OFF(fnc->t_nhg);
	thread_cancel_async(fnc->fthread->master, &fnc->t_read, NULL);
	thread_cancel_async(fnc->fthread->master, &fnc->t_write, NULL);
	thread_cancel_async(fnc->fthread->master, &fnc->t_connect, NULL);

	if (fnc->socket != -1) {
		close(fnc->socket);
		fnc->socket = -1;
	}

	return 0;
}

static int fpm_nl_finish_late(struct fpm_nl_ctx *fnc)
{
	/* Stop the running thread. */
	frr_pthread_stop(fnc->fthread, NULL);

	/* Free all allocated resources. */
	pthread_mutex_destroy(&fnc->obuf_mutex);
	pthread_mutex_destroy(&fnc->ctxqueue_mutex);
	stream_free(fnc->ibuf);
	stream_free(fnc->obuf);
	free(gfnc);
	gfnc = NULL;

	return 0;
}

static int fpm_nl_finish(struct zebra_dplane_provider *prov, bool early)
{
	struct fpm_nl_ctx *fnc;

	fnc = dplane_provider_get_data(prov);
	if (early)
		return fpm_nl_finish_early(fnc);

	return fpm_nl_finish_late(fnc);
}

static int fpm_nl_process(struct zebra_dplane_provider *prov)
{
	struct zebra_dplane_ctx *ctx;
	struct fpm_nl_ctx *fnc;
	int counter, limit;
	uint64_t cur_queue, peak_queue = 0, stored_peak_queue;

	fnc = dplane_provider_get_data(prov);
	limit = dplane_provider_get_work_limit(prov);
	for (counter = 0; counter < limit; counter++) {
		ctx = dplane_provider_dequeue_in_ctx(prov);
		if (ctx == NULL)
			break;

		/*
		 * Skip all notifications if not connected, we'll walk the RIB
		 * anyway.
		 */
		if (fnc->socket != -1 && fnc->connecting == false) {
			/*
			 * Update the number of queued contexts *before*
			 * enqueueing, to ensure counter consistency.
			 */
			atomic_fetch_add_explicit(&fnc->counters.ctxqueue_len,
						  1, memory_order_relaxed);

			frr_with_mutex (&fnc->ctxqueue_mutex) {
				dplane_ctx_enqueue_tail(&fnc->ctxqueue, ctx);
			}

			cur_queue = atomic_load_explicit(
				&fnc->counters.ctxqueue_len,
				memory_order_relaxed);
			if (peak_queue < cur_queue)
				peak_queue = cur_queue;
			continue;
		}

		dplane_ctx_set_status(ctx, ZEBRA_DPLANE_REQUEST_SUCCESS);
		dplane_provider_enqueue_out_ctx(prov, ctx);
	}

	/* Update peak queue length, if we just observed a new peak */
	stored_peak_queue = atomic_load_explicit(
		&fnc->counters.ctxqueue_len_peak, memory_order_relaxed);
	if (stored_peak_queue < peak_queue)
		atomic_store_explicit(&fnc->counters.ctxqueue_len_peak,
				      peak_queue, memory_order_relaxed);

	if (atomic_load_explicit(&fnc->counters.ctxqueue_len,
				 memory_order_relaxed)
	    > 0)
		thread_add_timer(fnc->fthread->master, fpm_process_queue,
				 fnc, 0, &fnc->t_dequeue);

	/* Ensure dataplane thread is rescheduled if we hit the work limit */
	if (counter >= limit)
		dplane_provider_work_ready();

	return 0;
}

static int fpm_nl_new(struct thread_master *tm)
{
	struct zebra_dplane_provider *prov = NULL;
	int rv;

	gfnc = calloc(1, sizeof(*gfnc));
	rv = dplane_provider_register(prov_name, DPLANE_PRIO_POSTPROCESS,
				      DPLANE_PROV_FLAG_THREADED, fpm_nl_start,
				      fpm_nl_process, fpm_nl_finish, gfnc,
				      &prov);

	if (IS_ZEBRA_DEBUG_DPLANE)
		zlog_debug("%s register status: %d", prov_name, rv);

	install_node(&fpm_node);
	install_element(ENABLE_NODE, &fpm_show_counters_cmd);
	install_element(ENABLE_NODE, &fpm_show_counters_json_cmd);
	install_element(ENABLE_NODE, &fpm_reset_counters_cmd);
	install_element(CONFIG_NODE, &fpm_set_address_cmd);
	install_element(CONFIG_NODE, &no_fpm_set_address_cmd);
	install_element(CONFIG_NODE, &fpm_use_nhg_cmd);
	install_element(CONFIG_NODE, &no_fpm_use_nhg_cmd);

	return 0;
}

static int fpm_nl_init(void)
{
	hook_register(frr_late_init, fpm_nl_new);
	return 0;
}

FRR_MODULE_SETUP(
	.name = "dplane_fpm_sonic",
	.version = "0.0.1",
	.description = "Data plane plugin for FPM using netlink.",
	.init = fpm_nl_init,
);
