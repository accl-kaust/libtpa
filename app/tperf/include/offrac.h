/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Author: Yixi Chen <yixi.chen@kaust.edu.sa>
 */

#ifndef _OFFRAC_H_
#define _OFFRAC_H_

#include <math.h>
#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef TF_ENABLED
#include <tensorflow/c/c_api.h>
#endif
#define MAX_BUF_SIZE 20480
#define IMAGE_SIZE (64 * 64 * 3)

// Enum for offrac supporting functions
enum {
      TOPK = 1,
      CNN = 2,
      LOGIT = 3,
      NORM = 5,
};

#ifdef TF_ENABLED
 typedef struct cnn_t{
  TF_Graph* graph;
  TF_Status* status;
  TF_SessionOptions* session_opts;
  TF_Buffer* run_options;
  TF_Session* session;
  TF_Output input_op;
  TF_Output output_op;
}cnn_tf;

int cnn(void* out_buf, int req_size, void* in_buf, cnn_tf *tf_obj);
#endif
int topk(void* out_buf, int req_size, void* in_buf);
int norm(void* out_buf, int req_size, void* in_buf);
int logit(void* out_buf, int req_size, void* in_buf);

#endif
