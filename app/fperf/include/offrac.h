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

// Enum for offrac supporting functions. These select what the software
// server in offrac.c computes; the FPGA has slots, not functions.
enum {
      TOPK = 1,
      CNN = 2,
      LOGIT = 3,
      NORM = 5,
};

/*
 * fRAC request header: one 64-byte AXIS beat prefixed to the request.
 *
 *   bytes 0-55   filler. No longer inspected: the slot modules used to detect
 *                their header line with s_axis_tdata[447:0] == {448{1'b1}},
 *                and that check is gone. The lowest bit any header parser
 *                reads is 448 (dispatcher.v:54).
 *   bytes 56-59  request size, little-endian, INCLUDING this header. (The
 *                reconfiguration controller is the exception: it excludes the
 *                header and dispatcher.v adds the 64 back.)
 *   bytes 60-61  top config. Bits 1:0 alias the FIRST/LAST request flags, so
 *                top_k reads its 16-bit result mask with those two bits
 *                overwritten.
 *   bytes 62-63  slot id, which selects the accelerator cell.
 *
 * dispatcher.v identifies a header purely from expecting_header && the FIRST
 * flag, so bits 1:0 of byte 60 are the only thing marking this line as one.
 */
#define FRAC_HDR_SIZE		64
#define FRAC_LINE_SIZE		64	/* one AXIS beat */
#define FRAC_HDR_FILL		0xff
#define FRAC_TOP_CONFIG		0xffff
#define FRAC_REQ_FLAG_FIRST	0x1
#define FRAC_REQ_FLAG_LAST	0x2

/* reserved by pkt_logic.v for the reconfiguration controller */
#define FRAC_RECONF_SLOT_ID	0x00ab

/* cells C00-C03: pkt_logic.v routes any other slot id to cell 0 */
#define FRAC_NR_SLOTS		4

/* norm_core.v holds 256 data lines and never finishes a longer request */
#define FRAC_NORM_MAX_REQ_SIZE	(FRAC_HDR_SIZE + 256 * FRAC_LINE_SIZE)

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
