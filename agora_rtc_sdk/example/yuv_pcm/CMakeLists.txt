cmake_minimum_required(VERSION 3.10)
project(DefaultSamples)

# Define platform for correct log path on Linux
add_definitions(-DTARGET_OS_LINUX)

# 头文件路径
include_directories(
    ../
    ../common
    ../common/file_parser
    ../../agora_sdk/include
)

# 库路径
link_directories(../../agora_sdk)

# 发送端可执行文件
add_executable(sample_send_yuv_pcm
    sample_send_yuv_pcm.cpp
    ../common/helper.cpp
    ../common/opt_parser.cpp
    ../common/sample_common.cpp
    ../common/sample_connection_observer.cpp
    ../common/sample_event.cpp
    ../common/sample_local_user_observer.cpp
    ../common/file_parser/helper_aac_parser.cpp
)






add_executable(sample_receive_yuv_pcm
    sample_receive_yuv_pcm.cpp
    ../common/helper.cpp
    ../common/opt_parser.cpp
    ../common/sample_common.cpp
    ../common/sample_connection_observer.cpp
    ../common/sample_event.cpp
    ../common/sample_local_user_observer.cpp
    ../common/file_parser/helper_aac_parser.cpp
)






# 链接库（只写名字即可）
target_link_libraries(sample_send_yuv_pcm
    agora_rtc_sdk
    agora-fdkaac
    aosl
    pthread
    
)



target_link_libraries(sample_receive_yuv_pcm
    agora_rtc_sdk
    agora-fdkaac
    aosl
    pthread
)



add_executable(youtube_rtmp_pusher
    youtube_rtmp_pusher.cpp
    ../common/helper.cpp
    ../common/opt_parser.cpp
    ../common/sample_common.cpp
    ../common/sample_connection_observer.cpp
    ../common/sample_event.cpp
    ../common/sample_local_user_observer.cpp
    ../common/file_parser/helper_aac_parser.cpp
)

target_link_libraries(youtube_rtmp_pusher
    agora_rtc_sdk
    agora-fdkaac
    aosl
    pthread
    avcodec
    avformat
    avutil
    swscale
    swresample
)



add_executable(youtube_rtmp_pusher_beken
    youtube_rtmp_pusher_beken.cpp
    ../common/helper.cpp
    ../common/opt_parser.cpp
    ../common/sample_common.cpp
    ../common/sample_connection_observer.cpp
    ../common/sample_event.cpp
    ../common/sample_local_user_observer.cpp
    ../common/file_parser/helper_aac_parser.cpp
)

target_link_libraries(youtube_rtmp_pusher_beken
    agora_rtc_sdk
    agora-fdkaac
    aosl
    pthread
    avcodec
    avformat
    avutil
    swscale
    swresample
)

# 查找Python开发库
execute_process(
    COMMAND python3-config --includes
    OUTPUT_VARIABLE PYTHON_INCLUDE_FLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND python3-config --libs --embed
    OUTPUT_VARIABLE PYTHON_LINK_FLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

add_executable(vad_asr_intergrated
    vad_asr_intergrated.cpp
    ../common/helper.cpp
    ../common/opt_parser.cpp
    ../common/sample_common.cpp
    ../common/sample_connection_observer.cpp
    ../common/sample_event.cpp
    ../common/sample_local_user_observer.cpp
    ../common/file_parser/helper_aac_parser.cpp
)

# 添加Python编译标志
set_target_properties(vad_asr_intergrated PROPERTIES
    COMPILE_FLAGS "${PYTHON_INCLUDE_FLAGS}"
)

target_link_libraries(vad_asr_intergrated
    agora_rtc_sdk
    agora-fdkaac
    aosl
    pthread
    python3.10
    crypt
    dl
    m
)

# VAD+ASR+GPT对话系统可执行文件
add_executable(vad_asr_gpt_intergrated
    vad_asr_gpt_intergrated.cpp
    ../common/helper.cpp
    ../common/opt_parser.cpp
    ../common/sample_common.cpp
    ../common/sample_connection_observer.cpp
    ../common/sample_event.cpp
    ../common/sample_local_user_observer.cpp
    ../common/file_parser/helper_aac_parser.cpp
)

# 添加Python编译标志
set_target_properties(vad_asr_gpt_intergrated PROPERTIES
    COMPILE_FLAGS "${PYTHON_INCLUDE_FLAGS}"
)

target_link_libraries(vad_asr_gpt_intergrated
    agora_rtc_sdk
    agora-fdkaac
    aosl
    pthread
    python3.10
    crypt
    dl
    m
)