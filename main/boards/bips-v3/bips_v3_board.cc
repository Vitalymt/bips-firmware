#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_netif_sntp.h>
#include <nvs_flash.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <cJSON.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "BipsV3"

// Activity-based power save — independent of Application::CanEnterSleepMode()
// (which always returns false when wake word engine is running)
#define DISPLAY_OFF_SECONDS  60
#define DEEP_SLEEP_SECONDS   300

class BipsV3 : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    esp_timer_handle_t activity_timer_ = nullptr;
    int idle_seconds_ = 0;
    bool display_off_ = false;
    int64_t last_button_press_ms_ = 0;  // Debounce: ignore rapid presses

    static void activityTimerCallback(void* arg) {
        auto* self = static_cast<BipsV3*>(arg);
        self->idle_seconds_++;

        // No display power save — screen always on

        if (self->idle_seconds_ >= DEEP_SLEEP_SECONDS) {
            ESP_LOGI(TAG, "Entering light sleep after %ds idle", DEEP_SLEEP_SECONDS);
            // Configure GPIO43 (touch button) as wakeup source
            // ext0: wake when GPIO43 is HIGH (active_high=true)
            esp_sleep_enable_ext0_wakeup(GPIO_NUM_43, 1);
            esp_light_sleep_start();
            // Woke up from light sleep — reset idle counter
            ESP_LOGI(TAG, "Woke from light sleep");
            self->idle_seconds_ = 0;
        }
    }

    void ResetActivity() {
        idle_seconds_ = 0;
        if (display_off_) {
            ESP_LOGI(TAG, "Wake display");
            display_off_ = false;
            if (display_) {
                display_->SetPowerSaveMode(false);
            }
        }
    }

    void InitializeActivityTimer() {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = &BipsV3::activityTimerCallback;
        timer_args.arg = this;
        timer_args.name = "activity_timer";
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &activity_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(activity_timer_, 1000000)); // 1 second
    }

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_LOGI(TAG, "Install SH1106 driver");
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_LOGI(TAG, "Install SSD1306 driver");
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            ResetActivity();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnDoubleClick([this]() {
            ESP_LOGI(TAG, "Boot double-click - erasing WiFi config and restarting");
            GetDisplay()->ShowNotification("WiFi Reset...");
            vTaskDelay(pdMS_TO_TICKS(500));
            nvs_flash_erase();
            esp_restart();
        });

        touch_button_.OnClick([this]() {
            // Debounce: ignore if less than 2 seconds since last press
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_button_press_ms_ < 2000) {
                ESP_LOGI(TAG, "Touch click IGNORED (debounce, %lld ms since last)",
                         now_ms - last_button_press_ms_);
                return;
            }
            last_button_press_ms_ = now_ms;

            ResetActivity();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                return;
            }
            ESP_LOGI(TAG, "Touch click - toggle chat");
            app.ToggleChatState();
        });
    }

    void InitializeNtp() {
        // Set timezone to Moscow (UTC+3)
        setenv("TZ", "MSK-3", 1);
        tzset();

        // Start SNTP
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.wait_for_sync = false;  // Don't block — sync in background
        esp_netif_sntp_init(&config);
        ESP_LOGI(TAG, "NTP initialized (timezone: UTC+3 Moscow)");
    }

    void InitializeTavilyTool() {
        auto& mcp_server = McpServer::GetInstance();

        // web_search tool — calls Tavily API
        PropertyList props;
        props.AddProperty(Property("query", kPropertyTypeString));
        mcp_server.AddTool("self.web_search",
            "Search the web using Tavily API. Returns search results with titles, URLs, and snippets.",
            props, [this](const PropertyList& properties) -> ReturnValue {
                auto query = properties["query"].value<std::string>();
                if (query.empty()) {
                    return std::string("Error: empty query");
                }

                // Read API key from NVS
                Settings settings("wifi", false);
                std::string api_key = settings.GetString("tavily_api_key", "");
                if (api_key.empty()) {
                    return std::string("Error: Tavily API key not configured. Set it in WiFi config (Advanced tab).");
                }

                // Build JSON request body
                cJSON* req_json = cJSON_CreateObject();
                cJSON_AddStringToObject(req_json, "api_key", api_key.c_str());
                cJSON_AddStringToObject(req_json, "query", query.c_str());
                cJSON_AddNumberToObject(req_json, "max_results", 3);
                cJSON_AddBoolToObject(req_json, "include_answer", true);
                char* post_data = cJSON_PrintUnformatted(req_json);
                cJSON_Delete(req_json);

                std::string result_str;
                esp_http_client_config_t config = {};
                config.url = "https://api.tavily.com/search";
                config.method = HTTP_METHOD_POST;
                config.crt_bundle_attach = esp_crt_bundle_attach;
                config.timeout_ms = 10000;

                esp_http_client_handle_t client = esp_http_client_init(&config);
                esp_http_client_set_header(client, "Content-Type", "application/json");
                esp_http_client_set_post_field(client, post_data, strlen(post_data));

                esp_err_t err = esp_http_client_perform(client);
                if (err == ESP_OK) {
                    int status = esp_http_client_get_status_code(client);
                    int content_length = esp_http_client_get_content_length(client);
                    if (status == 200 && content_length > 0) {
                        std::string response(content_length, 0);
                        int read = esp_http_client_read(client, &response[0], content_length);
                        if (read > 0) {
                            response.resize(read);
                            // Parse Tavily response and extract key info
                            cJSON* resp = cJSON_Parse(response.c_str());
                            if (resp) {
                                cJSON* answer = cJSON_GetObjectItem(resp, "answer");
                                cJSON* results = cJSON_GetObjectItem(resp, "results");

                                cJSON* output = cJSON_CreateObject();
                                if (answer && cJSON_IsString(answer)) {
                                    cJSON_AddStringToObject(output, "answer", answer->valuestring);
                                }
                                if (results && cJSON_IsArray(results)) {
                                    cJSON* short_results = cJSON_CreateArray();
                                    int count = cJSON_GetArraySize(results);
                                    for (int i = 0; i < count && i < 3; i++) {
                                        cJSON* item = cJSON_GetArrayItem(results, i);
                                        cJSON* short_item = cJSON_CreateObject();
                                        cJSON* title = cJSON_GetObjectItem(item, "title");
                                        cJSON* url = cJSON_GetObjectItem(item, "url");
                                        cJSON* content = cJSON_GetObjectItem(item, "content");
                                        if (title) cJSON_AddStringToObject(short_item, "title", title->valuestring);
                                        if (url) cJSON_AddStringToObject(short_item, "url", url->valuestring);
                                        if (content) {
                                            // Truncate content to 200 chars to save memory
                                            std::string snippet = std::string(content->valuestring).substr(0, 200);
                                            cJSON_AddStringToObject(short_item, "snippet", snippet.c_str());
                                        }
                                        cJSON_AddItemToArray(short_results, short_item);
                                    }
                                    cJSON_AddItemToObject(output, "results", short_results);
                                }
                                char* out_str = cJSON_PrintUnformatted(output);
                                result_str = std::string(out_str);
                                cJSON_free(out_str);
                                cJSON_Delete(output);
                                cJSON_Delete(resp);
                            } else {
                                result_str = response.substr(0, 500);
                            }
                        } else {
                            result_str = "Error: Failed to read response";
                        }
                    } else {
                        ESP_LOGE(TAG, "Tavily HTTP status: %d", status);
                        result_str = "Error: HTTP " + std::to_string(status);
                    }
                } else {
                    ESP_LOGE(TAG, "Tavily HTTP request failed: %s", esp_err_to_name(err));
                    result_str = std::string("Error: HTTP request failed: ") + esp_err_to_name(err);
                }

                esp_http_client_cleanup(client);
                cJSON_free(post_data);

                return result_str;
            });
    }

public:
    BipsV3() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO, true) {
        InitializeDisplayI2c();
        InitializeDisplay();
        InitializeButtons();
        InitializeActivityTimer();
        InitializeNtp();
        InitializeTavilyTool();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        // Don't reset activity timer here — audio service calls this frequently
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(BipsV3);
