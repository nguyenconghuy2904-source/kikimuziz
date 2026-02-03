#pragma once

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include <atomic>
#include <memory>

#include "drawing_display.h"

/**
 * @brief UDP Drawing Service - cho phép vẽ trên màn hình Otto từ xa qua UDP
 * 
 * Tương thích với Android app từ dự án Draw_on_OLED:
 * https://github.com/BenchRobotics/Draw_on_OLED
 * 
 * Protocol: UDP packets với format "x,y,state"
 * - x, y: Tọa độ pixel (0 đến width-1, 0 đến height-1)
 * - state: 1 = vẽ (white), 0 = xóa (black)
 * 
 * Sử dụng:
 * 1. Tạo instance: udp_draw_service_ = std::make_unique<UdpDrawService>(display_, 12345);
 * 2. Start service: udp_draw_service_->Start();
 * 3. Kết nối từ Android app với IP của ESP32
 * 4. Vẽ trên app, sẽ hiển thị realtime trên màn Otto
 */
class UdpDrawService {
public:
    /**
     * @brief Constructor
     * @param display Pointer to DrawingDisplay object
     * @param port UDP port to listen on (default 12345)
     */
    UdpDrawService(DrawingDisplay* display, uint16_t port = 12345);
    ~UdpDrawService();

    /**
     * @brief Start UDP drawing service
     * @return true nếu start thành công, false nếu thất bại
     */
    bool Start();

    /**
     * @brief Stop UDP drawing service
     */
    void Stop();

    /**
     * @brief Check if service is running
     * @return true nếu đang chạy, false nếu không
     */
    bool IsRunning() const { return running_; }

    /**
     * @brief Enable/disable drawing mode
     * @param enable true = enable drawing mode, false = disable
     * 
     * Khi enable: màn hình chuyển sang drawing canvas
     * Khi disable: màn hình quay về normal mode
     */
    void EnableDrawingMode(bool enable);

    /**
     * @brief Clear drawing canvas
     */
    void ClearCanvas();

    /**
     * @brief Get drawing statistics
     */
    struct Stats {
        uint32_t packets_received;
        uint32_t packets_processed;
        uint32_t pixels_drawn;
        uint32_t errors;
    };
    Stats GetStats() const;

private:
    static void UdpTaskWrapper(void* param);
    void UdpTask();
    void ProcessPacket(const char* data, int len);

    DrawingDisplay* display_;
    uint16_t port_;
    int socket_fd_;
    TaskHandle_t task_handle_;
    std::atomic<bool> running_;
    std::atomic<bool> drawing_mode_;
    
    // Statistics
    std::atomic<uint32_t> packets_received_;
    std::atomic<uint32_t> packets_processed_;
    std::atomic<uint32_t> pixels_drawn_;
    std::atomic<uint32_t> errors_;
};
