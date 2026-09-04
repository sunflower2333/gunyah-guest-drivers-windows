#!/system/bin/sh
set -eu

app_root=/data/data/cn.classfun.droidvm
console=$app_root/cache/console_458ffb2c-a1e9-4830-bec5-4e1fe323b493_stderr.log
expected_hash=8bffa853a6601e68ba795d039474bf2d1fc56683e84ed122c4dd503c91f2dec4

pid=$(pidof crosvm 2>/dev/null || true)
printf 'crosvm_pid=%s\n' "$pid"
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1
test "$(sha256sum "$app_root/usr/bin/crosvm" | awk '{print $1}')" = "$expected_hash"
tr '\000' ' ' <"/proc/$pid/cmdline"
printf '\n'

printf '\n===== high-MMIO evidence =====\n'
grep -E 'GUNYAH-HIGHMMIO|GUNYAH-ADD: slot=5|declaring pre-start shared memory slot=5|GH-SHIM: accepted|GUNYAH-SHARE-BLOB|exiting with error|external abort|stage.?2' \
    "$console" | tail -n 120
