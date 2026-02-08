/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"

#define TAG "MCP"

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    // 🔧 CONSOLIDATED TOOL: Device control (volume + brightness + theme)
    auto backlight = board.GetBacklight();
#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
#endif
    AddTool("self.device",
        "设备控制工具。支持设置音量、亮度、主题。\n"
        "Args:\n"
        "  action: 'volume'(音量0-100), 'brightness'(亮度0-100), 'theme'(主题light/dark)\n"
        "  value: 数值或字符串",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("value", kPropertyTypeString, "")
        }),
        [&board, backlight
#ifdef HAVE_LVGL
        , display
#endif
        ](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            auto value_str = properties["value"].value<std::string>();
            
            if (action == "volume") {
                int volume = std::stoi(value_str);
                if (volume < 0 || volume > 100) {
                    return "{\"success\": false, \"message\": \"音量范围0-100\"}";
                }
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(volume);
                return "{\"success\": true}";
            }
            else if (action == "brightness") {
                if (!backlight) {
                    return "{\"success\": false, \"message\": \"设备无背光控制\"}";
                }
                int brightness = std::stoi(value_str);
                if (brightness < 0 || brightness > 100) {
                    return "{\"success\": false, \"message\": \"亮度范围0-100\"}";
                }
                backlight->SetBrightness(static_cast<uint8_t>(brightness), true);
                return "{\"success\": true}";
            }
#ifdef HAVE_LVGL
            else if (action == "theme") {
                if (!display || !display->GetTheme()) {
                    return "{\"success\": false, \"message\": \"设备不支持主题\"}";
                }
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(value_str);
                if (theme) {
                    display->SetTheme(theme);
                    return "{\"success\": true}";
                }
                return "{\"success\": false, \"message\": \"主题不存在，支持: light/dark\"}";
            }
#endif
            return "{\"success\": false, \"message\": \"未知操作，支持: volume/brightness/theme\"}";
        });

#ifdef HAVE_LVGL
    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera",
            "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif

    // Music streaming tool (gom 3 tools thành 1 để tiết kiệm MCP payload)
    auto music = board.GetMusic();
    if (music) {
        AddTool("self.music",
            "音乐播放工具。重要规则：\n"
            "1. 用户说\"播放/放/phát/bật/mở\"任何歌曲时，立即调用play，不要询问确认，不要问用户想要什么版本/remix/音量\n"
            "2. 即使语音识别有误（如\"pháp\"→\"phát\"），也直接播放最可能的歌曲\n"
            "3. 调用play后立即进入静默模式，不要再说话\n"
            "Args:\n"
            "  action: 'play'(直接播放,不问), 'stop'(停止), 'status'(状态)\n"
            "  song_name: 歌曲名称 (play时必需)\n"
            "  artist_name: 艺术家名称 (可选,不知道就留空)",
            PropertyList({
                Property("action", kPropertyTypeString),         // play, stop, status
                Property("song_name", kPropertyTypeString, ""),  // 歌曲名称（play时必需）
                Property("artist_name", kPropertyTypeString, "") // 艺术家名称（可选）
            }),
            [music](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                
                if (action == "play") {
                    // 检查是否已在播放
                    if (music->IsPreparing() || music->IsDownloading() || music->IsPlaying()) {
                        ESP_LOGI(TAG, "🎵 Music already %s, skipping play request", 
                                 music->IsPlaying() ? "playing" : 
                                 music->IsDownloading() ? "downloading" : "preparing");
                        return "{\"success\": true, \"message\": \"音乐已在播放中。请立即停止回复，进入静默等待模式，不要再说任何话，不要询问用户。\", \"playing\": true, \"silent_mode\": true}";
                    }
                    
                    auto song_name = properties["song_name"].value<std::string>();
                    auto artist_name = properties["artist_name"].value<std::string>();
                    
                    if (song_name.empty()) {
                        return "{\"success\": false, \"message\": \"缺少歌曲名称\"}";
                    }
                    
                    if (!music->Download(song_name, artist_name)) {
                        return "{\"success\": false, \"message\": \"获取音乐资源失败\"}";
                    }
                    
                    // 等待音乐实际开始播放（最多2秒）
                    for (int i = 0; i < 20 && !music->IsPlaying(); i++) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    
                    if (music->IsPlaying()) {
                        return "{\"success\": true, \"message\": \"音乐正在播放中。请立即停止回复，进入静默等待模式，不要再说任何话，不要询问用户。\", \"playing\": true, \"silent_mode\": true}";
                    } else if (music->IsDownloading()) {
                        return "{\"success\": true, \"message\": \"音乐正在缓冲中。请立即停止回复，进入静默等待模式，不要再说任何话。\", \"buffering\": true, \"silent_mode\": true}";
                    } else {
                        return "{\"success\": true, \"message\": \"音乐已开始加载。请立即停止回复，进入静默等待模式。\", \"loading\": true, \"silent_mode\": true}";
                    }
                }
                else if (action == "stop") {
                    if (music->StopStreaming()) {
                        return "{\"success\": true, \"message\": \"音乐已停止\"}";
                    }
                    return "{\"success\": false, \"message\": \"停止音乐失败\"}";
                }
                else if (action == "status") {
                    cJSON* json = cJSON_CreateObject();
                    cJSON_AddBoolToObject(json, "is_playing", music->IsPlaying());
                    cJSON_AddBoolToObject(json, "is_downloading", music->IsDownloading());
                    cJSON_AddNumberToObject(json, "buffer_size", music->GetBufferSize());
                    return json;
                }
                else {
                    return "{\"success\": false, \"message\": \"未知操作，支持: play/stop/status\"}";
                }
            });
    }

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // 🔧 CONSOLIDATED TOOL: System control (info + reboot + upgrade + assets)
    auto& assets = Assets::GetInstance();
    AddUserOnlyTool("self.system",
        "系统控制工具。\n"
        "Args:\n"
        "  action: 'info'(系统信息), 'reboot'(重启), 'upgrade'(升级固件), 'assets_url'(设置资源URL)\n"
        "  url: 固件/资源URL (仅upgrade/assets_url时需要)",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("url", kPropertyTypeString, "")
        }),
        [&assets](const PropertyList& properties) -> ReturnValue {
            auto action = properties["action"].value<std::string>();
            auto url = properties["url"].value<std::string>();
            
            if (action == "info") {
                return Board::GetInstance().GetSystemInfoJson();
            }
            else if (action == "reboot") {
                auto& app = Application::GetInstance();
                app.Schedule([&app]() {
                    ESP_LOGW(TAG, "User requested reboot");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    app.Reboot();
                });
                return "{\"success\": true, \"message\": \"正在重启...\"}";
            }
            else if (action == "upgrade") {
                if (url.empty()) {
                    return "{\"success\": false, \"message\": \"缺少固件URL\"}";
                }
                ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
                auto& app = Application::GetInstance();
                app.Schedule([url, &app]() {
                    auto ota = std::make_unique<Ota>();
                    bool success = app.UpgradeFirmware(*ota, url);
                    if (!success) {
                        ESP_LOGE(TAG, "Firmware upgrade failed");
                    }
                });
                return "{\"success\": true, \"message\": \"开始升级固件...\"}";
            }
            else if (action == "assets_url") {
                if (!assets.partition_valid()) {
                    return "{\"success\": false, \"message\": \"资源分区无效\"}";
                }
                if (url.empty()) {
                    return "{\"success\": false, \"message\": \"缺少资源URL\"}";
                }
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return "{\"success\": true}";
            }
            return "{\"success\": false, \"message\": \"未知操作，支持: info/reboot/upgrade/assets_url\"}";
        });

    // 🔧 CONSOLIDATED TOOL: Screen control (info + snapshot + preview)
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen",
            "屏幕控制工具。\n"
            "Args:\n"
            "  action: 'info'(屏幕信息), 'snapshot'(截图上传), 'preview'(预览图片)\n"
            "  url: 上传/下载URL (snapshot/preview时需要)\n"
            "  quality: JPEG质量1-100 (仅snapshot，默认80)",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("url", kPropertyTypeString, ""),
                Property("quality", kPropertyTypeString, "80")
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                auto url = properties["url"].value<std::string>();
                
                if (action == "info") {
                    cJSON *json = cJSON_CreateObject();
                    cJSON_AddNumberToObject(json, "width", display->width());
                    cJSON_AddNumberToObject(json, "height", display->height());
                    if (dynamic_cast<OledDisplay*>(display)) {
                        cJSON_AddBoolToObject(json, "monochrome", true);
                    } else {
                        cJSON_AddBoolToObject(json, "monochrome", false);
                    }
                    return json;
                }
#if CONFIG_LV_USE_SNAPSHOT
                else if (action == "snapshot") {
                    if (url.empty()) {
                        return "{\"success\": false, \"message\": \"缺少上传URL\"}";
                    }
                    auto quality_str = properties["quality"].value<std::string>();
                    int quality = std::stoi(quality_str);
                    if (quality < 1 || quality > 100) quality = 80;
                    
                    std::string jpeg_data;
                    if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                        throw std::runtime_error("Failed to snapshot screen");
                    }

                    ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                    
                    std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                    if (!http->Open("POST", url)) {
                        throw std::runtime_error("Failed to open URL: " + url);
                    }
                    {
                        std::string file_header;
                        file_header += "--" + boundary + "\r\n";
                        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                        file_header += "Content-Type: image/jpeg\r\n\r\n";
                        http->Write(file_header.c_str(), file_header.size());
                    }
                    http->Write((const char*)jpeg_data.data(), jpeg_data.size());
                    {
                        std::string multipart_footer = "\r\n--" + boundary + "--\r\n";
                        http->Write(multipart_footer.c_str(), multipart_footer.size());
                    }
                    http->Write("", 0);

                    if (http->GetStatusCode() != 200) {
                        throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                    }
                    std::string result = http->ReadAll();
                    http->Close();
                    ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                    return "{\"success\": true}";
                }
                else if (action == "preview") {
                    if (url.empty()) {
                        return "{\"success\": false, \"message\": \"缺少图片URL\"}";
                    }
                    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                    if (!http->Open("GET", url)) {
                        throw std::runtime_error("Failed to open URL: " + url);
                    }
                    int status_code = http->GetStatusCode();
                    if (status_code != 200) {
                        throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                    }

                    size_t content_length = http->GetBodyLength();
                    char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                    if (data == nullptr) {
                        throw std::runtime_error("Failed to allocate memory for image: " + url);
                    }
                    size_t total_read = 0;
                    while (total_read < content_length) {
                        int ret = http->Read(data + total_read, content_length - total_read);
                        if (ret < 0) {
                            heap_caps_free(data);
                            throw std::runtime_error("Failed to download image: " + url);
                        }
                        if (ret == 0) break;
                        total_read += ret;
                    }
                    http->Close();

                    auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                    display->SetPreviewImage(std::move(image));
                    return "{\"success\": true}";
                }
#endif // CONFIG_LV_USE_SNAPSHOT
                return "{\"success\": false, \"message\": \"未知操作，支持: info/snapshot/preview\"}";
            });
    }
#endif // HAVE_LVGL
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Log raw JSON for debugging
    char* json_string = cJSON_Print(json);
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "McpServer::ParseMessage() called");
    ESP_LOGI(TAG, "  JSON length: %d bytes", json_string ? (int)strlen(json_string) : 0);
    if (json_string) {
        ESP_LOGI(TAG, "  JSON (first 500): %.500s", json_string);
        free(json_string);
    }
    
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        ESP_LOGI(TAG, "=========================================");
        return;
    }
    ESP_LOGI(TAG, "  JSONRPC version: %s", version->valuestring);
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        ESP_LOGI(TAG, "=========================================");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    ESP_LOGI(TAG, "  Method: %s", method_str.c_str());
    
    // Get params (optional)
    auto params = cJSON_GetObjectItem(json, "params");
    ESP_LOGI(TAG, "  Params: %s", params ? "present" : "null");
    
    if (method_str.find("notifications") == 0) {
        ESP_LOGI(TAG, "  Processing notification: %s", method_str.c_str());
        
        if (method_str == "notifications/action_completed") {
            if (cJSON_IsObject(params)) {
                auto action_type = cJSON_GetObjectItem(params, "action_type");
                auto status = cJSON_GetObjectItem(params, "status");
                if (cJSON_IsString(action_type) && cJSON_IsString(status)) {
                    ESP_LOGI(TAG, "🤖 Action completed notification: %s (%s)", 
                            action_type->valuestring, status->valuestring);
                }
            }
        }
        
        ESP_LOGI(TAG, "=========================================");
        return;
    }
    
    // Check params
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        ESP_LOGI(TAG, "=========================================");
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        ESP_LOGI(TAG, "=========================================");
        return;
    }
    auto id_int = id->valueint;
    ESP_LOGI(TAG, "  ID: %d", id_int);
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
