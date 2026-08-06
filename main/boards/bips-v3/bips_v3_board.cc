#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "power_save_timer.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <nvs_flash.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "BipsV3"

// SH1106 raw commands for display orientation
#define SH1106_CMD_SEGMENT_REMAP_NORMAL   0xA0
#define SH1106_CMD_SEGMENT_REMAP_REVERSE  0xA1
#define SH1106_CMD_COM_SCAN_NORMAL        0xC0
#define SH1106_CMD_COM_SCAN_REVERSE       0xC8
#define SH1106_CMD_DISPLAY_NORMAL         0xA6
#define SH1106_CMD_DISPLAY_INVERSE        0xA7

class BipsV3 : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
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

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, false, false);

        // SH1106 driver init already sends SEGMENT_REMAP_INVERSE (0xA1) and COM_SCAN_REVERSE (0xC8)
        // The driver's mirror() function is buggy for SH1106 - uses 0xA6/0xA7 (color inversion) for mirror_x
        // instead of 0xA0/0xA1 (segment remap).
        // 
        // Default state after init: SEG_REMAP=REVERSE, COM_SCAN=REVERSE
        // For our display orientation (MIRROR_Y=true), we need COM_SCAN=NORMAL (0xC0)
        // This flips the display vertically without affecting colors.
        //
        // Send raw command: COM_SCAN_MODE_NORMAL (0xC0) to flip vertically
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, SH1106_CMD_SEGMENT_REMAP_NORMAL, NULL, 0));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, SH1106_CMD_COM_SCAN_NORMAL, NULL, 0));
        ESP_LOGI(TAG, "Display orientation: COM_SCAN_NORMAL applied (vertical flip)");
    }

    void InitializeButtons() {
        // Boot button: click = toggle chat, double-click = WiFi reset
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
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

        // Touch button (TTP223 on GPIO43): same as boot - click to toggle chat
        touch_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                return;
            }
            ESP_LOGI(TAG, "Touch click - toggle chat");
            app.ToggleChatState();
        });
    }

public:
    BipsV3() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeDisplayI2c();
        InitializeDisplay();
        InitializeButtons();
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
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(BipsV3);
