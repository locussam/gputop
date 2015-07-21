/*
 * GPU Top
 *
 * Copyright (C) 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <config.h>

#include <linux/perf_event.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/stat.h>

#include <uv.h>

#include <h2o.h>
#include <h2o/websocket.h>
#include <wslay/wslay_event.h>

#include "gputop-server.h"
#include "gputop-perf.h"
#include "gputop-gl.h"
#include "gputop-util.h"
#include "gputop-ui.h"
#include "gputop.pb-c.h"

static h2o_websocket_conn_t *h2o_conn;
static h2o_globalconf_t config;
static h2o_context_t ctx;
static SSL_CTX *ssl_ctx;
static uv_tcp_t listener;

static uv_timer_t timer;

enum {
    WS_MESSAGE_PERF = 1,
    WS_MESSAGE_PROTOBUF,
};

struct protobuf_msg_closure;

typedef void (*gputop_closure_done_t)(struct protobuf_msg_closure *closure);

struct protobuf_msg_closure {
    int current_offset;
    int len;
    uint8_t *data;
    gputop_closure_done_t done_callback;
};

static ssize_t
fragmented_protobuf_msg_read_cb(wslay_event_context_ptr ctx,
				uint8_t *data, size_t len,
				const union wslay_event_msg_source *source,
				int *eof,
				void *user_data)
{
    struct protobuf_msg_closure *closure =
        (struct protobuf_msg_closure *)source->data;
    int remaining;
    int read_len;
    int total = 0;

    if (closure->current_offset == 0) {
	assert(len > 8);
	data[0] = WS_MESSAGE_PROTOBUF;
	total = 8;
	data += 8;
	len -= 8;
    }

    remaining = closure->len - closure->current_offset;
    read_len = MIN(remaining, len);

    memcpy(data, closure->data + closure->current_offset, read_len);
    closure->current_offset += read_len;
    total += read_len;

    if(closure->current_offset == closure->len)
        *eof = 1;

    return total;
}

static void
on_protobuf_msg_sent_cb(const union wslay_event_msg_source *source, void *user_data)
{
    struct protobuf_msg_closure *closure = (void *)source->data;

    free(closure->data);
    free(closure);
}

static gputop_list_t perf_streams;
static gputop_list_t closing_streams;

/*
 * FIXME: don't duplicate these...
 */

#define MAX_OA_PERF_SAMPLE_SIZE (8 +   /* perf_event_header */       \
                                 4 +   /* raw_size */                \
                                 256 + /* raw OA counter snapshot */ \
                                 4)    /* alignment padding */

#define TAKEN(HEAD, TAIL, POT_SIZE)	(((HEAD) - (TAIL)) & (POT_SIZE - 1))

/* Note: this will equate to 0 when the buffer is exactly full... */
#define REMAINING(HEAD, TAIL, POT_SIZE) (POT_SIZE - TAKEN (HEAD, TAIL, POT_SIZE))

#if defined(__i386__)
#define rmb()           __asm__ volatile("lock; addl $0,0(%%esp)" ::: "memory")
#define mb()            __asm__ volatile("lock; addl $0,0(%%esp)" ::: "memory")
#endif

#if defined(__x86_64__)
#define rmb()           __asm__ volatile("lfence" ::: "memory")
#define mb()            __asm__ volatile("mfence" ::: "memory")
#endif


struct perf_flush_closure {
    bool header_written;
    int id;
    int total_len;
    struct gputop_perf_stream *stream;
    uint64_t head;
    uint64_t tail;
};

static unsigned int
read_perf_head(struct perf_event_mmap_page *mmap_page)
{
    unsigned int head = (*(volatile uint64_t *)&mmap_page->data_head);
    rmb();

    return head;
}

static void
write_perf_tail(struct perf_event_mmap_page *mmap_page,
                unsigned int tail)
{
    /* Make sure we've finished reading all the sample data we
     * we're consuming before updating the tail... */
    mb();
    mmap_page->data_tail = tail;
}

static int flushing_perf = 0;

static void
on_perf_flush_done(const union wslay_event_msg_source *source, void *user_data)
{
    struct perf_flush_closure *closure =
        (struct perf_flush_closure *)source->data;

    //fprintf(stderr, "wrote perf message: len=%d\n", closure->total_len);
    gputop_perf_stream_unref(closure->stream);
    free(closure);
    flushing_perf--;
}

static ssize_t
fragmented_perf_read_cb(wslay_event_context_ptr ctx,
			uint8_t *data, size_t len,
			const union wslay_event_msg_source *source,
			int *eof,
			void *user_data)
{
    struct perf_flush_closure *closure =
        (struct perf_flush_closure *)source->data;
    struct gputop_perf_stream *stream = closure->stream;
    const uint64_t mask = stream->buffer_size - 1;
    int read_len;
    int total = 0;
    uint64_t head;
    uint64_t tail;
    uint64_t remainder;
    uint8_t *buffer;
    uint8_t *p;

    if (!closure->header_written) {
	assert(len > 8);
	data[0] = WS_MESSAGE_PERF;
	data[1] = closure->id;
	total = 8;
	data += 8;
	len -= 8;
	closure->header_written = true;
    }

    head = closure->head;
    tail = closure->tail;

    buffer = stream->buffer;

    if ((head & mask) < (tail & mask)) {
	int before;

	p = buffer + (tail & mask);
	before = stream->buffer_size - (tail & mask);
	read_len = MIN(before, len);
	memcpy(data, p, read_len);

	len -= read_len;
	tail += read_len;
	data += read_len;
	total += read_len;

	closure->tail = tail;
    }

    p = buffer + (tail & mask);
    remainder = TAKEN(head, tail, stream->buffer_size);
    read_len = MIN(remainder, len);
    memcpy(data, p, read_len);

    len -= read_len;
    tail += read_len;
    total += read_len;
    closure->tail = tail;

    closure->total_len += total;

    if (TAKEN(head, tail, stream->buffer_size) == 0) {
	*eof = 1;
	write_perf_tail(stream->mmap_page, tail);
    }

    return total;
}

static void
flush_stream_samples(struct gputop_perf_stream *stream)
{
    uint64_t head;
    uint64_t tail;

    if (fsync(stream->fd) < 0)
	dbg("Failed to flush i915_oa perf samples");

    head = read_perf_head(stream->mmap_page);
    tail = stream->mmap_page->data_tail;

    if (TAKEN(head, tail, stream->buffer_size)) {
	struct perf_flush_closure *closure;
	struct wslay_event_fragmented_msg msg;

	flushing_perf++;

	/* Ensure the stream can't be freed while we're in the
	 * middle of forwarding samples... */
	gputop_perf_stream_ref(stream);

	//gputop_perf_print_records(stream, head, tail, false);

	closure = xmalloc(sizeof(*closure));
	closure->header_written = false;
	closure->id = stream->user.id;
	closure->total_len = 0;
	closure->stream = stream;
	closure->head = head;
	closure->tail = tail;

	memset(&msg, 0, sizeof(msg));
	msg.opcode = WSLAY_BINARY_FRAME;
	msg.source.data = closure;
	msg.read_callback = fragmented_perf_read_cb;
	msg.finish_callback = on_perf_flush_done;

	wslay_event_queue_fragmented_msg(h2o_conn->ws_ctx, &msg);

	wslay_event_send(h2o_conn->ws_ctx);
    }
}

static void
flush_perf_samples(void)
{
    struct gputop_perf_stream *stream;

    if (flushing_perf) {
	fprintf(stderr, "Throttling websocket forwarding");
	return;
    }

    gputop_list_for_each(stream, &perf_streams, user.link) {
	flush_stream_samples(stream);
    }
}

static void
send_pb_message(h2o_websocket_conn_t *conn, ProtobufCMessage *pb_message)
{
    struct wslay_event_fragmented_msg msg;
    struct protobuf_msg_closure *closure;

    closure = xmalloc(sizeof(*closure));
    closure->current_offset = 0;
    closure->len = protobuf_c_message_get_packed_size(pb_message);
    closure->data = xmalloc(closure->len);

    protobuf_c_message_pack(pb_message, closure->data);

    msg.opcode = WSLAY_BINARY_FRAME;
    msg.source.data = closure;
    msg.read_callback = fragmented_protobuf_msg_read_cb;
    msg.finish_callback = on_protobuf_msg_sent_cb;

    wslay_event_queue_fragmented_msg(conn->ws_ctx, &msg);
    wslay_event_send(conn->ws_ctx);
}

static void
timer_cb(uv_timer_t *timer)
{
    Gputop__Log *log;

    flush_perf_samples();

    log = gputop_get_pb_log();
    if (log) {
	Gputop__Message msg = GPUTOP__MESSAGE__INIT;

	fprintf(stderr, "forwarding log to UI\n");

	msg.cmd_case = GPUTOP__MESSAGE__CMD_LOG;
	msg.log = log;

	send_pb_message(h2o_conn, &msg.base);

	gputop_pb_log_free(log);
    }
}

static void
perf_ready_cb(uv_poll_t *poll, int status, int events)
{
    /* Currently we just rely on periodic flusing instead
     *
     * flush_perf_samples();
     */
}

static void
stream_close_cb(struct gputop_perf_stream *stream)
{
    Gputop__Message message = GPUTOP__MESSAGE__INIT;
    Gputop__CloseNotify notify = GPUTOP__CLOSE_NOTIFY__INIT;

    notify.id = stream->user.id;
    message.cmd_case = GPUTOP__MESSAGE__CMD_CLOSE_NOTIFY;
    message.close_notify = &notify;

    send_pb_message(h2o_conn, &message.base);

    gputop_list_remove(&stream->user.link);
}

static void
handle_open_i915_oa_query(h2o_websocket_conn_t *conn,
			  uint32_t id,
			  Gputop__OAQueryInfo *oa_query_info)
{
    if (gputop_perf_initialize()) {
	int page_size = sysconf(_SC_PAGE_SIZE);
	struct gputop_perf_query *perf_query;
	struct gputop_perf_stream *stream;

	perf_query = &perf_queries[oa_query_info->metric_set];

	stream = gputop_perf_open_i915_oa_query(perf_query,
						oa_query_info->period_exponent,
						32 * page_size,
						perf_ready_cb,
						oa_query_info->overwrite);
	if (stream) {
	    stream->user.id = id;
	    stream->user.destroy_cb = stream_close_cb;
	    gputop_list_init(&stream->user.link);
	    gputop_list_insert(perf_streams.prev, &stream->user.link);

	    uv_timer_start(&timer, timer_cb, 200, 200);
	} else
	    dbg("Failed to open perf query set=%d period=%d: %s\n",
		oa_query_info->metric_set, oa_query_info->period_exponent,
		perf_query->error);
    }
}

static void
handle_open_query(h2o_websocket_conn_t *conn,
		  Gputop__OpenQuery *open_query)
{
    if (open_query->type_case == GPUTOP__OPEN_QUERY__TYPE_OA_QUERY) {
	handle_open_i915_oa_query(conn, open_query->id, open_query->oa_query);
    } else
	fprintf(stderr, "TODO: support opening GL queries");
}

static void
close_stream(struct gputop_perf_stream *stream)
{
    /* NB: we can't synchronously close the perf event since
     * we may be in the middle of writting samples to the
     * websocket.
     *
     * By moving the stream into the closing_streams list
     * we ensure we won't forward anymore for the stream.
     */
    gputop_list_remove(&stream->user.link);
    gputop_list_insert(closing_streams.prev, &stream->user.link);
    gputop_perf_stream_unref(stream);
}

static void
close_all_streams(void)
{
    struct gputop_perf_stream *stream, *tmp;

    gputop_list_for_each_safe(stream, tmp, &perf_streams, user.link) {
	close_stream(stream);
    }
}

static void
handle_close_query(h2o_websocket_conn_t *conn, uint32_t id)
{
    struct gputop_perf_stream *stream;

    gputop_list_for_each(stream, &perf_streams, user.link) {
	if (stream->user.id == id) {
	    close_stream(stream);
	    return;
	}
    }
}

static void
handle_get_features(h2o_websocket_conn_t *conn)
{
    Gputop__Message message = GPUTOP__MESSAGE__INIT;
    Gputop__Features features = GPUTOP__FEATURES__INIT;
    Gputop__DevInfo devinfo = GPUTOP__DEV_INFO__INIT;

    if (!gputop_perf_initialize()) {
	dbg("Failed to initialize perf\n");
	return;
    }

    devinfo.devid = gputop_devinfo.devid;
    devinfo.n_eus = gputop_devinfo.n_eus;
    devinfo.n_eu_slices = gputop_devinfo.n_eu_slices;
    devinfo.n_eu_sub_slices = gputop_devinfo.n_eu_sub_slices;
    devinfo.n_samplers = gputop_devinfo.n_samplers;

    features.devinfo = &devinfo;
    features.has_gl_performance_query = gputop_has_intel_performance_query_ext;
    features.has_i915_oa = true;

    message.cmd_case = GPUTOP__MESSAGE__CMD_FEATURES;
    message.features = &features;

    send_pb_message(conn, &message.base);
}

static void on_ws_message(h2o_websocket_conn_t *conn,
			  const struct wslay_event_on_msg_recv_arg *arg)
{
    fprintf(stderr, "on_ws_message\n");
    dbg("on_ws_message\n");

    if (arg == NULL) {
	dbg("socket closed\n");
	close_all_streams();
        h2o_websocket_close(conn);
        return;
    }

    if (!wslay_is_ctrl_frame(arg->opcode)) {
	Gputop__Request *request =
	    (void *)protobuf_c_message_unpack(&gputop__request__descriptor,
					      NULL, /* default allocator */
					      arg->msg_length,
					      arg->msg);

	if (!request) {
	    fprintf(stderr, "Failed to unpack message\n");
	    dbg("Failed to unpack message\n");
	    return;
	}

	switch (request->req_case) {
	case GPUTOP__REQUEST__REQ_GET_FEATURES:
	    fprintf(stderr, "GetFeatures request received\n");
	    handle_get_features(conn);
	    break;
	case GPUTOP__REQUEST__REQ_OPEN_QUERY:
	    fprintf(stderr, "OpenQuery request received\n");
	    handle_open_query(conn, request->open_query);
	    break;
	case GPUTOP__REQUEST__REQ_CLOSE_QUERY:
	    fprintf(stderr, "CloseQuery request received\n");
	    handle_close_query(conn, request->close_query);
	    break;
	case GPUTOP__REQUEST__REQ__NOT_SET:
	    assert(0);
	}

	free(request);
    }
}

static int on_req(h2o_handler_t *self, h2o_req_t *req)
{
    const char *client_key;
    ssize_t proto_header_index;

    dbg("on_req\n");

    if (h2o_is_websocket_handshake(req, &client_key) != 0 || client_key == NULL) {
        return -1;
    }

    proto_header_index = h2o_find_header_by_str(&req->headers,
                                                "sec-websocket-protocol",
                                                strlen("sec-websocket-protocol"),
                                                SIZE_MAX);
    if (proto_header_index != -1) {
        dbg("sec-websocket-protocols found\n");
        h2o_add_header_by_str(&req->pool, &req->res.headers,
                              "sec-websocket-protocol",
                              strlen("sec-websocket-protocol"),
                              0, "binary", strlen("binary"));
    }

    h2o_conn = h2o_upgrade_to_websocket(req, client_key, NULL, on_ws_message);

    return 0;
}

static void on_connect(uv_stream_t *server, int status)
{
    uv_tcp_t *conn;
    h2o_socket_t *sock;

    dbg("on_connect\n");

    if (status != 0)
        return;

    conn = h2o_mem_alloc(sizeof(*conn));
    uv_tcp_init(server->loop, conn);
    if (uv_accept(server, (uv_stream_t *)conn) != 0) {
        uv_close((uv_handle_t *)conn, (uv_close_cb)free);
        return;
    }

    sock = h2o_uv_socket_create((uv_stream_t *)conn, NULL, 0, (uv_close_cb)free);
    if (ssl_ctx != NULL)
        h2o_accept_ssl(&ctx, ctx.globalconf->hosts, sock, ssl_ctx);
    else
        h2o_http1_accept(&ctx, ctx.globalconf->hosts, sock);
}

static int setup_ssl(const char *cert_file, const char *key_file)
{
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    ssl_ctx = SSL_CTX_new(SSLv23_server_method());
    SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2);

    /* load certificate and private key */
    if (SSL_CTX_use_certificate_file(ssl_ctx, cert_file, SSL_FILETYPE_PEM) != 1) {
        dbg("an error occurred while trying to load server certificate file:%s\n", cert_file);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        dbg("an error occurred while trying to load private key file:%s\n", key_file);
        return -1;
    }

    return 0;
}

static h2o_iovec_t cache_control;
static h2o_headers_command_t uncache_cmd[2];

bool gputop_server_run(void)
{
    uv_loop_t *loop;
    struct sockaddr_in sockaddr;
    h2o_hostconf_t *hostconf;
    h2o_pathconf_t *pathconf;
    h2o_pathconf_t *root;
    int r;

    gputop_list_init(&perf_streams);
    gputop_list_init(&closing_streams);

    loop = gputop_ui_loop;

    uv_timer_init(gputop_ui_loop, &timer);

    if ((r = uv_tcp_init(loop, &listener)) != 0) {
        dbg("uv_tcp_init:%s\n", uv_strerror(r));
        goto error;
    }
    uv_ip4_addr("127.0.0.1", 7890, &sockaddr);
    if ((r = uv_tcp_bind(&listener, (struct sockaddr *)&sockaddr, sizeof(sockaddr))) != 0) {
        dbg("uv_tcp_bind:%s\n", uv_strerror(r));
        goto error;
    }
    if ((r = uv_listen((uv_stream_t *)&listener, 128, on_connect)) != 0) {
        dbg("uv_listen:%s\n", uv_strerror(r));
        goto error;
    }
    printf("http://localhost:7890\n");


    h2o_config_init(&config);
    hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 7890);
    pathconf = h2o_config_register_path(hostconf, "/gputop");
    h2o_create_handler(pathconf, sizeof(h2o_handler_t))->on_req = on_req;

    root = h2o_config_register_path(hostconf, "/");
    h2o_file_register(root, GPUTOP_WEB_ROOT, NULL, NULL, 0);

    cache_control = h2o_iovec_init(H2O_STRLIT("Cache-Control"));
    uncache_cmd[0].cmd = H2O_HEADERS_CMD_APPEND;
    uncache_cmd[0].name = &cache_control;
    uncache_cmd[0].value.base = "no-store";
    uncache_cmd[0].value.len = strlen("no-store");
    uncache_cmd[1].cmd = H2O_HEADERS_CMD_NULL;

    h2o_headers_register(root, uncache_cmd);

    h2o_context_init(&ctx, loop, &config);

    /* disabled by default: uncomment the block below to use HTTPS instead of HTTP */
    /*
    if (setup_ssl("server.crt", "server.key") != 0)
        goto Error;
    */

    return true;

error:
    return false;
}