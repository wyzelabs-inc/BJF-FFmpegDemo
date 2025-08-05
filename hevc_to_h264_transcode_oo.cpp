/**
 * HEVC(h265)到H.264 转码
 * 
 * 功能：将HEVC编码的MP4文件转码为H.264编码的MP4文件
 * 
 * 
 * 编译命令：
 * g++ -o hevc_to_h264_transcode_oo hevc_to_h264_transcode_oo.cpp -lavformat -lavcodec -lavutil -lswscale
 * 
 * 运行命令：
 * ./hevc_to_h264_transcode_oo input_hevc.mp4 output_h264.mp4 [比特率]
 */

extern "C" {
#include <libavformat/avformat.h>  // 容器格式处理
#include <libavcodec/avcodec.h>    // 编解码器
#include <libswscale/swscale.h>    // 图像格式转换
#include <libavutil/imgutils.h>    // 图像工具函数
#include <libavutil/opt.h>         // 选项设置
}

#include <iostream>
#include <string>

class HEVCToH264Transcoder {
private:
    // FFmpeg核心组件
    AVFormatContext* input_format_ctx;   // 输入容器格式上下文
    AVFormatContext* output_format_ctx;  // 输出容器格式上下文
    AVCodecContext* decoder_ctx;         // HEVC解码器上下文
    AVCodecContext* encoder_ctx;         // H.264编码器上下文
    const AVCodec* decoder;              // HEVC解码器
    const AVCodec* encoder;              // H.264编码器
    AVStream* output_stream;             // 输出视频流
    SwsContext* sws_ctx;                 // 图像转换上下文
    
    // 数据结构
    AVPacket* input_packet;              // 输入数据包
    AVPacket* output_packet;             // 输出数据包
    AVFrame* decoded_frame;              // 解码后的帧
    AVFrame* encoded_frame;              // 编码前的帧
    
    // 视频信息
    int video_stream_index;              // 视频流索引
    int width, height;                   // 视频尺寸
    AVRational frame_rate;               // 帧率
    int64_t bit_rate;                    // 比特率  -->决定视频质量
    int frame_count;                     // 帧计数器
    
    // 文件路径
    std::string input_filename;
    std::string output_filename;

public:
    HEVCToH264Transcoder() {
        // 初始化所有指针为nullptr
        input_format_ctx = nullptr;
        output_format_ctx = nullptr;
        decoder_ctx = nullptr;
        encoder_ctx = nullptr;
        decoder = nullptr;
        encoder = nullptr;
        output_stream = nullptr;
        sws_ctx = nullptr;
        input_packet = nullptr;
        output_packet = nullptr;
        decoded_frame = nullptr;
        encoded_frame = nullptr;
        video_stream_index = -1;
        frame_count = 0;
        bit_rate = 1000000;  //  
    }
    
    ~HEVCToH264Transcoder() {
        cleanup();
    }
    
    /**
     * 初始化转码器
     */
    bool initialize(const std::string& input, const std::string& output, int64_t bitrate = 1000000) {
        input_filename = input;
        output_filename = output;
        bit_rate = bitrate;
        
        std::cout << "=== HEVC到H.264转码器 ===" << std::endl;
        std::cout << "输入文件: " << input_filename << std::endl;
        std::cout << "输出文件: " << output_filename << std::endl;
        std::cout << "目标比特率: " << bit_rate << " bps" << std::endl;
        std::cout << std::endl;
        
        return true;
    }
    
    /**
     * 步骤1：打开输入文件
     */
    bool openInputFile() {
        std::cout << "=== 步骤1：打开输入文件 ===" << std::endl;
        std::cout << "文件路径: " << input_filename << std::endl;
        
        input_format_ctx = avformat_alloc_context();
        if (!input_format_ctx) {
            std::cerr << "错误：无法分配输入格式上下文" << std::endl;
            return false;
        }
        
        if (avformat_open_input(&input_format_ctx, input_filename.c_str(), nullptr, nullptr) < 0) {
            std::cerr << "错误：无法打开输入文件 " << input_filename << std::endl;
            return false;
        }
        
        std::cout << "  输入文件打开成功" << std::endl;
        return true;
    }
    
    /**
     * 步骤2：分析输入流信息
     */
    bool analyzeInputStream() {
        std::cout << "\n=== 步骤2：分析输入流信息 ===" << std::endl;
        
        if (avformat_find_stream_info(input_format_ctx, nullptr) < 0) {
            std::cerr << "错误：无法获取流信息" << std::endl;
            return false;
        }
        
        std::cout << "文件信息：" << std::endl;
        std::cout << "  格式: " << input_format_ctx->iformat->name << std::endl;
        std::cout << "  时长: " << input_format_ctx->duration / AV_TIME_BASE << " 秒" << std::endl;
        std::cout << "  流数量: " << input_format_ctx->nb_streams << std::endl;
        
        // 查找视频流
        for (unsigned i = 0; i < input_format_ctx->nb_streams; i++) {
            if (input_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_index = i;
                width = input_format_ctx->streams[i]->codecpar->width;
                height = input_format_ctx->streams[i]->codecpar->height;
                frame_rate = input_format_ctx->streams[i]->r_frame_rate;
                break;
            }
        }
        
        if (video_stream_index == -1) {
            std::cerr << "错误：未找到视频流" << std::endl;
            return false;
        }
        
        std::cout << "  找到视频流，索引: " << video_stream_index << std::endl;
        std::cout << "  分辨率: " << width << "x" << height << std::endl;
        std::cout << "  编码格式: " << avcodec_get_name(input_format_ctx->streams[video_stream_index]->codecpar->codec_id) << std::endl;
        std::cout << "  帧率: " << frame_rate.num << "/" << frame_rate.den << " fps" << std::endl;
        
        // 检查是否为HEVC编码
        if (input_format_ctx->streams[video_stream_index]->codecpar->codec_id != AV_CODEC_ID_HEVC) {
            std::cout << "警告：输入视频不是HEVC编码，当前编码为: " 
                     << avcodec_get_name(input_format_ctx->streams[video_stream_index]->codecpar->codec_id) << std::endl;
        }
        
        return true;
    }
    
    /**
     * 步骤3：初始化HEVC解码器
     */
    bool initializeDecoder() {
        std::cout << "\n=== 步骤3：初始化HEVC解码器 ===" << std::endl;
        
        // 查找解码器
        decoder = avcodec_find_decoder(input_format_ctx->streams[video_stream_index]->codecpar->codec_id);
        if (!decoder) {
            std::cerr << "错误：未找到对应的解码器" << std::endl;
            return false;
        }
        
        // 分配解码器上下文
        decoder_ctx = avcodec_alloc_context3(decoder);
        if (!decoder_ctx) {
            std::cerr << "错误：无法分配解码器上下文" << std::endl;
            return false;
        }
        
        // 复制编解码器参数到解码器上下文
        if (avcodec_parameters_to_context(decoder_ctx, input_format_ctx->streams[video_stream_index]->codecpar) < 0) {
            std::cerr << "错误：无法复制解码器参数" << std::endl;
            return false;
        }
        
        // 打开解码器
        if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) {
            std::cerr << "错误：无法打开解码器" << std::endl;
            return false;
        }
        
        std::cout << "解码器初始化成功: " << decoder->name << std::endl;
        return true;
    }
    
    /**
     * 步骤4：创建输出文件
     */
    bool createOutputFile() {
        std::cout << "\n=== 步骤4：创建输出文件 ===" << std::endl;
        std::cout << "输出文件: " << output_filename << std::endl;
        
        avformat_alloc_output_context2(&output_format_ctx, nullptr, nullptr, output_filename.c_str());
        if (!output_format_ctx) {
            std::cerr << "错误：无法创建输出格式上下文" << std::endl;
            return false;
        }
        
        std::cout << "输出格式: " << output_format_ctx->oformat->name << std::endl;
        return true;
    }
    
    /**
     * 步骤5：初始化H.264编码器
     */
    bool initializeEncoder() {
        std::cout << "\n=== 步骤5：初始化H.264编码器 ===" << std::endl;
        
        // "指定编码器"，编码时通过AV_CODEC_ID_H264直接指定编码器
        encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!encoder) {
            std::cerr << "错误：未找到H.264编码器" << std::endl;
            return false;
        }
        
        // 容器是空的，需要手动创建视频流
        output_stream = avformat_new_stream(output_format_ctx, nullptr);
        if (!output_stream) {
            std::cerr << "错误：无法创建输出视频流" << std::endl;
            return false;
        }
        
        // 根据具体的编码器"模板"(H.264)创建编码器上下文
        encoder_ctx = avcodec_alloc_context3(encoder);
        if (!encoder_ctx) {
            std::cerr << "错误：无法分配编码器上下文" << std::endl;
            return false;
        }
        
        // 设置编码参数
        encoder_ctx->codec_id = AV_CODEC_ID_H264; //编码器ID
        encoder_ctx->codec_type = AVMEDIA_TYPE_VIDEO; //媒体类型
        encoder_ctx->width = width; //宽
        encoder_ctx->height = height;   //高
        encoder_ctx->time_base = av_inv_q(frame_rate);  // 时间基准
        encoder_ctx->framerate = frame_rate;            // 帧率
        encoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;     // 像素格式
        encoder_ctx->bit_rate = bit_rate;              // 比特率
        
        // H.264特定设置
        av_opt_set(encoder_ctx->priv_data, "preset", "medium", 0);
        av_opt_set(encoder_ctx->priv_data, "crf", "23", 0);
        
        // 检查输出格式是否需要全局头信息(例如MP4这样的封装格式通常需要，头部信息包括如分辨率、编码参数等)
        if (output_format_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
            encoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        
        // 打开编码器
        if (avcodec_open2(encoder_ctx, encoder, nullptr) < 0) {
            std::cerr << "错误：无法打开H.264编码器" << std::endl;
            return false;
        }
        
        // 复制编码器参数到流
        if (avcodec_parameters_from_context(output_stream->codecpar, encoder_ctx) < 0) {
            std::cerr << "错误：无法复制编码器参数" << std::endl;
            return false;
        }
        
        std::cout << "H.264编码器初始化成功" << std::endl;
        std::cout << "  编码参数 - 分辨率: " << width << "x" << height 
                  << ", 帧率: " << frame_rate.num << "/" << frame_rate.den 
                  << ", 比特率: " << bit_rate << std::endl;
        
        return true;
    }
    
    /**
     * 步骤6：准备数据结构
     */
    bool prepareDataStructures() {
        std::cout << "\n=== 步骤6：准备数据结构 ===" << std::endl;
        
        // 分配输入包和输出包
        input_packet = av_packet_alloc();
        output_packet = av_packet_alloc();

        // 分配解码后的帧和编码后的帧
        decoded_frame = av_frame_alloc();
        encoded_frame = av_frame_alloc();
        
        if (!input_packet || !output_packet || !decoded_frame || !encoded_frame) {
            std::cerr << "错误：无法分配数据结构" << std::endl;
            return false;
        }
        
        std::cout << "✓ 数据结构准备完成" << std::endl;
        return true;
    }
    
    /**
     * 步骤7：初始化图像转换器
     */
    bool initializeConverter() {
        std::cout << "\n=== 步骤7：初始化图像转换器 ===" << std::endl;
        
        // 如果解码器和编码器的像素格式不同，需要转换
        if (decoder_ctx->pix_fmt != encoder_ctx->pix_fmt) {
            sws_ctx = sws_getContext(
                decoder_ctx->width, decoder_ctx->height, decoder_ctx->pix_fmt,
                encoder_ctx->width, encoder_ctx->height, encoder_ctx->pix_fmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            );
            
            if (!sws_ctx) {
                std::cerr << "错误：无法创建图像转换上下文" << std::endl;
                return false;
            }
            
            std::cout << "✓ 图像转换器初始化成功" << std::endl;
            std::cout << "  转换: " << av_get_pix_fmt_name(decoder_ctx->pix_fmt) 
                      << " -> " << av_get_pix_fmt_name(encoder_ctx->pix_fmt) << std::endl;
        } else {
            std::cout << "✓ 无需像素格式转换" << std::endl;
        }
        
        return true;
    }
    
    /**
     * 步骤8：打开输出文件并写入头信息
     */
    bool openOutputFile() {
        std::cout << "\n=== 步骤8：打开输出文件 ===" << std::endl;
        
        if (!(output_format_ctx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&output_format_ctx->pb, output_filename.c_str(), AVIO_FLAG_WRITE) < 0) {
                std::cerr << "错误：无法打开输出文件" << output_filename << std::endl;
                return false;
            }
        }
        
        // 写入文件头信息
        if (avformat_write_header(output_format_ctx, nullptr) < 0) {
            std::cerr << "错误：写入文件头信息失败" << std::endl;
            return false;
        }
        
        std::cout << "输出文件打开成功" << std::endl;
        return true;
    }
    
    /**
     * 步骤9：转码主循环
     */
    bool transcodeMainLoop() {
        std::cout << "\n=== 步骤9：开始转码 ===" << std::endl;
        
        // 为编码帧分配缓冲区
        encoded_frame->format = encoder_ctx->pix_fmt;
        encoded_frame->width = encoder_ctx->width;
        encoded_frame->height = encoder_ctx->height;
        if (av_frame_get_buffer(encoded_frame, 0) < 0) {
            std::cerr << "错误：无法为编码帧分配缓冲区" << std::endl;
            return false;
        }
        
        // 读取输入数据包并转码
        while (av_read_frame(input_format_ctx, input_packet) >= 0) {
            if (input_packet->stream_index == video_stream_index) {
                // 解码HEVC帧
                if (decodePacket()) {
                    frame_count++;
                    
                    // 像素格式转换(如果需要)
                    if (sws_ctx) {
                        convertPixelFormat();
                    } else {
                        // 直接复制帧数据
                        //1.复制帧数据
                        av_frame_copy(encoded_frame, decoded_frame);
                        //2.复制帧的元数据
                        av_frame_copy_props(encoded_frame, decoded_frame);
                    }
                    
                    // 设置编码帧的时间戳
                    encoded_frame->pts = frame_count - 1;
                    
                    // 编码为H.264
                    if (!encodeFrame()) {
                        std::cerr << "错误：编码帧失败" << std::endl;
                        av_packet_unref(input_packet);
                        return false;
                    }
                    
                    // 显示进度
                    if (frame_count % 30 == 0) {
                        std::cout << "已转码 " << frame_count << " 帧" << std::endl;
                    }
                }
            }
            av_packet_unref(input_packet);
        }
        
        std::cout << "转码完成，总共处理 " << frame_count << " 帧" << std::endl;
        return true;
    }
    
    /**
     * 步骤10：刷新编解码器
     */
    bool flushCodecs() {
        std::cout << "\n=== 步骤10：刷新编解码器 ===" << std::endl;
        
        std::cout << "刷新解码器缓冲区..." << std::endl;
        
        // 刷新解码器
        int ret = avcodec_send_packet(decoder_ctx, nullptr);
        if (ret < 0) {
            std::cerr << "错误：刷新解码器失败" << std::endl;
            return false;
        }
        
        // 处理解码器缓冲区中剩余的帧
        while ((ret = avcodec_receive_frame(decoder_ctx, decoded_frame)) >= 0) {
            frame_count++;
            
            // 像素格式转换(如果需要)
            if (sws_ctx) {
                convertPixelFormat();
            } else {
                av_frame_copy(encoded_frame, decoded_frame);
                av_frame_copy_props(encoded_frame, decoded_frame);
            }
            
            encoded_frame->pts = frame_count - 1;
            
            // 编码剩余帧
            if (!encodeFrame()) {
                std::cerr << "错误：编码剩余帧失败" << std::endl;
                return false;
            }
        }
        
        std::cout << "刷新编码器缓冲区..." << std::endl;
        
        // 刷新编码器
        ret = avcodec_send_frame(encoder_ctx, nullptr);
        if (ret < 0) {
            std::cerr << "错误：刷新编码器失败" << std::endl;
            return false;
        }
        
        // 处理编码器缓冲区中剩余的数据包
        while ((ret = avcodec_receive_packet(encoder_ctx, output_packet)) >= 0) {
            output_packet->stream_index = output_stream->index;
            av_packet_rescale_ts(output_packet, encoder_ctx->time_base, output_stream->time_base);
            
            if (av_interleaved_write_frame(output_format_ctx, output_packet) < 0) {
                std::cerr << "错误：写入剩余数据包失败" << std::endl;
                return false;
            }
            
            av_packet_unref(output_packet);
        }
        
        return true;
    }
    
    /**
     * 执行完整转码流程
     */
    bool transcode() {
        try {
            // 执行转码流程
            if (!openInputFile() ||
                !analyzeInputStream() ||
                !initializeDecoder() ||
                !createOutputFile() ||
                !initializeEncoder() ||
                !prepareDataStructures() ||
                !initializeConverter() ||
                !openOutputFile() ||
                !transcodeMainLoop() ||
                !flushCodecs()) {
                return false;
            }
            
            std::cout << "\n 转码成功完成！" << std::endl;
            std::cout << "输出文件: " << output_filename << std::endl;
            
            return true;
        } catch (const std::exception& e) {
            std::cerr << "转码过程中发生错误: " << e.what() << std::endl;
            return false;
        }
    }

private:
    /**
     * 解码单个数据包
     */
    bool decodePacket() {
        int ret = avcodec_send_packet(decoder_ctx, input_packet);
        if (ret < 0) {
            std::cerr << "错误：发送数据包到解码器失败" << std::endl;
            return false;
        }
        
        ret = avcodec_receive_frame(decoder_ctx, decoded_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return false;  // 需要更多数据或已结束
        } else if (ret < 0) {
            std::cerr << "错误：从解码器接收帧失败" << std::endl;
            return false;
        }
        
        return true;
    }
    
    /**
     * 编码帧
     */
    bool encodeFrame() {
        int ret = avcodec_send_frame(encoder_ctx, encoded_frame);
        if (ret < 0) {
            std::cerr << "错误：发送帧到编码器失败" << std::endl;
            return false;
        }
        
        while (ret >= 0) {
            ret = avcodec_receive_packet(encoder_ctx, output_packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                std::cerr << "错误：从编码器接收数据包失败" << std::endl;
                return false;
            }
            
            // 设置数据包的流索引
            output_packet->stream_index = output_stream->index;
            
            // 转换时间戳
            av_packet_rescale_ts(output_packet, encoder_ctx->time_base, output_stream->time_base);
            
            // 写入数据包到输出文件
            if (av_interleaved_write_frame(output_format_ctx, output_packet) < 0) {
                std::cerr << "错误：写入输出数据包失败" << std::endl;
                return false;
            }
            
            av_packet_unref(output_packet);
        }
        
        return true;
    }
    
    /**
     * 转换像素格式
     */
    void convertPixelFormat() {
        if (sws_ctx) {
            sws_scale(sws_ctx,
                      decoded_frame->data, decoded_frame->linesize,
                      0, decoded_frame->height,
                      encoded_frame->data, encoded_frame->linesize);
            
            // 复制时间戳和其他属性
            encoded_frame->pts = decoded_frame->pts;
            encoded_frame->pkt_dts = decoded_frame->pkt_dts;
            encoded_frame->key_frame = decoded_frame->key_frame;
            encoded_frame->pict_type = decoded_frame->pict_type;
        }
    }
    
    /**
     * 清理资源
     */
    void cleanup() {
        std::cout << "\n=== 清理资源 ===" << std::endl;
        
        // 写入文件尾
        if (output_format_ctx) {
            av_write_trailer(output_format_ctx);
        }
        
        // 释放图像转换上下文
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
        }
        
        // 释放帧
        if (encoded_frame) {
            av_frame_free(&encoded_frame);
        }
        if (decoded_frame) {
            av_frame_free(&decoded_frame);
        }
        
        // 释放数据包
        if (output_packet) {
            av_packet_free(&output_packet);
        }
        if (input_packet) {
            av_packet_free(&input_packet);
        }
        
        // 释放编解码器上下文
        if (encoder_ctx) {
            avcodec_free_context(&encoder_ctx);
        }
        if (decoder_ctx) {
            avcodec_free_context(&decoder_ctx);
        }
        
        // 关闭输出文件并释放格式上下文
        if (output_format_ctx) {
            if (!(output_format_ctx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&output_format_ctx->pb);
            }
            avformat_free_context(output_format_ctx);
        }
        
        // 释放输入格式上下文
        if (input_format_ctx) {
            avformat_close_input(&input_format_ctx);
        }
        
        std::cout << "资源清理完成" << std::endl;
    }
};

/**
 * 主函数
 */
int main(int argc, char* argv[]) {
    if (argc != 3 && argc != 4) {
        std::cout << "用法: " << argv[0] << " <输入MP4文件(HEVC)> <输出MP4文件(H.264)> [比特率(可选)]" << std::endl;
        std::cout << "示例: " << argv[0] << " input_hevc.mp4 output_h264.mp4" << std::endl;
        std::cout << "示例: " << argv[0] << " input_hevc.mp4 output_h264.mp4 2000000" << std::endl;
        return -1;
    }
    
    // 解析命令行参数
    std::string input_file = argv[1];
    std::string output_file = argv[2];
    int64_t bitrate = (argc == 4) ? std::stoll(argv[3]) : 1000000;  // 默认1Mbps
    
    // 创建转码器对象
    HEVCToH264Transcoder transcoder;
    
    // 初始化转码器
    if (!transcoder.initialize(input_file, output_file, bitrate)) {
        return -1;
    }
    
    // 执行转码
    if (!transcoder.transcode()) {
        std::cerr << "转码失败！" << std::endl;
        return -1;
    }
    
    return 0;
}