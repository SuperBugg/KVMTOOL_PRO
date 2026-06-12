# virtio-npu RKNN Runtime Pack

这个目录是从 `code/yolov5` 中抽出来的最小 RKNN Runtime 资源包，供后续 kvmtool userspace backend 使用。

## 内容

- `include/rknn_api.h`：RKNN Runtime 用户态 API 头文件。
- `models/yolov5n.rknn`：较小的 YOLOv5 RKNN 模型，优先用于虚拟化链路测试。
- `models/yolov5s.rknn`：较大的 YOLOv5 RKNN 模型，后续用于更完整的检测效果。
- `src/rknn_backend.h`
- `src/rknn_backend.cpp`

## 当前接口定位

当前 `src/rknn_backend.*` 是一个轻量适配层，不依赖 OpenCV：

```text
kvmtool virtio-npu backend
  -> virtio_npu_rknn_backend_infer_raw()
      -> rknn_inputs_set()
      -> rknn_run()
      -> rknn_outputs_get()
  -> 把 raw output 返回给 guest
```

它暂时假设输入已经是模型需要的格式，例如 `640x640 RGB/NHWC/uint8`。这样可以先打通真实 RKNPU 执行链路，避免一开始就把图片解码、resize、YOLO 后处理和 kvmtool 混在一起。

## 后续接入方向

1. 在 kvmtool 中新增 RKNN backend 初始化参数，例如 `--virtio-npu-model /path/yolov5n.rknn`。
2. kvmtool 启动时创建 `virtio_npu_rknn_backend`。
3. `VIRTIO_NPU_CMD_INFER_*` 收到 guest 输入后调用 RKNN backend。
4. 后处理可以有三种路线：
   - guest 做后处理：host 只返回 raw output，虚拟化边界清晰。
   - host helper 做后处理：kvmtool 与独立进程通信，kvmtool 保持简单。
   - kvmtool 内部做后处理：端到端最短，但会引入 OpenCV/C++ 复杂度。

当前建议先走第 1 或第 2 种。
