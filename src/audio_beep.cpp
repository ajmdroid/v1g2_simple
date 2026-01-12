// audio_beep.cpp
// TTS playback for VOL 0 warning using ES8311 DAC on Waveshare ESP32-S3-Touch-LCD-3.49
// Hardware: ES8311 (I2C/I2S), TCA9554 IO expander (I2C, pin 7 = speaker amp enable)
//
// I2C bus: SDA=47, SCL=48 (shared with battery manager TCA9554)
// I2S pins: MCLK=7, BCLK=15, WS=46, DOUT=45 (for playback)
// TCA9554 address: 0x20 (ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000)
// ES8311 address: 0x18

#include "audio_beep.h"
#include "battery_manager.h"  // For tca9554Wire (shared I2C bus)
#include <Arduino.h>
#include <Wire.h>
#include "driver/i2s_std.h"   // New I2S standard driver (requires ESP-IDF 5.x)
#include "driver/gpio.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Debug logging control - set to false for production to reduce serial overhead
static constexpr bool AUDIO_DEBUG_LOGS = false;
#define AUDIO_LOGF(...) do { if (AUDIO_DEBUG_LOGS) Serial.printf(__VA_ARGS__); } while(0)
#define AUDIO_LOGLN(msg) do { if (AUDIO_DEBUG_LOGS) Serial.println(msg); } while(0)

// ES8311 I2C address
#define ES8311_ADDR 0x18

// TCA9554 I2C address (same chip as battery manager, address 0x20)
#define TCA9554_ADDR 0x20
#define TCA9554_SPK_AMP_PIN 7

// I2S pins (from Waveshare board_cfg.txt for S3_LCD_3_49)
#define I2S_MCLK_PIN GPIO_NUM_7
#define I2S_BCLK_PIN GPIO_NUM_15
#define I2S_WS_PIN   GPIO_NUM_46
#define I2S_DOUT_PIN GPIO_NUM_45   // Data OUT for playback (not DIN=6 which is for recording)

// Audio parameters
#define SAMPLE_RATE 22050  // Match Waveshare BSP default (22.05kHz)

// Use battery manager's TwoWire instance (already initialized on SDA=47, SCL=48)
// ES8311 codec and TCA9554 IO expander are both on this bus
static TwoWire& audioWire = tca9554Wire;
static bool es8311_initialized = false;
static bool i2s_initialized = false;
static i2s_chan_handle_t i2s_tx_chan = NULL;  // New I2S driver handle

// Current volume level (0-100%) - must be declared before es8311_init() uses it
static uint8_t current_volume_percent = 75;

// Write a register to ES8311
static void es8311_write_reg(uint8_t reg, uint8_t val) {
    audioWire.beginTransmission(ES8311_ADDR);
    audioWire.write(reg);
    audioWire.write(val);
    uint8_t result = audioWire.endTransmission();
    if (result != 0) {
        AUDIO_LOGF("[AUDIO][I2C] ES8311 reg 0x%02X <= 0x%02X FAILED: %d\n", reg, val, result);
    }
}

// Enable/disable speaker amp via TCA9554 pin 7
// Note: Battery manager uses pin 6 for power latch, we use pin 7 for speaker amp
// Per ESP-ADF and Waveshare examples, PA_EN is active-HIGH
static void set_speaker_amp(bool enable) {
    // Step 1: Read current config register
    audioWire.beginTransmission(TCA9554_ADDR);
    audioWire.write(0x03); // Configuration register
    audioWire.endTransmission(false);
    audioWire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1);
    uint8_t config = 0xFF;
    if (audioWire.available()) {
        config = audioWire.read();
    }
    
    // Step 2: Read current output state
    audioWire.beginTransmission(TCA9554_ADDR);
    audioWire.write(0x01); // Output port register
    audioWire.endTransmission(false);
    audioWire.requestFrom((uint8_t)TCA9554_ADDR, (uint8_t)1);
    uint8_t output = 0xFF;
    if (audioWire.available()) {
        output = audioWire.read();
    }
    
    AUDIO_LOGF("[AUDIO] TCA9554 BEFORE: config=0x%02X output=0x%02X\n", config, output);
    
    // Step 3: Set the output value FIRST (before configuring as output)
    // Active HIGH per Waveshare esp_io_expander example: set_level(pin, 1) to enable
    if (enable) {
        output |= (1 << TCA9554_SPK_AMP_PIN);   // HIGH to enable
    } else {
        output &= ~(1 << TCA9554_SPK_AMP_PIN);  // LOW to disable
    }
    audioWire.beginTransmission(TCA9554_ADDR);
    audioWire.write(0x01); // Output port register
    audioWire.write(output);
    audioWire.endTransmission();
    
    // Step 4: Configure pin 7 as output (if not already)
    config &= ~(1 << TCA9554_SPK_AMP_PIN); // Bit = 0 means output
    audioWire.beginTransmission(TCA9554_ADDR);
    audioWire.write(0x03); // Configuration register
    audioWire.write(config);
    audioWire.endTransmission();
    
    AUDIO_LOGF("[AUDIO] Speaker amp %s\n", enable ? "ENABLED" : "DISABLED");
}

// ES8311 Register definitions (from ESP-ADF)
#define ES8311_RESET_REG00        0x00
#define ES8311_CLK_MANAGER_REG01  0x01
#define ES8311_CLK_MANAGER_REG02  0x02
#define ES8311_CLK_MANAGER_REG03  0x03
#define ES8311_CLK_MANAGER_REG04  0x04
#define ES8311_CLK_MANAGER_REG05  0x05
#define ES8311_CLK_MANAGER_REG06  0x06
#define ES8311_CLK_MANAGER_REG07  0x07
#define ES8311_CLK_MANAGER_REG08  0x08
#define ES8311_SDPIN_REG09        0x09
#define ES8311_SDPOUT_REG0A       0x0A
#define ES8311_SYSTEM_REG0B       0x0B
#define ES8311_SYSTEM_REG0C       0x0C
#define ES8311_SYSTEM_REG0D       0x0D
#define ES8311_SYSTEM_REG0E       0x0E
#define ES8311_SYSTEM_REG0F       0x0F
#define ES8311_SYSTEM_REG10       0x10
#define ES8311_SYSTEM_REG11       0x11
#define ES8311_SYSTEM_REG12       0x12
#define ES8311_SYSTEM_REG13       0x13
#define ES8311_SYSTEM_REG14       0x14
#define ES8311_ADC_REG15          0x15
#define ES8311_ADC_REG16          0x16
#define ES8311_ADC_REG17          0x17
#define ES8311_ADC_REG1B          0x1B
#define ES8311_ADC_REG1C          0x1C
#define ES8311_DAC_REG31          0x31
#define ES8311_DAC_REG32          0x32
#define ES8311_DAC_REG37          0x37
#define ES8311_GPIO_REG44         0x44
#define ES8311_GP_REG45           0x45

// Read a register from ES8311
static uint8_t es8311_read_reg(uint8_t reg) {
    audioWire.beginTransmission(ES8311_ADDR);
    audioWire.write(reg);
    audioWire.endTransmission(false);
    audioWire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1);
    if (audioWire.available()) {
        return audioWire.read();
    }
    return 0;
}

// Full ES8311 initialization - exact copy of ESP-ADF es8311_codec_init
// For 24kHz, MCLK=6.144MHz (256*fs), slave mode, DAC output
static void es8311_init() {
    if (es8311_initialized) return;
    
    AUDIO_LOGLN("[AUDIO] ES8311 init (ESP-ADF pattern)");
    
    // Coefficient for 24kHz with 6.144MHz MCLK from coeff_div table:
    // {6144000 , 24000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10}
    // pre_div=1, pre_multi=1, adc_div=1, dac_div=1, fs_mode=0, lrck_h=0, lrck_l=0xff, bclk_div=4, adc_osr=0x10, dac_osr=0x10
    
    // Step 1: Enhance I2C noise immunity (write twice per ESP-ADF)
    es8311_write_reg(ES8311_GPIO_REG44, 0x08);
    es8311_write_reg(ES8311_GPIO_REG44, 0x08);
    
    // Step 2: Initial clock setup
    es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0x30);  // Clock setup initial
    es8311_write_reg(ES8311_CLK_MANAGER_REG02, 0x00);  // Divider reset
    es8311_write_reg(ES8311_CLK_MANAGER_REG03, 0x10);  // ADC OSR
    es8311_write_reg(ES8311_ADC_REG16, 0x24);         // MIC gain
    es8311_write_reg(ES8311_CLK_MANAGER_REG04, 0x10);  // DAC OSR
    es8311_write_reg(ES8311_CLK_MANAGER_REG05, 0x00);  // ADC/DAC dividers
    es8311_write_reg(ES8311_SYSTEM_REG0B, 0x00);
    es8311_write_reg(ES8311_SYSTEM_REG0C, 0x00);
    es8311_write_reg(ES8311_SYSTEM_REG10, 0x1F);
    es8311_write_reg(ES8311_SYSTEM_REG11, 0x7F);
    
    // Step 3: Enable CSM (clock state machine) in slave mode
    es8311_write_reg(ES8311_RESET_REG00, 0x80);  // CSM_ON=1, slave mode (bit6=0)
    
    // Step 4: Enable all clocks, MCLK from external pin
    es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0x3F);  // bit7=0 (MCLK from pin), enable all clocks
    
    // Step 5: Configure clock dividers for 24kHz @ 6.144MHz MCLK
    // pre_div=1, pre_multi=1 => REG02 = ((1-1)<<5) | (0<<3) = 0x00
    es8311_write_reg(ES8311_CLK_MANAGER_REG02, 0x00);
    
    // adc_div=1, dac_div=1 => REG05 = ((1-1)<<4) | ((1-1)<<0) = 0x00
    es8311_write_reg(ES8311_CLK_MANAGER_REG05, 0x00);
    
    // fs_mode=0, adc_osr=0x10 => REG03 = (0<<6) | 0x10 = 0x10
    es8311_write_reg(ES8311_CLK_MANAGER_REG03, 0x10);
    
    // dac_osr=0x10 => REG04 = 0x10
    es8311_write_reg(ES8311_CLK_MANAGER_REG04, 0x10);
    
    // lrck_h=0x00, lrck_l=0xff => LRCK divider = 256
    es8311_write_reg(ES8311_CLK_MANAGER_REG07, 0x00);
    es8311_write_reg(ES8311_CLK_MANAGER_REG08, 0xFF);
    
    // bclk_div=4 => REG06 = (4-1)<<0 = 0x03
    es8311_write_reg(ES8311_CLK_MANAGER_REG06, 0x03);
    
    // Step 6: Additional setup from ESP-ADF
    es8311_write_reg(ES8311_SYSTEM_REG13, 0x10);
    es8311_write_reg(ES8311_ADC_REG1B, 0x0A);
    es8311_write_reg(ES8311_ADC_REG1C, 0x6A);
    
    // Step 7: START the DAC (from es8311_start)
    // REG09: DAC input config - bit6=0 for DAC enabled
    uint8_t dac_iface = es8311_read_reg(ES8311_SDPIN_REG09) & 0xBF;  // Clear bit 6 to enable
    dac_iface |= 0x0C;  // 16-bit samples (bits 4:2 = 0b11)
    es8311_write_reg(ES8311_SDPIN_REG09, dac_iface);
    
    es8311_write_reg(ES8311_ADC_REG17, 0xBF);
    es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02);  // Power up DAC
    es8311_write_reg(ES8311_SYSTEM_REG12, 0x00);  // DAC output enable
    es8311_write_reg(ES8311_SYSTEM_REG14, 0x1A);  // Output routing (no DMIC)
    es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01);  // Power up analog
    es8311_write_reg(ES8311_ADC_REG15, 0x40);
    es8311_write_reg(ES8311_DAC_REG37, 0x08);
    es8311_write_reg(ES8311_GP_REG45, 0x00);
    
    // Step 8: Set internal reference signal
    es8311_write_reg(ES8311_GPIO_REG44, 0x58);
    
    // Step 9: Set DAC volume based on saved setting
    // Use same mapping as audio_set_volume(): 0%=mute, 1-100%=0x90-0xBF
    uint8_t volReg;
    if (current_volume_percent == 0) {
        volReg = 0x00;
    } else {
        volReg = 0x90 + ((current_volume_percent - 1) * (0xBF - 0x90)) / 99;
    }
    es8311_write_reg(ES8311_DAC_REG32, volReg);
    
    // Step 10: Unmute DAC (clear bits 6:5 of REG31)
    uint8_t regv = es8311_read_reg(ES8311_DAC_REG31) & 0x9F;
    es8311_write_reg(ES8311_DAC_REG31, regv);
    
    es8311_initialized = true;
    
    delay(50);  // Let clocks stabilize
    
    // Debug: Dump key registers (only when debug logging enabled)
    if (AUDIO_DEBUG_LOGS) {
        Serial.println("[AUDIO] ES8311 registers after init:");
        Serial.printf("  REG00: 0x%02X\n", es8311_read_reg(ES8311_RESET_REG00));
        Serial.printf("  REG01: 0x%02X\n", es8311_read_reg(ES8311_CLK_MANAGER_REG01));
        Serial.printf("  REG06: 0x%02X\n", es8311_read_reg(ES8311_CLK_MANAGER_REG06));
        Serial.printf("  REG09: 0x%02X\n", es8311_read_reg(ES8311_SDPIN_REG09));
        Serial.printf("  REG0D: 0x%02X\n", es8311_read_reg(ES8311_SYSTEM_REG0D));
        Serial.printf("  REG0E: 0x%02X\n", es8311_read_reg(ES8311_SYSTEM_REG0E));
        Serial.printf("  REG12: 0x%02X\n", es8311_read_reg(ES8311_SYSTEM_REG12));
        Serial.printf("  REG14: 0x%02X\n", es8311_read_reg(ES8311_SYSTEM_REG14));
        Serial.printf("  REG31: 0x%02X\n", es8311_read_reg(ES8311_DAC_REG31));
        Serial.printf("  REG32: 0x%02X\n", es8311_read_reg(ES8311_DAC_REG32));
        Serial.printf("  REG44: 0x%02X\n", es8311_read_reg(ES8311_GPIO_REG44));
    }
}

// Set audio volume (0-100%)
// ES8311 DAC register 0x32: 0x00 = -95.5dB (mute), 0xBF = 0dB, 0xFF = +32dB
// The lower range (-95dB to -30dB) is inaudible, so we remap:
//   0% = 0x00 (mute)
//   1-100% = 0x90-0xBF (usable range: ~-24dB to 0dB)
void audio_set_volume(uint8_t volumePercent) {
    if (volumePercent > 100) volumePercent = 100;
    current_volume_percent = volumePercent;
    
    uint8_t regVal;
    if (volumePercent == 0) {
        regVal = 0x00;  // Mute
    } else {
        // Map 1-100% to 0x90-0xBF (144-191, a 47-step usable range)
        // This gives audible volume across the full slider
        regVal = 0x90 + ((volumePercent - 1) * (0xBF - 0x90)) / 99;
    }
    
    // Apply volume if ES8311 is initialized
    if (es8311_initialized) {
        es8311_write_reg(ES8311_DAC_REG32, regVal);
        AUDIO_LOGF("[AUDIO] Volume set to %d%% (reg=0x%02X)\n", volumePercent, regVal);
    }
}

// I2S init for playback using NEW I2S STD driver (like Waveshare BSP)
static void i2s_init() {
    if (i2s_initialized) return;
    
    AUDIO_LOGLN("[AUDIO] Initializing I2S (new STD driver)...");

    
    // Step 1: Create I2S channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // Auto clear legacy data in DMA buffer
    
    esp_err_t err = i2s_new_channel(&chan_cfg, &i2s_tx_chan, NULL);  // TX only, no RX
    if (err != ESP_OK) {
        AUDIO_LOGF("[AUDIO] i2s_new_channel failed: %d\\n", err);
        return;
    }
    
    // Step 2: Configure I2S standard mode (Philips format, STEREO, 16-bit)
    // Note: ES8311 may expect stereo I2S even for mono output
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = GPIO_NUM_NC,  // Not using input
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    err = i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        AUDIO_LOGF("[AUDIO] i2s_channel_init_std_mode failed: %d\\n", err);
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = NULL;
        return;
    }
    
    // Step 3: Enable the channel
    err = i2s_channel_enable(i2s_tx_chan);
    if (err != ESP_OK) {
        AUDIO_LOGF("[AUDIO] i2s_channel_enable failed: %d\\n", err);
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = NULL;
        return;
    }
    
    i2s_initialized = true;
    AUDIO_LOGF("[AUDIO] I2S initialized: %dHz, MCLK=%d BCLK=%d WS=%d DOUT=%d\\n",
               SAMPLE_RATE, I2S_MCLK_PIN, I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN);
}

// Include pre-recorded TTS audio
#include "../include/warning_audio.h"
#include "../include/alert_audio.h"

// Track if audio is currently playing to prevent overlapping
static volatile bool audio_playing = false;

// Audio task parameters for non-blocking playback
struct AudioTaskParams {
    const int16_t* pcm_data;
    int num_samples;
    int duration_ms;
};
static TaskHandle_t audioTaskHandle = NULL;

// Background task for audio playback - runs on separate core to avoid blocking main loop
static void audio_playback_task(void* pvParameters) {
    AudioTaskParams* params = (AudioTaskParams*)pvParameters;
    
    if (i2s_tx_chan == NULL) {
        // CRITICAL: Start I2S FIRST so MCLK is running before ES8311 init
        i2s_init();
        vTaskDelay(pdMS_TO_TICKS(50));  // Let clocks stabilize
    }
    
    if (!i2s_initialized) {
        AUDIO_LOGLN("[AUDIO] ERROR: I2S init failed!");
        audio_playing = false;
        free(params);
        vTaskDelete(NULL);
        return;
    }
    
    es8311_init();
    vTaskDelay(pdMS_TO_TICKS(50));  // Let ES8311 lock to MCLK
    
    // Enable speaker amp - let it fully stabilize
    set_speaker_amp(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Convert mono PCM to stereo for I2S Philips format
    const int stereo_samples = params->num_samples * 2;
    int16_t* buf = (int16_t*)malloc(stereo_samples * sizeof(int16_t));
    if (!buf) {
        AUDIO_LOGLN("[AUDIO] ERROR: malloc failed!");
        set_speaker_amp(false);
        audio_playing = false;
        free(params);
        vTaskDelete(NULL);
        return;
    }
    
    // Copy mono to stereo (both channels)
    for (int i = 0; i < params->num_samples; ++i) {
        int16_t sample = pgm_read_word(&params->pcm_data[i]);
        buf[i * 2] = sample;       // Left channel
        buf[i * 2 + 1] = sample;   // Right channel
    }
    
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(i2s_tx_chan, buf, stereo_samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    
    if (err != ESP_OK) {
        AUDIO_LOGF("[AUDIO] i2s_channel_write failed: %d\\n", err);
    }
    
    // Wait for audio to finish playing through DMA
    vTaskDelay(pdMS_TO_TICKS(params->duration_ms + 100));
    
    free(buf);
    set_speaker_amp(false);
    audio_playing = false;
    free(params);
    
    audioTaskHandle = NULL;
    vTaskDelete(NULL);
}

// Helper to play any PCM audio (mono input, converts to stereo for I2S)
// Now non-blocking - starts a FreeRTOS task for playback
static void play_pcm_audio(const int16_t* pcm_data, int num_samples, int duration_ms) {
    if (audio_playing) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        return;
    }
    
    // Allocate params for the task (task will free it)
    AudioTaskParams* params = (AudioTaskParams*)malloc(sizeof(AudioTaskParams));
    if (!params) {
        AUDIO_LOGLN("[AUDIO] ERROR: param malloc failed!");
        return;
    }
    params->pcm_data = pcm_data;
    params->num_samples = num_samples;
    params->duration_ms = duration_ms;
    
    audio_playing = true;
    
    // Create task on core 1 (core 0 is for WiFi/BLE) with adequate stack
    BaseType_t result = xTaskCreatePinnedToCore(
        audio_playback_task,
        "audio_play",
        4096,           // Stack size
        params,
        1,              // Priority (low)
        &audioTaskHandle,
        1               // Core 1
    );
    
    if (result != pdPASS) {
        AUDIO_LOGLN("[AUDIO] ERROR: Failed to create audio task!");
        audio_playing = false;
        free(params);
    }
}

// Play "Warning Volume Zero" speech (non-blocking)
void play_vol0_beep() {
    AUDIO_LOGLN("[AUDIO] play_vol0_beep() called");
    
    if (audio_playing) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        return;
    }
    
    AUDIO_LOGF("[AUDIO] Playing 'Warning Volume Zero' (%dms)\\n", WARNING_VOLUME_ZERO_PCM_DURATION_MS);
    play_pcm_audio(warning_volume_zero_pcm, WARNING_VOLUME_ZERO_PCM_SAMPLES, WARNING_VOLUME_ZERO_PCM_DURATION_MS);
}

// Play voice alert for band/direction (non-blocking)
void play_alert_voice(AlertBand band, AlertDirection direction) {
    AUDIO_LOGF("[AUDIO] play_alert_voice() band=%d dir=%d\\n", (int)band, (int)direction);
    
    if (audio_playing) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        return;
    }
    
    const int16_t* pcm_data = nullptr;
    int num_samples = 0;
    int duration_ms = 0;
    const char* phrase = "";
    
    // Select the appropriate audio clip
    switch (band) {
        case AlertBand::LASER:
            switch (direction) {
                case AlertDirection::AHEAD:
                    pcm_data = alert_laser_ahead;
                    num_samples = ALERT_LASER_AHEAD_SAMPLES;
                    duration_ms = ALERT_LASER_AHEAD_DURATION_MS;
                    phrase = "Laser ahead";
                    break;
                case AlertDirection::BEHIND:
                    pcm_data = alert_laser_behind;
                    num_samples = ALERT_LASER_BEHIND_SAMPLES;
                    duration_ms = ALERT_LASER_BEHIND_DURATION_MS;
                    phrase = "Laser behind";
                    break;
                case AlertDirection::SIDE:
                    pcm_data = alert_laser_side;
                    num_samples = ALERT_LASER_SIDE_SAMPLES;
                    duration_ms = ALERT_LASER_SIDE_DURATION_MS;
                    phrase = "Laser side";
                    break;
            }
            break;
        case AlertBand::KA:
            switch (direction) {
                case AlertDirection::AHEAD:
                    pcm_data = alert_ka_ahead;
                    num_samples = ALERT_KA_AHEAD_SAMPLES;
                    duration_ms = ALERT_KA_AHEAD_DURATION_MS;
                    phrase = "Ka ahead";
                    break;
                case AlertDirection::BEHIND:
                    pcm_data = alert_ka_behind;
                    num_samples = ALERT_KA_BEHIND_SAMPLES;
                    duration_ms = ALERT_KA_BEHIND_DURATION_MS;
                    phrase = "Ka behind";
                    break;
                case AlertDirection::SIDE:
                    pcm_data = alert_ka_side;
                    num_samples = ALERT_KA_SIDE_SAMPLES;
                    duration_ms = ALERT_KA_SIDE_DURATION_MS;
                    phrase = "Ka side";
                    break;
            }
            break;
        case AlertBand::K:
            switch (direction) {
                case AlertDirection::AHEAD:
                    pcm_data = alert_k_ahead;
                    num_samples = ALERT_K_AHEAD_SAMPLES;
                    duration_ms = ALERT_K_AHEAD_DURATION_MS;
                    phrase = "K ahead";
                    break;
                case AlertDirection::BEHIND:
                    pcm_data = alert_k_behind;
                    num_samples = ALERT_K_BEHIND_SAMPLES;
                    duration_ms = ALERT_K_BEHIND_DURATION_MS;
                    phrase = "K behind";
                    break;
                case AlertDirection::SIDE:
                    pcm_data = alert_k_side;
                    num_samples = ALERT_K_SIDE_SAMPLES;
                    duration_ms = ALERT_K_SIDE_DURATION_MS;
                    phrase = "K side";
                    break;
            }
            break;
        case AlertBand::X:
            switch (direction) {
                case AlertDirection::AHEAD:
                    pcm_data = alert_x_ahead;
                    num_samples = ALERT_X_AHEAD_SAMPLES;
                    duration_ms = ALERT_X_AHEAD_DURATION_MS;
                    phrase = "X ahead";
                    break;
                case AlertDirection::BEHIND:
                    pcm_data = alert_x_behind;
                    num_samples = ALERT_X_BEHIND_SAMPLES;
                    duration_ms = ALERT_X_BEHIND_DURATION_MS;
                    phrase = "X behind";
                    break;
                case AlertDirection::SIDE:
                    pcm_data = alert_x_side;
                    num_samples = ALERT_X_SIDE_SAMPLES;
                    duration_ms = ALERT_X_SIDE_DURATION_MS;
                    phrase = "X side";
                    break;
            }
            break;
    }
    
    if (pcm_data && num_samples > 0) {
        AUDIO_LOGF("[AUDIO] Playing '%s' (%dms)\\n", phrase, duration_ms);
        play_pcm_audio(pcm_data, num_samples, duration_ms);
    }
}

// Test beep on startup (for debugging audio hardware)
void play_test_beep() {
    AUDIO_LOGLN("[AUDIO] === TEST SPEECH ON STARTUP ===");
    play_vol0_beep();
}

// Play test voice for volume adjustment (short "Ka ahead" clip)
void play_test_voice() {
    AUDIO_LOGLN("[AUDIO] play_test_voice() called");
    
    if (audio_playing) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        return;
    }
    
    // Use "Ka ahead" as test phrase - short and recognizable (~822ms)
    play_pcm_audio(alert_ka_ahead, ALERT_KA_AHEAD_SAMPLES, ALERT_KA_AHEAD_DURATION_MS);
}

// ============================================================================
// SD Card-based Frequency Voice Playback
// ============================================================================
#include "storage_manager.h"
#include <LittleFS.h>

static bool sd_audio_ready = false;
static const char* AUDIO_PATH = "/audio";
static fs::FS* audioFS = nullptr;  // Filesystem containing audio files

// Initialize filesystem audio system
// Audio files are stored in LittleFS (uploaded with firmware)
// This works regardless of whether SD card is the primary storage
void audio_init_sd() {
    // Always check LittleFS for audio files (they're uploaded with firmware)
    if (!LittleFS.begin(false)) {  // Don't format if not found
        Serial.println("[AUDIO] LittleFS not available");
        return;
    }
    
    if (LittleFS.exists(AUDIO_PATH)) {
        audioFS = &LittleFS;
        sd_audio_ready = true;
        Serial.println("[AUDIO] Frequency audio initialized (LittleFS)");
    } else {
        Serial.println("[AUDIO] Audio folder not found in LittleFS");
    }
}

// Mu-law decode table (8-bit compressed -> 16-bit linear PCM)
// This is the standard ITU-T G.711 mu-law expansion table
static const int16_t mulaw_decode_table[256] = {
    -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
    -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
    -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
    -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
     -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
     -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
     -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
     -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
     -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
     -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
      -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
      -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
      -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
      -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
      -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
       -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
     32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
     23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
     15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
     11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
      7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
      5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
      3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
      2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
      1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
      1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
       876,   844,   812,   780,   748,   716,   684,   652,
       620,   588,   556,   524,   492,   460,   428,   396,
       372,   356,   340,   324,   308,   292,   276,   260,
       244,   228,   212,   196,   180,   164,   148,   132,
       120,   112,   104,    96,    88,    80,    72,    64,
        56,    48,    40,    32,    24,    16,     8,     0
};

// Structure for SD audio playback task
struct SDAudioTaskParams {
    char filePaths[5][48];  // Up to 5 clips: band, ghz, digit, tens, direction
    int numClips;
};

// Background task for SD audio concatenation playback (mu-law compressed)
static void sd_audio_playback_task(void* pvParameters) {
    SDAudioTaskParams* params = (SDAudioTaskParams*)pvParameters;
    
    if (i2s_tx_chan == NULL) {
        i2s_init();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    if (!i2s_initialized) {
        AUDIO_LOGLN("[AUDIO] ERROR: I2S init failed!");
        audio_playing = false;
        free(params);
        vTaskDelete(NULL);
        return;
    }
    
    es8311_init();
    vTaskDelay(pdMS_TO_TICKS(50));
    
    set_speaker_amp(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Use audioFS (LittleFS) which contains the audio files
    if (!audioFS) {
        AUDIO_LOGLN("[AUDIO] ERROR: audioFS is null!");
        set_speaker_amp(false);
        audio_playing = false;
        free(params);
        vTaskDelete(NULL);
        return;
    }
    
    // Play each clip in sequence
    for (int i = 0; i < params->numClips; i++) {
        File audioFile = audioFS->open(params->filePaths[i], "r");
        if (!audioFile) {
            AUDIO_LOGF("[AUDIO] Failed to open: %s\n", params->filePaths[i]);
            continue;
        }
        
        // Allocate buffers for mu-law decode and stereo conversion
        const int CHUNK_BYTES = 2048;  // Mu-law bytes to read at once
        uint8_t* mulawChunk = (uint8_t*)malloc(CHUNK_BYTES);
        int16_t* stereoChunk = (int16_t*)malloc(CHUNK_BYTES * 2 * sizeof(int16_t));
        
        if (!mulawChunk || !stereoChunk) {
            AUDIO_LOGLN("[AUDIO] ERROR: chunk malloc failed!");
            if (mulawChunk) free(mulawChunk);
            if (stereoChunk) free(stereoChunk);
            audioFile.close();
            continue;
        }
        
        size_t bytesRead;
        while ((bytesRead = audioFile.read(mulawChunk, CHUNK_BYTES)) > 0) {
            // Decode mu-law to stereo PCM
            for (size_t j = 0; j < bytesRead; j++) {
                int16_t sample = mulaw_decode_table[mulawChunk[j]];
                stereoChunk[j * 2] = sample;       // Left
                stereoChunk[j * 2 + 1] = sample;   // Right
            }
            
            size_t bytes_written = 0;
            i2s_channel_write(i2s_tx_chan, stereoChunk, bytesRead * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
        }
        
        free(mulawChunk);
        free(stereoChunk);
        audioFile.close();
    }
    
    // Wait for DMA to finish
    vTaskDelay(pdMS_TO_TICKS(150));
    
    set_speaker_amp(false);
    audio_playing = false;
    free(params);
    audioTaskHandle = NULL;
    vTaskDelete(NULL);
}

// Get GHz value for band and frequency
static int getGHz(AlertBand band, uint16_t freqMHz) {
    switch (band) {
        case AlertBand::KA:
            // Ka band: 33.4-36.0 GHz - determine which integer GHz
            if (freqMHz < 34000) return 33;
            if (freqMHz < 35000) return 34;
            if (freqMHz < 36000) return 35;
            return 36;
        case AlertBand::K:
            return 24;  // K band is 24.x GHz
        case AlertBand::X:
            return 10;  // X band is 10.x GHz
        default:
            return 0;   // Laser has no frequency
    }
}

// Play frequency voice announcement from SD card
// Format: "K A 34 7 49 rear" for Ka band, 34.749 GHz, rear direction
void play_frequency_voice(AlertBand band, uint16_t freqMHz, AlertDirection direction) {
    AUDIO_LOGF("[AUDIO] play_frequency_voice() band=%d freq=%d dir=%d\n", (int)band, freqMHz, (int)direction);
    
    if (audio_playing) {
        AUDIO_LOGLN("[AUDIO] Already playing, skipping");
        return;
    }
    
    if (!sd_audio_ready) {
        AUDIO_LOGLN("[AUDIO] Frequency audio not ready, falling back to simple alert");
        play_alert_voice(band, direction);
        return;
    }
    
    // Laser doesn't have frequency - use simple alert
    if (band == AlertBand::LASER) {
        play_alert_voice(band, direction);
        return;
    }
    
    // Allocate params for the task
    SDAudioTaskParams* params = (SDAudioTaskParams*)malloc(sizeof(SDAudioTaskParams));
    if (!params) {
        AUDIO_LOGLN("[AUDIO] ERROR: param malloc failed!");
        return;
    }
    params->numClips = 0;
    
    // 1. Band clip
    const char* bandFile = nullptr;
    switch (band) {
        case AlertBand::KA: bandFile = "band_ka.mul"; break;
        case AlertBand::K:  bandFile = "band_k.mul"; break;
        case AlertBand::X:  bandFile = "band_x.mul"; break;
        default: break;
    }
    if (bandFile) {
        snprintf(params->filePaths[params->numClips++], 48, "%s/%s", AUDIO_PATH, bandFile);
    }
    
    // 2. GHz clip
    int ghz = getGHz(band, freqMHz);
    if (ghz > 0) {
        snprintf(params->filePaths[params->numClips++], 48, "%s/ghz_%d.mul", AUDIO_PATH, ghz);
    }
    
    // 3. Hundreds digit of MHz (first digit after decimal point)
    // freqMHz is like 34749 for 34.749 GHz, so MHz part is 749
    // We want the hundreds digit: 7
    int mhz = freqMHz % 1000;  // Get last 3 digits (MHz portion)
    int hundredsDigit = mhz / 100;  // 749 -> 7
    snprintf(params->filePaths[params->numClips++], 48, "%s/digit_%d.mul", AUDIO_PATH, hundredsDigit);
    
    // 4. Last two digits as natural number (tens file)
    int lastTwo = mhz % 100;  // 749 -> 49
    snprintf(params->filePaths[params->numClips++], 48, "%s/tens_%02d.mul", AUDIO_PATH, lastTwo);
    
    // 5. Direction clip
    const char* dirFile = nullptr;
    switch (direction) {
        case AlertDirection::AHEAD:  dirFile = "dir_front.mul"; break;
        case AlertDirection::BEHIND: dirFile = "dir_rear.mul"; break;
        case AlertDirection::SIDE:   dirFile = "dir_side.mul"; break;
    }
    if (dirFile) {
        snprintf(params->filePaths[params->numClips++], 48, "%s/%s", AUDIO_PATH, dirFile);
    }
    
    AUDIO_LOGF("[AUDIO] Playing %d clips for freq announcement\n", params->numClips);
    for (int i = 0; i < params->numClips; i++) {
        AUDIO_LOGF("[AUDIO]   %d: %s\n", i, params->filePaths[i]);
    }
    
    audio_playing = true;
    
    BaseType_t result = xTaskCreatePinnedToCore(
        sd_audio_playback_task,
        "sd_audio",
        8192,           // Larger stack for file I/O
        params,
        1,
        &audioTaskHandle,
        1
    );
    
    if (result != pdPASS) {
        AUDIO_LOGLN("[AUDIO] ERROR: Failed to create SD audio task!");
        audio_playing = false;
        free(params);
    }
}