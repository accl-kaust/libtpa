/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>
#include <limits.h>

#include <utils.h>

#include "fperf.h"

void usage(void)
{
	fprintf(stderr, "usage: fperf [options]\n"
			"\n"
			"       fperf -s [options]\n"
			"       fperf -t test [options]\n"
			"\n"
			"Fperf, a tpa performance benchmark.\n"
			"\n"
			"Client options:\n"
			"  -c server         run in client mode (the default mode) and specifies the server \n"
			"                    address (default: 127.0.0.1)\n"
			"  -t test           specifies the test mode, which is listed below\n"
			"  -p port           specifies the port to connect to (default: %d)\n"
			"  -d duration       specifies the test duration (default: 10s)\n"
			"  -m message_size   specifies the message size (default: %d)\n"
			"  -n nr_thread      specifies the thread count (default: 1)\n"
			"  -i                do integrity verification (default: off)\n"
			"  -C nr_conn        specifies the connection to be created for each thread (default: 1)\n"
			"  -W 0|1            disable/enable zero copy write (default: on)\n"
			"  -S start_cpu      specifies the starting cpu to bind\n"
			"  -Z fpga_server    specifies if server is on CPU or FPGA\n"
			"  -X request size   specifies request size in case > message size then issues multi pkt req\n"
			"  -F function       specifies function ID to be exec on the server (CPU server only)\n"
			"  -K slot           specifies the FPGA slot id placed in header bytes 62-63 (with -Z 1)\n"
			"  -R response size  specifies response size expected after execution of function\n"
			"  -L log latency    logs latency value if set to 0 to the dir mentioned\n"
			"  -D log dir        stores log files in specified director\n"
			"  -E trace_file     replays a CSV trace (app,sleep_time,request_size,response_size)\n"
			"                    on the FPGA: each row goes to the slot of its app, sleep_time\n"
			"                    seconds after the previous response. Needs -t rr -Z 1 -n 1 -C 1,\n"
			"                    replaces -F/-K/-X/-R, and runs to the end of the trace unless -d\n"
			"                    is given\n"
			"\n"
			"Server options:\n"
			"  -s                run in server mode\n"
			"  -n nr_thread      specifies the thread count (default: 1)\n"
			"  -l addr           specifies local address to listen on\n"
			"  -p port           specifies the port to listen on (default: %d)\n"
			"  -S start_cpu      specifies the starting cpu to bind\n"
			"\n"
			"The supported test modes are:\n"
			"  * read            read data from the server end\n"
			"  * write           write data to the server end\n"
			"  * rw              test read and write simultaneously\n"
			"  * rr              send a request (with payload) to the server and\n"
			"                    expects a response will be returned from the server end\n"
			"  * crr             basically does the same thing like rr, except that a\n"
			"                    connection is created for each request\n",
			FPERF_PORT,
			DEFAULT_MESSAGE_SIZE,
			FPERF_PORT
	);

	exit(1);
}

#define PARSE_NUM(var, optarg, type, name)	do {			\
	var = tpa_parse_num(optarg, type);				\
	if (errno) {							\
		fprintf(stderr, "invalid %s: %s\n", name, optarg);	\
		exit(1);						\
	}								\
} while (0)

int parse_options(int argc, char **argv)
{
	int opt;
	int has_opt_c = 0;
	int has_opt_d = 0;

	memset(&ctx, 0, sizeof(ctx));

	ctx.is_client     = 1;
	ctx.server        = "127.0.0.1";
	ctx.test          = -1;
	ctx.port          = FPERF_PORT;
	ctx.nr_thread     = DEFAULT_NR_THREAD;
	ctx.duration      = DEFAULT_DURATION;
	ctx.message_size  = DEFAULT_MESSAGE_SIZE;
	ctx.enable_tso    = 1;
	ctx.enable_zwrite = 1;
	ctx.start_cpu     = -4096;
	ctx.nr_conn_per_thread = 1;
	ctx.integrity_enabled = 0;
	ctx.response_size = ctx.message_size;
	ctx.func = 0;
	ctx.slot = 0;
	ctx.req_size = ctx.message_size;
	ctx.fpga_srv = 0;
	ctx.log = 0;
	ctx.log_dir = "";
	while ((opt = getopt(argc, argv, "c:C:t:d:l:m:n:p:S:W:R:F:K:X:Z:L:D:E:isqh")) != -1) {
		switch (opt) {
		case 's':
			ctx.is_client = 0;
			break;

		case 'c':
			ctx.server = strdup(optarg);
			has_opt_c = 1;
			break;

		case 'C':
			PARSE_NUM(ctx.nr_conn_per_thread, optarg, NUM_TYPE_NONE, "connection count");
			break;

		case 't':
			ctx.test = str_to_test(optarg);
			if (ctx.test < 0) {
				fprintf(stderr, "invalid test mode: %s\n", optarg);
				exit(1);
			}
			break;

		case 'T':
			PARSE_NUM(ctx.enable_tso, optarg, NUM_TYPE_NONE, "tso enabling");
			break;

		case 'd':
			PARSE_NUM(ctx.duration, optarg, NUM_TYPE_TIME, "duration");
			has_opt_d = 1;
			break;

		case 'l':
			ctx.local = strdup(optarg);
			break;

		case 'm':
			PARSE_NUM(ctx.message_size, optarg, NUM_TYPE_SIZE, "message size");
			break;

		case 'n':
			PARSE_NUM(ctx.nr_thread, optarg, NUM_TYPE_NONE, "thread count");
			break;

		case 'p':
			PARSE_NUM(ctx.port, optarg, NUM_TYPE_NONE, "port");
			if (ctx.port <= 0 || ctx.port >= 65536) {
				fprintf(stderr, "invalid port: %d: out of range\n", ctx.port);
				exit(1);
			}
			break;

		case 'W':
			PARSE_NUM(ctx.enable_zwrite, optarg, NUM_TYPE_NONE, "zwrite enabling");
			break;

		case 'S':
			PARSE_NUM(ctx.start_cpu, optarg, NUM_TYPE_NONE, "start_cpu");
			break;

		case 'i':
			ctx.integrity_enabled = 1;
			break;

		case 'R':
		      PARSE_NUM(ctx.response_size, optarg, NUM_TYPE_SIZE, "response size");
		      break;

		case 'F':
		      PARSE_NUM(ctx.func, optarg, NUM_TYPE_SIZE, "function");
		      break;

		case 'K':
		      PARSE_NUM(ctx.slot, optarg, NUM_TYPE_NONE, "slot");
		      if (ctx.slot == FRAC_RECONF_SLOT_ID) {
			      fprintf(stderr, "invalid slot: %d is reserved by pkt_logic.v for the "
					      "reconfiguration controller\n", FRAC_RECONF_SLOT_ID);
			      exit(1);
		      }
		      break;

		case 'X':
		      PARSE_NUM(ctx.req_size, optarg, NUM_TYPE_SIZE, "Multi Packet Request");
		      break;

		case 'Z':
		      PARSE_NUM(ctx.fpga_srv, optarg, NUM_TYPE_NONE, "fpga server?");
		      break;

		case 'L':
			PARSE_NUM(ctx.log, optarg, NUM_TYPE_NONE, "log");
			if (ctx.log != 0 && ctx.log != 1 ) {
				fprintf(stderr, "invalid port: %d: out of range\n", ctx.log);
				exit(1);
			}
			break;

		case 'D':
			ctx.log_dir = strdup(optarg);
			break;

		case 'E':
			ctx.trace_file = strdup(optarg);
			break;

		case 'q':
			ctx.quiet = 1;
			break;

		case 'h':
			usage();
			break;

		default:
			usage();
		}
	}

	if (has_opt_c && ctx.is_client == 0) {
		fprintf(stderr, "error: -c and -s can not be given at the same time\n\n");
		usage();
	}

	if (ctx.is_client && ctx.test < 0) {
		fprintf(stderr, "error: missing mandatory option: -t test\n\n");
		usage();
	}

	if (ctx.message_size <= 0 || ctx.message_size > MAX_MESSAGE_SIZE) {
		fprintf(stderr, "error: message size must be 1..%d bytes\n\n", MAX_MESSAGE_SIZE);
		usage();
	}

	if (ctx.trace_file) {
		if (!ctx.is_client || ctx.test != TEST_RR || ctx.fpga_srv != 1 ||
		    ctx.nr_thread != 1 || ctx.nr_conn_per_thread != 1) {
			fprintf(stderr, "error: -E needs -t rr -Z 1 -n 1 -C 1\n\n");
			usage();
		}

		if (ctx.message_size < FRAC_HDR_SIZE) {
			fprintf(stderr, "error: -E needs a message size of at least %d, the FRAC header\n\n",
				FRAC_HDR_SIZE);
			usage();
		}

		if (!has_opt_d)
			ctx.duration = INT_MAX;
	}

	return 0;
}
