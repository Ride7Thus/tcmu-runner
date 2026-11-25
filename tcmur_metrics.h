/*
 * Copyright (c) 2026 tcmu-runner contributors
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

/*
 * Runtime metrics in the Prometheus text exposition format.
 *
 * Design rules:
 *  - The I/O path only does relaxed atomic updates of per device
 *    counters: no locks, no allocations, no system calls except one
 *    vDSO clock_gettime() per command.
 *  - Every recording function accepts a NULL metrics pointer, which is
 *    what devices get when metrics are disabled.
 *  - One exporter thread, running at idle CPU and I/O priority, reads
 *    the counters and writes them to a file for the node_exporter
 *    textfile collector. It never takes device locks and never calls
 *    into handlers, so a slow or failing exporter cannot stall I/O.
 */

#ifndef __TCMUR_METRICS_H
#define __TCMUR_METRICS_H

#include <stdbool.h>
#include <stdint.h>

struct tcmu_device;
struct tcmur_dev_metrics;

/* SCSI command classes, label "op" of tcmu_commands_total */
enum tcmur_metrics_op {
	TCMUR_METRICS_OP_READ,
	TCMUR_METRICS_OP_WRITE,
	TCMUR_METRICS_OP_FLUSH,
	TCMUR_METRICS_OP_UNMAP,
	TCMUR_METRICS_OP_WRITE_SAME,
	TCMUR_METRICS_OP_CAW,
	TCMUR_METRICS_OP_XCOPY,
	TCMUR_METRICS_OP_OTHER,
	TCMUR_METRICS_OP_MAX,
};

/* Device events, mirror the tcmur_event counters of struct tcmur_device */
enum tcmur_metrics_event {
	TCMUR_METRICS_EVT_LOCK_LOST,
	TCMUR_METRICS_EVT_CONN_LOST,
	TCMUR_METRICS_EVT_CMD_TIMED_OUT,
	TCMUR_METRICS_EVT_MAX,
};

/* Commands that would have unmapped blocks while UNMAP is disabled */
enum tcmur_metrics_unmap_rejected {
	TCMUR_METRICS_UNMAP_REJECTED_UNMAP,
	TCMUR_METRICS_UNMAP_REJECTED_WRITE_SAME,
	TCMUR_METRICS_UNMAP_REJECTED_MAX,
};

/* Backend operations, label "op" of tcmu_<handler>_errors_total */
enum tcmur_backend_op {
	TCMUR_BACKEND_OP_OPEN,
	TCMUR_BACKEND_OP_READ,
	TCMUR_BACKEND_OP_WRITE,
	TCMUR_BACKEND_OP_FLUSH,
	TCMUR_BACKEND_OP_DISCARD,
	TCMUR_BACKEND_OP_ZEROFILL,
	TCMUR_BACKEND_OP_LOCK,
	TCMUR_BACKEND_OP_MAX,
};

/* Pseudo errno for a backend request that completed short */
#define TCMUR_METRICS_ERR_SHORT_IO	(-1)

/*
 * Lifecycle (main.c). tcmur_metrics_start() with an empty or NULL file
 * leaves metrics disabled. Failing to start the exporter is not fatal:
 * tcmu-runner keeps running without metrics.
 */
int tcmur_metrics_start(const char *file, unsigned int interval_sec);
void tcmur_metrics_stop(void);
bool tcmur_metrics_enabled(void);

/*
 * Per device metrics (main.c). alloc returns NULL when metrics are
 * disabled or memory is short; the device then simply has no metrics.
 * A device is exported only between register and unregister.
 */
struct tcmur_dev_metrics *tcmur_metrics_dev_alloc(const char *handler,
						   const char *hba,
						   const char *dev);
void tcmur_metrics_dev_register(struct tcmur_dev_metrics *m);
void tcmur_metrics_dev_unregister(struct tcmur_dev_metrics *m);
void tcmur_metrics_dev_free(struct tcmur_dev_metrics *m);

/* Hot path recording, all accept m == NULL */
uint64_t tcmur_metrics_now_ns(void);
/*
 * Account one completed SCSI command: classify it by cdb[0], count it
 * by status, record its latency from start_ns (0: unknown) and, for a
 * successful READ or WRITE, its transfer size in bytes.
 */
void tcmur_metrics_cmd_done(struct tcmur_dev_metrics *m, uint8_t *cdb,
			    int status, uint64_t start_ns,
			    uint32_t block_size);
void tcmur_metrics_inflight_add(struct tcmur_dev_metrics *m, int delta);
void tcmur_metrics_event(struct tcmur_dev_metrics *m,
			 enum tcmur_metrics_event evt);
void tcmur_metrics_set_lock_state(struct tcmur_dev_metrics *m, uint8_t state);
void tcmur_metrics_set_in_recovery(struct tcmur_dev_metrics *m, bool val);
void tcmur_metrics_set_size(struct tcmur_dev_metrics *m, uint64_t bytes);
void tcmur_metrics_unmap_rejected(struct tcmur_dev_metrics *m,
				  enum tcmur_metrics_unmap_rejected kind);
void tcmur_metrics_backend_error(struct tcmur_dev_metrics *m,
				 enum tcmur_backend_op op, int err);

/*
 * Handler API, exported to handler modules (main-syms.txt). err is a
 * positive errno value or TCMUR_METRICS_ERR_SHORT_IO.
 */
void tcmur_dev_backend_error(struct tcmu_device *dev,
			     enum tcmur_backend_op op, int err);

#ifdef TCMUR_METRICS_TEST
/* Test hooks: sub-second interval and a fake configfs root */
void tcmur_metrics_test_set_interval_ms(unsigned int ms);
void tcmur_metrics_test_set_cfgfs_core(const char *path);
#endif

#endif /* __TCMUR_METRICS_H */
