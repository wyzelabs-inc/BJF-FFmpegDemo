# HEVC到H.264转码器


## 流程总结
转码流程由`HEVCToH264Transcoder`类实现，包含以下步骤：

1. **打开输入文件**：读取输入的MP4文件并初始化输入格式上下文。
2. **分析输入流**：提取视频流信息（例如分辨率、帧率、编码格式）。
3. **初始化HEVC解码器**：配置HEVC解码器以处理输入视频流。
4. **创建输出文件**：初始化输出MP4容器。
5. **初始化H.264编码器**：设置H.264编码器参数，如分辨率、帧率和比特率。
6. **准备数据结构**：分配用于解码和编码的数据包和帧。
7. **初始化图像转换器**：如果解码器和编码器的像素格式不同，配置像素格式转换（例如转换为H.264的YUV420P）。
8. **打开输出文件**：准备输出文件并写入头部信息。
9. **转码主循环**：
   - 读取输入数据包。
   - 解码HEVC帧。
   - 进行像素格式转换（如果需要）。
   - 将帧编码为H.264。
   - 将编码后的数据包写入输出文件。
10. **刷新编解码器**：处理解码器和编码器缓冲区中剩余的帧。
11. **清理资源**：释放资源并关闭文件。

## 使用方法
### 编译
```bash
g++ -o hevc_to_h264_transcode_oo hevc_to_h264_transcode_oo.cpp -lavformat -lavcodec -lavutil -lswscale
```

### 运行
```bash
./hevc_to_h264_transcode_oo input_hevc.mp4 output_h264.mp4 [比特率]
```
- `input_hevc.mp4`：输入的HEVC编码MP4文件路径。
- `output_h264.mp4`：输出的H.264编码MP4文件路径。
- `比特率`（可选）：目标比特率，单位为比特每秒（默认：1Mbps）。

### 示例
```bash
./hevc_to_h264_transcode_oo input_hevc.mp4 output_h264.mp4
./hevc_to_h264_transcode_oo input_hevc.mp4 output_h264.mp4 5000000
```
