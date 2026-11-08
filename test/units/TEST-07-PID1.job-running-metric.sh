#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

# Verify that the manager counts how long jobs ran in the JobRunningUSec histogram metric, by job type and
# result, and keeps the counts across daemon-reload and daemon-reexec.

SOCKET=/run/systemd/report/io.systemd.Manager
METRIC=io.systemd.Manager.JobRunningUSec

test -S "$SOCKET"

[[ "$(varlinkctl --more --json=short call "$SOCKET" io.systemd.Metrics.Describe '{}' |
      jq --seq -r --arg name "$METRIC" 'select(.name == $name) | .type' | tr -d '\036')" == histogram ]]

list_cells() {
    # Prints one JSON object per line, one for each combination of job type and result observed so far.
    # Strip the RS separator that jq --seq re-emits on output.
    varlinkctl --more --json=short call "$SOCKET" io.systemd.Metrics.List '{}' |
        jq --seq -c --arg name "$METRIC" 'select(.name == $name)' | tr -d '\036'
}

cell_count() {
    # $1: file written by list_cells, $2: job type, $3: job result. Combinations that have not been observed
    # yet are not listed, so default to 0.
    jq -s -r --arg type "$2" --arg result "$3" \
        '[.[] | select(.fields.type == $type and .fields.result == $result) | .value.count] | add // 0' "$1"
}

check_cells() {
    # $1: file written by list_cells. Bucket bounds are strictly increasing, bucket counts are cumulative, and
    # the total count covers the last bucket.
    jq -s -e '
        all(.[];
            (.value.buckets | length > 0) and
            (.value.buckets | map(.upperBound) | . == (sort | unique)) and
            (.value.buckets | map(.count) | . == sort) and
            (.value.count >= (.value.buckets | last | .count)) and
            (.value.sum | type == "number"))' "$1" >/dev/null
}

check_kept() {
    # $1: file written by list_cells before, $2: after. Every combination listed before must still be listed,
    # and no count may have decreased. Other jobs finish in the meantime, so counts may have increased.
    jq -n -e --slurpfile before "$1" --slurpfile after "$2" '
        all($before[]; . as $b |
            any($after[]; . as $a |
                $a.fields == $b.fields and
                $a.value.count >= $b.value.count and
                $a.value.sum >= $b.value.sum and
                ($a.value.buckets | length) == ($b.value.buckets | length) and
                all(range($b.value.buckets | length); $a.value.buckets[.].count >= $b.value.buckets[.].count)))' \
        >/dev/null
}

WORKDIR=$(mktemp -d)
UNIT="test-job-running-$RANDOM"
trap 'systemctl stop "$UNIT.service" || :; systemctl reset-failed "$UNIT-fail.service" || :; rm -rf "$WORKDIR"' EXIT

list_cells >"$WORKDIR/before"
check_cells "$WORKDIR/before"

# A start job, a restart job (which is counted as "restart" for the stop phase and as "start" for the start
# phase), a stop job, and a start job that fails.
systemd-run --unit="$UNIT.service" sleep infinity
systemctl restart "$UNIT.service"
systemctl stop "$UNIT.service"
(! systemd-run --wait --unit="$UNIT-fail.service" --service-type=oneshot false)

list_cells >"$WORKDIR/after"
check_cells "$WORKDIR/after"
(( $(cell_count "$WORKDIR/after" start "done") >= $(cell_count "$WORKDIR/before" start "done") + 2 ))
(( $(cell_count "$WORKDIR/after" restart "done") >= $(cell_count "$WORKDIR/before" restart "done") + 1 ))
(( $(cell_count "$WORKDIR/after" stop "done") >= $(cell_count "$WORKDIR/before" stop "done") + 1 ))
(( $(cell_count "$WORKDIR/after" start failed) >= $(cell_count "$WORKDIR/before" start failed) + 1 ))
check_kept "$WORKDIR/before" "$WORKDIR/after"

# The counts survive a daemon-reload...
systemctl daemon-reload
list_cells >"$WORKDIR/reload"
check_cells "$WORKDIR/reload"
check_kept "$WORKDIR/after" "$WORKDIR/reload"

# ... and a daemon-reexec. `systemctl daemon-reexec` returns as soon as the old PID 1 closes its bus
# connection, before the new PID 1 is up. Ping it over the bus, the broker holds the call until it answers.
systemctl daemon-reexec
busctl --watch-bind=yes call org.freedesktop.systemd1 /org/freedesktop/systemd1 \
                           org.freedesktop.DBus.Peer Ping >/dev/null
list_cells >"$WORKDIR/reexec"
check_cells "$WORKDIR/reexec"
check_kept "$WORKDIR/reload" "$WORKDIR/reexec"
