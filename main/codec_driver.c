// codec_driver.c
//
// Two audio backends, both compiled in and selected at runtime from the web
// "Settings -> Audio output" setting:
//
//   AUDIO_OUT_I2S_AMP  - plain I2S DAC/amp (MAX98357A, PCM5102, UDA1334, ...).
//                        No control bus, so volume is scaled in software.
//   AUDIO_OUT_ES8388   - ES8388/ES8311 codec configured over I2C.
//   AUDIO_OUT_AUTO     - probe the I2C codec only when I2C pins are configured;
//                        if it does not answer, fall back to the plain I2S amp
//                        instead of failing the whole audio pipeline.
#include "codec_driver.h"
#include "app_config.h"
#include "config_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "CODEC";

// ---------------------------------------------------------------------------
//  ES8388 backend
// ---------------------------------------------------------------------------
#include "driver/i2c.h"

#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_FREQ_HZ  100000
// ES8388 7-bit I2C address. CE/AD0 strap selects 0x10 (low) or 0x11 (high).
#define ES8388_ADDR         0x10

// --- ES8388 register map (subset used here) ---
#define ES8388_CONTROL1     0x00
#define ES8388_CONTROL2     0x01
#define ES8388_CHIPPOWER    0x02
#define ES8388_ADCPOWER     0x03
#define ES8388_DACPOWER     0x04
#define ES8388_ADCCONTROL1  0x09
#define ES8388_ADCCONTROL2  0x0A
#define ES8388_ADCCONTROL3  0x0B
#define ES8388_ADCCONTROL4  0x0C
#define ES8388_ADCCONTROL5  0x0D
#define ES8388_ADCCONTROL8  0x10
#define ES8388_ADCCONTROL9  0x11
#define ES8388_DACCONTROL1  0x17
#define ES8388_DACCONTROL2  0x18
#define ES8388_DACCONTROL3  0x19
#define ES8388_DACCONTROL21 0x2B
#define ES8388_DACCONTROL23 0x2D
#define ES8388_DACCONTROL24 0x2E   // LOUT1 volume
#define ES8388_DACCONTROL25 0x2F   // ROUT1 volume
#define ES8388_DACCONTROL26 0x30   // LOUT2 volume
#define ES8388_DACCONTROL27 0x31   // ROUT2 volume

// Which backend is active for this boot.
static uint8_t  active_out = AUDIO_OUT_I2S_AMP;
static uint8_t  es8388_addr = ES8388_ADDR;
static bool     i2c_installed = false;
// Software volume for backends without a hardware volume control (0..100).
static uint8_t  sw_volume = 100;

static esp_err_t i2c_master_init(void) {
    hardware_settings_t hw;
    config_manager_load_hw(&hw);
    // -1 means "not connected": without a bus there is no I2C codec.
    if (hw.pin_i2c_sda == -1 || hw.pin_i2c_scl == -1) {
        ESP_LOGW(TAG, "No I2C pins configured (SDA/SCL = -1)");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = hw.pin_i2c_sda,
        .scl_io_num = hw.pin_i2c_scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        i2c_installed = true;
        return ESP_OK;
    }
    if (err == ESP_OK) i2c_installed = true;
    return err;
}

static esp_err_t es_write(uint8_t reg, uint8_t data) {
    uint8_t buf[2] = { reg, data };
    return i2c_master_write_to_device(I2C_MASTER_NUM, es8388_addr, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

// Probe + configure the chip. Returns ESP_OK when it answered and was set up.
static esp_err_t es8388_init(void) {
    esp_err_t ret = i2c_master_init();
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t probe = 0;
    ret = i2c_master_write_read_device(I2C_MASTER_NUM, ES8388_ADDR, &probe, 0, &probe, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ret = i2c_master_write_read_device(I2C_MASTER_NUM, 0x11, &probe, 0, &probe, 1, pdMS_TO_TICKS(100));
        if (ret == ESP_OK) {
            es8388_addr = 0x11;
        }
    }
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "ES8388 responded at 0x%02X", es8388_addr);

    // Standard ES8388 bring-up sequence (line-in mic -> ADC, DAC -> HP/line out).
    // Mirrors the widely-used ESP-ADF / AI-Thinker reference configuration.
    ret  = es_write(ES8388_CONTROL2,     0x50);
    ret |= es_write(ES8388_CHIPPOWER,    0x00); // power up all
    ret |= es_write(ES8388_CONTROL1,     0x12); // enable ref, soft ramp
    ret |= es_write(ES8388_DACPOWER,     0x00); // power up DAC + outputs
    // ADC path
    ret |= es_write(ES8388_ADCPOWER,     0x00);
    ret |= es_write(ES8388_ADCCONTROL1,  0x88); // PGA gain +24dB (mic)
    ret |= es_write(ES8388_ADCCONTROL2,  0x00); // LINSEL/RINSEL = LIN1/RIN1
    ret |= es_write(ES8388_ADCCONTROL3,  0x02);
    ret |= es_write(ES8388_ADCCONTROL4,  0x0C); // 16-bit, I2S format
    ret |= es_write(ES8388_ADCCONTROL5,  0x02); // MCLK/256 ratio
    ret |= es_write(ES8388_ADCCONTROL8,  0x00); // ADC L volume 0dB
    ret |= es_write(ES8388_ADCCONTROL9,  0x00); // ADC R volume 0dB
    // DAC path
    ret |= es_write(ES8388_DACCONTROL1,  0x18); // 16-bit, I2S format
    ret |= es_write(ES8388_DACCONTROL2,  0x02); // MCLK/256 ratio
    ret |= es_write(ES8388_DACCONTROL3,  0x00); // unmute DAC
    ret |= es_write(ES8388_DACCONTROL23, 0x00);
    // Route DAC to output mixer
    ret |= es_write(0x27, 0xB8);
    ret |= es_write(0x2A, 0xB8);
    // Output volumes (0..0x21). Start at a safe mid level.
    ret |= es_write(ES8388_DACCONTROL24, 0x1E);
    ret |= es_write(ES8388_DACCONTROL25, 0x1E);
    ret |= es_write(ES8388_DACCONTROL26, 0x1E);
    ret |= es_write(ES8388_DACCONTROL27, 0x1E);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 init sequence failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ES8388 initialized.");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

esp_err_t codec_init(void) {
    app_settings_t settings;
    config_manager_load(&settings);
    uint8_t want = settings.audio_out;

    if (want == AUDIO_OUT_AUTO) {
        hardware_settings_t hw;
        config_manager_load_hw(&hw);
        if (hw.pin_i2c_sda == -1 || hw.pin_i2c_scl == -1) {
            // No control bus wired: it can only be a plain I2S amp.
            want = AUDIO_OUT_I2S_AMP;
        } else {
            want = AUDIO_OUT_ES8388; // probe below, fall back on failure
        }
    }

    if (want == AUDIO_OUT_ES8388) {
        ESP_LOGI(TAG, "Audio output: I2C codec (ES8388/ES8311)");
        if (es8388_init() == ESP_OK) {
            active_out = AUDIO_OUT_ES8388;
            sw_volume = 100; // hardware volume in use
            return ESP_OK;
        }
        if (settings.audio_out == AUDIO_OUT_ES8388) {
            ESP_LOGE(TAG, "ES8388 did not respond. Is the codec wired and powered, "
                          "or should the audio output be a plain I2S amp?");
            if (i2c_installed) { i2c_driver_delete(I2C_MASTER_NUM); i2c_installed = false; }
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "No I2C codec found - falling back to plain I2S output "
                      "(set 'Audio output' in the web Settings if this is wrong)");
        if (i2c_installed) { i2c_driver_delete(I2C_MASTER_NUM); i2c_installed = false; }
    }

    active_out = AUDIO_OUT_I2S_AMP;
    ESP_LOGI(TAG, "Audio output: plain I2S amp (MAX98357A/PCM5102 style, no control bus)");
    return ESP_OK;
}

esp_err_t codec_deinit(void) {
    if (i2c_installed) {
        i2c_driver_delete(I2C_MASTER_NUM);
        i2c_installed = false;
    }
    return ESP_OK;
}

esp_err_t codec_set_volume(uint8_t volume_percent) {
    if (volume_percent > 100) volume_percent = 100;

    if (active_out != AUDIO_OUT_ES8388) {
        // No hardware volume: remember it so the pipeline can scale the PCM.
        sw_volume = volume_percent;
        ESP_LOGI(TAG, "Software volume %d%% (amp has no volume control)", volume_percent);
        return ESP_OK;
    }

    uint8_t reg = (uint8_t)((volume_percent * 0x21) / 100); // 0..33
    ESP_LOGI(TAG, "Volume %d%% (reg 0x%02X)", volume_percent, reg);
    esp_err_t r  = es_write(ES8388_DACCONTROL24, reg);
    r |= es_write(ES8388_DACCONTROL25, reg);
    r |= es_write(ES8388_DACCONTROL26, reg);
    r |= es_write(ES8388_DACCONTROL27, reg);
    return r;
}

esp_err_t codec_set_mic_gain(uint8_t gain_db) {
    if (active_out != AUDIO_OUT_ES8388) {
        ESP_LOGD(TAG, "No codec: mic gain is fixed by the I2S microphone.");
        return ESP_OK;
    }
    // ADCCONTROL1 holds the L/R PGA gain in 3dB steps (0x00..0x88 = 0..+24dB).
    uint8_t step = gain_db / 3;
    if (step > 8) step = 8;
    uint8_t reg = (uint8_t)((step << 4) | step);
    ESP_LOGI(TAG, "Mic gain ~%ddB (reg 0x%02X)", gain_db, reg);
    return es_write(ES8388_ADCCONTROL1, reg);
}

void codec_apply_volume(int16_t *samples, size_t count) {
    if (!samples || sw_volume >= 100 || active_out == AUDIO_OUT_ES8388) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        samples[i] = (int16_t)(((int32_t)samples[i] * sw_volume) / 100);
    }
}

bool codec_has_hardware_volume(void) {
    return active_out == AUDIO_OUT_ES8388;
}
