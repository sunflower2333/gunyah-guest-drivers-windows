#!/system/bin/sh
set -eu

printf 'crosvm_pid=%s\n' "$(pidof crosvm 2>/dev/null || true)"

printf '\n===== kernel tail, filtered =====\n'
dmesg | tail -n 5000 |
    grep -Ei 'crosvm|gunyah|gh_rm|gh_vm|memparcel|stage.?2|external abort|user.mem|ipa|ioctl' |
    grep -Ev '^\[[^]]+\] Modules linked in:' |
    tail -n 800 || true

printf '\n===== Android log, crosvm launch window =====\n'
logcat -d -v threadtime -T '08-25 08:06:45.000' 2>/dev/null |
    grep -Ei 'crosvm|droidvm|gunyah|458ffb2c|high.mmio|vm_start|vm stopped|exit' |
    tail -n 1000 || true

printf '\n===== recent VM files =====\n'
find /data/data/cn.classfun.droidvm/cache /data/local/tmp -maxdepth 1 -type f \
    -newermt '2026-08-25 08:06:45' -printf '%TY-%Tm-%Td %TH:%TM:%TS %s %p\n' \
    2>/dev/null | sort || true
