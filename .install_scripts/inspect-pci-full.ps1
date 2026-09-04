$ErrorActionPreference = 'Stop'
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\arm64\cdb.exe'
$adapter = 'ffff930946c36050'
$base = 'C:\Users\Administrator\viogpu-58071-signed\drivers\viogpu'
$commands = ".reload /f viogpuwddm.sys; dt viogpuwddm!CPciResources $adapter+0x258; dt viogpuwddm!CPciBar $adapter+0x298; dt viogpuwddm!CPciBar $adapter+0x2b8; dt viogpuwddm!CPciBar $adapter+0x2d8; dt viogpuwddm!CPciBar $adapter+0x2f8; dt viogpuwddm!CPciBar $adapter+0x318; dt viogpuwddm!CPciBar $adapter+0x338; q"
& $debugger -z C:\Windows\MEMORY.DMP -y $base -i $base -c $commands 2>&1
