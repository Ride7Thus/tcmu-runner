# node_exporter on tcmu-runner gateways

tcmu-runner does not serve metrics over HTTP. It writes them to a file,
and [node_exporter](https://github.com/prometheus/node_exporter) serves
that file through its *textfile collector*, together with the host
metrics. The same mechanism exports the statistics the kernel SCSI target
(LIO) keeps in configfs, collected by `lio-stats.sh`.

```
 tcmu-runner ──writes──> tcmu_runner.prom ─┐
                                            ├─> node_exporter :9100 <── Prometheus
 lio-stats.timer ─writes─> lio.prom ───────┘   (textfile collector)
```

Set this up on every gateway node, that is every node running
tcmu-runner. The metrics themselves are described in
[monitoring.md](monitoring.md).

## 1. Install node_exporter

Use the upstream release (the steps below) or your distribution package.
With a package, only make sure node_exporter runs with the
`--collector.textfile.directory` option of step 2.

```sh
VER=1.8.2
ARCH=amd64          # arm64 on aarch64 nodes
curl -LO https://github.com/prometheus/node_exporter/releases/download/v$VER/node_exporter-$VER.linux-$ARCH.tar.gz
tar xzf node_exporter-$VER.linux-$ARCH.tar.gz
install -m 0755 node_exporter-$VER.linux-$ARCH/node_exporter /usr/local/bin/
useradd --system --no-create-home --shell /sbin/nologin node_exporter
```

## 2. Textfile collector directory

tcmu-runner and `lio-stats.sh` run as root and write the files,
node_exporter only reads them:

```sh
mkdir -p /var/lib/node_exporter/textfile_collector
chmod 0755 /var/lib/node_exporter /var/lib/node_exporter/textfile_collector
```

`/etc/systemd/system/node_exporter.service`:

```ini
[Unit]
Description=Prometheus node_exporter
After=network-online.target
Wants=network-online.target

[Service]
User=node_exporter
ExecStart=/usr/local/bin/node_exporter \
    --collector.textfile.directory=/var/lib/node_exporter/textfile_collector
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

```sh
systemctl daemon-reload
systemctl enable --now node_exporter
firewall-cmd --permanent --add-port=9100/tcp && firewall-cmd --reload
```

Only the Prometheus servers need to reach port 9100. Restrict it in the
firewall zone when the storage network is shared.

## 3. tcmu-runner metrics

In `/etc/tcmu/tcmu.conf`:

```
metrics_file = "/var/lib/node_exporter/textfile_collector/tcmu_runner.prom"
metrics_interval = 15
```

The file name must end with `.prom`, node_exporter ignores everything
else. The settings are read at startup only:

```sh
systemctl restart tcmu-runner
```

Restarting tcmu-runner briefly interrupts I/O on its devices. Do it
during a maintenance window, one gateway at a time, when multipath
initiators can fail over to the other gateways.

The tcmu-runner log confirms the export:

```
Exporting metrics to /var/lib/node_exporter/textfile_collector/tcmu_runner.prom every 15 seconds
```

## 4. LIO statistics

The `tcmu-runner` package installs `/usr/libexec/tcmu-runner/lio-stats.sh`
and a systemd timer that runs it every 15 seconds at idle CPU and I/O
priority:

```sh
systemctl enable --now lio-stats.timer
```

The script only reads configfs, it works with any LIO backstore and does
not depend on tcmu-runner metrics being enabled. It exports:

| Metric | Labels | Meaning |
|---|---|---|
| `lio_lu_commands_total` | `hba`, `device` | SCSI commands received by the backstore |
| `lio_lu_read_bytes_total`, `lio_lu_written_bytes_total` | `hba`, `device` | Data read and written, 1 MiB granularity |
| `lio_lu_resets_total` | `hba`, `device` | LU resets |
| `lio_device_aborts_complete_total`, `lio_device_aborts_no_task_total` | `hba`, `device` | Task aborts (initiator timeouts) |
| `lio_port_commands_total` | `fabric`, `target`, `tpgt`, `lun`, `device` | Commands per target port group: shows which gateway path carries the I/O |
| `lio_port_read_bytes_total`, `lio_port_written_bytes_total` | same | Data per target port, 1 MiB granularity |
| `lio_iscsi_sessions` | `target` | Active iSCSI sessions |
| `lio_iscsi_failed_sessions_total` | `target` | Failed sessions |
| `lio_iscsi_digest_errors_total`, `lio_iscsi_connection_errors_total`, `lio_iscsi_format_errors_total` | `target` | Session failures by cause |
| `lio_iscsi_login_accepts_total`, `lio_iscsi_login_redirects_total` | `target` | Accepted and redirected logins |
| `lio_iscsi_login_failures_total` | `target`, `reason` | Rejected logins: `other`, `authorize`, `authenticate`, `negotiate` |
| `lio_stats_timestamp_seconds` | | Collection time, to detect a stopped timer |

Notes:

- The values come from the configfs statistics of the kernel
  (`target_core_stat.c`, `iscsi_target_stat.c`). Files missing on the
  running kernel are skipped.
- The kernel resets them when a backstore or target is recreated, for
  example when gluster-block recreates a block on a gateway. Prometheus
  `rate()` and `increase()` handle such resets.
- To run the script by hand: `/usr/libexec/tcmu-runner/lio-stats.sh /tmp/lio.prom`.

## 5. Check the setup

```sh
curl -s localhost:9100/metrics | grep -E '^(tcmu|lio)_' | head
curl -s localhost:9100/metrics | grep node_textfile_scrape_error
```

`node_textfile_scrape_error 0` means node_exporter parsed all files. With
1, a file is malformed or unreadable, and node_exporter logs which one.

node_exporter also reports `node_textfile_mtime_seconds{file="..."}` for
each file, the [alerting rules](monitoring.md#alerts) use the timestamps
inside the files instead.

## SELinux

With SELinux enforcing, check for denials when a file does not appear:

```sh
ausearch -m AVC,USER_AVC -ts recent | grep -E 'tcmu-runner|lio-stats|node_exporter'
```

The labels that work depend on the policy of the distribution. Label the
directory so that the writers may create files and node_exporter may read
them, then `restorecon -Rv /var/lib/node_exporter`.
