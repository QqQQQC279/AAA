#pragma once

#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"

// WAV文件头的结构体
// __attribute__((packed)) 确保编译器不会为了对齐而添加额外的填充字节
struct WavHeader {
    // RIFF Header
    char riff_header[4]; // Contains "RIFF"
    int wav_size;        // Size of the wav portion of the file, which follows the first 8 bytes. File size - 8
    char wave_header[4]; // Contains "WAVE"

    // Format Header
    char fmt_header[4]; // Contains "fmt " (includes trailing space)
    int fmt_chunk_size; // Should be 16 for PCM
    short audio_format; // Should be 1 for PCM. 3 for IEEE Float
    short num_channels;
    int sample_rate;
    int byte_rate;       // Number of bytes per second. sample_rate * num_channels * Bytes Per Sample
    short sample_alignment; // num_channels * Bytes Per Sample
    short bit_depth;     // Number of bits per sample

    // Data
    char data_header[4]; // Contains "data"
    int data_bytes;      // Number of bytes in data. Number of samples * num_channels * Bytes Per Sample
};


class ESP32AudioRecorder {
public:
    /**
     * @brief 构造函数
     * @param i2s_port I2S端口号 (e.g., I2S_NUM_0)
     * @param pin_config I2S引脚配置
     */
    ESP32AudioRecorder(i2s_port_t i2s_port, const i2s_pin_config_t& pin_config);

    /**
     * @brief 析构函数，确保资源被释放
     */
    ~ESP32AudioRecorder();

    /**
     * @brief 初始化I2S驱动。必须在使用前调用。
     * @param sample_rate 采样率 (e.g., 16000)
     * @param bits_per_sample 采样位深 (e.g., I2S_BITS_PER_SAMPLE_16BIT)
     * @return 如果初始化成功，返回 true
     */
    bool InstallDriver(int sample_rate, i2s_bits_per_sample_t bits_per_sample);

    /**
     * @brief 开始录音。这是一个非阻塞方法。
     * 它会创建一个后台任务来处理实际的录音过程。
     * @param filepath 要保存的WAV文件的完整路径 (e.g., "/sdcard/record.wav")
     * @return 如果成功启动后台任务，返回 true
     */
    bool StartRecording(const std::string& filepath);

    /**
     * @brief 停止录音。
     * 它会向后台任务发送停止信号，并等待任务完成文件写入。
     * @return 如果成功停止并保存文件，返回 true
     */
    bool StopRecording();

    /**
     * @brief 检查当前是否正在录音
     * @return 如果正在录音，返回 true
     */
    bool IsRecording() const;

private:
    /**
     * @brief FreeRTOS 静态任务函数。
     * 这是在后台运行以捕获I2S数据并写入文件的实际工作者。
     * @param arg 指向 ESP32AudioRecorder 实例的指针
     */
    static void recording_task_function(void* arg);

    /**
     * @brief 向文件写入一个占位的WAV文件头。
     * @param file 指向打开文件的指针
     */
    void write_wav_header(FILE* file);

    /**
     * @brief 录音结束后，更新WAV文件头中的大小信息。
     * @param file 指向打开文件的指针
     * @param total_data_bytes 录制的音频数据总字节数
     */
    void update_wav_header(FILE* file, int total_data_bytes);

    // 成员变量
    i2s_port_t m_i2s_port;
    i2s_pin_config_t m_pin_config;
    int m_sample_rate;
    i2s_bits_per_sample_t m_bits_per_sample;

    std::string m_filepath;
    FILE* m_file_handle;
    TaskHandle_t m_recording_task_handle;

    // volatile 关键字确保多任务访问该变量时的可见性
    volatile bool m_is_recording;
    int m_data_bytes_written;
};