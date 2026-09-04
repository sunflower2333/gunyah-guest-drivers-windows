#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 EVIDENCE_DIR [WINDOWS_OUTPUT_DIRECTORY]" >&2
    exit 2
fi

evidence_dir=$1
windows_output=${2:-C:/DroidVM/TurnipRuns/evidence/viogpu-58186-c913fd87-host-$(date -u +%Y%m%dT%H%M%SZ)}
adb_bin=${ADB_BIN:-adb}
ssh_bin=${SSH_BIN:-ssh}
adb_serial=${ADB_SERIAL:-192.168.60.237:5555}
windows_host=${WINDOWS_HOST:-USER@192.168.60.237}
ssh_connect_timeout=${SSH_CONNECT_TIMEOUT_SECONDS:-10}
workload_timeout=${WORKLOAD_TIMEOUT_SECONDS:-1000}
remote_capture=/data/local/tmp/capture-crosvm-context-diagnostics-bbd5eacd.sh

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
capture_helper=$script_dir/capture-crosvm-context-diagnostics-bbd5eacd.sh
classifier=$script_dir/classify-crosvm-context-diagnostics.py
turnip_runner=$script_dir/run-viogpu-58186-turnip.ps1
windows_runner=C:/DroidVM/TurnipRuns/viogpu-58186-c913fd87/run-viogpu-58186-turnip.ps1

expected_capture_hash=d45cc3a8377f72544a1f97488b5118ee7a33b4bf469801053e44faff581ef635
expected_classifier_hash=b1ea43fb99b933658f0f6e4000635469839b1f4e7173ddbdb62cf0aa71460d5a
expected_turnip_hash=f8f9e26ca5e5512b8c7d000d3515277e702babc05b84dfca5a650212fe299513

case "$evidence_dir" in
    '') echo "EVIDENCE_DIR must not be empty" >&2; exit 2 ;;
esac
case "$windows_output" in
    *[!A-Za-z0-9_./:\\-]*)
        echo "WINDOWS_OUTPUT_DIRECTORY contains unsupported characters" >&2
        exit 2
        ;;
esac
case "$ssh_connect_timeout:$workload_timeout" in
    *[!0-9:]*|:*|*:) echo "timeout values must be positive decimal integers" >&2; exit 2 ;;
esac
test "$ssh_connect_timeout" -gt 0
test "$workload_timeout" -gt 0

hash_file()
{
    sha256sum "$1" | awk '{print $1}'
}

verify_hash()
{
    file=$1
    expected=$2
    actual=$(hash_file "$file")
    if [ "$actual" != "$expected" ]; then
        echo "SHA-256 mismatch for $file: expected $expected, got $actual" >&2
        exit 3
    fi
}

read_key()
{
    file=$1
    key=$2
    count=$(awk -F= -v key="$key" '$1 == key { count += 1 } END { print count + 0 }' "$file")
    if [ "$count" -ne 1 ]; then
        echo "expected one $key entry in $file, found $count" >&2
        exit 4
    fi
    awk -F= -v key="$key" '$1 == key { print substr($0, index($0, "=") + 1) }' "$file"
}

verify_hash "$capture_helper" "$expected_capture_hash"
verify_hash "$classifier" "$expected_classifier_hash"
verify_hash "$turnip_runner" "$expected_turnip_hash"

if [ -e "$evidence_dir" ]; then
    echo "evidence directory already exists: $evidence_dir" >&2
    exit 2
fi
mkdir "$evidence_dir"

if [ "$("$adb_bin" -s "$adb_serial" get-state 2>/dev/null)" != "device" ]; then
    echo "wireless ADB device is not online: $adb_serial" >&2
    exit 5
fi

"$adb_bin" -s "$adb_serial" push "$capture_helper" "$remote_capture" \
    >"$evidence_dir/android-helper-push.txt" 2>&1
"$adb_bin" -s "$adb_serial" shell chmod 0755 "$remote_capture"
remote_capture_hash=$(
    "$adb_bin" -s "$adb_serial" shell sha256sum "$remote_capture" |
        awk '{print $1}' | tr -d '\r'
)
if [ "$remote_capture_hash" != "$expected_capture_hash" ]; then
    echo "Android capture helper SHA-256 mismatch: $remote_capture_hash" >&2
    exit 5
fi

baseline=$evidence_dir/host-baseline.txt
if ! "$adb_bin" -s "$adb_serial" shell "$remote_capture" --baseline \
    >"$baseline" 2>"$evidence_dir/host-baseline.stderr.txt"; then
    echo "Host baseline failed" >&2
    exit 5
fi
baseline_complete=$(read_key "$baseline" BASELINE_COMPLETE)
baseline_line=$(read_key "$baseline" BASELINE_LINE)
baseline_inode=$(read_key "$baseline" BASELINE_INODE)
case "$baseline_complete:$baseline_line:$baseline_inode" in
    1:[0-9]*:[0-9]*) ;;
    *) echo "malformed Host baseline" >&2; exit 5 ;;
esac

remote_hash_output=$evidence_dir/windows-runner-hash.txt
if ! "$ssh_bin" -o BatchMode=yes -o ConnectTimeout="$ssh_connect_timeout" \
    "$windows_host" certutil.exe -hashfile "$windows_runner" SHA256 \
    >"$remote_hash_output" 2>"$evidence_dir/windows-runner-hash.stderr.txt"; then
    echo "Windows SSH or runner hash check failed" >&2
    exit 6
fi
remote_turnip_hash=$(
    tr -d ' \r' < "$remote_hash_output" |
        grep -Eo '[[:xdigit:]]{64}' | head -n 1 | tr 'A-F' 'a-f' || true
)
if [ "$remote_turnip_hash" != "$expected_turnip_hash" ]; then
    echo "Windows Turnip runner SHA-256 mismatch: $remote_turnip_hash" >&2
    exit 6
fi

set +e
timeout --signal=TERM --kill-after=10s "${workload_timeout}s" \
    "$ssh_bin" -o BatchMode=yes -o ConnectTimeout="$ssh_connect_timeout" \
    "$windows_host" powershell.exe -NoProfile -NonInteractive \
    -ExecutionPolicy Bypass -File "$windows_runner" \
    -OutputDirectory "$windows_output" \
    >"$evidence_dir/windows-workload.stdout.txt" \
    2>"$evidence_dir/windows-workload.stderr.txt"
workload_status=$?
set -e
printf '%s\n' "$workload_status" > "$evidence_dir/windows-workload.exit.txt"

capture=$evidence_dir/host-capture.txt
set +e
"$adb_bin" -s "$adb_serial" shell "$remote_capture" --capture \
    "$baseline_line" "$baseline_inode" \
    >"$capture" 2>"$evidence_dir/host-capture.stderr.txt"
capture_status=$?
set -e
printf '%s\n' "$capture_status" > "$evidence_dir/host-capture.exit.txt"
if [ "$capture_status" -ne 0 ]; then
    echo "Host capture failed with exit $capture_status" >&2
    exit 7
fi

capture_complete=$(read_key "$capture" CAPTURE_COMPLETE)
created_contexts=$(read_key "$capture" CONTEXT_TABLE_ADD)
if [ "$capture_complete" != "1" ]; then
    echo "Host capture is not complete" >&2
    exit 7
fi
case "$created_contexts" in
    ''|*[!0-9]*)
        echo "Host created-context count is not a decimal integer" >&2
        exit 7
        ;;
esac
if [ "$created_contexts" -eq 0 ]; then
    echo "Host capture contains no created contexts" >&2
    exit 7
fi

set +e
python3 "$classifier" "$capture" --expected-created "$created_contexts" \
    >"$evidence_dir/host-classification.json" \
    2>"$evidence_dir/host-classification.stderr.txt"
classifier_status=$?
set -e
printf '%s\n' "$classifier_status" > "$evidence_dir/host-classification.exit.txt"

if [ "$workload_status" -ne 0 ]; then
    echo "Windows Turnip workload failed with exit $workload_status" >&2
    exit 8
fi
if [ "$classifier_status" -ne 0 ]; then
    echo "Host teardown classification failed with exit $classifier_status" >&2
    exit 9
fi

echo "WINDOWS_OUTPUT_DIRECTORY=$windows_output"
echo "CREATED_CONTEXTS=$created_contexts"
echo "HOST_TEARDOWN_COMPLETE=1"
