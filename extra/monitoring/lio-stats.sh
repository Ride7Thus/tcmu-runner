#!/bin/bash
#
# Export LIO (kernel SCSI target) statistics from configfs in the
# Prometheus text format, for the node_exporter textfile collector.
#
# USAGE: lio-stats.sh [output file]
#
# Default output: /var/lib/node_exporter/textfile_collector/lio.prom
# CONFIGFS can point to another configfs target root (tests).
#
# The script only reads configfs files, it needs no tcmu-runner support
# and works with any LIO backstore. Files missing on the running kernel
# are skipped. See docs/node-exporter.md.

set -u

CONFIGFS=${CONFIGFS:-/sys/kernel/config/target}
OUT=${1:-/var/lib/node_exporter/textfile_collector/lio.prom}
TMP="$OUT.tmp"
MIB=1048576

# Samples are collected per metric and printed family by family: the
# text format requires the samples of one metric to be contiguous.
declare -a ORDER=()
declare -A TYPE=() HELP=() SAMPLES=()

metric() {
	ORDER+=("$1")
	TYPE[$1]=$2
	HELP[$1]=$3
	SAMPLES[$1]=""
}

# sample <metric> <labels> <value>
sample() {
	SAMPLES[$1]+="$1{$2} $3"$'\n'
}

# Prometheus label value escaping
esc() {
	local s=${1//\\/\\\\}
	s=${s//\"/\\\"}
	printf '%s' "$s"
}

# First word of a configfs file, fails when it is missing or unreadable
val() {
	local v
	[ -r "$1" ] || return 1
	read -r v _ < "$1" 2>/dev/null || return 1
	[ -n "$v" ] || return 1
	printf '%s' "$v"
}

core_stats() {
	local d hba dev v lbl

	for d in "$CONFIGFS"/core/*/*/statistics; do
		[ -d "$d" ] || continue
		dev=${d%/statistics}
		hba=${dev%/*}
		hba=${hba##*/}
		dev=${dev##*/}
		lbl="hba=\"$(esc "$hba")\",device=\"$(esc "$dev")\""

		v=$(val "$d/scsi_lu/num_cmds") &&
			sample lio_lu_commands_total "$lbl" "$v"
		v=$(val "$d/scsi_lu/read_mbytes") &&
			sample lio_lu_read_bytes_total "$lbl" "$((v * MIB))"
		v=$(val "$d/scsi_lu/write_mbytes") &&
			sample lio_lu_written_bytes_total "$lbl" "$((v * MIB))"
		v=$(val "$d/scsi_lu/resets") &&
			sample lio_lu_resets_total "$lbl" "$v"
		v=$(val "$d/scsi_tgt_dev/aborts_complete") &&
			sample lio_device_aborts_complete_total "$lbl" "$v"
		v=$(val "$d/scsi_tgt_dev/aborts_no_task") &&
			sample lio_device_aborts_no_task_total "$lbl" "$v"
	done
}

# Per target portal group LUN: shows which gateway path carries the I/O
port_stats() {
	local lun tpg tgt fabric dev link st lbl v

	for lun in "$CONFIGFS"/*/*/tpgt_*/lun/lun_*; do
		[ -d "$lun" ] || continue
		st="$lun/statistics/scsi_tgt_port"
		[ -d "$st" ] || continue
		tpg=${lun%/lun/*}
		tgt=${tpg%/*}
		fabric=${tgt%/*}
		fabric=${fabric##*/}
		tpg=${tpg##*/}
		tgt=${tgt##*/}

		# The LUN links to its backstore: .../core/<hba>/<device>
		dev=""
		for link in "$lun"/*; do
			if [ -L "$link" ]; then
				dev=$(readlink "$link")
				dev=${dev##*/}
				break
			fi
		done

		lbl="fabric=\"$(esc "$fabric")\",target=\"$(esc "$tgt")\",tpgt=\"${tpg#tpgt_}\",lun=\"${lun##*/lun_}\",device=\"$(esc "$dev")\""
		v=$(val "$st/in_cmds") &&
			sample lio_port_commands_total "$lbl" "$v"
		v=$(val "$st/read_mbytes") &&
			sample lio_port_read_bytes_total "$lbl" "$((v * MIB))"
		v=$(val "$st/write_mbytes") &&
			sample lio_port_written_bytes_total "$lbl" "$((v * MIB))"
	done
}

iscsi_stats() {
	local t tgt fs lbl v r

	for t in "$CONFIGFS"/iscsi/iqn.* "$CONFIGFS"/iscsi/eui.* "$CONFIGFS"/iscsi/naa.*; do
		[ -d "$t" ] || continue
		tgt=${t##*/}
		fs="$t/fabric_statistics"
		lbl="target=\"$(esc "$tgt")\""

		v=$(val "$fs/iscsi_instance/sessions") &&
			sample lio_iscsi_sessions "$lbl" "$v"
		v=$(val "$fs/iscsi_instance/fail_sess") &&
			sample lio_iscsi_failed_sessions_total "$lbl" "$v"
		v=$(val "$fs/iscsi_sess_err/digest_errors") &&
			sample lio_iscsi_digest_errors_total "$lbl" "$v"
		v=$(val "$fs/iscsi_sess_err/cxn_errors") &&
			sample lio_iscsi_connection_errors_total "$lbl" "$v"
		v=$(val "$fs/iscsi_sess_err/format_errors") &&
			sample lio_iscsi_format_errors_total "$lbl" "$v"
		v=$(val "$fs/iscsi_login_stats/accepts") &&
			sample lio_iscsi_login_accepts_total "$lbl" "$v"
		v=$(val "$fs/iscsi_login_stats/redirects") &&
			sample lio_iscsi_login_redirects_total "$lbl" "$v"
		for r in other authorize authenticate negotiate; do
			v=$(val "$fs/iscsi_login_stats/${r}_fails") &&
				sample lio_iscsi_login_failures_total "$lbl,reason=\"$r\"" "$v"
		done
	done
}

metric lio_stats_timestamp_seconds gauge "Unix time when the LIO statistics were collected."
metric lio_lu_commands_total counter "SCSI commands received by the backstore LU."
metric lio_lu_read_bytes_total counter "Bytes read from the backstore LU, 1 MiB granularity."
metric lio_lu_written_bytes_total counter "Bytes written to the backstore LU, 1 MiB granularity."
metric lio_lu_resets_total counter "LU resets."
metric lio_device_aborts_complete_total counter "Aborted tasks that had completed."
metric lio_device_aborts_no_task_total counter "Abort requests for tasks that did not exist."
metric lio_port_commands_total counter "SCSI commands received through the target port (TPG LUN)."
metric lio_port_read_bytes_total counter "Bytes read through the target port, 1 MiB granularity."
metric lio_port_written_bytes_total counter "Bytes written through the target port, 1 MiB granularity."
metric lio_iscsi_sessions gauge "Active iSCSI sessions of the target."
metric lio_iscsi_failed_sessions_total counter "Failed iSCSI sessions."
metric lio_iscsi_digest_errors_total counter "iSCSI sessions failed by digest errors."
metric lio_iscsi_connection_errors_total counter "iSCSI sessions failed by connection errors."
metric lio_iscsi_format_errors_total counter "iSCSI sessions failed by PDU format errors."
metric lio_iscsi_login_accepts_total counter "Accepted iSCSI logins."
metric lio_iscsi_login_redirects_total counter "Redirected iSCSI logins."
metric lio_iscsi_login_failures_total counter "Rejected iSCSI logins by reason."

SAMPLES[lio_stats_timestamp_seconds]="lio_stats_timestamp_seconds $(date +%s)"$'\n'
core_stats
port_stats
iscsi_stats

{
	for m in "${ORDER[@]}"; do
		printf '# HELP %s %s\n# TYPE %s %s\n%s' \
			"$m" "${HELP[$m]}" "$m" "${TYPE[$m]}" "${SAMPLES[$m]}"
	done
} > "$TMP" && mv -f "$TMP" "$OUT"
