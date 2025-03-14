/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2021-2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#include <stdio.h>

#include "tperf.h"

static void process_conn(struct connection *conn)
{
	uint32_t events;
	int ret = 0;

	events = conn->events;
	conn->events = 0;

	if (events & (TPA_EVENT_IN | TPA_EVENT_ERR | TPA_EVENT_HUP))
		ret = conn_on_read(conn);

	if (ret >= 0 && (events & (TPA_EVENT_OUT | TPA_EVENT_ERR | TPA_EVENT_HUP)))
		ret = conn_on_write(conn);

	if (ret < 0 || (events & (TPA_EVENT_ERR | TPA_EVENT_HUP)) || conn->to_close){
	  if (!conn->is_client && conn->reassemble.reassembly_buf != NULL){
		    free(conn->reassemble.reassembly_buf);
		    if(conn->func == CNN){
		          TF_DeleteSession(conn->tf_obj.session, conn->tf_obj.status);
			  TF_DeleteSessionOptions(conn->tf_obj.session_opts);
			  TF_DeleteGraph(conn->tf_obj.graph);
			  TF_DeleteStatus(conn->tf_obj.status);
		    }
	  }
	      conn_close(conn);
	}
}

static void process_event_queue(struct test_thread *thread)
{
	struct connection *conn;
	int nr_event = thread->nr_event;

	/* to avoid dead loop */
	while (nr_event--) {
		conn = event_queue_pop(thread);

		process_conn(conn);
	}
}

int poll_and_process(struct test_thread *thread)
{
	struct tpa_event events[BATCH_SIZE];
	int nr_event;
	int i;

	nr_event = tpa_event_poll(thread->worker, events, BATCH_SIZE);
	if (nr_event < 0) {
		fprintf(stderr, "err_epoll_wait: %s\n", strerror(errno));
		return -1;
	}

	for (i = 0; i < nr_event; i++)
		event_queue_add(events[i].data, events[i].events);

	process_event_queue(thread);

	return 0;
}
