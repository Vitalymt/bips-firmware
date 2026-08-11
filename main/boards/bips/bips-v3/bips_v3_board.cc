#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "protocols/protocol.h"
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
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <atomic>
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
#define NON_IDLE_SLEEP_SECONDS 300  // 5 min — auto-sleep during activation / WiFi config

class BipsV3 : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    esp_timer_handle_t activity_timer_ = nullptr;
    std::atomic<int> idle_seconds_{0};
    std::atomic<bool> display_off_{false};
    std::atomic<bool> sleep_requested_{false};
    std::atomic<int> non_idle_seconds_{0};  // Counts in activation/WiFi-config states
    int64_t last_button_press_ms_ = 0;  // Debounce: ignore rapid presses
    bool just_woke_ = false;  // Skip first button press after light sleep

    static void activityTimerCallback(void* arg) {
        auto* self = static_cast<BipsV3*>(arg);

        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();

        if (state == kDeviceStateIdle) {
            // Normal idle path — count toward display-off and deep sleep
            self->non_idle_seconds_ = 0;
            self->idle_seconds_++;

            if (self->idle_seconds_ == DISPLAY_OFF_SECONDS && !self->display_off_) {
                ESP_LOGI(TAG, "Display sleep mode after %ds idle", DISPLAY_OFF_SECONDS);
                self->display_off_ = true;
                if (self->display_) {
                    self->display_->SetPowerSaveMode(true);
                }
            }

            if (self->idle_seconds_ >= DEEP_SLEEP_SECONDS && !self->sleep_requested_) {
                ESP_LOGI(TAG, "Requesting light sleep after %ds idle", DEEP_SLEEP_SECONDS);
                self->sleep_requested_ = true;
            }
        } else if (state == kDeviceStateActivating || state == kDeviceStateWifiConfiguring) {
            // Non-idle but "waiting" states — user isn't actively chatting
            // Count toward auto-sleep to prevent battery drain
            self->idle_seconds_ = 0;
            self->non_idle_seconds_++;

            if (self->non_idle_seconds_ >= NON_IDLE_SLEEP_SECONDS && !self->sleep_requested_) {
                ESP_LOGW(TAG, "Non-idle timeout (%ds) in state %d — requesting sleep",
                         self->non_idle_seconds_.load(), (int)state);
                self->sleep_requested_ = true;
            }
        } else {
            // Active conversation (listening, speaking, connecting) — reset everything
            self->idle_seconds_ = 0;
            self->non_idle_seconds_ = 0;
        }
    }

    // Called from dedicated task to enter light sleep safely
    void EnterLightSleepIfIdle() {
        if (!sleep_requested_) return;

        // SAFETY: only enter sleep when device is truly idle.
        // During activation/WiFi-config the non-idle timer may set
        // sleep_requested_, but we must wait for the state to
        // transition to idle before actually sleeping.
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            return;  // Will retry on next tick when state becomes idle
        }

        sleep_requested_ = false;

        ESP_LOGI(TAG, "Entering light sleep (idle %ds)", idle_seconds_.load());

        // 1. Cleanly shut down the display before sleep
        if (panel_) {
            esp_lcd_panel_disp_on_off(panel_, false);
        }

        // 2. Destroy I2C bus to release pins cleanly before sleep
        if (display_i2c_bus_) {
            i2c_del_master_bus(display_i2c_bus_);
            display_i2c_bus_ = nullptr;
        }

        // 3. Enter light sleep — GPIO43 wakes us
        gpio_wakeup_enable(GPIO_NUM_43, GPIO_INTR_HIGH_LEVEL);
        esp_sleep_enable_gpio_wakeup();
        esp_err_t err = esp_light_sleep_start();
        gpio_wakeup_disable(GPIO_NUM_43);
        ESP_LOGI(TAG, "Woke from light sleep (err=%s)", esp_err_to_name(err));

        // 4. Re-initialize the entire display pipeline
        ReinitDisplay();

        // 5. Restore display state
        idle_seconds_ = 0;
        display_off_ = false;
        just_woke_ = true;

        // 6. Show the UI — ReinitDisplay already loaded the screen
        if (display_) {
            display_->SetPowerSaveMode(false);
        }

        // 7. Give WiFi/websocket time to reconnect before accepting input
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    void ReinitDisplay() {
        ESP_LOGI(TAG, "Reinitializing I2C + display after light sleep");

        // Re-create I2C bus
        InitializeDisplayI2c();

        // Re-create panel IO
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

        // Re-create panel
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        // Update OledDisplay with new handles (keeps LVGL UI objects alive)
        auto* oled = static_cast<OledDisplay*>(display_);
        if (oled) {
            oled->Reinit(panel_io_, panel_);
        }

        ESP_LOGI(TAG, "Display re-initialized after light sleep");
    }

    static void sleepTask(void* arg) {
        auto* self = static_cast<BipsV3*>(arg);
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(500));
            self->EnterLightSleepIfIdle();
        }
    }

    void ResetActivity() {
        idle_seconds_ = 0;
        non_idle_seconds_ = 0;  // Also reset non-idle timer (activation / WiFi config)
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

        // Dedicated task for light sleep — cannot call esp_light_sleep_start()
        // from timer callback because it blocks the timer task
        xTaskCreate(sleepTask, "sleep_task", 3072, this, 5, nullptr);
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
            ESP_LOGI(TAG, "Boot double-click - entering WiFi config mode");
            ResetActivity();
            EnterWifiConfigMode();
        });

        // Long press boot = force stop (unstuck from listening)
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "Boot long-press - force stop");
            ResetActivity();
            auto& app = Application::GetInstance();
            app.AbortSpeaking(kAbortReasonNone);
        });

        touch_button_.OnClick([this]() {
            // Skip first press after light sleep — it was the wake event
            if (just_woke_) {
                just_woke_ = false;
                ESP_LOGI(TAG, "Touch click ignored (just woke from sleep)");
                return;
            }

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
            // If activation timed out, restart it instead of toggling chat
            if (app.IsActivationTimedOut()) {
                ESP_LOGI(TAG, "Touch click - restart activation");
                app.RestartActivation();
                return;
            }
            ESP_LOGI(TAG, "Touch click - toggle chat");
            app.ToggleChatState();
        });

        // Double-click touch = force stop (unstuck from listening/speaking)
        touch_button_.OnDoubleClick([this]() {
            ESP_LOGI(TAG, "Touch double-click - force stop");
            ResetActivity();
            auto& app = Application::GetInstance();
            app.AbortSpeaking(kAbortReasonNone);
        });

        volume_up_button_.OnClick([this]() {
            ResetActivity();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) volume = 100;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_up_button_.OnPressDown([this]() {
            ResetActivity();
        });
        volume_up_button_.OnLongPress([this]() {
            ResetActivity();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            ResetActivity();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) volume = 0;
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_down_button_.OnPressDown([this]() {
            ResetActivity();
        });
        volume_down_button_.OnLongPress([this]() {
            ResetActivity();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
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
        touch_button_(TOUCH_BUTTON_GPIO, true),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
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
        if (level == PowerSaveLevel::PERFORMANCE) {
            ResetActivity();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(BipsV3);
