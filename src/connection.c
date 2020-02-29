/*
 * Copyright (c) 2018-2020 SignalWire, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "signalwire-client-c/client.h"

#define ks_time_now_ms() ks_time_ms(ks_time_now())

static swclt_cmd_t *deregister_cmd(swclt_conn_t *ctx, ks_uuid_t id);

/* 13107 commands per second over 5 second average TTL */
#define TTL_HEAP_MAX_SIZE 65536

typedef struct swclt_ttl_node {
	ks_time_t expiry;
	ks_uuid_t id;
} swclt_ttl_node_t;

struct swclt_ttl_tracker {
	swclt_ttl_node_t heap[TTL_HEAP_MAX_SIZE]; // min heap of TTLs to expire
	int count;
	ks_cond_t *cond;
	ks_thread_t *thread;
	swclt_conn_t *conn;
};

#define TTL_HEAP_ROOT 0
#define TTL_HEAP_PARENT(pos) ((pos - 1) / 2)
#define TTL_HEAP_LEFT_CHILD(pos) ((pos * 2) + 1)
#define TTL_HEAP_RIGHT_CHILD(pos) ((pos * 2) + 2)

inline static void ttl_heap_swap(swclt_ttl_tracker_t *ttl, int pos1, int pos2)
{
	if (pos1 == pos2) {
		return;
	}
	swclt_ttl_node_t tmp = ttl->heap[pos1];
	ttl->heap[pos1] = ttl->heap[pos2];
	ttl->heap[pos2] = tmp;
}

static ks_status_t ttl_heap_remove(swclt_ttl_tracker_t *ttl)
{
	if (ttl->count <= 0) {
		return KS_STATUS_FAIL;
	}
	// clear entry at root and swap with last entry
	memset(&ttl->heap[TTL_HEAP_ROOT], 0, sizeof(ttl->heap[TTL_HEAP_ROOT]));
	ttl_heap_swap(ttl, ttl->count - 1, TTL_HEAP_ROOT);
	ttl->count--;

	// sift down the value...
	int pos = TTL_HEAP_ROOT;
	int swap;
	while ((swap = TTL_HEAP_LEFT_CHILD(pos)) < ttl->count) {
		int right = TTL_HEAP_RIGHT_CHILD(pos);
		// if there is no left child or there is a right child and it is higher priority than left
		if (!ttl->heap[swap].expiry || (ttl->heap[right].expiry && ttl->heap[right].expiry < ttl->heap[swap].expiry)) {
			swap = right;
		}
		if (ttl->heap[swap].expiry && ttl->heap[pos].expiry > ttl->heap[swap].expiry) {
			ttl_heap_swap(ttl, pos, swap);
			pos = swap;
		} else {
			// done
			break;
		}
	}
	return KS_STATUS_SUCCESS;
}

static ks_status_t ttl_heap_insert(swclt_ttl_tracker_t *ttl, ks_time_t expiry, ks_uuid_t id)
{
	if (ttl->count >= TTL_HEAP_MAX_SIZE) {
		return KS_STATUS_FAIL;
	}

	if (expiry == 0) {
		return KS_STATUS_FAIL;
	}

	// add to last position in the heap
	int pos = ttl->count;
	ttl->count++;
	ttl->heap[pos].expiry = expiry;
	ttl->heap[pos].id = id;

	// now sift up the value
	while (pos > TTL_HEAP_ROOT) {
		int parent = TTL_HEAP_PARENT(pos);
		if (ttl->heap[parent].expiry > expiry) {
			ttl_heap_swap(ttl, parent, pos);
			pos = parent;
		} else {
			break;
		}
	}
	return KS_STATUS_SUCCESS;
}

static ks_status_t ttl_tracker_watch(swclt_ttl_tracker_t *ttl, ks_time_t expiry, ks_uuid_t id)
{
	ks_cond_lock(ttl->cond);
	// need to wake thread if this TTL is before the next one
	int wake_ttl_tracker_thread = !ttl->heap[TTL_HEAP_ROOT].expiry || ttl->heap[TTL_HEAP_ROOT].expiry > expiry;
	if (ttl_heap_insert(ttl, expiry, id) != KS_STATUS_SUCCESS) {
		ks_cond_unlock(ttl->cond);
		ks_log(KS_LOG_ERROR, "Failed to track command %s TTL", ks_uuid_thr_str(&id));
		return KS_STATUS_FAIL;
	}
	if (wake_ttl_tracker_thread) {
		// notify of new shortest TTL...
		ks_cond_broadcast(ttl->cond);
	}
	ks_cond_unlock(ttl->cond);
	return KS_STATUS_SUCCESS;
}

static ks_status_t ttl_tracker_next(swclt_ttl_tracker_t *ttl, ks_uuid_t *id)
{
	ks_cond_lock(ttl->cond);
	ks_time_t wait_ms = 0;
	ks_time_t now_ms = ks_time_now_ms();

	// how long to wait for next TTL expiration?
	if (!ttl->heap[TTL_HEAP_ROOT].expiry) {
		// nothing to wait for
		wait_ms = 5000;
	} else if (ttl->heap[TTL_HEAP_ROOT].expiry > now_ms) {		
		wait_ms = ttl->heap[TTL_HEAP_ROOT].expiry - now_ms;
		ks_log(KS_LOG_INFO, "Waiting %d for TTL expiration of %s", (uint32_t)wait_ms, ks_uuid_thr_str(&ttl->heap[TTL_HEAP_ROOT].id));
	}

	// wait for TTL, up to 5 seconds
	if (wait_ms > 5000) {
		wait_ms = 5000;
	}
	if (wait_ms) {
		ks_cond_timedwait(ttl->cond, wait_ms);
		now_ms = ks_time_now_ms();
	}

	// check for TTL expiration
	if (ttl->heap[TTL_HEAP_ROOT].expiry && ttl->heap[TTL_HEAP_ROOT].expiry <= now_ms) {
		// TTL expired
		*id = ttl->heap[TTL_HEAP_ROOT].id;
		ttl_heap_remove(ttl);
		ks_cond_unlock(ttl->cond);
		return KS_STATUS_SUCCESS;
	}

	// Nothing expired
	ks_cond_unlock(ttl->cond);
	return KS_STATUS_TIMEOUT;
}

static void *ttl_tracker_thread(ks_thread_t *thread, void *data)
{
	swclt_ttl_tracker_t *ttl = (swclt_ttl_tracker_t *)data;
	ks_log(KS_LOG_INFO, "TTL tracker thread running");
	while (ks_thread_stop_requested(thread) == KS_FALSE) {
		ks_uuid_t id = { 0 };
		if (ttl_tracker_next(ttl, &id) == KS_STATUS_SUCCESS) {
			swclt_cmd_t *cmd;
			if ((cmd = deregister_cmd(ttl->conn, id))) {
				swclt_cmd_cb_t cb = { 0 };
				void *cb_data = NULL;
				swclt_cmd_cb(cmd, &cb, &cb_data);
				swclt_cmd_report_failure_fmt(cmd, KS_STATUS_TIMEOUT, "TTL expired for command %s", ks_uuid_thr_str(&id));
				ks_log(KS_LOG_INFO, "TTL expired for command %s", ks_uuid_thr_str(&id));
				swclt_cmd_destroy(&cmd);
			}
		}
	}
	ks_log(KS_LOG_INFO, "TTL tracker thread finished");
	return NULL;
}

static void ttl_tracker_destroy(swclt_ttl_tracker_t **ttl)
{
	if (ttl && *ttl) {
		ks_log(KS_LOG_INFO, "Destroying TTL tracker");
		if ((*ttl)->thread && ks_thread_request_stop((*ttl)->thread) != KS_STATUS_SUCCESS) {
			*ttl = NULL;
			ks_log(KS_LOG_ERROR, "Failed to stop TTL thread.  Leaking TTL data and moving on.");
			return;
		}
		ks_cond_lock((*ttl)->cond);
		ks_cond_broadcast((*ttl)->cond);
		ks_cond_unlock((*ttl)->cond);
		ks_thread_destroy(&(*ttl)->thread);
		ks_cond_destroy(&(*ttl)->cond);
		ks_pool_free(ttl);
	}
}

static void ttl_tracker_create(ks_pool_t *pool, swclt_ttl_tracker_t **ttl, swclt_conn_t *ctx)
{
	ks_status_t status;
	*ttl = ks_pool_alloc(pool, sizeof(swclt_ttl_tracker_t));
	ks_cond_create(&(*ttl)->cond, pool);
	(*ttl)->conn = ctx;
	if (status = ks_thread_create(&(*ttl)->thread, ttl_tracker_thread, *ttl, NULL)) {
		ks_abort_fmt("Failed to allocate connection TTL thread: %lu", status);
	}
}

static void report_connection_failure(swclt_conn_t *conn)
{
	ks_mutex_lock(conn->failed_mutex);
	if (!conn->failed) {
		conn->failed = 1;
		ks_mutex_unlock(conn->failed_mutex);
		ks_log(KS_LOG_WARNING, "Reporting connection state failure");
		if (conn->failed_cb) {
			conn->failed_cb(conn, conn->failed_cb_data);
		}
	} else {
		ks_mutex_unlock(conn->failed_mutex);
	}
}

static ks_status_t register_cmd(swclt_conn_t *ctx, swclt_cmd_t **cmd)
{
	ks_status_t status;
	ks_uuid_t id = { 0 };
	uint32_t ttl_ms = { 0 };

	if (status = swclt_cmd_id(*cmd, &id))
		return status;
	if (status = swclt_cmd_ttl(*cmd, &ttl_ms))
		return status;

	ks_log(KS_LOG_DEBUG, "Tracking command with id: %s and TTL: %d", ks_uuid_thr_str(&id), ttl_ms);

	if (!ctx->ttl || (status = ttl_tracker_watch(ctx->ttl, ks_time_now_ms() + (ks_time_t)ttl_ms, id))) {
		ks_log(KS_LOG_ERROR, "Failed to track TTL for command with id: %s and TTL: %d", ks_uuid_thr_str(&id), ttl_ms);
		report_connection_failure(ctx);
		return status;
	}
	ks_hash_insert(ctx->outstanding_requests, ks_uuid_dup(ctx->pool, &id), *cmd);
	*cmd = NULL;

	return KS_STATUS_SUCCESS;
}

static swclt_cmd_t *deregister_cmd(swclt_conn_t *conn, ks_uuid_t id)
{
	return ks_hash_remove(conn->outstanding_requests, &id);
}

static ks_status_t submit_result(swclt_conn_t *ctx, swclt_cmd_t *cmd)
{
	ks_status_t status;

	if (ctx->failed || !ctx->wss) {
		return KS_STATUS_FAIL;
	}

	if (cmd->type != SWCLT_CMD_TYPE_RESULT && cmd->type != SWCLT_CMD_TYPE_ERROR) {
		char *cmd_str = swclt_cmd_describe(cmd);
		ks_log(KS_LOG_ERROR, "Invalid command type to send as result: %s", cmd_str);
		ks_pool_free(&cmd_str);
		return KS_STATUS_FAIL;
	}

	/* convert command to JSON string */
	char *data = NULL;
	if (status = swclt_cmd_print(cmd, cmd->pool, &data)) {
		ks_log(KS_LOG_CRIT, "Invalid command, failed to render payload string: %lu", status);
		return KS_STATUS_FAIL;
	}

	/* Write the command data on the socket */
	if ((status = swclt_wss_write(ctx->wss, data))) {
		ks_log(KS_LOG_WARNING, "Failed to write to websocket: %lu, %s", status, data);
	}
	ks_pool_free(&data);

	return status;
}

static ks_status_t submit_request(swclt_conn_t *ctx, swclt_cmd_t **cmdP, swclt_cmd_future_t **cmd_future)
{
	ks_status_t status = KS_STATUS_SUCCESS;
	uint32_t flags = 0;
	swclt_cmd_t *cmd = *cmdP;
	*cmdP = NULL;

	/* Check state of connection and websocket */
	if (ctx->failed || !ctx->wss) {
		swclt_cmd_destroy(&cmd);
		return KS_STATUS_FAIL;
	}

	char *cmd_str = swclt_cmd_describe(cmd);
	ks_log(KS_LOG_DEBUG, "Submitting request: %s", cmd_str);
	ks_pool_free(&cmd_str);

	/* convert command to JSON string */
	char *data = NULL;
	if (status = swclt_cmd_print(cmd, ctx->pool, &data)) {
		ks_log(KS_LOG_CRIT, "Invalid command, failed to render payload string: %lu", status);
		swclt_cmd_destroy(&cmd);
		return KS_STATUS_FAIL;
	}

	/* Register this cmd in our outstanding requests if there is a reply */
	if (!(cmd->flags & SWCLT_CMD_FLAG_NOREPLY)) {
		swclt_cmd_cb_t cb;
		void *cb_data;
		if (status = swclt_cmd_cb(cmd, &cb, &cb_data)) {
			ks_log(KS_LOG_CRIT, "Failed to get command callback");
			swclt_cmd_destroy(&cmd);
			return KS_STATUS_FAIL;
		}
		if (!cb && cmd_future) {
			/* set up our own callbacks to wait if none exist */
			if (status = swclt_cmd_future_create(cmd_future, cmd)) {
				ks_log(KS_LOG_CRIT, "Failed to create command cmd_future");
				swclt_cmd_destroy(&cmd);
				return KS_STATUS_FAIL;
			}
		}

		if ((cb || cmd_future) && (status = register_cmd(ctx, &cmd))) {
			ks_log(KS_LOG_WARNING, "Failed to register cmd: %lu", status);
			swclt_cmd_future_destroy(cmd_future);
			swclt_cmd_destroy(&cmd);
			return status;
		}
	}

	/* Write the command data on the socket */
	if ((status = swclt_wss_write(ctx->wss, data))) {
		ks_log(KS_LOG_WARNING, "Failed to write to websocket: %lu, %s", status, data);
	}
	ks_pool_free(&data);

	/* destroy command if we didn't register it (it is not NULL) */
	swclt_cmd_destroy(&cmd);

	return status;
}

static ks_status_t on_incoming_request(swclt_conn_t *ctx, ks_json_t *payload, swclt_frame_t **frame)
{
	const char *method;
	ks_uuid_t id;
	swclt_cmd_t *cmd = NULL;
	ks_status_t status;

	/* Check state */
	if (ctx->failed || !ctx->wss) {
		return KS_STATUS_FAIL;
	}

	ks_log(KS_LOG_DEBUG, "Handling incoming request: %s", (*frame)->data);

	if (!(method = ks_json_get_object_string(payload, "method", NULL))) {
		ks_log(KS_LOG_WARNING, "Invalid response received: %s", (*frame)->data);
		return KS_STATUS_INVALID_ARGUMENT;
	}

	id = ks_uuid_from_str(ks_json_get_object_string(payload, "id", ""));
	if (ks_uuid_is_null(&id)) {
		ks_log(KS_LOG_WARNING, "Response missing id: %s", (*frame)->data);
		return KS_STATUS_INVALID_ARGUMENT;
	}

	/* Create the command */
	if (status = swclt_cmd_create_frame(
			&cmd,
			NULL,
			NULL,
			*frame,
			0,
			BLADE_METHOD_FLAGS(method))) {
		ks_log(KS_LOG_WARNING, "Failed to create command (status: %lu) from frame: %s", status, (*frame)->data);
		return status;
	}

	ks_log(KS_LOG_DEBUG, "Dispatching incoming request method: %s id: %s", method, ks_uuid_thr_str(&id));

	/* And we're in charge of the frame now, we copied it, so free it */
	ks_pool_free(frame);

	/* Send command to client */
	ctx->incoming_cmd_cb(ctx, cmd, ctx->incoming_cmd_cb_data);

	/* Free the command */
	swclt_cmd_destroy(&cmd);

	return KS_STATUS_SUCCESS;
}

static ks_status_t on_incoming_frame(swclt_wss_t *wss, swclt_frame_t **frame, swclt_conn_t *ctx)
{
	ks_json_t *payload = NULL;
	ks_status_t status = KS_STATUS_SUCCESS;
	const char *method;
	ks_uuid_t id;
	swclt_cmd_t *cmd = NULL;

	ks_log(KS_LOG_DEBUG, "Handling incoming frame: %s", (*frame)->data);

	/* Parse the json out of the frame to figure out what it is */
	if (status = swclt_frame_to_json(*frame, &payload)) {
		ks_log(KS_LOG_ERROR, "Failed to get frame json: %lu", status);
		goto done;
	}

	/* If it's a request, we need to raise this directly with the callback */
	if (ks_json_get_object_item(payload, "params")) {
		status = on_incoming_request(ctx, payload, frame);
		goto done;
	}

	/* Must be a reply, look up our outstanding request */
	id = ks_uuid_from_str(ks_json_get_object_string(payload, "id", ""));
	if (ks_uuid_is_null(&id)) {
		ks_log(KS_LOG_WARNING, "Received invalid payload, missing id: %s", (*frame)->data);
		status = KS_STATUS_INVALID_ARGUMENT;
		goto done;
	}

	if (!(cmd = deregister_cmd(ctx, id))) {
		/* Command probably timed out or we don't care about the reply, or a node sent bad data - 
		   this is completely fine and should not cause us to tear down the connection.
		 */
		ks_log(KS_LOG_DEBUG, "Could not locate cmd for frame: %s", (*frame)->data);
		status = KS_STATUS_SUCCESS;
		goto done;
	}

	if (status = swclt_cmd_method(cmd, &method)) {
		ks_log(KS_LOG_WARNING, "Failed to get command method: %lu", status);
		status = KS_STATUS_SUCCESS; // keep the connection ONLINE
		goto done;
	}

	/* Great, feed it the reply */
	if (status = swclt_cmd_parse_reply_frame(cmd, *frame)) {
		ks_log(KS_LOG_ERROR, "Failed to parse command reply: %lu", status);
		status = KS_STATUS_SUCCESS; // keep the connection ONLINE
		goto done;
	}

done:

	ks_pool_free(frame);

	if (payload) {
		ks_json_delete(&payload);
	}

	if (cmd) {
		char *cmd_str = swclt_cmd_describe(cmd);
		ks_log(KS_LOG_DEBUG, "Destroying command: %s", cmd_str);
		ks_pool_free(&cmd_str);
		swclt_cmd_destroy(&cmd);
	}

	if (status == KS_STATUS_INVALID_ARGUMENT) {
		status = KS_STATUS_SUCCESS; // keep the connection ONLINE
	}

	return status;
}

SWCLT_DECLARE(char *) swclt_conn_describe(swclt_conn_t *ctx)
{
	if (ctx) {
		return ks_psprintf(ctx->pool, "SWCLT Connection to %s:%d - ", ctx->info.wss.address, ctx->info.wss.port);
	}
	return NULL;
}

static ks_status_t do_logical_connect(swclt_conn_t *ctx,
									  ks_uuid_t previous_sessionid,
									  ks_json_t **authentication,
									  const char *agent,
									  const char *identity)
{
	swclt_cmd_t *cmd = CREATE_BLADE_CONNECT_CMD(ctx->pool, previous_sessionid, authentication, agent, identity);
	ks_status_t status = KS_STATUS_SUCCESS;
	ks_json_t *error = NULL;
	swclt_cmd_future_t *future = NULL;
	swclt_cmd_reply_t *reply = NULL;

	if (!cmd) {
		status = KS_STATUS_NO_MEM;
		goto done;
	}

	if (status = submit_request(ctx, &cmd, &future))
		goto done;

	if (!future) {
		ks_log(KS_LOG_CRIT, "No blade.connect future received");
		goto done;
	}
	status = swclt_cmd_future_get(future, &reply);
	swclt_cmd_future_destroy(&future);
	if (swclt_cmd_reply_ok(reply) != KS_STATUS_SUCCESS) {
		ks_log(KS_LOG_ERROR, "blade.connect failed");
		if (reply && reply->type == SWCLT_CMD_TYPE_ERROR) {
			error = reply->json;
		}
		goto done;
	}

	if (ctx->blade_connect_rpl) {
		BLADE_CONNECT_RPL_DESTROY(&ctx->blade_connect_rpl);
	}
	if (status = swclt_cmd_reply_parse(reply, ctx->pool,
								  (swclt_cmd_parse_cb_t)BLADE_CONNECT_RPL_PARSE, (void **)&ctx->blade_connect_rpl)) {
		ks_log(KS_LOG_ERROR, "Unable to parse connect reply");
		goto done;
	}

	/* Great snapshot our info types */
	ctx->info.sessionid = ctx->blade_connect_rpl->sessionid;
	ctx->info.nodeid = ks_pstrdup(ctx->pool, ctx->blade_connect_rpl->nodeid);
	ctx->info.master_nodeid = ctx->blade_connect_rpl->master_nodeid;

done:
	/* If the caller wants a call back for connect do that too */
	if (ctx->connect_cb) {
		if (status = ctx->connect_cb(ctx, error, ctx->blade_connect_rpl, ctx->connect_cb_data)) {
			ks_log(KS_LOG_WARNING, "Connect callback returned error: %lu", status);
		}
	}

	swclt_cmd_destroy(&cmd);
	swclt_cmd_reply_destroy(&reply);

	return status;
}

static void on_wss_failed(swclt_wss_t *wss, void *data)
{
	swclt_conn_t *conn = (swclt_conn_t *)data;
	report_connection_failure(conn);
}

static ks_status_t connect_wss(swclt_conn_t *ctx, ks_uuid_t previous_sessionid, ks_json_t **authentication, const char *agent, const char *identity)
{
	ks_status_t status;

	ks_log(KS_LOG_DEBUG, "Initiating websocket connection");

	/* First destroy the previous wss if set */
	if (ctx->wss) {
		if (!ks_uuid_is_null(&ctx->info.sessionid))
			ks_log(KS_LOG_DEBUG, "Destroying previous web socket handle, re-connecting with exiting sessionid: %s", ks_uuid_thr_str(&ctx->info.sessionid));
		else
			ks_log(KS_LOG_DEBUG, "Destroying previous web socket handle, re-connecting with new sessionid");

		swclt_wss_destroy(&ctx->wss);
	}

	if (!ctx->info.wss.port) {
		ks_log(KS_LOG_INFO, "Port not specified, defaulting to 2100");
		ctx->info.wss.port = 2100;
	}

	ks_log(KS_LOG_INFO, "Connecting to %s:%d/%s", ctx->info.wss.address, ctx->info.wss.port, ctx->info.wss.path);

	/* Create our websocket transport */
	if (status = swclt_wss_connect(ctx->pool, &ctx->wss,
			(swclt_wss_incoming_frame_cb_t)on_incoming_frame, ctx,
			(swclt_wss_failed_cb_t)on_wss_failed, ctx,
			ctx->info.wss.address, ctx->info.wss.port, ctx->info.wss.path, ctx->info.wss.connect_timeout_ms, ctx->info.wss.ssl))
		return status;

	/* Create TTL tracking thread */
	ttl_tracker_create(ctx->pool, &ctx->ttl, ctx);

	/* Now perform a logical connect to blade with the connect request */
	if (status = do_logical_connect(ctx, previous_sessionid, authentication, agent, identity))
		return status;

	return status;
}

SWCLT_DECLARE(void) swclt_conn_destroy(swclt_conn_t **conn)
{
	if (conn && *conn) {
		if ((*conn)->blade_connect_rpl) {
			BLADE_CONNECT_RPL_DESTROY(&(*conn)->blade_connect_rpl);
		}
		ttl_tracker_destroy(&(*conn)->ttl);
		swclt_wss_destroy(&(*conn)->wss);
		ks_hash_destroy(&(*conn)->outstanding_requests);
		ks_mutex_destroy(&(*conn)->failed_mutex);
		ks_pool_free(conn);
	}
}

SWCLT_DECLARE(ks_status_t) swclt_conn_connect_ex(
	ks_pool_t *pool,
	swclt_conn_t **conn,
	swclt_conn_incoming_cmd_cb_t incoming_cmd_cb,
	void *incoming_cmd_cb_data,
	swclt_conn_connect_cb_t connect_cb,
	void *connect_cb_data,
	swclt_conn_failed_cb_t failed_cb,
	void *failed_cb_data,
	swclt_ident_t *ident,
	ks_uuid_t previous_sessionid,
	ks_json_t **authentication,
	const char *agent,
	const char *identity,
	const SSL_CTX *ssl)
{
	ks_status_t status = KS_STATUS_SUCCESS;

	swclt_conn_t *new_conn = ks_pool_alloc(pool, sizeof(swclt_conn_t));
	new_conn->pool = pool;

	ks_log(KS_LOG_INFO, "Initiating connection to: %s (parsed port: %u) at /%s", ident->host, (unsigned int)ident->portnum, ident->path ? ident->path : "");

	new_conn->incoming_cmd_cb = incoming_cmd_cb;
	new_conn->incoming_cmd_cb_data = incoming_cmd_cb_data;
	new_conn->connect_cb = connect_cb;
	new_conn->connect_cb_data = connect_cb_data;
	new_conn->failed_cb = failed_cb;
	new_conn->failed_cb_data = failed_cb_data;

	/* Fill in the info on behalf of wss so we can re-use the same connect api
	 * either during a reconnect or an initial connect */
	strncpy(new_conn->info.wss.address, ident->host, sizeof(new_conn->info.wss.address));
	new_conn->info.wss.port = ident->portnum;
	new_conn->info.wss.ssl = (SSL_CTX *)ssl;
	if (ident->path) strncpy(new_conn->info.wss.path, ident->path, sizeof(new_conn->info.wss.path));
	new_conn->info.wss.connect_timeout_ms = 10000;

	/* Create our request hash */
	if (status = ks_hash_create(&new_conn->outstanding_requests, KS_HASH_MODE_UUID,
			KS_HASH_FLAG_DUP_CHECK | KS_HASH_FLAG_FREE_KEY, new_conn->pool))
		goto done;

	ks_mutex_create(&new_conn->failed_mutex, KS_MUTEX_FLAG_DEFAULT, new_conn->pool);

	/* Connect our websocket */
	if (status = connect_wss(new_conn, previous_sessionid, authentication, agent, identity))
		goto done;

done:
	if (status != KS_STATUS_SUCCESS) {
		swclt_conn_destroy(&new_conn);
	}
	*conn = new_conn;
	return status;
}

SWCLT_DECLARE(ks_status_t) swclt_conn_connect(
	ks_pool_t *pool,
	swclt_conn_t **conn,
	swclt_conn_incoming_cmd_cb_t incoming_cmd_cb,
	void *incoming_cmd_cb_data,
	swclt_ident_t *ident,
	ks_json_t **authentication,
	const char *agent,
	const char *identity,
	const SSL_CTX *ssl)
{
	return swclt_conn_connect_ex(
		pool,
		conn,
		incoming_cmd_cb,
		incoming_cmd_cb_data,
		NULL,
		NULL,
		NULL,
		NULL,
		ident,
		ks_uuid_null(),
		authentication,
		agent,
		identity,
		ssl);
}

SWCLT_DECLARE(ks_status_t) swclt_conn_submit_result(swclt_conn_t *conn, swclt_cmd_t *cmd)
{
	if (conn) {
		return submit_result(conn, cmd);
	}
	return KS_STATUS_FAIL;
}

SWCLT_DECLARE(ks_status_t) swclt_conn_submit_request(swclt_conn_t *conn, swclt_cmd_t **cmd, swclt_cmd_future_t **future)
{
	if (conn) {
		return submit_request(conn, cmd, future);
	}
	return KS_STATUS_FAIL;
}

/* Private due to un-implemented caller ownership semantics, internal use only */
ks_status_t swclt_conn_info(swclt_conn_t *conn, swclt_conn_info_t *info)
{
	memcpy(info, &conn->info, sizeof(conn->info));
	return KS_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
