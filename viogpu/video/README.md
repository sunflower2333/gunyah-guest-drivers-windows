# VioGPU Video：perf 分支上的首个实现增量

## 范围和状态

基线：`perf/droidvm-gpu-20260914`，提交 `5ab27d405588`。
工作分支：`work/vpu-video-perf-20260915`。

这是实际的 virtio-media 协议核心和 KMDF 传输实现，不是 DXVA/MFT 支持已经完成的声明。
便携 C 核心已执行 Linux UAPI 字节对照及 ASan/UBSan 测试。
Windows ARM64 编译以 `VioGPU Video bring-up` CI 的具体结果为准；实机编解码尚未验收。

代码归属 `viogpu/video`；`viogpuvideo.sys` 是 VioGPU 包里的媒体伴随功能，绑定
`PCI\VEN_1AF4&DEV_1070`（virtio device 48），不是把媒体命令塞进 virtio-gpu 的 controlq。
显示 miniport、OpenGL/Turnip 路径、原有安装默认值均不修改。

## 不引入 rdmapool

视频输入和输出使用驱动运行时分配的普通、连续、cacheable guest RAM，通过 USERPTR + SG 发送。
描述符内存复用现有 VirtIO/WDF。没有 rdmapool.sys 依赖，不申请 guest 共享媒体池，不映射 host BAR。
用户态只提交 queue/index 和数据，不能提交 GPA、内核地址或任意 SG。

**仅适用于宿主能访问普通 guest RAM 的 unprotected 配置。** 当前拒绝启用 IOMMU 的设备，
不把 DMA logical address 当作 GPA；不要用于 host 无权访问系统 RAM 的 protected guest。
宿主端现有 MediaCodec helper 仍有 `media_host` 的启动要求；这不等于 Windows 引入 rdmapool。

v1 每设备最多 256 MiB，每队列最多 64 个 buffer，每 buffer 最多 32 MiB。
只接受一个有效 plane（单平面或 mplane API 的 one-plane NV12/码流）；不支持 NV12M 等分离平面。
分配失败明确报错，不回退到不可达内存，也不静默修改 codec 能力。

## 已实现

1. 固定 little-endian、Linux arch64 布局；208B format、88B buffer、64B plane；命令号发送 `_IOC_NR`。
2. commandq/eventq、OPEN/CLOSE、格式和固定布局控制查询、编码/解码命令、STREAMON/STREAMOFF。
3. 驱动管理 REQBUFS，使用宿主实际返回 count 和 G_FMT 的 sizeimage；QBUF 序列化由内核生成。
4. 区分 QBUF ACK 和 DQBUF；收到合法 DQBUF 后才能复制/复用 buffer；空 LAST 不被伪装成帧。
5. eventq 立即回补；用户态只读取消毒后的完成事件，不发送 VIDIOC_DQBUF/DQEVENT。
6. 单设备独占会话、管理员 ACL、固定接口版本、长度/索引/范围校验。
7. 命令超时、事件溢出、畸形响应会令设备 fail-closed，保留在途 backing 至 CLOSE 或 reset 屏障。
8. ARM64 WDK 项目、实验 INF、实际设备探测程序和可复用用户态客户端接口。

## 构建和检查

便携测试不需要 Windows 或手机：

```sh
python3 viogpu/video/tests/run.py
CC=gcc CXX=g++ python3 viogpu/video/tests/run.py
```

ARM64 WDK 开发环境中，按顺序构建依赖和目标（SDK/WDK 版本使用仓库已有 kit locator）：

```powershell
$common = @('/m', '/p:Configuration=Win11 Release', '/p:Platform=ARM64',
            '/p:SignMode=Off', '/p:SpectreMitigation=false', '/p:ApiValidator_Enable=false')
msbuild VirtIO/VirtioLib.vcxproj @common
msbuild VirtIO/WDF/VirtioLib-WDF.vcxproj @common
msbuild viogpu/video/kmd/viogpuvideo.vcxproj @common
cl /nologo /std:c++17 /EHsc /W4 /WX viogpu/video/tools/video_probe.cpp /Fe:video_probe.exe
```

CI 只编译和运行不触碰硬件的测试，不安装、不加载驱动，不更改签名或系统安全设置。
输出是 **unsigned bring-up** 构建，不应直接替换已验收的驱动包。

在隔离测试 VM 中完成签名和实验驱动安装后，提升权限运行：

```text
video_probe.exe
video_probe.exe 0
```

第一条枚举接口，第二条打开第 0 个媒体功能并打印实际格式。
没有设备时失败；不会伪造硬件支持或回退到软件解码后再报告成功。

## 上层调用顺序

`tools/video_client.h` 暴露实际 DeviceIoControl 调用，不提供虚构的 codec 实现。
编码会话通过 CAPTURE=coded format、OUTPUT=NV12 配置；解码会话反向配置。
订阅 SOURCE_CHANGE/EOS 后分配、回补 CAPTURE，并提交 OUTPUT；读取完成事件后再复用对应 index。
`Control(VV_IOCTL_ENCODER_CMD, ...)` 和 `Control(VV_IOCTL_DECODER_CMD, ...)` 使用相应 40B/72B payload。
DRAIN 不是 STREAMOFF：必须继续取 CAPTURE 到 LAST，并等所有在途输入归还。

当前 `VV_OP_EVENT` 是非阻塞本地事件查询；返回 Linux EAGAIN=11 说明暂时没有事件，**不是 EOS**。
`Read`/`Write` 通过 bounded copy 移动数据；没有零拷贝声明。

## 尚未完成的合入门槛

- 原始 H.264/NV12 编解码实机测试、帧数/PTS/尾帧验证和 codec 确实为硬件的证据。
- 更完整的 mock-transport 生命周期测试，以及真实 ARM64 Driver Verifier/PnP/中断并发检查。
- 分辨率变化后的 CAPTURE 重新分配；当前 v1 分配释放使用 CLOSE/reopen，不能用于无损 DRC。
- 多会话、可取消异步用户请求；当前控制请求序列化且有 15 秒上界，事件在内核异步收取。
- 不依赖大块连续 RAM 的分散页和 host page-size 对齐方案。
- Media Foundation 异步 MFT、D3D11/DXVA 视频 DDI、纹理/同步互操作及编码器系统注册。
- 格式/profile/位深/色彩信息的端到端筛选；不发布 P010/HDR/AV1 或系统 DXVA 能力。

因此不能把“构建通过”“设备探测通过”写成“Windows 播放器已经硬解”。
先在测试 VM 验收原始队列路径，再扩展 MFT/视频 DDI；该实验组件不进入默认安装流程。

## 对齐的上游源码

- `Droid-VM/virtio-media@6b6d2b3307ce75ed35b0ab5b4703d9d1bb830cf8`：`device/src/protocol.rs`、`device/src/ioctl.rs`。
- `Droid-VM/v4l2r@7eb3afa6c4ff7d795394ad09dece6099b4e6a38e`：`lib/src/ioctl.rs` 的 `repr(C) V4l2Buffer`，含固定八个 plane。
- `Droid-VM/crosvm@bfccd3d5a7abc7a8d2c0fd1b2ab5bee119321b75`：`devices/src/virtio/media` 下的 MediaCodec 后端和 guest-buffer 导入。
