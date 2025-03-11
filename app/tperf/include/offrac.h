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

#include <tensorflow/c/c_api.h>

#define MAX_BUF_SIZE 20480
#define IMAGE_SIZE (64 * 64 * 3)

// Enum for offrac supporting functions
enum {
      TOPK = 1,
      CNN = 2,
      LOGIT = 3,
      NORM = 5,
};

int topk(void* out_buf, int req_size, void* in_buf);
int norm(void* out_buf, int req_size, void* in_buf);
int logit(void* out_buf, int req_size, void* in_buf);
int cnn(void* out_buf, int req_size, void* in_buf);

#endif
