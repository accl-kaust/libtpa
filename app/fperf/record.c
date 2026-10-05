/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Recording (-r): requests carry random data suited to the unit in their
 * slot, and every request and response is written to
 * <-D dir>/record_thread_<id>.bin for app/fperf/scripts/checkrecord.go to
 * check.
 *
 * The file is a struct record_file_hdr, then records, each a struct
 * record_hdr and its bytes. A request, FRAC header included, is written
 * once all of it is handed to the stack, and its response once all of it
 * is in, each with one writev(): a run that hangs or is killed keeps every
 * exchange before that, and the request left unanswered.
 */
#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/uio.h>

#include "fperf.h"

#define RECORD_MAGIC		"FPERFREC"
#define RECORD_VERSION		1

enum {
	RECORD_REQUEST = 1,
	RECORD_RESPONSE = 2,
};

struct record_file_hdr {
	char magic[8];
	uint32_t version;
	uint32_t message_size;
	uint64_t seed;
	uint16_t slot_func[FRAC_NR_SLOTS];	/* function of the unit in each slot, 0 for none */
};

struct record_hdr {
	uint32_t type;
	uint32_t sid;
	uint64_t seq;		/* per connection: pairs a response with its request */
	uint64_t time_ns;	/* request handed to the stack, or response all in */
	uint32_t len;		/* bytes that follow */
	uint32_t reserved;
};

/* checkrecord.go reads them as laid out here, little endian */
_Static_assert(sizeof(struct record_file_hdr) == 32, "record file header layout");
_Static_assert(sizeof(struct record_hdr) == 32, "record header layout");

/* splitmix64 */
static uint64_t rand_next(uint64_t *state)
{
	uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static void reserve(uint8_t **buf, uint32_t *cap, size_t size)
{
	if (*cap >= size)
		return;

	*buf = realloc(*buf, size);
	assert(*buf != NULL);
	*cap = size;
}

/*
 * Without -E every request goes to the -K slot, so the unit there decides
 * the data and how long the answer is.
 */
void record_setup(void)
{
	if (ctx.trace_file)
		return;

	ctx.func = slot_func(ctx.slot);
	if (!ctx.func) {
		fprintf(stderr, "error: -r: slot %d holds no unit fperf knows, see -M\n", ctx.slot);
		exit(1);
	}

	if (ctx.req_size <= FRAC_HDR_SIZE) {
		fprintf(stderr, "error: -r: a request needs a data line after the header\n");
		exit(1);
	}

	if (ctx.func == NORM && ctx.req_size > FRAC_NORM_MAX_REQ_SIZE) {
		fprintf(stderr, "error: -r: norm never finishes a request over %d bytes\n",
			FRAC_NORM_MAX_REQ_SIZE);
		exit(1);
	}

	ctx.response_size = func_response_size(ctx.func, ctx.req_size);
}

void record_open(struct test_thread *thread)
{
	struct record_file_hdr hdr;
	char path[PATH_MAX];
	int i;

	snprintf(path, sizeof(path), "%s/record_thread_%d.bin", thread->log_dir, thread->id);
	thread->record_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (thread->record_fd < 0) {
		fprintf(stderr, "error: failed to create %s: %s\n", path, strerror(errno));
		exit(1);
	}

	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, RECORD_MAGIC, sizeof(hdr.magic));
	hdr.version = RECORD_VERSION;
	hdr.message_size = ctx.message_size;
	hdr.seed = ctx.seed;
	for (i = 0; i < FRAC_NR_SLOTS; i++)
		hdr.slot_func[i] = slot_func(i);

	if (write(thread->record_fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		fprintf(stderr, "error: failed to write %s: %s\n", path, strerror(errno));
		exit(1);
	}

	/* a stream of its own for each thread, all from -g */
	thread->rng = ctx.seed + ((uint64_t)thread->id << 32);
}

/* the request: its FRAC header @hdr, then random data for the unit it goes to */
void record_fill(struct connection *conn, const uint8_t *hdr)
{
	uint32_t nr_word = (conn->req_size - FRAC_HDR_SIZE) / sizeof(uint32_t);
	uint64_t *rng = &conn->thread->rng;
	uint32_t *data;
	uint32_t i;
	float f;

	reserve(&conn->req_buf, &conn->req_cap, conn->req_size);
	memcpy(conn->req_buf, hdr, FRAC_HDR_SIZE);
	data = (uint32_t *)(conn->req_buf + FRAC_HDR_SIZE);

	for (i = 0; i < nr_word; i++) {
		uint64_t r = rand_next(rng);

		switch (conn->func) {
		case LOGIT:
			/* k / 2^24 for k in 1..2^24-1: inside (0, 1), where the logit is finite */
			f = ((r >> 40) ? (r >> 40) : 1) * 0x1p-24f;
			memcpy(&data[i], &f, sizeof(f));
			break;

		case NORM:
			f = (int32_t)(r >> 32) * (1000.0 / 2147483648.0);
			memcpy(&data[i], &f, sizeof(f));
			break;

		default:
			/* any word for top_k, any bytes for cnn */
			data[i] = r;
			break;
		}
	}
}

/* @len bytes of the response, @off bytes into it */
void record_response_data(struct connection *conn, size_t off, const void *data, int len)
{
	reserve(&conn->resp_buf, &conn->resp_cap, off + len);
	memcpy(conn->resp_buf + off, data, len);
}

static void record_write(struct connection *conn, uint32_t type, uint64_t time_ns,
			 const void *data, uint32_t len)
{
	struct record_hdr hdr = {
		.type = type,
		.sid = conn->sid,
		.seq = conn->rec_seq,
		.time_ns = time_ns,
		.len = len,
	};
	struct iovec iov[2] = {
		{ .iov_base = &hdr, .iov_len = sizeof(hdr) },
		{ .iov_base = (void *)data, .iov_len = len },
	};
	ssize_t ret;

	ret = writev(conn->thread->record_fd, iov, 2);
	if (ret != (ssize_t)(sizeof(hdr) + len)) {
		fprintf(stderr, "error: failed to record: %s\n", ret < 0 ? strerror(errno) : "short write");
		exit(1);
	}
}

/* called once the whole request is handed to the stack, at last_ns */
void record_request(struct connection *conn)
{
	record_write(conn, RECORD_REQUEST, conn->last_ns, conn->req_buf, conn->req_size);
}

/* called once at least the whole response is in: all that came is recorded */
void record_response(struct connection *conn)
{
	record_write(conn, RECORD_RESPONSE, get_time_in_ns(), conn->resp_buf, conn->read.off);
	conn->rec_seq += 1;
}
