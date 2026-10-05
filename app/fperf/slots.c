/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The unit loaded in each FPGA slot (-M): -E sends a trace row to the slot
 * running its function, and -r fills a request with data for the unit in
 * its slot.
 */
#include <stdio.h>
#include <stdarg.h>

#include "fperf.h"

/* what the slots hold when -M is not given */
static int slot_funcs[FRAC_NR_SLOTS] = { TOPK, LOGIT, CNN, NORM };

static const struct {
	const char *name;
	int func;
} unit_names[] = {
	{ "top_k", TOPK  },
	{ "topk",  TOPK  },
	{ "log",   LOGIT },
	{ "logit", LOGIT },
	{ "norm",  NORM  },
	{ "cnn",   CNN   },
};

static void slots_error(const char *path, int lineno, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "error: %s:%d: ", path, lineno);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

static int unit_func(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(unit_names) / sizeof(unit_names[0]); i++) {
		if (strcmp(unit_names[i].name, name) == 0)
			return unit_names[i].func;
	}

	return 0;
}

/* lines of "<slot> <unit>", # starting a comment; a slot not listed holds nothing */
void slots_load(const char *path)
{
	char line[256];
	char name[32];
	char extra;
	int lineno = 0;
	int slot;
	int func;
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "error: failed to open slot map %s: %s\n", path, strerror(errno));
		exit(1);
	}

	memset(slot_funcs, 0, sizeof(slot_funcs));
	while (fgets(line, sizeof(line), f)) {
		lineno += 1;

		line[strcspn(line, "#")] = '\0';
		switch (sscanf(line, "%d %31s %c", &slot, name, &extra)) {
		case EOF:
			continue;
		case 2:
			break;
		default:
			slots_error(path, lineno, "expected \"<slot> <unit>\"");
		}

		if (slot < 0 || slot >= FRAC_NR_SLOTS)
			slots_error(path, lineno, "slot %d is not 0..%d", slot, FRAC_NR_SLOTS - 1);
		if (slot_funcs[slot])
			slots_error(path, lineno, "slot %d is listed twice", slot);

		func = unit_func(name);
		if (!func)
			slots_error(path, lineno, "unit %s is not top_k, log, norm or cnn", name);
		slot_funcs[slot] = func;
	}
	fclose(f);
}

/* the function the unit in @slot runs, 0 for none */
int slot_func(int slot)
{
	return slot >= 0 && slot < FRAC_NR_SLOTS ? slot_funcs[slot] : 0;
}

/* the first slot running @func, or -1 */
int func_slot(int func)
{
	int slot;

	for (slot = 0; slot < FRAC_NR_SLOTS; slot++) {
		if (func && slot_funcs[slot] == func)
			return slot;
	}

	return -1;
}

/* how many bytes @func answers to a request of @req_size, the header included */
uint32_t func_response_size(int func, uint32_t req_size)
{
	/* log and norm answer each data line, top_k and cnn with one line */
	if (func == LOGIT || func == NORM)
		return req_size - FRAC_HDR_SIZE;

	return FRAC_LINE_SIZE;
}
