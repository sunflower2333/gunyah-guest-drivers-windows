# XInput / vioinput：Windows 驱动实现记录

日期：2026-09-15。开发分支：`work/vioinput-xinput-haptics-20260915`。只在分支提交，不创建 PR。

## 1. 已接入实际驱动

`HidGamepad.c` / `GamepadCore.h`：按 Microsoft XInputHID 文档提供标准输入 ID 1（17B）与输出 ID 2（9B）描述符；完整摇杆、扳机、按钮、方向帽与 Share 映射。私有能力协商通过后才选择独立 Gamepad PDO；旧宿主仍走原键鼠/通用 HID 路径，不伪造 Microsoft VID。

`Hid.c`：实际 WRITE_REPORT、SET_OUTPUT_REPORT、GET_INPUT_REPORT 路由；严格检查报告 ID、长度、指针、幅值与保留位。振动输出不再只存在于离线头文件。

`Haptics.c`：固定 16 个发送槽（2 个控制保留槽）、独立 1536B DMA wire 区、每帧 96B 单描述符 StatusQ 发布、READY 握手、单一 guest 序号域、主机撤销、有限组帧期限、精确 revision 续租、1s 请求完成超时及取消处理。报告中的四通道幅值、时长、延迟、重复次数字段完整保存，租约不能替代效果时长。

`Device.c` / `IsrDpc.c`：生命周期接入、DPC 有限批次、重新启用通知后检查竞争、输出锁外完成、D0Exit 对 EventQ 和 StatusQ 同时回收。WDF 请求取消/超时不释放宿主仍拥有的 DMA。上电清空持键与振动；同一旧 epoch 不允许自动恢复。

`vioinput.inx`：独立 `VIOINPUT\XINPUTHID_01` 安装项，保留原 `VIOINPUT\REV_01`。只为新 PDO 设置 xinputhid upper filter 与 device-scoped GenericHidDevice 属性，不修改全局服务属性，不随包分发系统 xinputhid.sys。

## 2. 不能据此宣称验收完成

Windows ARM64 编译/链接、INF 打包与逻辑回归必须分别看当前提交的 CI，旧提交成功不能替代本批验证。实际 Windows VM 的 XInputHID 绑定、系统 XInputGetState/XInputSetState、Driver Verifier、电源循环与手机马达尚未验收。

实际审核的 ARM64 Windows 10.0.26100.8972 inbox INF 中，文档所提通用安装节被注释。现安装项采用限定新 PDO 的显式配置作为实验性适配，仍需在目标镜像验证，不能把安装语法通过当作运行绑定通过。

crosvm/Android Broker 本轮不修改；宿主必须实现 HAPTICS-ABI.md 后才能打开能力。旧宿主不震动是兼容性门控的预期结果。当前 REVOKED 会话需通过设备 reset / 新 epoch 恢复，不支持在同一 D0 内任意 HOST_READY 重新授权。

## 3. 测试

```sh
python3 vioinput/tests/haptics/run.py --cc clang --sanitize --negative-control
python3 vioinput/tests/haptics/run.py --cc gcc --sanitize --negative-control
```

本地已通过：原 7 组核心测试及 768 次单 bit 破坏、独立 HID 描述符解析、全部控制映射/边界、真实 Haptics.c 的显式 WDF mock 回归、真实 IsrDpc.c 回归和锁内完成负向对照。mock 验证取消/used 交错、队列满、STOP 资源保留、超时、有限效果结束及旧 epoch 拒绝，不代替真实 KMDF 并发和硬件 DMA 证明。

系统探针仍位于 `tools/xinput_probe.c`。无参数只枚举，显式指定 slot/左右强度/时长才发振动；必须关联本设备日志排除外接手柄。ARM64、x64、x86 探针是用户态构建，不是内核驱动构建。

## 4. 部署与回滚

构建产物为开发用未签名驱动，不能当作生产签名包安装。测试前保存原输入驱动与 VM 快照，确保不依赖待替换驱动的恢复入口。不要向现有键盘 PDO 强制绑定 Gamepad 安装节。回滚先关闭宿主 haptics 能力并重建虚拟设备，再恢复原签名驱动。

## 5. 依据

- Microsoft XInputHID Driver Documentation：<https://aka.ms/gipdocs>
- XInputSetState：<https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputsetstate>
- HID 架构：<https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture>
- WdfRequestUnmarkCancelable：<https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestunmarkcancelable>
- EvtIoStop：<https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfio/nc-wdfio-evt_wdf_io_queue_io_stop>
- Virtio 1.3 Input：<https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html>
