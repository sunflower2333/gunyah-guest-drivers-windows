# DroidVM XInputHID 私有传输 ABI v1

这是开发分支约定，不是标准 EV_FF、标准 Virtio feature 或已分配的扩展。宿主不得向普通 evdev 设备转发私有事件。编号仅在三端同时明确选择本 profile 后使用；标准化或冲突处理必须升版本，不能静默复用。

## 1. 能力与身份

virtio-input config select=0x80 / subsel=1 / size=32。全部小端：offset0 magic u32=0x31485644（DVH1），offset4 version u16=1，offset6 bytes u16=32，offset8 flags u32=7，offset12 event_type u16=0xff80，offset14 profile u16=1，offset16 epoch u64，offset24 lease_ms u32，offset28 reserved u32=0。

flags bit0=完整 XInputHID 输出，bit1=宿主强制租约与 reset 停振，bit2=READY/STATUS/REVOKE 控制。三项缺一不可；未知位拒绝。epoch 非零，每次设备 reset 后必须更新，不能重用；lease_ms=100..5000。当前驱动每次读两份一致记录才接受。设备存活期间 profile/报告格式不允许切换，切换需重新枚举 PDO。

宿主建立专用 gamepad virtio-input：BTN_GAMEPAD、ABS_X/Y/RX/RY/Z/RZ 及有效有符号 min/max；Z/RZ 分别作为左右扳机。方向帽使用 ABS_HAT0X/Y 或 BTN_DPAD_UP/DOWN/LEFT/RIGHT。宿主输入范围的 Y 轴正向按标准 HID 向下约定。不要同时发送相互冲突的方向帽两种表示。

## 2. 两条原队列

eventq 宿主→客体，普通输入仍为单个 8B 事件；私有控制帧也按 12 个单事件缓冲依次送达。statusq 客体→宿主，每个私有帧为一个 96B 只读描述符，必须按完整帧校验，不得只读第一个事件。没有第三条队列。

每个记录：u16 type、u16 code、u32 value。code 从0到11依次：version低16/opcode高16、epoch低/高32、sequence低/高32、revision低/高32、motors、lease_ms、detail、reserved=0、CRC32C。CRC覆盖前88B，Castagnoli reflected=0x82f63b78，init/xorout=0xffffffff。CRC不提供认证；请求必须绑定可信 VM/控制器连接。

## 3. 序号与握手

guest 与 host 各自单调64位序号，0无效，不允许回绕；guest握手和振动共享一个域。D0读取新epoch，发送GUEST_READY(op2,seq1)，收到同epoch HOST_READY(op3)后才允许输出。HOST_READY必须在2s期限内。HOST_HELLO(op1)不能代替配置身份授权。

每个实际输出增加revision；KEEPALIVE只增加sequence。过期epoch、旧sequence、未知方向opcode、非法长度/CRC/保留位不得改变状态。组帧期限从BEGIN起100ms，不被零散后续记录延长。宿主必须独立限制跨socket字节缓冲，不能无限累计半帧。

## 4. 振动内容

实际驱动发送 SET_XINPUT_REPORT(op10)，motors 为4个8位百分比幅值，字节0..3依次为左扳机、右扳机、左主体、右主体，范围0..100。detail 字节0=actuator mask（bit3..0对应上述顺序），字节1=duration（10ms单位），字节2=delay（10ms单位），字节3=repeat count。源HID报告完整9B：ID2、mask、4幅值、duration、delay、repeat。

有效幅值/使能/时长为空映射为STOP(op6)，清零motors/lease/detail。有效输出租约取能力中lease_ms，不是效果时长。KEEPALIVE_XINPUT_REPORT(op11)携带原始同revision内容，只续租，禁止从头播放或延长有限效果。宿主对重复次数、间隔及完成时机的实现必须遵守XInputHID报告约定并单独测试。

旧草案SET_RUMBLE(op4)/KEEPALIVE(op5)保留在协议核心测试中，当前profile驱动不使用它们；不能把op10的4字节幅值误解释成两个16位马达。

## 5. 停振、故障与宿主责任

HOST_STATUS(op7)同revision detail=1代表有限效果结束，guest转IDLE停止续租；detail=2..8为故障。HOST_REVOKE(op8)撤销当前会话，guest不再接受非零输出。CLOSE(op9)是驱动取消/超时/关闭时的会话级停止，不得被后续旧KEEPALIVE恢复。

当前guest 16个槽中14个普通、2个控制保留。StatusQ至少16描述符。取消/1s完成超时只结束WDF请求，不代表DMA归还。只有used或已确认reset屏障后才能复用wire。used代表传输缓冲归还，不代表Android实际播放成功。

guest暂停、崩溃、应用退出或连接断开时不一定能送出STOP；宿主必须在租约过期、VM reset/销毁、输入连接断开、Broker失联、用户禁用/切后台时独立停止。有限效果到期后也须独立停止，即使guest仍发同revision续租。恢复连接不能重放旧效果。guest心跳不是游戏进程存活证明。

REVOKED恢复目前要求重新reset虚拟设备、重新读取新epoch并握手；同D0内自动重授权未实现。宿主应返回明确状态，不可仅靠忽略消息实现静默失败。
