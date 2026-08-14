# MiniTun SDK 示例

- `local_control.cpp`：本地控制 SDK（`libminitun-client`）——连接本机 daemon、
  读取状态与稳定 `client_id`；
- `remote_codec.cpp`：Remote Protocol SDK（`libminitun-remote-protocol`）——
  编码、增量解码并回读一条 REGISTER 帧。

构建（需要已安装的 SDK 包）：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/usr
cmake --build build
```
