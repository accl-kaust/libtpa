/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, Krishnan Iyer
 *
 * Trace replay (-E): one connection sends the rows of a CSV trace to the
 * FPGA in order, in 512-byte pieces. Each row goes out sleep_time seconds
 * after the response to the row before it: request_size bytes, the FRAC
 * header among them.
 */
#include <stdio.h>
#include <ctype.h>

#include "fperf.h"

#define TRACE_HEADER		"app,sleep_time,request_size,response_size"

static void trace_error(const char *path, int lineno, const char *what)
{
	fprintf(stderr, "error: %s:%d: %s\n", path, lineno, what);
	exit(1);
}

/* strip the line ending, \r\n included */
static char *chomp(char *line)
{
	line[strcspn(line, "\r\n")] = '\0';
	return line;
}

/* parse an unsigned number that ends at @sep; return what follows @sep */
static char *parse_ulong(char *p, unsigned long *val, char sep)
{
	char *end;

	if (!isdigit((unsigned char)*p))
		return NULL;

	errno = 0;
	*val = strtoul(p, &end, 10);
	if (errno || *end != sep)
		return NULL;

	return end + 1;
}

/* fill @e from one "app,sleep_time,request_size,response_size" row */
static const char *parse_row(char *p, struct trace_entry *e)
{
	static char err[64];
	unsigned long func;
	unsigned long req;
	unsigned long resp;
	double gap;
	char *end;
	int slot;

	p = parse_ulong(p, &func, ',');
	if (!p)
		return "bad app";

	gap = strtod(p, &end);
	if (end == p || *end != ',' || !(gap >= 0 && gap < 1e9))
		return "bad sleep_time";
	p = end + 1;

	p = parse_ulong(p, &req, ',');
	if (!p || req > UINT32_MAX)
		return "bad request_size";
	if (req <= FRAC_HDR_SIZE || req % FRAC_LINE_SIZE)
		return "request_size is not the header and whole 64-byte data lines";

	/* read, but not used: see the answer size below */
	if (!parse_ulong(p, &resp, '\0'))
		return "bad response_size";

	/* the app column is a function; the slot map says where it runs */
	slot = func > UINT16_MAX ? -1 : func_slot(func);
	if (slot < 0) {
		snprintf(err, sizeof(err), "no slot holds app %lu, see -M", func);
		return err;
	}

	if (func == NORM && req > FRAC_NORM_MAX_REQ_SIZE) {
		snprintf(err, sizeof(err), "norm never finishes a request over %d bytes",
			 FRAC_NORM_MAX_REQ_SIZE);
		return err;
	}

	e->gap_ns = (uint64_t)(gap * 1e9 + 0.5);
	e->req_size = req;
	/*
	 * The unit decides the answer's size. With the header counted in
	 * request_size, log and norm answer 64 bytes less than response_size
	 * says, one line for each data line.
	 */
	e->response_size = func_response_size(func, req);
	e->func = func;
	e->slot = slot;

	return NULL;
}

void trace_load(const char *path)
{
	struct trace_entry *trace = NULL;
	uint32_t size = 0;
	uint32_t nr = 0;
	char line[256];
	const char *err;
	int lineno = 1;
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "error: failed to open trace %s: %s\n", path, strerror(errno));
		exit(1);
	}

	if (!fgets(line, sizeof(line), f) || strcmp(chomp(line), TRACE_HEADER) != 0)
		trace_error(path, lineno, "expected the header " TRACE_HEADER);

	while (fgets(line, sizeof(line), f)) {
		lineno += 1;

		if (!strchr(line, '\n') && !feof(f))
			trace_error(path, lineno, "line too long");
		if (*chomp(line) == '\0')
			continue;

		if (nr == size) {
			size = size ? size * 2 : 4096;
			trace = realloc(trace, size * sizeof(*trace));
			assert(trace != NULL);
		}

		err = parse_row(line, &trace[nr]);
		if (err)
			trace_error(path, lineno, err);
		nr += 1;
	}
	fclose(f);

	if (nr == 0)
		trace_error(path, lineno, "no rows");

	ctx.trace = trace;
	ctx.nr_trace = nr;
}

static void trace_load_row(struct connection *conn)
{
	const struct trace_entry *e = &ctx.trace[conn->trace_idx];

	conn->func = e->func;
	conn->slot = e->slot;
	conn->req_size = e->req_size;
	conn->response_size = e->response_size;
	conn->read.budget = e->response_size;
}

static void trace_send(struct connection *conn)
{
	conn->trace_wait = 0;
	conn->write.budget = conn->message_size;
	event_queue_add(conn, TPA_EVENT_OUT);
}

/*
 * Row 0 has no response to wait for: it goes out on the first TPA_EVENT_OUT,
 * which libtpa raises once the connection is established. A write before
 * that fails with ENOTCONN, which closes the connection.
 */
void trace_conn_init(struct connection *conn)
{
	conn->trace_idx = 0;
	conn->trace_wait = 0;
	trace_load_row(conn);
	conn->write.budget = conn->message_size;
}

/* called once the first piece of the current row is written */
void trace_on_send(struct connection *conn)
{
	const struct trace_entry *e = &ctx.trace[conn->trace_idx];
	char line[192];
	int len;

	if (!conn->thread->log)
		return;

	len = snprintf(line, sizeof(line),
		       "[%lu] trace_send func=%u msg_bytes=%d req_bytes=%u resp_bytes=%u sleep_sec=%f sid=%d\n",
		       get_time_in_ns(), e->func, conn->message_size, e->req_size,
		       e->response_size, e->gap_ns / 1e9, conn->sid);
	log_append(conn->thread, line, len);
}

/* called once the whole response to the current row is in */
void trace_on_response(struct connection *conn, uint64_t latency)
{
	char line[128];
	int len;

	if (conn->thread->log) {
		/* update_latency() left the response time in last_ns */
		len = snprintf(line, sizeof(line), "[%lu] trace_resp bytes=%u latency_us=%.3f\n",
			       conn->last_ns, conn->response_size, latency / 1e3);
		log_append(conn->thread, line, len);
	}

	conn->trace_idx += 1;
	if (conn->trace_idx == ctx.nr_trace) {
		printf("trace: all %u rows replayed\n", ctx.nr_trace);
		ctx.trace_done = 1;
		return;
	}

	trace_load_row(conn);
	conn->next_send_ns = conn->last_ns + ctx.trace[conn->trace_idx].gap_ns;
	if (ctx.trace[conn->trace_idx].gap_ns == 0)
		trace_send(conn);
	else
		conn->trace_wait = 1;
}

/*
 * The answer to the current row is longer than its unit answers, so the
 * replay cannot go on: say which row, and end it.
 */
void trace_wrong_answer(struct connection *conn)
{
	const struct trace_entry *e = &ctx.trace[conn->trace_idx];

	if (!ctx.trace_done)
		fprintf(stderr, "trace: %s:%u: app %u in slot %u got %zu answer bytes to its %u-byte "
			"request, not %u: stopping\n", ctx.trace_file, conn->trace_idx + 2, e->func,
			e->slot, conn->read.off, e->req_size, e->response_size);
	ctx.trace_done = 1;
}

/* send the waiting row once its sleep_time has passed */
void trace_release(struct test_thread *thread)
{
	struct connection *conn;

	TAILQ_FOREACH(conn, &thread->conn_list, thread_node) {
		if (conn->trace_wait && get_time_in_ns() >= conn->next_send_ns)
			trace_send(conn);
	}
}
