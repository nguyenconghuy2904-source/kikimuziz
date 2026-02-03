#include "udp_draw_service.h"

#include <esp_log.h>
#include <cstring>
#include <cstdio>

#define TAG "UdpDrawService"

UdpDrawService::UdpDrawService(DrawingDisplay* display, uint16_t port)
    : display_(display),
      port_(port),
      socket_fd_(-1),
      task_handle_(nullptr),
      running_(false),
      drawing_mode_(false),
      packets_received_(0),
      packets_processed_(0),
      pixels_drawn_(0),
      errors_(0) {
    ESP_LOGI(TAG, "🎨 UDP Drawing Service initialized on port %d", port_);
}

UdpDrawService::~UdpDrawService() {
    Stop();
}

bool UdpDrawService::Start() {
    if (running_) {
        ESP_LOGW(TAG, "Service already running");
        return true;
    }

    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: %d", errno);
        return false;
    }

    // Bind to port
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port_);

    if (bind(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket: %d", errno);
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    // Create UDP receive task
    running_ = true;
    if (xTaskCreate(UdpTaskWrapper, "udp_draw", 4096, this, 5, &task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UDP task");
        close(socket_fd_);
        socket_fd_ = -1;
        running_ = false;
        return false;
    }

    ESP_LOGI(TAG, "✅ UDP Drawing Service started on port %d", port_);
    return true;
}

void UdpDrawService::Stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // Close socket to unblock receive
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }

    // Wait for task to finish
    if (task_handle_) {
        vTaskDelay(pdMS_TO_TICKS(100));  // Give task time to exit
        task_handle_ = nullptr;
    }

    ESP_LOGI(TAG, "UDP Drawing Service stopped");
}

void UdpDrawService::UdpTaskWrapper(void* param) {
    UdpDrawService* service = static_cast<UdpDrawService*>(param);
    service->UdpTask();
    vTaskDelete(nullptr);
}

void UdpDrawService::UdpTask() {
    char buffer[128];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    ESP_LOGI(TAG, "📡 UDP receive task started");

    while (running_) {
        // Receive UDP packet with timeout
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int len = recvfrom(socket_fd_, buffer, sizeof(buffer) - 1, 0,
                          (struct sockaddr*)&client_addr, &client_len);

        if (len > 0) {
            buffer[len] = '\0';
            packets_received_++;
            ProcessPacket(buffer, len);
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (running_) {  // Only log error if we're still supposed to be running
                ESP_LOGE(TAG, "recvfrom error: %d", errno);
                errors_++;
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));  // Small delay to prevent watchdog
    }

    ESP_LOGI(TAG, "UDP receive task ended");
}

void UdpDrawService::ProcessPacket(const char* data, int len) {
    // Parse packet format: "x,y,state"
    int x, y, state;
    if (sscanf(data, "%d,%d,%d", &x, &y, &state) != 3) {
        ESP_LOGD(TAG, "Invalid packet format: %s", data);
        errors_++;
        return;
    }

    // Validate coordinates
    int width = display_->GetWidth();
    int height = display_->GetHeight();
    if (x < 0 || x >= width || y < 0 || y >= height) {
        ESP_LOGD(TAG, "Coordinates out of bounds: (%d,%d), display size: %dx%d", x, y, width, height);
        errors_++;
        return;
    }

    // Draw pixel on DrawingDisplay
    if (display_->IsCanvasEnabled()) {
        display_->DrawPixel(x, y, state != 0);
        packets_processed_++;
        pixels_drawn_++;
        ESP_LOGD(TAG, "✏️ Drew pixel at (%d,%d) state=%d", x, y, state);
    } else {
        errors_++;
    }
}

void UdpDrawService::EnableDrawingMode(bool enable) {
    if (enable == drawing_mode_) {
        return;
    }

    drawing_mode_ = enable;

    // Enable/disable canvas on DrawingDisplay
    display_->EnableCanvas(enable);
    
    if (enable) {
        ESP_LOGI(TAG, "🎨 Drawing mode ENABLED - Ready to receive drawings");
    } else {
        ESP_LOGI(TAG, "🎨 Drawing mode DISABLED");
    }
}

void UdpDrawService::ClearCanvas() {
    if (display_->IsCanvasEnabled()) {
        display_->ClearCanvas();
        ESP_LOGI(TAG, "🧹 Canvas cleared");
    } else {
        ESP_LOGW(TAG, "⚠️ No canvas to clear");
    }
}

UdpDrawService::Stats UdpDrawService::GetStats() const {
    return Stats{
        .packets_received = packets_received_.load(),
        .packets_processed = packets_processed_.load(),
        .pixels_drawn = pixels_drawn_.load(),
        .errors = errors_.load()
    };
}
