/*
 * Agora RTC/MEDIA SDK with VAD+ASR Integration
 * 
 * 整合Agora音频流拉取和Python VAD+ASR处理
 * C++负责高性能音频流接收，Python负责VAD和ASR处理
 */

#include <csignal>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <memory>
#include <Python.h>  // Python C API
#include <atomic>

#include "AgoraRefCountedObject.h"
#include "IAgoraService.h"
#include "NGIAgoraRtcConnection.h"
#include "common/log.h"
#include "common/opt_parser.h"
#include "common/sample_common.h"
#include "common/sample_connection_observer.h"
#include "common/sample_local_user_observer.h"

#include "NGIAgoraAudioTrack.h"
#include "NGIAgoraLocalUser.h"
#include "NGIAgoraMediaNodeFactory.h"
#include "NGIAgoraMediaNode.h"
#include "NGIAgoraVideoTrack.h"

#define DEFAULT_SAMPLE_RATE (16000)
#define DEFAULT_NUM_OF_CHANNELS (1)
#define STREAM_TYPE_HIGH "high"
#define STREAM_TYPE_LOW "low"

struct SampleOptions {
  std::string appId;
  std::string channelId;
  std::string userId;
  std::string remoteUserId;
  std::string streamType = STREAM_TYPE_HIGH;
  
  // VAD+ASR配置
  std::string asrModelDir = "../models/sherpa-onnx-whisper-base.en";
  std::string vadModelDir = "../models/snakers4_silero-vad";
  double vadThreshold = 0.1;
  double vadThresholdLow = 0.005;

  struct {
    int sampleRate = DEFAULT_SAMPLE_RATE;
    int numOfChannels = DEFAULT_NUM_OF_CHANNELS;
  } audio;
};

/**
 * Python VAD+ASR+GPT对话系统桥接类
 * 
 * @brief
 * 此类封装了与Python VAD+ASR+GPT系统的所有交互。
 * 它负责：
 * 1. 初始化和关闭Python解释器。
 * 2. 加载指定的Python模块 (`agora_vad_asr_gpt_system.py`) 并获取其中的函数。
 * 3. 在C++和Python之间安全地传递数据（如音频帧和统计信息）。
 * 4. 管理多线程环境下的Python全局解释器锁 (GIL)，确保线程安全。
 */
class PythonConversationBridge {
private:
    PyObject* pModule_;
    PyObject* pProcessFunc_;
    PyObject* pStatsFunc_;

public:
    PythonConversationBridge() : initialized_(false), pModule_(nullptr), pProcessFunc_(nullptr), pStatsFunc_(nullptr) {}

    ~PythonConversationBridge() {
        shutdown();
    }
    
    /**
     * @brief 初始化Python桥接。
     * @param options 包含ASR/VAD模型路径、阈值等配置的结构体。
     * @return 如果初始化成功，返回true；否则返回false。
     */
    bool initialize(const SampleOptions& options) {
        AG_LOG(INFO, "初始化Python对话系统桥接...");

        // 初始化Python解释器
        if (!Py_IsInitialized()) {
            Py_Initialize();
            if (!Py_IsInitialized()) {
                AG_LOG(ERROR, "Failed to initialize Python interpreter");
                return false;
            }
            AG_LOG(INFO, "Python解释器初始化成功");
        }

        // 添加当前目录到Python路径
        // 添加当前目录到Python的模块搜索路径 (sys.path)，这样Python才能找到我们的脚本
        PyRun_SimpleString("import sys");
        PyRun_SimpleString("sys.path.append('.')");

        // 导入对话系统模块并缓存
        // 导入Python模块 (agora_vad_asr_gpt_system.py)
        pModule_ = PyImport_ImportModule("agora_vad_asr_gpt_system");
        if (!pModule_) {
            AG_LOG(ERROR, "Failed to import agora_vad_asr_gpt_system module");
            PyErr_Print();
            return false;
        }

        // 缓存处理函数
        // 从模块中获取音频处理函数 (bridge_process_audio) 并缓存
        pProcessFunc_ = PyObject_GetAttrString(pModule_, "bridge_process_audio");
        if (!pProcessFunc_ || !PyCallable_Check(pProcessFunc_)) {
            AG_LOG(ERROR, "Failed to get bridge_process_audio function");
            Py_DECREF(pModule_);
            pModule_ = nullptr;
            return false;
        }

        // 缓存统计函数
        // 从模块中获取统计函数 (bridge_get_statistics) 并缓存
        pStatsFunc_ = PyObject_GetAttrString(pModule_, "bridge_get_statistics");
        if (!pStatsFunc_ || !PyCallable_Check(pStatsFunc_)) {
            AG_LOG(ERROR, "Failed to get bridge_get_statistics function");
            Py_DECREF(pProcessFunc_);
            Py_DECREF(pModule_);
            pProcessFunc_ = nullptr;
            pModule_ = nullptr;
            return false;
        }

        // 获取初始化函数
        // 从模块中获取初始化函数 (bridge_initialize)
        PyObject* pInitFunc = PyObject_GetAttrString(pModule_, "bridge_initialize");
        if (!pInitFunc || !PyCallable_Check(pInitFunc)) {
            AG_LOG(ERROR, "Failed to get bridge_initialize function");
            Py_DECREF(pStatsFunc_);
            Py_DECREF(pProcessFunc_);
            Py_DECREF(pModule_);
            pStatsFunc_ = nullptr;
            pProcessFunc_ = nullptr;
            pModule_ = nullptr;
            return false;
        }
        
        // 调用初始化函数
        // 准备调用Python初始化函数的参数
        PyObject* pArgs = PyTuple_New(5);
        PyTuple_SetItem(pArgs, 0, PyUnicode_FromString(options.asrModelDir.c_str()));
        PyTuple_SetItem(pArgs, 1, PyUnicode_FromString(options.vadModelDir.c_str()));
        PyTuple_SetItem(pArgs, 2, PyFloat_FromDouble(options.vadThreshold));
        PyTuple_SetItem(pArgs, 3, PyFloat_FromDouble(options.vadThresholdLow));
        PyTuple_SetItem(pArgs, 4, PyLong_FromLong(options.audio.sampleRate));
        
        // 调用Python中的 bridge_initialize 函数
        PyObject* pResult = PyObject_CallObject(pInitFunc, pArgs);
        bool success = false;
        if (pResult) {
            success = PyObject_IsTrue(pResult);
            Py_DECREF(pResult); // 释放返回结果的引用
        }
        
        // 释放参数和函数对象的引用
        Py_DECREF(pArgs);
        Py_DECREF(pInitFunc);
        
        if (success) {
            initialized_ = true;
            AG_LOG(INFO, "Python对话系统桥接初始化成功");
            // 在所有主线程Python初始化完成后，释放GIL，允许其他线程使用
            // PyEval_SaveThread() 会释放全局解释器锁 (GIL)，
            // 允许其他线程（如Agora的音频回调线程）在需要时获取GIL并执行Python代码。
            // 这是在C++主线程完成Python初始化后，将Python控制权交给其他线程的关键步骤。
            if (Py_IsInitialized()) {
                // 这个函数返回一个 `PyThreadState*`，我们不需要保存它，因为后续将使用 `PyGILState_Ensure`
                // 和 `PyGILState_Release` 来管理GIL，这种方式更简单、更安全。
                PyEval_SaveThread();
            }
        } else {
            AG_LOG(ERROR, "Python对话系统桥接初始化失败");
            PyErr_Print();
        }
        
        return success;
    }
    
    /**
     * @brief 处理单个音频帧。此函数会被高频调用。
     * @param audioData 音频数据指针。
     * @param dataSize 音频数据大小（字节）。
     * @param samplesPerChannel 每通道采样点数。
     * @param channels 通道数。
     * @param sampleRate 采样率。
     * @return 如果处理成功，返回true；否则返回false。
     */
    bool processAudioFrame(const void* audioData,
                          size_t dataSize,
                          int samplesPerChannel,
                          int channels,
                          int sampleRate) {
        if (!initialized_ || !pProcessFunc_) {
            return false;
        }
        
        // 获取Python全局解释器锁 (GIL)
        // 在调用任何Python C API之前，当前线程必须获取全局解释器锁 (GIL)。
        // PyGILState_Ensure() 确保当前线程持有GIL，并返回一个状态token。
        // 即使当前线程已经持有GIL，它也能安全地工作。
        PyGILState_STATE gstate = PyGILState_Ensure();

        // 准备参数
        // 准备传递给Python函数的参数。
        // 注意：我们直接传递原始指针 (audioData) 的地址。
        // Python端会使用ctypes将其转换回指针并读取数据。
        // 这种方式避免了在C++和Python之间复制整个音频缓冲区，从而提高了性能。
        PyObject* pArgs = PyTuple_New(5);
        PyTuple_SetItem(pArgs, 0, PyLong_FromVoidPtr(const_cast<void*>(audioData)));
        PyTuple_SetItem(pArgs, 1, PyLong_FromSize_t(dataSize));
        PyTuple_SetItem(pArgs, 2, PyLong_FromLong(samplesPerChannel));
        PyTuple_SetItem(pArgs, 3, PyLong_FromLong(channels));
        PyTuple_SetItem(pArgs, 4, PyLong_FromLong(sampleRate));
        
        // 调用处理函数

        // 调用Python中的 bridge_process_audio 函数
        PyObject* pResult = PyObject_CallObject(pProcessFunc_, pArgs);
        bool success = false;
        if (pResult) {
            success = PyObject_IsTrue(pResult);
            Py_DECREF(pResult); // 释放返回结果的引用
        } else {
            // 如果Python函数执行出错，打印错误信息
            PyErr_Print();
        }

        // 释放参数元组的引用
        Py_DECREF(pArgs);

        // 释放Python全局解释器锁 (GIL)
        // 处理完毕后，释放GIL，让其他线程有机会执行Python代码。
        // 必须使用从 PyGILState_Ensure() 获取的token来调用。
        PyGILState_Release(gstate);
        
        return success;
    }
    
    std::string getStatistics() {
        if (!initialized_ || !pStatsFunc_) {
            return "{}";
        }

        // 获取Python全局解释器锁 (GIL)
        // 同样，在调用Python API前获取GIL
        PyGILState_STATE gstate = PyGILState_Ensure();

        // 调用Python中的 bridge_get_statistics 函数 (此函数无参数)
        PyObject* pResult = PyObject_CallObject(pStatsFunc_, nullptr);
        // 将返回的Python字符串转换为C++ std::string
        std::string stats = "{}";
        if (pResult && PyUnicode_Check(pResult)) {
            const char* statsStr = PyUnicode_AsUTF8(pResult);
            if (statsStr) {
                stats = std::string(statsStr);
            }
            Py_DECREF(pResult);
        } else if (pResult == nullptr) {
            PyErr_Print();
        }

        // 释放Python全局解释器锁 (GIL)
        // 释放GIL
        PyGILState_Release(gstate);

        return stats;
    }
    
    /**
     * @brief 关闭Python桥接并释放所有资源。
     */
    void shutdown() {
        if (initialized_) {
            // 为了安全地清理所有Python资源，我们必须先获取GIL
            PyGILState_STATE gstate = PyGILState_Ensure();

            // 1. 调用Python端的清理函数
            if (pModule_) {
                PyObject* pShutdownFunc = PyObject_GetAttrString(pModule_, "bridge_shutdown");
                if (pShutdownFunc && PyCallable_Check(pShutdownFunc)) {
                    PyObject_CallObject(pShutdownFunc, nullptr);
                    Py_DECREF(pShutdownFunc);
                } else if (PyErr_Occurred()) {
                    PyErr_Print();
                }
            }

            // 2. 清理所有缓存的Python对象（必须在GIL保护下）
            if (pStatsFunc_) {
                Py_DECREF(pStatsFunc_); 
                pStatsFunc_ = nullptr;
            }
            if (pProcessFunc_) {
                Py_DECREF(pProcessFunc_); 
                pProcessFunc_ = nullptr;
            }
            if (pModule_) {
                Py_DECREF(pModule_);
                pModule_ = nullptr;
            }

            // 3. 释放GIL。
            PyGILState_Release(gstate);

            initialized_ = false;
            AG_LOG(INFO, "Python对话系统桥接已关闭");
        }

     
    }
    
private:
    bool initialized_;
};

static std::atomic<bool> exitFlag{false};
static void SignalHandler(int sigNo) {
    exitFlag = true;
    AG_LOG(INFO,"收到退出信号(%d),准备退出...",sigNo);
}

/**
 * 音频帧观察器 - 集成对话系统处理
 */
class ConversationAudioFrameObserver : public agora::media::IAudioFrameObserverBase {
public:
    ConversationAudioFrameObserver(std::shared_ptr<PythonConversationBridge> bridge)
        : bridge_(bridge), frameCount_(0) {}

    bool onPlaybackAudioFrame(const char* channelId, AudioFrame& audioFrame) override { 
        return true; 
    }

    bool onRecordAudioFrame(const char* channelId, AudioFrame& audioFrame) override { 
        return true; 
    }

    bool onMixedAudioFrame(const char* channelId, AudioFrame& audioFrame) override { 
        return true; 
    }

    bool onPlaybackAudioFrameBeforeMixing(const char* channelId, 
                                         agora::media::base::user_id_t userId, 
                                         AudioFrame& audioFrame) override {
        
        
        if(exitFlag){
            AG_LOG(INFO,"程序正在退出，忽略音频帧处理");
            return false;
        }
        frameCount_++;
        
        // 计算数据大小
        size_t dataSize = audioFrame.samplesPerChannel * audioFrame.channels * sizeof(int16_t);
        
        // 发送到Python对话系统处理
        bool success = bridge_->processAudioFrame(
            audioFrame.buffer,
            dataSize,
            audioFrame.samplesPerChannel,
            audioFrame.channels,
            audioFrame.samplesPerSec
        );
        
        // 每1000帧打印一次统计
        if (frameCount_ % 1000 == 0) {
            std::string stats = bridge_->getStatistics();
            AG_LOG(INFO, "已处理音频帧: %d, 统计: %s", frameCount_, stats.c_str());
        }
        
        return success;
    }

    bool onEarMonitoringAudioFrame(AudioFrame& audioFrame) override { 
        return true; 
    }

    AudioParams getEarMonitoringAudioParams() override { 
        return AudioParams(); 
    }

    int getObservedAudioFramePosition() override { 
        return 0; 
    }

    AudioParams getPlaybackAudioParams() override { 
        return AudioParams(); 
    }

    AudioParams getRecordAudioParams() override { 
        return AudioParams(); 
    }

    AudioParams getMixedAudioParams() override { 
        return AudioParams(); 
    }

private:
    std::shared_ptr<PythonConversationBridge> bridge_;
    int frameCount_;
};

int main(int argc, char* argv[]) {
    SampleOptions options;
    opt_parser optParser;

    optParser.add_long_opt("token", &options.appId, "The token for authentication");
    optParser.add_long_opt("channelId", &options.channelId, "Channel Id");
    optParser.add_long_opt("userId", &options.userId, "User Id / default is 0");
    optParser.add_long_opt("remoteUserId", &options.remoteUserId, "The remote user to receive stream from");
    optParser.add_long_opt("sampleRate", &options.audio.sampleRate, "Sample rate for received audio");
    optParser.add_long_opt("numOfChannels", &options.audio.numOfChannels, "Number of channels for received audio");
    optParser.add_long_opt("streamtype", &options.streamType, "the stream type");
    optParser.add_long_opt("asrModelDir", &options.asrModelDir, "ASR model directory");
    optParser.add_long_opt("vadModelDir", &options.vadModelDir, "VAD model directory");
    optParser.add_long_opt("vadThreshold", &options.vadThreshold, "VAD threshold");
    optParser.add_long_opt("vadThresholdLow", &options.vadThresholdLow, "VAD low threshold");

    if ((argc <= 1) || !optParser.parse_opts(argc, argv)) {
        std::ostringstream strStream;
        optParser.print_usage(argv[0], strStream);
        std::cout << strStream.str() << std::endl;
        return -1;
    }

    if (options.appId.empty()) {
        AG_LOG(ERROR, "Must provide appId!");
        return -1;
    }

    if (options.channelId.empty()) {
        AG_LOG(ERROR, "Must provide channelId!");
        return -1;
    }

    std::signal(SIGQUIT, SignalHandler);
    std::signal(SIGABRT, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    // 初始化Python对话系统桥接
    auto conversationBridge = std::make_shared<PythonConversationBridge>();
    if (!conversationBridge->initialize(options)) {
        AG_LOG(ERROR, "Failed to initialize Python conversation bridge!");
        return -1;
    }

    // Create Agora service
    auto service = createAndInitAgoraService(false, true, true);
    if (!service) {
        AG_LOG(ERROR, "Failed to creating Agora service!");
        return -1;
    }

    // Create Agora connection
    agora::rtc::RtcConnectionConfiguration ccfg;
    ccfg.clientRoleType = agora::rtc::CLIENT_ROLE_AUDIENCE;
    ccfg.autoSubscribeAudio = false;
    ccfg.autoSubscribeVideo = false;
    ccfg.enableAudioRecordingOrPlayout = false;

    agora::agora_refptr<agora::rtc::IRtcConnection> connection = service->createRtcConnection(ccfg);
    if (!connection) {
        AG_LOG(ERROR, "Failed to creating Agora connection!");
        return -1;
    }

    // Subscribe audio streams
    if (options.remoteUserId.empty()) {
        AG_LOG(INFO, "Subscribe audio streams from all remote users");
        connection->getLocalUser()->subscribeAllAudio();
    } else {
        connection->getLocalUser()->subscribeAudio(options.remoteUserId.c_str());
    }

    // Register connection observer
    auto connObserver = std::make_shared<SampleConnectionObserver>();
    connection->registerObserver(connObserver.get());

    // Create local user observer
    auto localUserObserver = std::make_shared<SampleLocalUserObserver>(connection->getLocalUser());

    // Register conversation audio frame observer
    auto conversationFrameObserver = std::make_shared<ConversationAudioFrameObserver>(conversationBridge);
    if (connection->getLocalUser()->setPlaybackAudioFrameBeforeMixingParameters(
            options.audio.numOfChannels, options.audio.sampleRate)) {
        AG_LOG(ERROR, "Failed to set audio frame parameters!");
        return -1;
    }
    localUserObserver->setAudioFrameObserver(conversationFrameObserver.get());

    // Connect to Agora channel
    if (connection->connect(options.appId.c_str(), options.channelId.c_str(), options.userId.c_str())) {
        AG_LOG(ERROR, "Failed to connect to Agora channel!");
        return -1;
    }

    // Start receiving incoming media data
    AG_LOG(INFO, "Start receiving audio data with conversation processing...");

    // Periodically check exit flag and print statistics
    int statsPrintCounter = 0;
    while (!exitFlag) {
        usleep(10000);  // 10ms

        // 每10秒打印一次统计信息
        if (++statsPrintCounter >= 1000) {  // 10ms * 1000 = 10s
            std::string stats = conversationBridge->getStatistics();
            AG_LOG(INFO, "对话系统统计: %s", stats.c_str());
            statsPrintCounter = 0;
        }
    }

    // --- Cleanup ---
    // 1. 首先断开Agora连接，并注销所有观察者。
    //    这会停止所有回调（包括音频帧回调），确保没有其他线程再尝试访问Python。
    AG_LOG(INFO, "开始清理...");

    localUserObserver->unsetAudioFrameObserver();
    connection->unregisterObserver(connObserver.get());

    if (connection->disconnect()) {
        AG_LOG(ERROR, "Failed to disconnect from Agora channel!");
    }else{
        AG_LOG(INFO, "Disconnected from Agora channel successfully");
    }
    

    // 等待一小段时间，确保所有回调线程都已完全退出
    usleep(500000); // 100ms

    // 2. 在确认没有其他线程会调用Python后，安全地关闭Python桥接。
    std::string finalStats = conversationBridge->getStatistics();
    AG_LOG(INFO, "最终对话系统统计: %s", finalStats.c_str());

    conversationBridge->shutdown();

    // 3. 释放其他C++资源
    localUserObserver.reset();
    conversationFrameObserver.reset();
    connection = nullptr;
    service->release();
    service = nullptr;

    if(Py_IsInitialized()){
        // 在调用 Py_FinalizeEx 之前，必须确保当前线程持有GIL
        // 因为我们在初始化后调用了 PyEval_SaveThread()，所以主线程不再持有GIL
        PyGILState_STATE gstate = PyGILState_Ensure();
        int ret = Py_FinalizeEx();
        if (ret < 0) AG_LOG(ERROR, "Py_FinalizeEx failed with code %d", ret);
        AG_LOG(INFO, "Python解释器已关闭");
    }
    AG_LOG(INFO, "清理完成");
    
    return 0;
}
