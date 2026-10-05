/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Trace replay (-E): one connection sends the rows of a CSV trace to the
 * FPGA in order. Each row goes out sleep_time seconds after the response
 * to the row before it.
 */
#include <stdio.h>
#include <ctype.h>

#include "fperf.h"

#define TRACE_HEADER		"app,sleep_time,request_size,response_size"

/*
 * The app column names a function, the FPGA runs it by slot. pkt_logic.v
 * routes a slot id past its last cell to cell 0, so an app missing here
 * is rejected rather than sent there.
 */
static const struct {
	uint16_t func;
	uint16_t slot;
} func_slots[] = {
	{ TOPK,  0 },
	{ LOGIT, 1 },
	{ CNN,   2 },
	{ NORM,  3 },
};

static int func_to_slot(unsigned long func)
{
	size_t i;

	for (i = 0; i < sizeof(func_slots) / sizeof(func_slots[0]); i++) {
		if (func_slots[i].func == func)
			return func_slots[i].slot;
	}

	return -1;
}

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
	if (!p || req == 0 || req > UINT32_MAX - FRAC_HDR_SIZE)
		return "bad request_size";
	if (req % FRAC_LINE_SIZE)
		return "request_size is not whole 64-byte lines";

	if (!parse_ulong(p, &resp, '\0') || resp == 0 || resp > UINT32_MAX)
		return "bad response_size";

	slot = func_to_slot(func);
	if (slot < 0) {
		snprintf(err, sizeof(err), "app %lu has no slot in func_slots[]", func);
		return err;
	}

	e->gap_ns = (uint64_t)(gap * 1e9 + 0.5);
	/* request_size is the payload; the FRAC header goes in front of it */
	e->req_size = req + FRAC_HDR_SIZE;
	e->response_size = resp;
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

/* send the waiting row once its sleep_time has passed */
void trace_release(struct test_thread *thread)
{
	struct connection *conn;

	TAILQ_FOREACH(conn, &thread->conn_list, thread_node) {
		if (conn->trace_wait && get_time_in_ns() >= conn->next_send_ns)
			trace_send(conn);
	}
}
