#!/usr/bin/env python3
"""
Agora VAD+ASR+GPT 一体化对话系统

配置说明:
设置环境变量 OPENAI_API_KEY:
export OPENAI_API_KEY="your-openai-api-key-here"

如果未设置API密钥，系统将使用模拟回复模式
"""

import os
import time

import threading
import numpy as np
import ctypes
from typing import Optional, Callable
from collections import deque
from enum import Enum

# 检查依赖
try:
    import torch
    TORCH_AVAILABLE = True
    print("✅ PyTorch可用")
except ImportError:
    TORCH_AVAILABLE = False
    print("❌ PyTorch不可用，将使用简化VAD")

try:
    import sherpa_onnx
    SHERPA_AVAILABLE = True
    print("✅ Sherpa-ONNX可用")
except ImportError:
    SHERPA_AVAILABLE = False
    print("❌ Sherpa-ONNX不可用，将使用简化ASR")

try:
    import openai
    OPENAI_AVAILABLE = True
    print("✅ OpenAI可用")
except ImportError:
    OPENAI_AVAILABLE = False
    print("❌ OpenAI不可用，将使用模拟回复")

# 对话状态枚举
class ConversationState(Enum):
    LISTENING = "listening"      # 监听语音
    RECOGNIZING = "recognizing"  # 语音识别中
    CHATTING = "chatting"       # GPT思考中
    SPEAKING = "speaking"       # 播放回复中

class SileroVAD:
    """Silero VAD语音活动检测"""
    
    def __init__(self, model_path: str, threshold: float = 0.04, threshold_low: float = 0.005):
        self.threshold = threshold # 进一步提高阈值以降低敏感度
        self.threshold_low = threshold_low
        self.sample_rate = 16000
        self.frame_size = 512  # 32ms @ 16kHz
        
        # 加载模型
        print(f"🔄 加载Silero VAD模型: {model_path}")
        self.model, _ = torch.hub.load(
            repo_or_dir=model_path,
            model='silero_vad',
            source='local',
            force_reload=False,
            onnx=False
        )
        self.model.eval()
        print("✅ Silero VAD模型加载成功")
        
        # 状态变量
        self.audio_buffer = []
        self.last_is_voice = False
    
    def process_audio(self, audio_chunk: np.ndarray) -> bool:
        """处理音频块，返回是否检测到语音"""
        # 添加到缓冲区
        self.audio_buffer.extend(audio_chunk)
        
        # 检查是否有足够的数据进行VAD
        if len(self.audio_buffer) < self.frame_size:
            return self.last_is_voice
        
        # 提取一帧数据
        frame = np.array(self.audio_buffer[:self.frame_size], dtype=np.float32)
        self.audio_buffer = self.audio_buffer[self.frame_size:]
        
        # VAD检测
        with torch.no_grad():
            frame_tensor = torch.from_numpy(frame)
            speech_prob = self.model(frame_tensor, self.sample_rate).item()
        
        # 决策逻辑
        if speech_prob >= self.threshold:
            is_voice = True
        elif speech_prob <= self.threshold_low:
            is_voice = False
        else:
            is_voice = self.last_is_voice  # 保持上一状态
        
        self.last_is_voice = is_voice
        return is_voice

class SherpaOnnxASR:
    """Sherpa-ONNX ASR语音识别"""
    
    def __init__(self, model_dir: str):
        self.model_dir = model_dir
        print(f"模型目录: {model_dir}")
        
        # 初始化识别器
        print("正在初始化 Whisper 模型...")
        recognizer = sherpa_onnx.OfflineRecognizer.from_whisper(
            encoder=f"{model_dir}/base.en-encoder.onnx",
            decoder=f"{model_dir}/base.en-decoder.onnx",
            tokens=f"{model_dir}/base.en-tokens.txt"
        )
        self.recognizer = recognizer
        print("模型初始化完成!")
    
    def transcribe_audio_data(self, audio_data: np.ndarray, sample_rate: int) -> str:
        """直接从numpy数组识别语音"""
        try:
            # 创建音频流
            stream = self.recognizer.create_stream()
            
            # 输入音频数据
            stream.accept_waveform(sample_rate, audio_data)
            
            # 执行识别
            self.recognizer.decode_stream(stream)
            result = stream.result.text.strip()
            
            return result
            
        except Exception as e:
            print(f"❌ ASR识别错误: {e}")
            return ""

class GPTClient:
    """GPT对话客户端"""
    
    def __init__(self):
        # 从环境变量获取API密钥
        api_key = os.getenv('OPENAI_API_KEY')

        if api_key:
            openai.api_key = api_key
            print("✅ GPT客户端初始化成功")
            self.use_mock = False
        else:
            print("⚠️  未设置OpenAI API密钥，将使用模拟回复")
            self.use_mock = True
        
        # 对话历史
        self.conversation_history = []
    
    def chat(self, user_message: str) -> str:
        """与GPT对话"""
        try:
            # 添加用户消息到历史
            self.conversation_history.append({"role": "user", "content": user_message})

            # 保持历史长度
            if len(self.conversation_history) > 10:
                self.conversation_history = self.conversation_history[-10:]

            # 创建OpenAI客户端
            client = openai.OpenAI(api_key=openai.api_key)

            # 调用GPT API (新版本语法)
            response = client.chat.completions.create(
                model="gpt-3.5-turbo",
                messages=[
                    {"role": "system", "content": "you are a friendly ai robot,response my question"},
                    *self.conversation_history
                ],
                max_tokens=150,
                temperature=0.7
            )

            reply = response.choices[0].message.content.strip()

            # 添加AI回复到历史
            self.conversation_history.append({"role": "assistant", "content": reply})

            return reply

        except Exception as e:
            print(f"❌ GPT API错误: {e}")
            return "抱歉，我现在无法回复。"

class AgoraConversationSystem:
    """Agora对话系统主类"""
    
    def __init__(self,
                 asr_model_dir: str = "../models/sherpa-onnx-whisper-base.en",
                 vad_model_dir: str = "../models/snakers4_silero-vad",
                 vad_threshold: float = 0.1, # 统一默认值为0.1
                 vad_threshold_low: float = 0.005,
                 sample_rate: int = 16000):
        
        self.sample_rate = sample_rate
        
        # 初始化组件
        if TORCH_AVAILABLE:
            self.vad = SileroVAD(vad_model_dir, vad_threshold, vad_threshold_low)
        else:
            raise RuntimeError("需要PyTorch支持")
        
        if SHERPA_AVAILABLE:
            self.asr = SherpaOnnxASR(asr_model_dir)
        else:
            raise RuntimeError("需要Sherpa-ONNX支持")
        
        self.gpt = GPTClient()
        
        # 状态管理
        self.state = ConversationState.LISTENING
        self.state_lock = threading.Lock()
        
        # VAD状态机
        self.is_speaking = False
        self.speech_chunk_count = 0
        self.silence_chunk_count = 0
        self.min_speech_chunks = 7      # 最小连续语音块数 (7块=224ms)
        self.max_silence_chunks = 15    # 最大连续静音块数 (15块=480ms)，增加此值可容忍更长的停顿
        
        # 语音收集
        self.is_collecting_speech = False
        self.speech_buffer = []

        # 前置缓冲区，用于捕获语音开始前的音频
        self.pre_buffer_duration_ms = 200  # 缓存200ms的音频
        self.pre_buffer = deque(maxlen=int(self.sample_rate * self.pre_buffer_duration_ms / 1000))
        
        # 统计信息
        self.total_chunks = 0
        self.voice_chunks = 0
        self.conversation_count = 0
        
        print("✅ Agora对话系统初始化完成")

    def get_state(self) -> ConversationState:
        """获取当前状态"""
        with self.state_lock:
            return self.state

    def set_state(self, new_state: ConversationState):
        """设置新状态"""
        with self.state_lock:
            if self.state != new_state:
                print(f"🔄 状态切换: {self.state.value} → {new_state.value}")
                self.state = new_state

    def process_audio_chunk(self, audio_chunk: np.ndarray):
        """处理音频块"""
        self.total_chunks += 1
        current_state = self.get_state()

        # 只在LISTENING状态处理VAD
        if current_state != ConversationState.LISTENING:
            return

        # VAD检测
        has_voice = self.vad.process_audio(audio_chunk)

        if has_voice:
            self.voice_chunks += 1

        # 无论是否有语音，都填充前置缓冲区
        if not self.is_collecting_speech:
            self.pre_buffer.extend(audio_chunk)

        # VAD状态机
        speech_start = False
        speech_end = False

        if has_voice:
            self.speech_chunk_count += 1
            self.silence_chunk_count = 0

            if not self.is_speaking and self.speech_chunk_count >= self.min_speech_chunks:
                self.is_speaking = True
                speech_start = True
        else:
            self.silence_chunk_count += 1
            self.speech_chunk_count = 0

            if self.is_speaking and self.silence_chunk_count >= self.max_silence_chunks:
                self.is_speaking = False
                speech_end = True

        # 处理语音事件
        if speech_start:
            print(f"🎤 开始监听语音...")
            self.speech_buffer = []
            self.is_collecting_speech = True
            # 将前置缓冲区的内容加入语音缓冲区，以捕获完整的句子开头
            self.speech_buffer = list(self.pre_buffer)

        if self.is_collecting_speech:
            self.speech_buffer.extend(audio_chunk)

        if speech_end:
            speech_duration = len(self.speech_buffer) / self.sample_rate if self.speech_buffer else 0
            print(f"🔇 语音结束，时长: {speech_duration:.2f}s")
            self.is_collecting_speech = False

            if len(self.speech_buffer) > self.sample_rate * 0.5:  # 最小语音时长：0.5秒
                print(f"📝 开始语音识别...")
                self.set_state(ConversationState.RECOGNIZING)

                # 异步处理ASR和GPT
                speech_data = np.array(self.speech_buffer, dtype=np.float32)
                threading.Thread(target=self._process_conversation, args=(speech_data,), daemon=True).start()
            else:
                print(f"⚠️  语音太短，继续监听")

    def _process_conversation(self, speech_data: np.ndarray):
        """处理完整的对话流程（ASR + GPT）"""
        try:
            # ASR识别
            start_time = time.time()
            user_text = self.asr.transcribe_audio_data(speech_data, self.sample_rate)
            asr_time = time.time() - start_time

            if not user_text.strip():
                print("🔇 未识别到有效语音，继续监听")
                self.set_state(ConversationState.LISTENING)
                return

            print(f"📝 用户说: {user_text} (识别耗时: {asr_time:.2f}s)")

            # GPT对话
            self.set_state(ConversationState.CHATTING)
            print(f"🤖 正在思考回复: {user_text}")

            start_time = time.time()
            gpt_reply = self.gpt.chat(user_text)
            gpt_time = time.time() - start_time

            print(f"🤖 AI回复: {gpt_reply} (思考耗时: {gpt_time:.2f}s)")

            # --- ！！！在这里集成TTS和SPEAKING状态 !!! ---
            # self.set_state(ConversationState.SPEAKING)
            # print("🔊 正在播放AI回复...")
            #
            # # 伪代码：调用TTS引擎播放语音
            # # tts_engine.play(gpt_reply)
            #
            # print("✅ 回复播放完成")
            # -----------------------------------------

            self.conversation_count += 1

            # 回到监听状态
            self.set_state(ConversationState.LISTENING)
            print(f"👂 继续监听新的语音输入...")

        except Exception as e:
            print(f"❌ 对话处理错误: {e}")
            self.set_state(ConversationState.LISTENING)


# ============================================================================
# C++桥接接口
# ============================================================================

# 全局实例
_conversation_system: Optional[AgoraConversationSystem] = None

def bridge_initialize(asr_model_dir: str = None,
                     vad_model_dir: str = None,
                     vad_threshold: float = 0.1, # 确认此处的默认值为0.1
                     vad_threshold_low: float = 0.005,
                     sample_rate: int = 16000) -> bool:
    """C++调用的初始化函数"""
    global _conversation_system

    try:
        print("🔧 初始化Agora对话系统...")

        # 设置默认路径
        if asr_model_dir is None:
            asr_model_dir = "../models/sherpa-onnx-whisper-base.en"
        if vad_model_dir is None:
            vad_model_dir = "../models/snakers4_silero-vad"

        _conversation_system = AgoraConversationSystem(
            asr_model_dir=asr_model_dir,
            vad_model_dir=vad_model_dir,
            vad_threshold=vad_threshold,
            vad_threshold_low=vad_threshold_low,
            sample_rate=sample_rate
        )

        return True

    except Exception as e:
        print(f"❌ 初始化失败: {e}")
        return False

def bridge_process_audio(audio_data_ptr: int,
                        data_size: int,
                        samples_per_channel: int,
                        channels: int,
                        sample_rate: int) -> bool:
    """C++调用的音频处理函数 - 增强内存安全"""
    global _conversation_system

    # 严格的参数检查
    if _conversation_system is None:
        return False

    if audio_data_ptr == 0 or data_size <= 0:
        return False

    try:
        # 安全地从C++指针读取数据
        audio_data = ctypes.string_at(audio_data_ptr, data_size)

        if len(audio_data) != data_size:
            print(f"⚠️ 数据大小不匹配: 期望{data_size}, 实际{len(audio_data)}")
            return False

        # 转换音频数据
        audio_array = np.frombuffer(audio_data, dtype=np.int16)

        # 处理多声道
        if channels > 1:
            audio_array = audio_array[::channels]

        # 验证数据有效性
        if len(audio_array) == 0:
            return False

        # 转换为float32
        audio_float32 = audio_array.astype(np.float32) / 32768.0

        # 检查数据范围
        if np.any(np.isnan(audio_float32)) or np.any(np.isinf(audio_float32)):
            print(f"⚠️ 检测到无效音频数据")
            return False

        # 安全地处理音频
        _conversation_system.process_audio_chunk(audio_float32)

        return True

    except (ctypes.ArgumentError, ValueError, MemoryError) as e:
        print(f"❌ 内存/数据错误: {e}")
        return False
    except Exception as e:
        print(f"❌ 音频处理错误: {e}")
        return False

def bridge_get_statistics() -> str:
    """C++调用的统计信息获取函数"""
    global _conversation_system

    if _conversation_system is None:
        return "{}"

    try:
        import json
        voice_ratio = (_conversation_system.voice_chunks / _conversation_system.total_chunks * 100) if _conversation_system.total_chunks > 0 else 0

        stats = {
            'total_chunks': _conversation_system.total_chunks,
            'voice_chunks': _conversation_system.voice_chunks,
            'voice_ratio': voice_ratio,
            'conversation_count': _conversation_system.conversation_count,
            'current_state': _conversation_system.get_state().value
        }

        return json.dumps(stats)

    except Exception as e:
        print(f"❌ 统计获取错误: {e}")
        return "{}"

def bridge_shutdown():
    """C++调用的关闭函数"""
    global _conversation_system

    if _conversation_system:
        print("🛑 对话系统关闭")
        _conversation_system = None
