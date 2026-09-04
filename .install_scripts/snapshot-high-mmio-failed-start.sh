#!/system/bin/sh
set -eu

stage=/data/data/com.termux/files/home/high-mmio-guard-8bffa853/failed-start-evidence
app_root=/data/data/cn.classfun.droidvm

pid=$(pidof crosvm 2>/dev/null || true)
test -n "$pid"
test "$(echo "$pid" | wc -w)" -eq 1

rm -rf "$stage"
mkdir -p "$stage"
dmesg >"$stage/dmesg.txt"
logcat -d -v threadtime -T '08-25 08:06:45.000' >"$stage/logcat.txt" 2>&1 || true

for file in \
    "$app_root/cache/console_458ffb2c-a1e9-4830-bec5-4e1fe323b493_stderr.log" \
    "$app_root/cache/console_458ffb2c-a1e9-4830-bec5-4e1fe323b493_stdio.log" \
    "$app_root/cache/console_458ffb2c-a1e9-4830-bec5-4e1fe323b493_uart.log" \
    /data/local/tmp/crosvm-native-58070-high-mmio-guard-8bffa853.log; do
    if [ -f "$file" ]; then
        cp -p "$file" "$stage/"
    fi
done

chown -R 10316:10316 "$stage"
find "$stage" -maxdepth 1 -type f -printf '%s %p\n' | sort
