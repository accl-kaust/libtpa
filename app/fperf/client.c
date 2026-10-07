/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Copyright (c) 2025-2026, Krishnan Iyer
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <errno.h>
#include <limits.h>

#include "fperf.h"


volatile int client_shutdown = 0;

static struct connection *create_client_conn(struct test_thread *thread, int sid)
{
	struct connection *conn;
	int message_size = ctx.message_size;
	int response_size = ctx.response_size;

	conn = conn_create(thread, sid);

	conn->is_client = 1;
	conn->test = ctx.test;
	conn->integrity_enabled = ctx.integrity_enabled;
	conn->integrity_off = get_time_in_ns();
	conn->enable_zwrite = ctx.enable_zwrite;
	conn->message_size = message_size;
	conn->response_size = response_size;
	conn->func = ctx.func;
	conn->slot = ctx.slot;
	conn->req_size = ctx.req_size;
	conn->fpga_srv = ctx.fpga_srv;
	conn->pkt_idx = 0;

	switch (conn->test) {
	case TEST_READ:
		conn->read.budget  = message_size;
		conn->write.budget = 0;
		break;

	case TEST_WRITE:
		conn->read.budget  = 0;
		conn->write.budget = message_size;
		break;

	case TEST_RR:
	case TEST_CRR:
		conn->last_ns = get_time_in_ns();
		conn->read.budget  = response_size;
		conn->write.budget = message_size;
		break;

	case TEST_RW:
		conn->read.budget  = message_size;
		conn->write.budget = message_size;
		break;
	}

	if (ctx.trace_file)
		trace_conn_init(conn);

	return conn;
}

static void bootstrap_test(struct test_thread *thread)
{
	int sid;

	/* a trace is replayed once, on one connection */
	if (ctx.trace_file && thread->stats->nr_conn_total)
		return;

	while (thread->nr_conn < ctx.nr_conn_per_thread) {
		sid = tpa_connect_to(ctx.server, ctx.port, NULL);
		if (sid < 0)
			break;

		create_client_conn(thread, sid);
	}
}

static void *client_test_loop(void *arg)
{
	struct test_thread *thread = arg;
	struct tpa_worker *worker;

	worker = tpa_worker_init();
	if (!worker) {
		fprintf(stderr, "failed to init worker: %s\n", strerror(errno));
		return NULL;
	}
	thread->worker = worker;

	while (!client_shutdown) {
		bootstrap_test(thread);

		tpa_worker_run(thread->worker);

		if (ctx.trace_file && !ctx.trace_done) {
			if (thread->nr_conn == 0 && thread->stats->nr_conn_total) {
				fprintf(stderr, "trace: connection closed after %lu of %u rows\n",
					thread->stats->latency.count, ctx.nr_trace);
				ctx.trace_done = 1;
			}
			trace_release(thread);
		}

		if (poll_and_process(thread) < 0)
			break;
	}
	printf("exiting client: %d\n", thread->id);

	if (thread->log){
		char outfile[PATH_MAX];
		snprintf(outfile, sizeof(outfile), "%s/hugepage_thread_%lu.txt", thread->log_dir, (unsigned long)thread->id);
		FILE *fout = fopen(outfile, "w");
		if (!fout) {
			perror("fopen");
			for (int i=0; i<=thread->curr_hugepg;i++)
				munmap(thread->hugepg[i], HUGEPAGE_SIZE);
			return NULL;
		}
		for (int i = 0; i <= thread->curr_hugepg; i++) {
			size_t to_write = (i == thread->curr_hugepg) ? thread->hugepg_off : thread->hugepg_len[i];
			fwrite(thread->hugepg[i], 1, to_write, fout);
		}

		fclose(fout);
		if (thread->log_dropped)
			fprintf(stderr, "warn: log of thread %d is full, dropped its last %lu lines\n",
				thread->id, thread->log_dropped);
		for (int i=0;i<=thread->curr_hugepg; i++)
			munmap(thread->hugepg[i], HUGEPAGE_SIZE);
	}
	return NULL;
}

int fperf_client(void)
{
	if (ctx.slots_file)
		slots_load(ctx.slots_file);
	if (ctx.trace_file)
		trace_load(ctx.trace_file);
	if (ctx.record)
		record_setup();

	spawn_test_threads(client_test_loop);
	show_stats();

	client_shutdown = 1;

	for (int i = 0; i < ctx.nr_thread; i++)
	  pthread_join(ctx.tid[i], NULL);

	return 0;
}
