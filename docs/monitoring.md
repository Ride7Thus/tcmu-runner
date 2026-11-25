# Monitoring tcmu-runner

tcmu-runner can export Prometheus metrics about its devices: the state of
each device (lock, recovery, ALUA), every SCSI command it completes with
its latency and status, and the errors of the storage backend. This page
describes the metrics, how they are collected and how to use them with
Prometheus and Grafana.

- Setting up node_exporter and the LIO kernel statistics:
  [node-exporter.md](node-exporter.md)
- Example files, installed in `/usr/share/tcmu-runner/monitoring/`:
  - [`prometheus.yml`](../extra/monitoring/prometheus.yml): scrape configuration
  - [`tcmu-runner.rules.yml`](../extra/monitoring/tcmu-runner.rules.yml): alerting rules
  - [`grafana-dashboard.json`](../extra/monitoring/grafana-dashboard.json): Grafana dashboard

## How it works

```
                         tcmu-runner
  ┌──────────────────────────────────────────────────────────┐
  │ cmdproc / handler threads          exporter thread       │
  │   command done ──atomic add──┐     (SCHED_IDLE, idle I/O) │
  │   lock / recovery ──store────┼──>  copies counters  ─────┼──> tcmu_runner.prom
  │   GFAPI error ──atomic add───┘     every 15 s            │         │
  └──────────────────────────────────────────────────────────┘         │
                                                     node_exporter <───┘
                                                          ▲
                                                     Prometheus ──> Grafana
```

There is no HTTP endpoint in tcmu-runner. One exporter thread writes the
metrics in the Prometheus text format to a file, atomically (write to
`<file>.tmp`, then rename), and node_exporter serves the file through its
textfile collector.

### Isolation from the I/O path

The exporter must never slow down or block storage I/O. The code follows
these rules:

- **The I/O path only does atomic increments.** Per device counters are
  updated with relaxed atomic operations: no locks, no memory
  allocation, no system calls except reading `CLOCK_MONOTONIC` (vDSO)
  when a command is fetched and when it completes. Measured cost: about
  50 ns per command with metrics enabled, 1 ns with metrics disabled.
- **The exporter never touches device state.** It does not take device
  locks, does not call handlers and does not read `struct tcmur_device`.
  Lock and recovery state are mirrored into the metrics block where they
  change.
- **One short critical section.** A registry mutex, separate from all
  I/O locks, protects the list of exported devices. Device add and
  remove take it, and the exporter takes it only to copy the counters.
  Memory for the copy is allocated with the mutex released.
- **The exporter runs only when the system is idle.** It runs with the
  `SCHED_IDLE` CPU policy and the idle I/O class, blocks all signals
  and logs its own errors at most once every 10 minutes.
- **It is not fatal.** If the exporter cannot start, or the file cannot
  be written, tcmu-runner keeps serving I/O. A write error is counted in
  `tcmu_metrics_write_errors_total`.
- **Shutdown does not wait for it.** On stop, tcmu-runner waits up to 2
  seconds for the exporter, for example if it is blocked on a hung
  filesystem, then continues without it. The exporter removes the metrics
  file on stop so that node_exporter does not serve stale values.

Because the exporter runs at idle priority, a host saturated by other
work delays it. `tcmu_metrics_timestamp_seconds` shows when the file was
written, and the `TcmuRunnerMetricsStale` alert fires when it gets old.

## Enabling

In `/etc/tcmu/tcmu.conf`:

```
metrics_file = "/var/lib/node_exporter/textfile_collector/tcmu_runner.prom"
metrics_interval = 15
```

| Option | Default | Meaning |
|---|---|---|
| `metrics_file` | empty: export disabled | Absolute path of the file to write. It must end with `.prom` for node_exporter. |
| `metrics_interval` | `15` | Seconds between two exports, 1 to 3600. Keep it at or below the Prometheus scrape interval. |

Both options are read at startup only: restart tcmu-runner after changing
them. With `metrics_file` unset, tcmu-runner allocates nothing and the
I/O path skips all accounting.

## Metrics

All device metrics carry two labels:

- `device`: the backstore name, which is the block name for gluster-block.
- `handler`: the handler subtype, for example `glfs`, `rbd`, `qcow` or `file`.

Prometheus adds `instance` (the gateway) when it scrapes node_exporter.

### Exporter

| Metric | Type | Meaning |
|---|---|---|
| `tcmu_runner_info{version}` | gauge | Always 1, with the tcmu-runner version. |
| `tcmu_metrics_timestamp_seconds` | gauge | Unix time when the file was written. |
| `tcmu_metrics_writes_total` | counter | Files written successfully. |
| `tcmu_metrics_write_errors_total` | counter | Exports that failed: no memory, or the file could not be written. |
| `tcmu_metrics_export_duration_seconds` | gauge | Duration of the previous export. |
| `tcmu_devices` | gauge | Number of devices handled by tcmu-runner. |

### Device state

| Metric | Type | Extra labels | Meaning |
|---|---|---|---|
| `tcmu_device_info` | gauge | `hba` | Always 1, one series per device. |
| `tcmu_device_size_bytes` | gauge | | Device size, updated on resize. |
| `tcmu_lock_state` | gauge | `state` | 1 for the current state of the exclusive lock, 0 for the others. |
| `tcmu_in_recovery` | gauge | | 1 while tcmu-runner reopens the device after a failure. I/O to the device waits meanwhile. |
| `tcmu_inflight_commands` | gauge | | Commands submitted to the handler and not completed yet. |
| `tcmu_lock_lost_total` | counter | | Times the device lost its lock to another gateway. |
| `tcmu_conn_lost_total` | counter | | Times the handler reported a lost backend connection. |
| `tcmu_cmd_timed_out_total` | counter | | Commands that exceeded the device `cmd_time_out`. |
| `tcmu_alua_access_state` | gauge | `group`, `state` | 1 for the current ALUA state of each target port group of the device. |

`state` of `tcmu_lock_state`: `unknown`, `unlocked`, `read_locking`,
`read_locked`, `write_locking`, `write_locked`. A gateway serving I/O to
a device with implicit ALUA failover (glfs, rbd) holds `write_locked`.
The lock state only means something for handlers that implement locking.

`state` of `tcmu_alua_access_state`: `active_optimized`,
`active_nonoptimized`, `standby`, `unavailable`, `lba_dependent`,
`offline`, `transitioning`. The exporter reads it from
`/sys/kernel/config/target/core/<hba>/<device>/alua/<group>/alua_access_state`.

### Commands

| Metric | Type | Extra labels | Meaning |
|---|---|---|---|
| `tcmu_commands_total` | counter | `op`, `status` | Completed SCSI commands. |
| `tcmu_command_duration_seconds` | histogram | `op` | Latency inside tcmu-runner. |
| `tcmu_bytes_total` | counter | `direction` | Bytes of successful READ (`read`) and WRITE (`write`) commands. |

`op` is the command class:

| `op` | SCSI commands |
|---|---|
| `read` | READ (6), (10), (12), (16) |
| `write` | WRITE (6), (10), (12), (16), WRITE AND VERIFY (10), (16) |
| `flush` | SYNCHRONIZE CACHE (10), (16) |
| `unmap` | UNMAP |
| `write_same` | WRITE SAME (10), (16) |
| `compare_and_write` | COMPARE AND WRITE |
| `extended_copy` | EXTENDED COPY |
| `other` | everything else: INQUIRY, TEST UNIT READY, READ CAPACITY, ... |

`status` is the tcmu-runner completion status: `ok`, `busy`,
`hardware_error`, `read_error`, `write_error`, `timeout`, `fenced`,
`no_resource`, `miscompare`, `lba_out_of_range`, `alua_transition`,
`invalid_command`, `invalid_cdb`, `invalid_param_list`,
`invalid_param_list_len`, `not_handled`, and a few rarer ones (see
`sts_names` in `tcmur_metrics.c`). Initiators see `hardware_error`,
`read_error`, `write_error` and `timeout` as I/O errors. `busy` and
`alua_transition` make them retry.

To keep the number of series small, `status="ok"` is always exported and
other statuses appear once they have occurred.

`tcmu_command_duration_seconds` is measured from the moment tcmu-runner
fetches the command from the kernel ring to the moment it completes it.
It includes the backend (Gluster, Ceph) and the tcmu-runner queueing. It
does not include the iSCSI network and the kernel target. Buckets: 0.1 ms
to 30 s. Histograms are exported for `read` and `write` always, and for
the other classes once such a command has completed.

### Backend errors and UNMAP

| Metric | Type | Extra labels | Meaning |
|---|---|---|---|
| `tcmu_glfs_errors_total` | counter | `op`, `errno` | Failed GFAPI requests of the glfs handler. |
| `tcmu_unmap_rejected_total` | counter | `command` | Commands asking to unmap blocks while UNMAP is disabled for the device. |

`tcmu_glfs_errors_total` counts every GFAPI request that failed, when it
was submitted or when it completed, whatever tcmu-runner did next (retry,
fallback, failing the command):

- `op`: `open` (opening or reopening the device), `read`, `write`,
  `flush`, `discard`, `zerofill` (native WRITE SAME), `lock`
  (exclusive lock and unlock).
- `errno`: `EIO`, `ENOTCONN`, `ETIMEDOUT`, `EBUSY`, `EAGAIN`, `ENOSPC`,
  `EDQUOT`, `EROFS`, `EOPNOTSUPP`, `ENOSYS`, `EINVAL`, `ENOENT`,
  `ESTALE`, `ENOMEM`, `EBADF`, `EACCES`, `EPERM`, `short_io` (the request
  completed with fewer bytes than requested), `other`. A series appears
  once the error has occurred.

Typical meanings: `ENOTCONN` and `ETIMEDOUT` mean the bricks cannot be
reached, `EBUSY` means another gateway took the lock, `EIO` may mean
split-brain or a brick failure, `EOPNOTSUPP` on `zerofill` means a
translator does not support it, and tcmu-runner switched to WRITE SAME
emulation.

The family is named after the handler: devices of the rbd handler would
report `tcmu_rbd_errors_total` once that handler reports errors. Only the
glfs handler reports backend errors today.

`tcmu_unmap_rejected_total`, `command`:

- `unmap`: an UNMAP command was rejected.
- `write_same`: a WRITE SAME with the UNMAP bit was served as a plain
  WRITE SAME.

UNMAP is disabled when the handler does not support it or, with glfs,
when the volume is dispersed, to keep the backing file fully allocated.
tcmu-runner then no longer advertises logical block provisioning, so a
growing counter means an initiator ignores the Logical Block Provisioning
VPD page.

### LIO kernel statistics

`lio_*` metrics come from `lio-stats.sh`, see
[node-exporter.md](node-exporter.md#4-lio-statistics).

## Prometheus

Scrape node_exporter on every gateway, see
[`prometheus.yml`](../extra/monitoring/prometheus.yml):

```yaml
rule_files:
  - tcmu-runner.rules.yml

scrape_configs:
  - job_name: node
    static_configs:
      - targets: ['gateway-1:9100', 'gateway-2:9100', 'gateway-3:9100']
```

The alerting rules and the dashboard expect the job to be called `node`.
Adjust `up{job="node"}` in `TcmuRunnerMetricsMissing` otherwise.

Useful queries:

```promql
# IOPS per device and command class
sum by (instance, device, op) (rate(tcmu_commands_total{status="ok"}[5m]))

# Throughput
sum by (instance, device, direction) (rate(tcmu_bytes_total[5m]))

# p99 write latency
histogram_quantile(0.99,
  sum by (instance, device, le) (rate(tcmu_command_duration_seconds_bucket{op="write"}[5m])))

# Share of failed commands
sum by (instance, device) (rate(tcmu_commands_total{status!="ok"}[5m]))
  / sum by (instance, device) (rate(tcmu_commands_total[5m]))

# Which gateway owns each device
tcmu_lock_state{state="write_locked"} == 1

# Gluster errors in the last hour
sum by (instance, device, op, errno) (increase(tcmu_glfs_errors_total[1h])) > 0
```

## Alerts

[`tcmu-runner.rules.yml`](../extra/monitoring/tcmu-runner.rules.yml)
contains the following rules. The thresholds are starting points, adjust
them to your storage.

| Alert | Severity | Fires when |
|---|---|---|
| `TcmuRunnerMetricsMissing` | warning | node_exporter is up but has no tcmu-runner metrics. |
| `TcmuRunnerMetricsStale` | warning | The metrics file is older than 2 minutes. |
| `TcmuRunnerMetricsWriteErrors` | warning | tcmu-runner cannot write its metrics file. |
| `TcmuHardwareErrors` | critical | Commands complete with HARDWARE ERROR. |
| `TcmuCommandErrors` | warning | Commands fail with read/write errors, timeouts, fencing, no resources or miscompare. |
| `TcmuCommandTimeouts` | warning | Commands exceed `cmd_time_out`. |
| `TcmuWriteLatencyHigh` | warning | p99 write latency above 200 ms for 10 minutes. |
| `TcmuInflightHigh` | warning | More than 128 commands wait for the backend for 5 minutes. |
| `TcmuLockLost` | warning | A device lost its lock (failover). |
| `TcmuLockFlapping` | critical | A device lost its lock more than 3 times in 30 minutes. |
| `TcmuMultipleLockOwners` | critical | More than one gateway holds the write lock of a device. |
| `TcmuDeviceInRecovery` | warning | A device is being reopened for more than 2 minutes. |
| `TcmuBackendConnectionLost` | warning | The handler lost its backend connection. |
| `TcmuGlfsErrors` | warning | GFAPI requests fail. |
| `TcmuUnmapRejected` | info | An initiator unmaps blocks although UNMAP is disabled. |
| `LioIscsiSessionFailures` | warning | iSCSI sessions fail. |
| `LioIscsiLoginFailures` | warning | iSCSI logins are rejected. |
| `LioStatsStale` | warning | `lio-stats.timer` stopped updating the LIO statistics. |

`TcmuMultipleLockOwners` is the most important one with multipath. Two
gateways that both believe they own a device write to it concurrently.
On a Gluster volume without enforced mandatory locks, tcmu-runner logs
`lock based fencing disabled` and cannot detect this situation by itself.

Check the rules before loading them:

```sh
promtool check rules tcmu-runner.rules.yml
```

## Grafana

Import [`grafana-dashboard.json`](../extra/monitoring/grafana-dashboard.json)
in Grafana (Dashboards, New, Import; tested with Grafana 11.2) and choose
the Prometheus data source. The `Gateway` and `Device` variables filter
all panels.

| Row | Panels |
|---|---|
| Overview | devices, commands/s, failed commands/s, lock losses in the last hour, devices in recovery, metrics age |
| I/O | IOPS, throughput, p50/p99 latency, write latency heatmap, average latency, in-flight commands |
| Errors | failed commands by status, Gluster backend errors, timeouts and lost connections, rejected UNMAP |
| HA and locking | lock owner and ALUA optimized path timelines, current lock state table, lock losses |
| LIO (kernel) | commands per target port, iSCSI sessions |
| Exporter | metrics age, export duration, write errors |

## Troubleshooting

**No `tcmu_*` metrics.** Check that tcmu-runner logged
`Exporting metrics to ...` at startup. Without that line, `metrics_file`
is not set or is not an absolute path. Check that the file exists and
that `node_textfile_scrape_error` is 0.

**`tcmu_metrics_write_errors_total` grows.** The directory does not
exist, is not writable, or SELinux denies the write. tcmu-runner logs the
reason, at most once every 10 minutes.

**Stale metrics.** The exporter runs at idle priority. On a host
saturated by other work it runs late. Check the host CPU usage before
suspecting tcmu-runner.

**A device is missing.** Devices are exported once they are fully set up,
and removed from the file when they are removed from tcmu-runner.
