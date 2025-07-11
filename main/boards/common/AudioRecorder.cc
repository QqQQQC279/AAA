#include "AudioRecorder.h"
#include "esp_log.h"
#include <sys/stat.h> // 用于检查文件是否存在

// 为日志输出定义一个标签
static const char* TAG = "AudioRecorder";

ESP32AudioRecorder::ESP32AudioRecorder(i2s_port_t i2s_port, const i2s_pin_config_t& pin_config)
    : m_i2s_port(i2s_port),
      m_pin_config(pin_config),
      m_sample_rate(0),
      m_bits_per_sample(I2S_BITS_PER_SAMPLE_16BIT),
      m_file_handle(nullptr),
      m_recording_task_handle(nullptr),
      m_is_recording(false),
      m_data_bytes_written(0) {
}

ESP32AudioRecorder::~ESP32AudioRecorder() {
    if (m_is_recording) {
        StopRecording();
    }
    i2s_driver_uninstall(m_i2s_port);
}

bool ESP32AudioRecorder::InstallDriver(int sample_rate, i2s_bits_per_sample_t bits_per_sample) {
    m_sample_rate = sample_rate;
    m_bits_per_sample = bits_per_sample;

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = m_sample_rate,
        .bits_per_sample = m_bits_per_sample,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // 根据你的麦克风选择
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false
    };

    esp_err_t err = i2s_driver_install(m_i2s_port, &i2s_config, 0, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S driver install failed");
        return false;
    }

    err = i2s_set_pin(m_i2s_port, &m_pin_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S set pin failed");
        return false;
    }

    ESP_LOGI(TAG, "I2S driver installed successfully");
    return true;
}

bool ESP32AudioRecorder::StartRecording(const std::string& filepath) {
    if (m_is_recording) {
        ESP_LOGE(TAG, "Recording is already in progress.");
        return false;
    }

    m_filepath = filepath;
    m_is_recording = true;
    m_data_bytes_written = 0;

    // 创建后台录音任务
    BaseType_t result = xTaskCreate(
        recording_task_function,    // 任务函数
        "RecordingTask",            // 任务名称
        4096,                       // 任务栈大小 (Bytes)
        this,                       // 传递给任务的参数 (指向当前对象的指针)
        5,                          // 任务优先级
        &m_recording_task_handle    // 任务句柄
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create recording task");
        m_is_recording = false;
        return false;
    }

    ESP_LOGI(TAG, "Recording started, saving to %s", m_filepath.c_str());
    return true;
}

bool ESP32AudioRecorder::StopRecording() {
    if (!m_is_recording) {
        ESP_LOGE(TAG, "Not currently recording.");
        return false;
    }
    
    ESP_LOGI(TAG, "Stopping recording...");
    m_is_recording = false;

    // 等待任务结束。更稳健的实现可以使用信号量(semaphore)来同步
    // 这里我们简单地给任务一点时间来完成文件写入和清理
    // 如果任务句柄不为NULL，说明任务可能还在运行
    if (m_recording_task_handle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(200)); // 等待200ms
    }

    ESP_LOGI(TAG, "Recording stopped. Total data bytes written: %d", m_data_bytes_written);
    return true;
}

bool ESP32AudioRecorder::IsRecording() const {
    return m_is_recording;
}

// ---- Private Methods ----

void ESP32AudioRecorder::write_wav_header(FILE* file) {
    WavHeader header;
    header.num_channels = 1; // 单声道
    header.sample_rate = m_sample_rate;
    header.bit_depth = (short)m_bits_per_sample;
    header.byte_rate = header.sample_rate * header.num_channels * (header.bit_depth / 8);
    header.sample_alignment = header.num_channels * (header.bit_depth / 8);

    // 写入占位符
    memcpy(header.riff_header, "RIFF", 4);
    header.wav_size = 0; // 稍后更新
    memcpy(header.wave_header, "WAVE", 4);
    memcpy(header.fmt_header, "fmt ", 4);
    header.fmt_chunk_size = 16;
    header.audio_format = 1; // PCM
    memcpy(header.data_header, "data", 4);
    header.data_bytes = 0; // 稍后更新

    fwrite(&header, 1, sizeof(WavHeader), file);
}

void ESP32AudioRecorder::update_wav_header(FILE* file, int total_data_bytes) {
    fseek(file, 0, SEEK_SET); // 移动到文件开头
    WavHeader header;
    // 只读取，不覆盖已有内容
    fread(&header, 1, sizeof(WavHeader), file);

    header.data_bytes = total_data_bytes;
    header.wav_size = total_data_bytes + sizeof(WavHeader) - 8;
    
    fseek(file, 0, SEEK_SET); // 再次移动到文件开头
    fwrite(&header, 1, sizeof(WavHeader), file); // 写入更新后的文件头
}


void ESP32AudioRecorder::recording_task_function(void* arg) {
    ESP32AudioRecorder* recorder = static_cast<ESP32AudioRecorder*>(arg);

    // 打开文件用于写入
    recorder->m_file_handle = fopen(recorder->m_filepath.c_str(), "wb");
    if (!recorder->m_file_handle) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", recorder->m_filepath.c_str());
        recorder->m_is_recording = false; // 出错，设置标志位以避免问题
        vTaskDelete(NULL); // 删除任务
        return;
    }
    
    // 写入初始的WAV文件头
    recorder->write_wav_header(recorder->m_file_handle);

    // 分配读取缓冲区
    const size_t buffer_size = 1024;
    uint8_t* buffer = (uint8_t*)malloc(buffer_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for buffer");
        fclose(recorder->m_file_handle);
        recorder->m_is_recording = false;
        vTaskDelete(NULL);
        return;
    }
    
    size_t bytes_read;
    recorder->m_data_bytes_written = 0;

    // 录音主循环
    while (recorder->m_is_recording) {
        // 从I2S驱动读取数据
        esp_err_t result = i2s_read(recorder->m_i2s_port, buffer, buffer_size, &bytes_read, pdMS_TO_TICKS(100));
        if (result == ESP_OK && bytes_read > 0) {
            // 将读取到的数据写入文件
            fwrite(buffer, 1, bytes_read, recorder->m_file_handle);
            recorder->m_data_bytes_written += bytes_read;
        } else {
            ESP_LOGW(TAG, "I2S read error or timeout");
        }
    }

    // 录音结束，清理工作
    free(buffer);
    
    // 更新WAV文件头中的大小信息
    recorder->update_wav_header(recorder->m_file_handle, recorder->m_data_bytes_written);

    // 关闭文件
    fclose(recorder->m_file_handle);
    recorder->m_file_handle = nullptr;

    ESP_LOGI(TAG, "File saved. Task is finishing.");
    
    // 清理任务句柄并删除任务
    recorder->m_recording_task_handle = nullptr;
    vTaskDelete(NULL);
}