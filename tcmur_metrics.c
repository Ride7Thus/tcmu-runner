/*
 * Copyright (c) 2026 tcmu-runner contributors
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

/*
 * Prometheus metrics for tcmu-runner, see tcmur_metrics.h for the
 * design rules and docs/monitoring.md for the exported metrics.
 *
 * Concurrency:
 *  - Counters and gauges in struct tcmur_dev_metrics are only accessed
 *    with relaxed __atomic builtins. Writers are the command processing,
 *    handler callback and event threads; the only reader is the exporter.
 *  - registry_lock protects the list of exported devices. It is taken by
 *    device add/remove and, for the time needed to copy the counters, by
 *    the exporter. It is never taken on the I/O path.
 *  - Everything else (output buffer, snapshots, exporter statistics) is
 *    private to the exporter thread.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <scsi/scsi.h>

#include "ccan/list/list.h"
#include "libtcmu_log.h"
#include "libtcmu_common.h"
#include "scsi_defs.h"
#include "tcmur_metrics.h"
#include "version.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_RELAXED)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define ADD(x, v)	__atomic_fetch_add(&(x), (v), __ATOMIC_RELAXED)

/* Latency histogram upper bounds, in nanoseconds and as "le" labels */
static const uint64_t hist_bounds_ns[] = {
	100000ULL, 250000ULL, 500000ULL,
	1000000ULL, 2500000ULL, 5000000ULL,
	10000000ULL, 25000000ULL, 50000000ULL,
	100000000ULL, 250000000ULL, 500000000ULL,
	1000000000ULL, 2500000000ULL, 5000000000ULL,
	10000000000ULL, 30000000000ULL,
};
static const char *const hist_bounds_le[] = {
	"0.0001", "0.00025", "0.0005",
	"0.001", "0.0025", "0.005",
	"0.01", "0.025", "0.05",
	"0.1", "0.25", "0.5",
	"1", "2.5", "5",
	"10", "30",
};
/* One extra bucket for +Inf */
#define HIST_NR		(ARRAY_LEN(hist_bounds_ns) + 1)

/* Command status: TCMU_STS_* index, then not_handled and other */
#define STS_NOT_HANDLED	(TCMU_STS_TOO_MANY_TGT_DESC + 1)
#define STS_OTHER	(TCMU_STS_TOO_MANY_TGT_DESC + 2)
#define STS_NR		(TCMU_STS_TOO_MANY_TGT_DESC + 3)

static const char *const sts_names[STS_NR] = {
	[TCMU_STS_OK]			= "ok",
	[TCMU_STS_NO_RESOURCE]		= "no_resource",
	[TCMU_STS_PASSTHROUGH_ERR]	= "passthrough_error",
	[TCMU_STS_BUSY]			= "busy",
	[TCMU_STS_WR_ERR]		= "write_error",
	[TCMU_STS_RD_ERR]		= "read_error",
	[TCMU_STS_MISCOMPARE]		= "miscompare",
	[TCMU_STS_INVALID_CMD]		= "invalid_command",
	[TCMU_STS_INVALID_CDB]		= "invalid_cdb",
	[TCMU_STS_INVALID_PARAM_LIST]	= "invalid_param_list",
	[TCMU_STS_INVALID_PARAM_LIST_LEN] = "invalid_param_list_len",
	[TCMU_STS_TIMEOUT]		= "timeout",
	[TCMU_STS_FENCED]		= "fenced",
	[TCMU_STS_HW_ERR]		= "hardware_error",
	[TCMU_STS_RANGE]		= "lba_out_of_range",
	[TCMU_STS_FRMT_IN_PROGRESS]	= "format_in_progress",
	[TCMU_STS_CAPACITY_CHANGED]	= "capacity_changed",
	[TCMU_STS_NOTSUPP_SAVE_PARAMS]	= "save_params_unsupported",
	[TCMU_STS_WR_ERR_INCOMPAT_FRMT]	= "write_incompatible_format",
	[TCMU_STS_TRANSITION]		= "alua_transition",
	[TCMU_STS_IMPL_TRANSITION_ERR]	= "implicit_transition_error",
	[TCMU_STS_EXPL_TRANSITION_ERR]	= "explicit_transition_error",
	[TCMU_STS_NO_LOCK_HOLDERS]	= "no_lock_holders",
	[TCMU_STS_NOTSUPP_SEG_DESC_TYPE] = "xcopy_unsupported_segment",
	[TCMU_STS_NOTSUPP_TGT_DESC_TYPE] = "xcopy_unsupported_target",
	[TCMU_STS_CP_TGT_DEV_NOTCONN]	= "xcopy_target_not_connected",
	[TCMU_STS_INVALID_CP_TGT_DEV_TYPE] = "xcopy_invalid_target_type",
	[TCMU_STS_TOO_MANY_SEG_DESC]	= "xcopy_too_many_segments",
	[TCMU_STS_TOO_MANY_TGT_DESC]	= "xcopy_too_many_targets",
	[STS_NOT_HANDLED]		= "not_handled",
	[STS_OTHER]			= "other",
};

static const char *const op_names[TCMUR_METRICS_OP_MAX] = {
	[TCMUR_METRICS_OP_READ]		= "read",
	[TCMUR_METRICS_OP_WRITE]	= "write",
	[TCMUR_METRICS_OP_FLUSH]	= "flush",
	[TCMUR_METRICS_OP_UNMAP]	= "unmap",
	[TCMUR_METRICS_OP_WRITE_SAME]	= "write_same",
	[TCMUR_METRICS_OP_CAW]		= "compare_and_write",
	[TCMUR_METRICS_OP_XCOPY]	= "extended_copy",
	[TCMUR_METRICS_OP_OTHER]	= "other",
};

static const char *const backend_op_names[TCMUR_BACKEND_OP_MAX] = {
	[TCMUR_BACKEND_OP_OPEN]		= "open",
	[TCMUR_BACKEND_OP_READ]		= "read",
	[TCMUR_BACKEND_OP_WRITE]	= "write",
	[TCMUR_BACKEND_OP_FLUSH]	= "flush",
	[TCMUR_BACKEND_OP_DISCARD]	= "discard",
	[TCMUR_BACKEND_OP_ZEROFILL]	= "zerofill",
	[TCMUR_BACKEND_OP_LOCK]		= "lock",
};

static const char *const unmap_rejected_names[TCMUR_METRICS_UNMAP_REJECTED_MAX] = {
	[TCMUR_METRICS_UNMAP_REJECTED_UNMAP]		= "unmap",
	[TCMUR_METRICS_UNMAP_REJECTED_WRITE_SAME]	= "write_same",
};

/* Must match the TCMUR_DEV_LOCK_* order in tcmur_device.h */
static const char *const lock_state_names[] = {
	"unknown", "unlocked", "read_locking", "read_locked",
	"write_locking", "write_locked",
};
#define LOCK_STATE_NR	ARRAY_LEN(lock_state_names)

/* Backend errno values with their own label, the rest is "other" */
static const struct {
	int err;
	const char *name;
} errno_tab[] = {
	{ EIO,		"EIO" },
	{ ENOTCONN,	"ENOTCONN" },
	{ ETIMEDOUT,	"ETIMEDOUT" },
	{ EBUSY,	"EBUSY" },
	{ EAGAIN,	"EAGAIN" },
	{ ENOSPC,	"ENOSPC" },
	{ EDQUOT,	"EDQUOT" },
	{ EROFS,	"EROFS" },
	{ EOPNOTSUPP,	"EOPNOTSUPP" },
	{ ENOSYS,	"ENOSYS" },
	{ EINVAL,	"EINVAL" },
	{ ENOENT,	"ENOENT" },
	{ ESTALE,	"ESTALE" },
	{ ENOMEM,	"ENOMEM" },
	{ EBADF,	"EBADF" },
	{ EACCES,	"EACCES" },
	{ EPERM,	"EPERM" },
};
#define ERR_SHORT_IO	ARRAY_LEN(errno_tab)
#define ERR_OTHER	(ARRAY_LEN(errno_tab) + 1)
#define ERR_NR		(ARRAY_LEN(errno_tab) + 2)

/* ALUA access states, values as in the configfs alua_access_state file */
static const struct {
	int val;
	const char *name;
} alua_states[] = {
	{ 0,	"active_optimized" },
	{ 1,	"active_nonoptimized" },
	{ 2,	"standby" },
	{ 3,	"unavailable" },
	{ 4,	"lba_dependent" },
	{ 14,	"offline" },
	{ 15,	"transitioning" },
};

#define MAX_ALUA_GROUPS	8

struct tcmur_dev_metrics {
	/* Registry, protected by registry_lock */
	struct list_node entry;
	bool registered;

	/* Identity, immutable after alloc */
	char handler[32];
	char hba[32];
	char dev[128];

	/* Gauges */
	uint64_t size_bytes;
	int32_t inflight;
	uint8_t lock_state;
	uint8_t in_recovery;

	/* Counters */
	uint64_t events[TCMUR_METRICS_EVT_MAX];
	uint64_t cmds[TCMUR_METRICS_OP_MAX][STS_NR];
	uint64_t bytes_read;
	uint64_t bytes_written;
	uint64_t hist[TCMUR_METRICS_OP_MAX][HIST_NR];
	uint64_t hist_sum_ns[TCMUR_METRICS_OP_MAX];
	uint64_t unmap_rejected[TCMUR_METRICS_UNMAP_REJECTED_MAX];
	uint64_t backend_err[TCMUR_BACKEND_OP_MAX][ERR_NR];
};

/* Exporter private copy of one device, including what it read itself */
struct dev_snapshot {
	struct tcmur_dev_metrics m;
	int alua_nr;
	struct {
		char name[64];
		int state;
	} alua[MAX_ALUA_GROUPS];
};

struct out_buf {
	char *p;
	size_t len;
	size_t cap;
	bool failed;
};

/* Set once by tcmur_metrics_start() before any device exists */
static bool metrics_enabled;
static char metrics_path[PATH_MAX];
static char metrics_tmp_path[PATH_MAX + 8];
static unsigned int metrics_interval_ms = 15000;
static const char *cfgfs_core = CFGFS_CORE;

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(registry);
static unsigned int registry_nr;

static pthread_t exporter_thread;
static bool exporter_running;
static pthread_mutex_t stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stop_cond;
static bool stop_requested;
/* Set by the exporter when it is done, polled by tcmur_metrics_stop() */
static bool exporter_exited;

/* Exporter private state */
static struct dev_snapshot *snaps;
static unsigned int snaps_cap;
static struct out_buf out;
static uint64_t writes_total;
static uint64_t write_errors_total;
static double last_duration_s;
static time_t last_error_log;

/* ------------------------------------------------------------------ */
/* Recording, called from I/O and event paths                          */
/* ------------------------------------------------------------------ */

bool tcmur_metrics_enabled(void)
{
	return metrics_enabled;
}

uint64_t tcmur_metrics_now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int metrics_op(uint8_t opcode)
{
	switch (opcode) {
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		return TCMUR_METRICS_OP_READ;
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
	case WRITE_VERIFY:
	case WRITE_VERIFY_16:
		return TCMUR_METRICS_OP_WRITE;
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		return TCMUR_METRICS_OP_FLUSH;
	case UNMAP:
		return TCMUR_METRICS_OP_UNMAP;
	case WRITE_SAME:
	case WRITE_SAME_16:
		return TCMUR_METRICS_OP_WRITE_SAME;
	case COMPARE_AND_WRITE:
		return TCMUR_METRICS_OP_CAW;
	case EXTENDED_COPY:
		return TCMUR_METRICS_OP_XCOPY;
	default:
		return TCMUR_METRICS_OP_OTHER;
	}
}

static unsigned int status_idx(int status)
{
	if (status >= TCMU_STS_OK && status <= TCMU_STS_TOO_MANY_TGT_DESC)
		return status;
	if (status == TCMU_STS_NOT_HANDLED)
		return STS_NOT_HANDLED;
	return STS_OTHER;
}

static unsigned int hist_idx(uint64_t ns)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(hist_bounds_ns); i++) {
		if (ns <= hist_bounds_ns[i])
			return i;
	}
	return HIST_NR - 1;
}

void tcmur_metrics_cmd_done(struct tcmur_dev_metrics *m, uint8_t *cdb,
			    int status, uint64_t start_ns,
			    uint32_t block_size)
{
	uint64_t now, ns, bytes;
	int op;

	if (!m)
		return;

	op = metrics_op(cdb[0]);
	ADD(m->cmds[op][status_idx(status)], 1);

	if (start_ns) {
		now = tcmur_metrics_now_ns();
		ns = now > start_ns ? now - start_ns : 0;
		ADD(m->hist[op][hist_idx(ns)], 1);
		ADD(m->hist_sum_ns[op], ns);
	}

	if (status != TCMU_STS_OK)
		return;

	if (op == TCMUR_METRICS_OP_READ || op == TCMUR_METRICS_OP_WRITE) {
		bytes = (uint64_t)tcmu_cdb_get_xfer_length(cdb) * block_size;
		if (op == TCMUR_METRICS_OP_READ)
			ADD(m->bytes_read, bytes);
		else
			ADD(m->bytes_written, bytes);
	}
}

void tcmur_metrics_inflight_add(struct tcmur_dev_metrics *m, int delta)
{
	if (m)
		ADD(m->inflight, delta);
}

void tcmur_metrics_event(struct tcmur_dev_metrics *m,
			 enum tcmur_metrics_event evt)
{
	if (m && evt < TCMUR_METRICS_EVT_MAX)
		ADD(m->events[evt], 1);
}

void tcmur_metrics_set_lock_state(struct tcmur_dev_metrics *m, uint8_t state)
{
	if (m)
		STORE(m->lock_state, state);
}

void tcmur_metrics_set_in_recovery(struct tcmur_dev_metrics *m, bool val)
{
	if (m)
		STORE(m->in_recovery, val ? 1 : 0);
}

void tcmur_metrics_set_size(struct tcmur_dev_metrics *m, uint64_t bytes)
{
	if (m)
		STORE(m->size_bytes, bytes);
}

void tcmur_metrics_unmap_rejected(struct tcmur_dev_metrics *m,
				  enum tcmur_metrics_unmap_rejected kind)
{
	if (m && kind < TCMUR_METRICS_UNMAP_REJECTED_MAX)
		ADD(m->unmap_rejected[kind], 1);
}

void tcmur_metrics_backend_error(struct tcmur_dev_metrics *m,
				 enum tcmur_backend_op op, int err)
{
	unsigned int i, idx = ERR_OTHER;

	if (!m || op >= TCMUR_BACKEND_OP_MAX)
		return;

	if (err == TCMUR_METRICS_ERR_SHORT_IO) {
		idx = ERR_SHORT_IO;
	} else {
		if (err < 0)
			err = -err;
		for (i = 0; i < ARRAY_LEN(errno_tab); i++) {
			if (errno_tab[i].err == err) {
				idx = i;
				break;
			}
		}
	}
	ADD(m->backend_err[op][idx], 1);
}

/* ------------------------------------------------------------------ */
/* Device registry                                                     */
/* ------------------------------------------------------------------ */

struct tcmur_dev_metrics *tcmur_metrics_dev_alloc(const char *handler,
						   const char *hba,
						   const char *dev)
{
	struct tcmur_dev_metrics *m;

	if (!metrics_enabled)
		return NULL;

	m = calloc(1, sizeof(*m));
	if (!m) {
		tcmu_warn("No memory for device metrics, %s is not monitored\n",
			  dev);
		return NULL;
	}
	list_node_init(&m->entry);
	snprintf(m->handler, sizeof(m->handler), "%s", handler ? handler : "");
	snprintf(m->hba, sizeof(m->hba), "%s", hba ? hba : "");
	snprintf(m->dev, sizeof(m->dev), "%s", dev ? dev : "");
	return m;
}

void tcmur_metrics_dev_register(struct tcmur_dev_metrics *m)
{
	if (!m)
		return;

	pthread_mutex_lock(&registry_lock);
	if (!m->registered) {
		list_add_tail(&registry, &m->entry);
		m->registered = true;
		registry_nr++;
	}
	pthread_mutex_unlock(&registry_lock);
}

void tcmur_metrics_dev_unregister(struct tcmur_dev_metrics *m)
{
	if (!m)
		return;

	pthread_mutex_lock(&registry_lock);
	if (m->registered) {
		list_del_init(&m->entry);
		m->registered = false;
		registry_nr--;
	}
	pthread_mutex_unlock(&registry_lock);
}

void tcmur_metrics_dev_free(struct tcmur_dev_metrics *m)
{
	if (!m)
		return;

	tcmur_metrics_dev_unregister(m);
	free(m);
}

/* ------------------------------------------------------------------ */
/* Exporter                                                            */
/* ------------------------------------------------------------------ */

/* Log exporter problems at most once every 10 minutes */
static void exporter_error(const char *fmt, ...)
{
	char msg[512];
	time_t now = time(NULL);
	va_list ap;

	if (last_error_log && now - last_error_log < 600)
		return;
	last_error_log = now;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	tcmu_warn("metrics: %s (further errors suppressed for 10 minutes)\n",
		  msg);
}

static void buf_printf(struct out_buf *b, const char *fmt, ...)
{
	va_list ap;
	size_t avail;
	int n;
	char *p;

	if (b->failed)
		return;

	for (;;) {
		avail = b->cap - b->len;
		va_start(ap, fmt);
		n = vsnprintf(b->p + b->len, avail, fmt, ap);
		va_end(ap);
		if (n < 0) {
			b->failed = true;
			return;
		}
		if ((size_t)n < avail) {
			b->len += n;
			return;
		}
		p = realloc(b->p, b->cap * 2 + n);
		if (!p) {
			b->failed = true;
			return;
		}
		b->p = p;
		b->cap = b->cap * 2 + n;
	}
}

/* Prometheus label value escaping: backslash, double quote, newline */
static const char *esc(const char *s, char *dst, size_t len)
{
	size_t j = 0;

	for (; *s && j + 2 < len; s++) {
		if (*s == '\\' || *s == '"') {
			dst[j++] = '\\';
			dst[j++] = *s;
		} else if (*s == '\n') {
			dst[j++] = '\\';
			dst[j++] = 'n';
		} else {
			dst[j++] = *s;
		}
	}
	dst[j] = '\0';
	return dst;
}

/* Metric name component from a handler subtype: [a-zA-Z0-9_] only */
static void metric_name_part(const char *s, char *dst, size_t len)
{
	size_t j = 0;

	for (; *s && j + 1 < len; s++) {
		if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		    (*s >= '0' && *s <= '9'))
			dst[j++] = *s;
		else
			dst[j++] = '_';
	}
	dst[j] = '\0';
	if (!j)
		snprintf(dst, len, "unknown");
}

static void snapshot_dev(struct dev_snapshot *s, struct tcmur_dev_metrics *m)
{
	unsigned int i, j;

	memset(s, 0, sizeof(*s));
	memcpy(s->m.handler, m->handler, sizeof(m->handler));
	memcpy(s->m.hba, m->hba, sizeof(m->hba));
	memcpy(s->m.dev, m->dev, sizeof(m->dev));

	s->m.size_bytes = LOAD(m->size_bytes);
	s->m.inflight = LOAD(m->inflight);
	s->m.lock_state = LOAD(m->lock_state);
	s->m.in_recovery = LOAD(m->in_recovery);
	for (i = 0; i < TCMUR_METRICS_EVT_MAX; i++)
		s->m.events[i] = LOAD(m->events[i]);
	for (i = 0; i < TCMUR_METRICS_OP_MAX; i++) {
		for (j = 0; j < STS_NR; j++)
			s->m.cmds[i][j] = LOAD(m->cmds[i][j]);
		for (j = 0; j < HIST_NR; j++)
			s->m.hist[i][j] = LOAD(m->hist[i][j]);
		s->m.hist_sum_ns[i] = LOAD(m->hist_sum_ns[i]);
	}
	s->m.bytes_read = LOAD(m->bytes_read);
	s->m.bytes_written = LOAD(m->bytes_written);
	for (i = 0; i < TCMUR_METRICS_UNMAP_REJECTED_MAX; i++)
		s->m.unmap_rejected[i] = LOAD(m->unmap_rejected[i]);
	for (i = 0; i < TCMUR_BACKEND_OP_MAX; i++)
		for (j = 0; j < ERR_NR; j++)
			s->m.backend_err[i][j] = LOAD(m->backend_err[i][j]);
}

/*
 * Copy all registered devices. Memory is allocated with the lock
 * dropped, so device add/remove never waits for an allocation.
 */
static int snapshot_all(unsigned int *nr)
{
	struct tcmur_dev_metrics *m;
	struct dev_snapshot *p;
	unsigned int n, cap;

	for (;;) {
		pthread_mutex_lock(&registry_lock);
		n = registry_nr;
		if (n <= snaps_cap)
			break;
		pthread_mutex_unlock(&registry_lock);

		cap = n + 8;
		p = realloc(snaps, cap * sizeof(*snaps));
		if (!p)
			return -ENOMEM;
		snaps = p;
		snaps_cap = cap;
	}

	n = 0;
	list_for_each(&registry, m, entry)
		snapshot_dev(&snaps[n++], m);
	pthread_mutex_unlock(&registry_lock);

	*nr = n;
	return 0;
}

static int read_int_file(const char *path, int *val)
{
	char buf[32];
	ssize_t len;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len <= 0)
		return -EIO;
	buf[len] = '\0';
	*val = atoi(buf);
	return 0;
}

/* Read the ALUA group states of a device from configfs */
static void read_alua(struct dev_snapshot *s)
{
	char path[PATH_MAX];
	struct dirent *de;
	size_t len;
	DIR *dir;
	int state = 0;

	s->alua_nr = 0;
	snprintf(path, sizeof(path), "%s/%s/%s/alua", cfgfs_core,
		 s->m.hba, s->m.dev);
	dir = opendir(path);
	if (!dir)
		return;

	while ((de = readdir(dir)) && s->alua_nr < MAX_ALUA_GROUPS) {
		len = strlen(de->d_name);
		/* A truncated group name would be a wrong label: skip it */
		if (de->d_name[0] == '.' ||
		    len >= sizeof(s->alua[s->alua_nr].name))
			continue;
		snprintf(path, sizeof(path), "%s/%s/%s/alua/%s/alua_access_state",
			 cfgfs_core, s->m.hba, s->m.dev, de->d_name);
		if (read_int_file(path, &state))
			continue;
		memcpy(s->alua[s->alua_nr].name, de->d_name, len + 1);
		s->alua[s->alua_nr].state = state;
		s->alua_nr++;
	}
	closedir(dir);
}

static void family(struct out_buf *b, const char *name, const char *type,
		   const char *help)
{
	buf_printf(b, "# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
}

/* Labels identifying a device: device="...",handler="..." */
static const char *dev_labels(const struct dev_snapshot *s, char *dst,
			      size_t len)
{
	char d[2 * sizeof(s->m.dev)], h[2 * sizeof(s->m.handler)];

	snprintf(dst, len, "device=\"%s\",handler=\"%s\"",
		 esc(s->m.dev, d, sizeof(d)), esc(s->m.handler, h, sizeof(h)));
	return dst;
}

static void format_metrics(struct out_buf *b, unsigned int nr)
{
	char labels[512], esc1[256], esc2[256];
	const struct dev_snapshot *s;
	struct timespec now;
	unsigned int i, j, k, h;
	const char *handlers[64];
	unsigned int handlers_nr = 0;
	char name[96], part[40];
	uint64_t cum, cnt;

	b->len = 0;
	b->failed = false;

	family(b, "tcmu_runner_info", "gauge",
	       "tcmu-runner version, always 1.");
	buf_printf(b, "tcmu_runner_info{version=\"%s\"} 1\n",
		   esc(TCMUR_VERSION, esc1, sizeof(esc1)));

	clock_gettime(CLOCK_REALTIME, &now);
	family(b, "tcmu_metrics_timestamp_seconds", "gauge",
	       "Unix time when this metrics file was generated.");
	buf_printf(b, "tcmu_metrics_timestamp_seconds %ld.%03ld\n",
		   (long)now.tv_sec, now.tv_nsec / 1000000);

	family(b, "tcmu_metrics_writes_total", "counter",
	       "Metrics files written by the exporter.");
	buf_printf(b, "tcmu_metrics_writes_total %llu\n",
		   (unsigned long long)writes_total);
	family(b, "tcmu_metrics_write_errors_total", "counter",
	       "Metrics file writes that failed.");
	buf_printf(b, "tcmu_metrics_write_errors_total %llu\n",
		   (unsigned long long)write_errors_total);
	family(b, "tcmu_metrics_export_duration_seconds", "gauge",
	       "Duration of the previous metrics export.");
	buf_printf(b, "tcmu_metrics_export_duration_seconds %.6f\n",
		   last_duration_s);

	family(b, "tcmu_devices", "gauge", "Devices handled by tcmu-runner.");
	buf_printf(b, "tcmu_devices %u\n", nr);

	family(b, "tcmu_device_info", "gauge",
	       "Device identity, always 1.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		buf_printf(b, "tcmu_device_info{%s,hba=\"%s\"} 1\n",
			   dev_labels(s, labels, sizeof(labels)),
			   esc(s->m.hba, esc1, sizeof(esc1)));
	}

	family(b, "tcmu_device_size_bytes", "gauge", "Device size.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		buf_printf(b, "tcmu_device_size_bytes{%s} %llu\n",
			   dev_labels(s, labels, sizeof(labels)),
			   (unsigned long long)s->m.size_bytes);
	}

	family(b, "tcmu_lock_state", "gauge",
	       "Exclusive lock state of the device, 1 for the current state.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		dev_labels(s, labels, sizeof(labels));
		for (j = 0; j < LOCK_STATE_NR; j++)
			buf_printf(b, "tcmu_lock_state{%s,state=\"%s\"} %d\n",
				   labels, lock_state_names[j],
				   s->m.lock_state == j);
	}

	family(b, "tcmu_in_recovery", "gauge",
	       "1 while the device is being reopened after a failure.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		buf_printf(b, "tcmu_in_recovery{%s} %u\n",
			   dev_labels(s, labels, sizeof(labels)),
			   s->m.in_recovery);
	}

	family(b, "tcmu_inflight_commands", "gauge",
	       "Commands submitted to the handler and not completed yet.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		buf_printf(b, "tcmu_inflight_commands{%s} %d\n",
			   dev_labels(s, labels, sizeof(labels)),
			   s->m.inflight > 0 ? s->m.inflight : 0);
	}

	family(b, "tcmu_lock_lost_total", "counter",
	       "Times the device lost its exclusive lock.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		buf_printf(b, "tcmu_lock_lost_total{%s} %llu\n",
			   dev_labels(s, labels, sizeof(labels)),
			   (unsigned long long)s->m.events[TCMUR_METRICS_EVT_LOCK_LOST]);
	}

	family(b, "tcmu_conn_lost_total", "counter",
	       "Times the handler reported a lost backend connection.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		buf_printf(b, "tcmu_conn_lost_total{%s} %llu\n",
			   dev_labels(s, labels, sizeof(labels)),
			   (unsigned long long)s->m.events[TCMUR_METRICS_EVT_CONN_LOST]);
	}

	family(b, "tcmu_cmd_timed_out_total", "counter",
	       "Commands that exceeded the device cmd_time_out.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		buf_printf(b, "tcmu_cmd_timed_out_total{%s} %llu\n",
			   dev_labels(s, labels, sizeof(labels)),
			   (unsigned long long)s->m.events[TCMUR_METRICS_EVT_CMD_TIMED_OUT]);
	}

	family(b, "tcmu_alua_access_state", "gauge",
	       "ALUA access state of each target port group, 1 for the current state.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		dev_labels(s, labels, sizeof(labels));
		for (j = 0; j < (unsigned int)s->alua_nr; j++) {
			for (k = 0; k < ARRAY_LEN(alua_states); k++)
				buf_printf(b, "tcmu_alua_access_state{%s,group=\"%s\",state=\"%s\"} %d\n",
					   labels,
					   esc(s->alua[j].name, esc1, sizeof(esc1)),
					   alua_states[k].name,
					   s->alua[j].state == alua_states[k].val);
		}
	}

	/*
	 * Commands: status="ok" is always exported, other statuses once
	 * they have been seen, to keep the number of series small.
	 */
	family(b, "tcmu_commands_total", "counter",
	       "Completed SCSI commands by command class and status.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		dev_labels(s, labels, sizeof(labels));
		for (j = 0; j < TCMUR_METRICS_OP_MAX; j++) {
			for (k = 0; k < STS_NR; k++) {
				if (k != TCMU_STS_OK && !s->m.cmds[j][k])
					continue;
				buf_printf(b, "tcmu_commands_total{%s,op=\"%s\",status=\"%s\"} %llu\n",
					   labels, op_names[j], sts_names[k],
					   (unsigned long long)s->m.cmds[j][k]);
			}
		}
	}

	family(b, "tcmu_command_duration_seconds", "histogram",
	       "SCSI command latency inside tcmu-runner, from fetching the command to completing it.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		dev_labels(s, labels, sizeof(labels));
		for (j = 0; j < TCMUR_METRICS_OP_MAX; j++) {
			cnt = 0;
			for (k = 0; k < HIST_NR; k++)
				cnt += s->m.hist[j][k];
			if (!cnt && j != TCMUR_METRICS_OP_READ &&
			    j != TCMUR_METRICS_OP_WRITE)
				continue;
			cum = 0;
			for (k = 0; k < HIST_NR - 1; k++) {
				cum += s->m.hist[j][k];
				buf_printf(b, "tcmu_command_duration_seconds_bucket{%s,op=\"%s\",le=\"%s\"} %llu\n",
					   labels, op_names[j], hist_bounds_le[k],
					   (unsigned long long)cum);
			}
			buf_printf(b, "tcmu_command_duration_seconds_bucket{%s,op=\"%s\",le=\"+Inf\"} %llu\n",
				   labels, op_names[j], (unsigned long long)cnt);
			buf_printf(b, "tcmu_command_duration_seconds_sum{%s,op=\"%s\"} %.9f\n",
				   labels, op_names[j],
				   s->m.hist_sum_ns[j] / 1e9);
			buf_printf(b, "tcmu_command_duration_seconds_count{%s,op=\"%s\"} %llu\n",
				   labels, op_names[j], (unsigned long long)cnt);
		}
	}

	family(b, "tcmu_bytes_total", "counter",
	       "Bytes transferred by successful READ and WRITE commands.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		dev_labels(s, labels, sizeof(labels));
		buf_printf(b, "tcmu_bytes_total{%s,direction=\"read\"} %llu\n",
			   labels, (unsigned long long)s->m.bytes_read);
		buf_printf(b, "tcmu_bytes_total{%s,direction=\"write\"} %llu\n",
			   labels, (unsigned long long)s->m.bytes_written);
	}

	family(b, "tcmu_unmap_rejected_total", "counter",
	       "Commands asking to unmap blocks while UNMAP is disabled for the device.");
	for (i = 0; i < nr; i++) {
		s = &snaps[i];
		dev_labels(s, labels, sizeof(labels));
		for (j = 0; j < TCMUR_METRICS_UNMAP_REJECTED_MAX; j++)
			buf_printf(b, "tcmu_unmap_rejected_total{%s,command=\"%s\"} %llu\n",
				   labels, unmap_rejected_names[j],
				   (unsigned long long)s->m.unmap_rejected[j]);
	}

	/* One error family per handler, e.g. tcmu_glfs_errors_total */
	for (i = 0; i < nr; i++) {
		for (h = 0; h < handlers_nr; h++)
			if (!strcmp(handlers[h], snaps[i].m.handler))
				break;
		if (h == handlers_nr && handlers_nr < ARRAY_LEN(handlers))
			handlers[handlers_nr++] = snaps[i].m.handler;
	}
	for (h = 0; h < handlers_nr; h++) {
		metric_name_part(handlers[h], part, sizeof(part));
		snprintf(name, sizeof(name), "tcmu_%s_errors_total", part);
		family(b, name, "counter",
		       "Failed backend requests by operation and errno.");
		for (i = 0; i < nr; i++) {
			s = &snaps[i];
			if (strcmp(s->m.handler, handlers[h]))
				continue;
			dev_labels(s, labels, sizeof(labels));
			for (j = 0; j < TCMUR_BACKEND_OP_MAX; j++) {
				for (k = 0; k < ERR_NR; k++) {
					if (!s->m.backend_err[j][k])
						continue;
					snprintf(esc2, sizeof(esc2), "%s",
						 k == ERR_SHORT_IO ? "short_io" :
						 k == ERR_OTHER ? "other" :
						 errno_tab[k].name);
					buf_printf(b, "%s{%s,op=\"%s\",errno=\"%s\"} %llu\n",
						   name, labels,
						   backend_op_names[j], esc2,
						   (unsigned long long)s->m.backend_err[j][k]);
				}
			}
		}
	}
}

static int write_metrics_file(const struct out_buf *b)
{
	size_t off = 0;
	ssize_t n;
	int fd, err = 0;

	fd = open(metrics_tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		  0644);
	if (fd < 0)
		return -errno;

	while (off < b->len) {
		n = write(fd, b->p + off, b->len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			err = -errno;
			break;
		}
		off += n;
	}
	if (close(fd) && !err)
		err = -errno;
	if (!err && rename(metrics_tmp_path, metrics_path))
		err = -errno;
	if (err)
		unlink(metrics_tmp_path);
	return err;
}

static void export_once(void)
{
	uint64_t start = tcmur_metrics_now_ns();
	unsigned int i, nr = 0;
	int ret;

	ret = snapshot_all(&nr);
	if (ret) {
		write_errors_total++;
		exporter_error("cannot snapshot devices: %s", strerror(-ret));
		return;
	}

	/* configfs reads happen without any tcmu-runner lock held */
	for (i = 0; i < nr; i++)
		read_alua(&snaps[i]);

	format_metrics(&out, nr);
	if (out.failed) {
		write_errors_total++;
		exporter_error("cannot format metrics: out of memory");
		return;
	}

	ret = write_metrics_file(&out);
	if (ret) {
		write_errors_total++;
		exporter_error("cannot write %s: %s", metrics_path,
			       strerror(-ret));
		return;
	}

	writes_total++;
	last_duration_s = (tcmur_metrics_now_ns() - start) / 1e9;
}

/* Run only when nothing else wants the CPU or the disk */
static void exporter_lower_priority(void)
{
	struct sched_param sp = { .sched_priority = 0 };
	pid_t tid = syscall(SYS_gettid);

	if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp))
		setpriority(PRIO_PROCESS, tid, 19);
#ifdef SYS_ioprio_set
	/* IOPRIO_WHO_PROCESS = 1, IOPRIO_CLASS_IDLE = 3 << IOPRIO_CLASS_SHIFT */
	syscall(SYS_ioprio_set, 1, tid, 3 << 13);
#endif
}

/* Returns true when a stop was requested */
static bool exporter_wait(unsigned int ms)
{
	struct timespec ts;
	bool stop;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&stop_lock);
	while (!stop_requested) {
		if (pthread_cond_timedwait(&stop_cond, &stop_lock, &ts) ==
		    ETIMEDOUT)
			break;
	}
	stop = stop_requested;
	pthread_mutex_unlock(&stop_lock);
	return stop;
}

static void *exporter_main(void *arg)
{
	tcmu_set_thread_name("metrics", NULL);
	exporter_lower_priority();

	out.cap = 64 * 1024;
	out.p = malloc(out.cap);
	if (!out.p)
		out.cap = 0;

	do {
		export_once();
	} while (!exporter_wait(metrics_interval_ms));

	/* Do not leave stale numbers behind for node_exporter */
	unlink(metrics_path);

	free(out.p);
	free(snaps);
	__atomic_store_n(&exporter_exited, true, __ATOMIC_RELEASE);
	return NULL;
}

int tcmur_metrics_start(const char *file, unsigned int interval_sec)
{
	pthread_condattr_t cattr;
	pthread_attr_t attr;
	sigset_t all, old;
	int ret;

	if (!file || !file[0]) {
		tcmu_info("Metrics export disabled\n");
		return 0;
	}
	if (exporter_running)
		return -EBUSY;

	if (file[0] != '/' || strlen(file) >= sizeof(metrics_path)) {
		tcmu_err("metrics_file must be an absolute path: %s\n", file);
		return -EINVAL;
	}
	snprintf(metrics_path, sizeof(metrics_path), "%s", file);
	snprintf(metrics_tmp_path, sizeof(metrics_tmp_path), "%s.tmp", file);

	if (!interval_sec)
		interval_sec = 15;
	if (interval_sec > 3600)
		interval_sec = 3600;
#ifndef TCMUR_METRICS_TEST
	metrics_interval_ms = interval_sec * 1000;
#endif

	pthread_condattr_init(&cattr);
	pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
	pthread_cond_init(&stop_cond, &cattr);
	pthread_condattr_destroy(&cattr);
	stop_requested = false;
	exporter_exited = false;

	/* Must be visible before the first device is added */
	metrics_enabled = true;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);

	/* The exporter never handles signals, glib does in the main thread */
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &old);
	ret = pthread_create(&exporter_thread, &attr, exporter_main, NULL);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	pthread_attr_destroy(&attr);

	if (ret) {
		metrics_enabled = false;
		pthread_cond_destroy(&stop_cond);
		tcmu_err("Could not start the metrics exporter: %s; metrics disabled\n",
			 strerror(ret));
		return -ret;
	}

	exporter_running = true;
	tcmu_info("Exporting metrics to %s every %u seconds\n", metrics_path,
		  interval_sec);
	return 0;
}

void tcmur_metrics_stop(void)
{
	int i;

	if (!exporter_running)
		return;

	pthread_mutex_lock(&stop_lock);
	stop_requested = true;
	pthread_cond_signal(&stop_cond);
	pthread_mutex_unlock(&stop_lock);

	/*
	 * A write to a hung filesystem must not block the shutdown: wait up
	 * to 2 seconds, then leave the thread behind and let exit() end it.
	 */
	for (i = 0; i < 200; i++) {
		if (__atomic_load_n(&exporter_exited, __ATOMIC_ACQUIRE))
			break;
		usleep(10000);
	}
	if (__atomic_load_n(&exporter_exited, __ATOMIC_ACQUIRE)) {
		pthread_join(exporter_thread, NULL);
		pthread_cond_destroy(&stop_cond);
	} else {
		tcmu_warn("Metrics exporter did not stop in time, detaching it\n");
		pthread_detach(exporter_thread);
	}

	exporter_running = false;
	metrics_enabled = false;
}

#ifdef TCMUR_METRICS_TEST
void tcmur_metrics_test_set_interval_ms(unsigned int ms)
{
	metrics_interval_ms = ms;
}

void tcmur_metrics_test_set_cfgfs_core(const char *path)
{
	cfgfs_core = path;
}
#endif
