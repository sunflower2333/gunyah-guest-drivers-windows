# XInput / vioinput haptics：第一批实现

日期：2026-09-15。基线：`perf/droidvm-gpu-20260914` / `5ab27d4055887f14e97195079ef51716b7b707fb`。

## 1. 本批已经修改什么

`sys/IsrDpc.c` 是真实驱动代码：StatusQ used buffer 归还后，先在锁内摘除请求、回收 DMA slice，再在锁外完成 WDF 请求。上层完成回调可以重新提交输出，不再在持有 StatusQLock 时重入。空队列和未建立队列不解引用空指针。

EventQ 必须返回恰好 8 字节的事件，才能交给原输入解码。0、7、9 等无效长度不会重放旧 DMA 内容；重新入队失败时回收未交给宿主的 slice。错误按一次 DPC 汇总、分行输出；ISR 不打印逐次进入／退出日志。不修改中断模式、原有 HID 描述符或输入映射。

`sys/HapticsCore.h` 已实现可执行的协议／状态机基础，但**尚未接到驱动的 HID 输出、配置协商、EventQ 或 StatusQ 发送路径**。包括固定 12 条 8 字节记录的编解码、小端字段、CRC32C、有限空间组帧和期限、STOP、KEEPALIVE、epoch/revision/sequence 检查，以及 prepare → enqueue → commit 的状态提交契约。

`tools/xinput_probe.c` 使用系统 `XInputGetState` / `XInputSetState`。无参数仅枚举；显式指定 slot、左右强度和时长才发送输出，随后发送零值 STOP。控制台退出尝试 STOP；强杀进程不保证回调执行，不能用探针替代宿主租约保护。

## 2. 不应误读为已完成功能

没有完成 XInputHID 设备匹配／报告格式核验，没有新增已启用的 XInput Gamepad profile，没有把 Generic HID 当作 XInput。现有 `HidJoystick.c` 的 BTN_GAMEPAD 缺口仍需下一批修改。

没有实现实际 96 字节 StatusQ DMA slot、取消／超时上下文、STOP 资源预留、续租定时器、完整 D0Exit/reset 收尾，也没有修改 crosvm 或 Android Broker。特别是原 D0Exit 对 StatusQ outstanding buffer 的清理仍必须单独处理；本批 DPC 正常完成修复不等于已解决断电取消。

核心头文件目前只有离线测试使用；它不会自动向旧宿主发送任何私有事件，也不会增加运行时振动功能。测试通过不等于 Windows 游戏或手机马达已经验收。

## 3. 协议草案契约

沿用 PLAN.md v2 的 96 字节草案：BEGIN(version/opcode)、epoch lo/hi、sequence lo/hi、revision lo/hi、motors、lease_ms、detail、reserved、CRC32C。字段 code 为 0..11；低频／左马达在 motors 低 16 位，高频／右马达在高 16 位。

CRC32C 使用 reflected Castagnoli polynomial `0x82f63b78`，init/xorout `0xffffffff`；`123456789` 的校验值为 `0xe3069283`。CRC 不是身份认证。

不分配 feature bit、config selector 或私有 event type。编解码器接受调用者明确传入的已协商 type；测试中的 `0xff80` 仅为测试值，不得复制为未经核查的生产编号。当前字段／opcode 是开发草案，尚未宣布跨仓 ABI 冻结。

收帧期限从 BEGIN 开始，不被后续半帧延长；实际 transport 还要处理跨字节 read、可信连接绑定、主机消息序号和方向检查。`DvhGuestBegin` 必须由可信且已协商的生命周期适配器调用；不得把任意 HOST_HELLO 解码成功视为授权。CRC 相符也不能证明 epoch 来自当前宿主。

Guest prepare 不修改状态；只有成功入队后才能 commit，prepare/enqueue/commit 必须在同一外部锁保护下序列化。队列满时不 commit。KEEPALIVE 不创建 revision、不改变马达值，不能在 STOP／REVOKE 后重新起振。若一个握手消息也使用相同发送序号域，接入层必须统一分配序号；不能让握手和本核心各自从 1 开始发送。

本批不将 Windows 内核指针或 WDFREQUEST 放入新协议。未来 DMA 数据与客体私有 TX context 必须分离，成功发布后直到 used completion 或设备已 quiesce/reset 才允许回收；WDF 请求取消不代表 DMA 所有权已归还。

## 4. 可重复测试

```sh
python3 vioinput/tests/haptics/run.py --cc clang --sanitize --negative-control
python3 vioinput/tests/haptics/run.py --cc gcc --sanitize --negative-control
```

7 组核心测试，包括 768 次单 bit 破坏、全部截断长度、额外字节、重算 CRC 后的非法保留位、版本、非法租约、乱序组帧、半帧超时、序号溢出、背压不提交、STOP 后旧续租、撤销和换 epoch。

ISR/DPC 测试编译实际 `sys/IsrDpc.c`；只替换 Windows/WDF/VirtIO API 为显式 mock，验证 shared IRQ、MSI、通知开关、空／未建立队列、无效长度、输入重投失败、无请求输出缓冲，以及会重入输出路径的请求完成。负向对照重新引入“锁内完成”，必须由断言拒绝。这些是逻辑测试，不是 WDK ABI、KMDF 并发或硬件中断证明。

在 Visual Studio Native Tools 环境构建探针；ARM64 必须使用 ARM64 cross tools：

```bat
cl /nologo /W4 /WX /TC /D_WIN32_WINNT=0x0602 vioinput\tools\xinput_probe.c /Fe:xinput_probe.exe /link Xinput.lib
xinput_probe.exe
xinput_probe.exe 1 20000 0 250
xinput_probe.exe 1 0 20000 250
xinput_probe.exe 1 0 0 0
```

`1` 是示例 slot，执行前按枚举结果选择目标；不能假定 DroidVM 控制器总是 0 或 1。API 成功还需与真实 vioinput 输出日志关联，才能通过 G1；外接手柄能震不代表 DroidVM 已闭环。

## 5. 后续必须按顺序接入的工作

1. G1：在 Windows ARM64 锁定 XInputHID 匹配和实际输入／输出／Feature 报告；增加同一控制器的完整输入 profile，并确认子 PDO / viohidkmdf 转发，不猜 VID/PID 或字节偏移。
2. 冻结三端私有能力与 ABI，统一握手和数据序号域；接入当前协议核心的收发／授权适配器。
3. 分离 DMA wire buffer 和私有请求 context，接入实际 StatusQ 批量发布、控制帧预算、取消、超时、D0Exit/reset、停振和通知竞争回归。
4. crosvm 与 Android 端接入、有限租约执行、前后台撤销和真机测试。

## 6. 验收记录

本地 Clang ASan/UBSan、GCC 测试与锁重入负向对照的具体结果随本次提交记录。GitHub 工作流另行产生可追溯结果；没有成功日志时不能声称 CI、ARM64 驱动构建或硬件验收完成。

## 7. 接口依据

- Microsoft XInputSetState: <https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputsetstate>
- Microsoft XINPUT_VIBRATION: <https://learn.microsoft.com/en-us/windows/win32/api/xinput/ns-xinput-xinput_vibration>
- Microsoft WdfRequestComplete: <https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestcomplete>
- Virtio 1.3, Input Device: <https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html>
