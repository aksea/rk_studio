# rk_studio sample

RK3588 上的多路摄像头预览、录制和顺序拍照样例。核心链路包括 Qt 预览、GStreamer 采集、视频录制、四路顺序拍照、可选音频录制和会话元数据。

## 快速部署

### 1. 开启摄像头 overlay

在板子的 `/boot/uEnv/uEnv.txt` 中启用 IMX415 overlay，4 路运行使用 `cam0` 到 `cam3`：

```text
dtoverlay=/dtb/overlay/rk3588-lubancat-5-cam0-imx415-1920x1080-60fps-overlay.dtbo
dtoverlay=/dtb/overlay/rk3588-lubancat-5-cam1-imx415-1920x1080-60fps-overlay.dtbo
dtoverlay=/dtb/overlay/rk3588-lubancat-5-cam2-imx415-1920x1080-60fps-overlay.dtbo
dtoverlay=/dtb/overlay/rk3588-lubancat-5-cam3-imx415-1920x1080-60fps-overlay.dtbo
```

重启后检查：

```bash
dmesg | grep -i imx415
```

### 2. 同步项目

从 Mac 同步到板子时建议用 `tar`，避免带上 `.DS_Store` / `._*`：

```bash
cd /Users/aksea/Project/Linux/RK3588
tar --exclude='.git' --exclude='build' --exclude='records' \
    --exclude='.DS_Store' --exclude='._*' --exclude='__MACOSX' \
    -czf /tmp/rk_studio.tar.gz rk_studio

scp /tmp/rk_studio.tar.gz cat@<board-ip>:/tmp/
ssh cat@<board-ip> 'rm -rf /home/cat/rk_studio && cd /home/cat && tar -xzf /tmp/rk_studio.tar.gz'
```

### 3. 安装依赖

```bash
cd /home/cat/rk_studio
./scripts/install_board_deps.sh
```

### 4. 配置、构建、运行

```bash
cd /home/cat/rk_studio
cp config/board.example.toml config/board.toml
cp config/profile.example.toml config/profile.toml

cmake -S . -B build
cmake --build build -j$(nproc)

./build/rk_studio
```

默认 camera 节点：

```text
cam0 -> /dev/video55
cam1 -> /dev/video64
cam2 -> /dev/video73
cam3 -> /dev/video82
```

如果新板子 video 编号不同，只需要改 `config/board.toml` 里的 `[camera.<id>].record_device`。

## 新板子部署

这个 sample 分支只保留预览和录制，部署比完整版本轻很多：不需要 RKNN、模型文件、Zenoh 或 RTSP 依赖。新板子上主要确认三件事。

### 1. 确认摄像头 mainpath

程序当前只使用 `rkisp_mainpath`，不使用 `selfpath`。在板子上查看映射：

```bash
v4l2-ctl --list-devices
```

典型输出会像这样：

```text
rkisp_mainpath (platform:rkisp0-vir0):
        /dev/video44
        /dev/video45
        ...

rkisp_mainpath (platform:rkisp0-vir2):
        /dev/video53
        /dev/video54
        ...
```

每组第一个节点是 mainpath。比如当前测试板接的是 `cam0, cam1, cam2, cam4`，对应配置是：

```toml
[camera.cam0]
record_device = "/dev/video44"
preview_device = "/dev/video45"

[camera.cam1]
record_device = "/dev/video53"
preview_device = "/dev/video54"

[camera.cam2]
record_device = "/dev/video62"
preview_device = "/dev/video63"

[camera.cam4]
record_device = "/dev/video71"
preview_device = "/dev/video72"
```

如果接线口变了，优先按实际端口命名，避免 UI 和录制文件名错位。

### 2. 确认音频设备

查看可录音设备：

```bash
arecord -l
arecord -L
```

USB 麦克风建议使用 `plughw:CARD=<name>,DEV=0`，比 `hw:<card>,0` 更能兼容采样率和声道转换。例如：

```toml
[audio.usb0]
device = "plughw:CARD=Audio,DEV=0"
```

然后在 `config/profile.toml` 中启用：

```toml
audio_source = "usb0"
```

不录音时保持：

```toml
audio_source = ""
```

### 3. 做一次 smoke test

构建后先跑：

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

再测试音频链路：

```bash
gst-launch-1.0 -q alsasrc device=plughw:CARD=Audio,DEV=0 num-buffers=50 \
  ! audio/x-raw,format=S16LE,layout=interleaved,rate=16000,channels=2 \
  ! fakesink
```

后续可以加一个 `scripts/probe_board.sh`，自动输出当前 mainpath/selfpath 映射、ALSA capture 设备和推荐配置，这样新板子只需要跑脚本后复制推荐的 `board.toml`。

## 配置

### board.toml

```toml
[audio.mic0]
device = "hw:rockchipes8388,0"

[camera.cam0]
record_device = "/dev/video55"
preview_device = "/dev/video56"
preview_width = 640
preview_height = 360
fps = 30
```

摄像头字段：

- `record_device`：V4L2 节点。
- `preview_device`：低分辨率预览使用的 ISP selfpath 节点；未配置时兼容性回退到 `record_device`。
- `preview_width` / `preview_height`：预览分辨率。
- `record_width` / `record_height`：录制分辨率，未配置时默认 1920x1080。
- `fps`：采集帧率。
- `bitrate`：录制码率。

### profile.toml

```toml
[session]
preview_cameras = ["cam0", "cam1", "cam2", "cam3"]
record_cameras = []
output_dir = "./records"
prefix = "rk_studio"
audio_source = ""
```

`record_cameras` 留空时录制全部预览摄像头；如果只想录部分摄像头，可以填入摄像头 id。`audio_source` 留空表示不录音。

程序启动时会自动加载默认的 `config/board.toml` 和 `config/profile.toml`。修改配置后重启程序生效。

## 使用规则

- `启动预览`：显示配置里的多路预览画面。
- `启动录制`：从 Idle 或 Previewing 进入录制；已有 selfpath 预览会继续显示，mainpath 独立录制 1920x1080 视频。
- `停止录制`：关闭录制并写入会话元数据。
- `一键拍照`：按 `record_cameras` 的顺序逐路使用 mainpath 拍摄 1920x1080 PNG。拍摄期间请保持头盔和棋盘静止，selfpath 预览持续显示。

同一摄像头的 selfpath 专门用于 640x360 预览，mainpath 专门用于 1920x1080 录制和拍照，因此预览可以与其中任一操作同时工作。录制和拍照仍然互斥；拍照始终只打开一路全分辨率管线，拍完释放后才打开下一路，避免四路全分辨率 PNG 编码并发。

## 输出

录制会话输出到 `records/`：

```text
records/rk_studio-YYYYMMDD-HHMMSS/
├── cam0.mkv
├── cam1.mkv
├── mic0.wav
├── session.meta.json
├── session.sync.json
└── studio.events.jsonl
```

一次完整拍照输出到单独目录：

```text
records/rk_studio-photo-YYYYMMDD-HHMMSS/
├── cam0.png
├── cam1.png
├── cam2.png
├── cam4.png
└── capture.meta.json
```

调试时可不启动界面，直接执行一次同样的顺序拍照：

```bash
cd /home/cat/rk_studio
./build/rk_studio --capture-once
```

## 上板测试清单

1. `./build/rk_studio` 能启动并自动加载配置。
2. 只开预览：多路画面正常，帧率限制生效。
3. 只开录制：生成视频文件和 `session.meta.json`。
4. 先预览再录制：selfpath 预览不中断，mainpath 录制正常开始；停止录制后仍保持 Previewing。
5. 预览中点击一次 `一键拍照`：预览不中断，四张图片均为 1920x1080，文件夹中同时生成 `capture.meta.json`。

## 目录

```text
rk_studio/
├── config/                     # board/profile 配置模板
├── include/                    # 头文件
├── src/                        # 源码
├── scripts/
│   └── install_board_deps.sh   # 安装板端构建/运行依赖
├── third_party/
│   └── tomlplusplus/           # TOML 解析头文件
└── CMakeLists.txt
```
