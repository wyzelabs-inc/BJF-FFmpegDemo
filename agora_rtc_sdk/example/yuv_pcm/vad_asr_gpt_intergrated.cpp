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
  double vadThreshold = 0.02;
  double vadThresholdLow = 0.005;

  struct {
    int sampleRate = DEFAULT_SAMPLE_RATE;
    int numOfChannels = DEFAULT_NUM_OF_CHANNELS;
  } audio;
};

/**
 * Python VAD+ASR+GPT对话系统桥接类
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
        PyRun_SimpleString("import sys");
        PyRun_SimpleString("sys.path.append('.')");

        // 导入对话系统模块并缓存
        pModule_ = PyImport_ImportModule("agora_vad_asr_gpt_system");
        if (!pModule_) {
            AG_LOG(ERROR, "Failed to import agora_vad_asr_gpt_system module");
            PyErr_Print();
            return false;
        }

        // 缓存处理函数
        pProcessFunc_ = PyObject_GetAttrString(pModule_, "bridge_process_audio");
        if (!pProcessFunc_ || !PyCallable_Check(pProcessFunc_)) {
            AG_LOG(ERROR, "Failed to get bridge_process_audio function");
            Py_DECREF(pModule_);
            pModule_ = nullptr;
            return false;
        }

        // 缓存统计函数
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
        PyObject* pArgs = PyTuple_New(5);
        PyTuple_SetItem(pArgs, 0, PyUnicode_FromString(options.asrModelDir.c_str()));
        PyTuple_SetItem(pArgs, 1, PyUnicode_FromString(options.vadModelDir.c_str()));
        PyTuple_SetItem(pArgs, 2, PyFloat_FromDouble(options.vadThreshold));
        PyTuple_SetItem(pArgs, 3, PyFloat_FromDouble(options.vadThresholdLow));
        PyTuple_SetItem(pArgs, 4, PyLong_FromLong(options.audio.sampleRate));
        
        PyObject* pResult = PyObject_CallObject(pInitFunc, pArgs);
        bool success = false;
        if (pResult) {
            success = PyObject_IsTrue(pResult);
            Py_DECREF(pResult);
        }
        
        Py_DECREF(pArgs);
        Py_DECREF(pInitFunc);
        
        if (success) {
            initialized_ = true;
            AG_LOG(INFO, "Python对话系统桥接初始化成功");
        } else {
            AG_LOG(ERROR, "Python对话系统桥接初始化失败");
            PyErr_Print();
        }
        
        return success;
    }
    
    bool processAudioFrame(const void* audioData,
                          size_t dataSize,
                          int samplesPerChannel,
                          int channels,
                          int sampleRate) {
        if (!initialized_ || !pProcessFunc_) {
            return false;
        }
        
        // 准备参数
        PyObject* pArgs = PyTuple_New(5);
        PyTuple_SetItem(pArgs, 0, PyLong_FromVoidPtr(const_cast<void*>(audioData)));
        PyTuple_SetItem(pArgs, 1, PyLong_FromSize_t(dataSize));
        PyTuple_SetItem(pArgs, 2, PyLong_FromLong(samplesPerChannel));
        PyTuple_SetItem(pArgs, 3, PyLong_FromLong(channels));
        PyTuple_SetItem(pArgs, 4, PyLong_FromLong(sampleRate));
        
        // 调用处理函数
        PyObject* pResult = PyObject_CallObject(pProcessFunc_, pArgs);
        bool success = false;
        if (pResult) {
            success = PyObject_IsTrue(pResult);
            Py_DECREF(pResult);
        }

        Py_DECREF(pArgs);
        
        return success;
    }
    
    std::string getStatistics() {
        if (!initialized_ || !pStatsFunc_) {
            return "{}";
        }

        PyObject* pResult = PyObject_CallObject(pStatsFunc_, nullptr);
        std::string stats = "{}";
        if (pResult && PyUnicode_Check(pResult)) {
            const char* statsStr = PyUnicode_AsUTF8(pResult);
            if (statsStr) {
                stats = std::string(statsStr);
            }
            Py_DECREF(pResult);
        }

        return stats;
    }
    
    void shutdown() {
        if (initialized_) {
            // 调用Python关闭函数
            if (pModule_) {
                PyObject* pShutdownFunc = PyObject_GetAttrString(pModule_, "bridge_shutdown");
                if (pShutdownFunc && PyCallable_Check(pShutdownFunc)) {
                    PyObject_CallObject(pShutdownFunc, nullptr);
                    Py_DECREF(pShutdownFunc);
                }
            }

            // 清理缓存的Python对象
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

            initialized_ = false;
            AG_LOG(INFO, "Python对话系统桥接已关闭");
        }
    }
    
private:
    bool initialized_;
};

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

static bool exitFlag = false;
static void SignalHandler(int sigNo) {
    exitFlag = true;
}

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

    // Cleanup
    localUserObserver->unsetAudioFrameObserver();
    connection->unregisterObserver(connObserver.get());

    if (connection->disconnect()) {
        AG_LOG(ERROR, "Failed to disconnect from Agora channel!");
        return -1;
    }
    AG_LOG(INFO, "Disconnected from Agora channel successfully");

    // 输出最终统计信息
    std::string finalStats = conversationBridge->getStatistics();
    AG_LOG(INFO, "最终对话系统统计: %s", finalStats.c_str());

    // Destroy resources
    conversationBridge->shutdown();
    localUserObserver.reset();
    conversationFrameObserver.reset();
    connection = nullptr;
    service->release();
    service = nullptr;

    return 0;
}
