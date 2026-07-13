/*
 * Audio output for the Mac Plus emulator on ESP32-S3.
 *
 * The emulator submits one 370-sample frame at the VBL cadence while sound is
 * active. The boot setting selects LEDC PWM, I2S PDM TX DMA, RMT scanline
 * edge output.
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/i2s_common.h"
#include "driver/i2s_pdm.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// #include "soc/ledc_struct.h" // Direct register timing experiments.
#include "app_settings.h"
#include "snd_rmt.h"

extern "C" {
#include "tme/snd.h"
}

constexpr gpio_num_t AUDIO_GPIO = (gpio_num_t) 16;
// Macintosh Plus audio is tied to the video timing rather than being an
// independent audio-clock stream. The emulated sound buffer has one 8-bit
// value per scanline, and the 370 scanlines are submitted at the VBL cadence.
// The RMT backend reconstructs that hardware model: each value becomes the
// duty position within its 352-CPU-cycle line, while VIA PB7 transitions can
// gate the output at positions inside the same line.
constexpr int AUDIO_FRAME_SAMPLES = 370;
constexpr int AUDIO_RINGBUF_SAMPLES = AUDIO_FRAME_SAMPLES * 3 + 1;
constexpr int AUDIO_PDM_DMA_DESC = 3;
constexpr int AUDIO_PDM_BITS_PER_SAMPLE = 128;
constexpr int AUDIO_TIMER_HZ = 40000000;
// emu.c documents the Macintosh timing as 7,833,600 CPU cycles/s, with one
// scanline taking 352 CPU cycles and 370 scanlines per VBI. The sound buffer
// contains one sample per scanline, so its rate is the scanline rate rather
// than a nominal 22.05 kHz audio rate.
constexpr int AUDIO_MAC_CPU_HZ = 7833600;
constexpr int AUDIO_MAC_LINE_CYCLES = 352;
constexpr int AUDIO_SAMPLE_RATE =
    // round(AUDIO_MAC_CPU_HZ / AUDIO_MAC_LINE_CYCLES) = 22,255 Hz
    (AUDIO_MAC_CPU_HZ + (AUDIO_MAC_LINE_CYCLES / 2)) / AUDIO_MAC_LINE_CYCLES;
constexpr int AUDIO_SAMPLE_TICKS =
    // round(AUDIO_TIMER_HZ * AUDIO_MAC_LINE_CYCLES / AUDIO_MAC_CPU_HZ).
    // The integer timer period introduces only a sub-tick rate error.
    ((int64_t)AUDIO_TIMER_HZ * AUDIO_MAC_LINE_CYCLES + (AUDIO_MAC_CPU_HZ / 2)) / AUDIO_MAC_CPU_HZ;
constexpr int AUDIO_UPDATE_RATE = AUDIO_TIMER_HZ / AUDIO_SAMPLE_TICKS;
constexpr int AUDIO_PWM_RATE = AUDIO_UPDATE_RATE * 4;
constexpr ledc_timer_bit_t AUDIO_PWM_BITS = LEDC_TIMER_8_BIT;
constexpr TickType_t AUDIO_WRITE_WAIT_TICKS = pdMS_TO_TICKS(25);

static AudioBackend activeBackend = AUDIO_BACKEND_PWM;
static bool audioStarted = false;
static volatile bool audioEnabled = true;
static uint8_t audioRing[AUDIO_RINGBUF_SAMPLES];
static volatile uint32_t audioRingHead = 0;
static volatile uint32_t audioRingTail = 0;
static volatile bool audioSpaceSignaled = false;
static uint8_t pwmLastDuty = 0xFF;
static gptimer_handle_t audioTimer = nullptr;
static TaskHandle_t audioProducerTask = nullptr;
// static decltype(&LEDC.channel_group[LEDC_LOW_SPEED_MODE].channel[LEDC_CHANNEL_0]) audioLedcChannel = nullptr;
static uint8_t pcmFrameBuffer[AUDIO_FRAME_SAMPLES];
static int16_t pdmFrameBuffer[AUDIO_FRAME_SAMPLES];
static i2s_chan_handle_t pdmTxChannel = nullptr;

static const int32_t AUDIO_VOLUME_LUT[8] = {
    0,      /* vol 0: mute */
    1038,   /* vol 1: -36dB */
    2615,   /* vol 2: -28dB */
    6579,   /* vol 3: -20dB */
    13107,  /* vol 4: -14dB */
    26109,  /* vol 5: -8dB */
    46426,  /* vol 6: -3dB */
    65536   /* vol 7: 0dB */
};

// -----------------------------------------------------------------------------
// Common ring, gate, and producer-wakeup helpers
// -----------------------------------------------------------------------------

static inline uint32_t IRAM_ATTR audioRingCount(void) {
    const uint32_t head = audioRingHead;
    const uint32_t tail = audioRingTail;
    return head >= tail ? head - tail : AUDIO_RINGBUF_SAMPLES - (tail - head);
}

static inline uint32_t IRAM_ATTR audioRingFree(void) {
    return AUDIO_RINGBUF_SAMPLES - audioRingCount() - 1;
}

static inline void IRAM_ATTR audioWakeProducerFromIsr(BaseType_t *taskWoken) {
    TaskHandle_t task = audioProducerTask;
    if (task) {
        vTaskNotifyGiveFromISR(task, taskWoken);
    }
}

static inline void audioWakeProducer(void) {
    TaskHandle_t task = audioProducerTask;
    if (task) {
        xTaskNotifyGive(task);
    }
}

static inline bool IRAM_ATTR audioRingRead(uint8_t *sample) {
    uint32_t tail = audioRingTail;
    if (tail == audioRingHead) {
        return false;
    }
    *sample = audioRing[tail];
    ++tail;
    if (tail == AUDIO_RINGBUF_SAMPLES) {
        tail = 0;
    }
    audioRingTail = tail;
    return true;
}

static uint8_t audioApplyVolumeUint8t(uint8_t sample, int volume) {
    if (volume >= 7) {
        return sample;
    }
    if (volume == 0) {
        return 128;
    }
    if (volume == -1) {
        return 0;
    }
    int32_t scaled = ((((int32_t)sample - 128) * AUDIO_VOLUME_LUT[volume]) >> 16) + 128;
    if (scaled < 0) scaled = 0;
    if (scaled > 255) scaled = 255;
    return (uint8_t)scaled;
}

static int16_t audioApplyVolumeInt16t(uint8_t sample, int volume) {
    if (volume >= 7) {
        return (int16_t)(((int32_t)sample - 128) * 256);
    }
    if (volume == 0) {
        return 0;
    }
    if (volume == -1) {
        return -32768;
    }
    int32_t scaled = (((int32_t)sample - 128) * AUDIO_VOLUME_LUT[volume]) >> 8;   // -16 + 8
    if (scaled < -32768) scaled = -32768;
    if (scaled > 32767)  scaled = 32767;
    return (int16_t)scaled;
}

// -----------------------------------------------------------------------------
// PWM backend
// -----------------------------------------------------------------------------

static inline void IRAM_ATTR audioSetDuty(uint8_t duty) {
    if (duty == pwmLastDuty) {
        return;
    }
    pwmLastDuty = duty;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    // Direct register path kept for timing experiments.
    // audioLedcChannel->duty.val = ((uint32_t)duty) << 4;
    // audioLedcChannel->conf0.val |= 0x00000014;
    // audioLedcChannel->conf1.val |= 0x80000000;
}

static bool IRAM_ATTR audioTimerAlarm(gptimer_handle_t timer,
                                      const gptimer_alarm_event_data_t *edata,
                                      void *userCtx) {
    (void)timer;
    (void)edata;
    (void)userCtx;

    uint8_t sample;
    if (!audioEnabled) {
        audioSetDuty(0);
    } else if (audioRingRead(&sample)) {
        audioSetDuty(sample);
    }

    BaseType_t taskWoken = pdFALSE;
    if (!audioSpaceSignaled && audioRingFree() >= AUDIO_FRAME_SAMPLES) {
        audioSpaceSignaled = true;
        audioWakeProducerFromIsr(&taskWoken);
    }
    return taskWoken == pdTRUE;
}

static esp_err_t audioInitPwm(void) {
    ledc_timer_config_t timerCfg = {};
    timerCfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timerCfg.duty_resolution = AUDIO_PWM_BITS;
    timerCfg.timer_num = LEDC_TIMER_0;
    timerCfg.freq_hz = AUDIO_PWM_RATE;
    timerCfg.clk_cfg = LEDC_USE_APB_CLK;
    esp_err_t err = ledc_timer_config(&timerCfg);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channelCfg = {};
    channelCfg.gpio_num = AUDIO_GPIO;
    channelCfg.speed_mode = LEDC_LOW_SPEED_MODE;
    channelCfg.channel = LEDC_CHANNEL_0;
    channelCfg.timer_sel = LEDC_TIMER_0;
    channelCfg.duty = 128;
    channelCfg.hpoint = 0;
    err = ledc_channel_config(&channelCfg);
    if (err != ESP_OK) {
        return err;
    }

    // audioLedcChannel = &LEDC.channel_group[LEDC_LOW_SPEED_MODE].channel[LEDC_CHANNEL_0];
    return ESP_OK;
}

static esp_err_t audioInitTimer(void) {
    gptimer_config_t timerConfig = {};
    timerConfig.clk_src = GPTIMER_CLK_SRC_APB;
    timerConfig.direction = GPTIMER_COUNT_UP;
    timerConfig.resolution_hz = AUDIO_TIMER_HZ;

    esp_err_t err = gptimer_new_timer(&timerConfig, &audioTimer);
    if (err != ESP_OK) {
        return err;
    }

    gptimer_event_callbacks_t callbacks = {};
    callbacks.on_alarm = audioTimerAlarm;
    err = gptimer_register_event_callbacks(audioTimer, &callbacks, nullptr);
    if (err != ESP_OK) {
        return err;
    }

    gptimer_alarm_config_t alarmConfig = {};
    alarmConfig.alarm_count = AUDIO_SAMPLE_TICKS;
    alarmConfig.reload_count = 0;
    alarmConfig.flags.auto_reload_on_alarm = true;
    err = gptimer_set_alarm_action(audioTimer, &alarmConfig);
    if (err != ESP_OK) {
        return err;
    }

    err = gptimer_enable(audioTimer);
    if (err != ESP_OK) {
        return err;
    }
    return gptimer_start(audioTimer);
}

static void audioRingWriteFrame(const uint8_t *data) {
    uint32_t head = audioRingHead;
    uint32_t firstCount = AUDIO_RINGBUF_SAMPLES - head;
    if (firstCount > AUDIO_FRAME_SAMPLES) {
        firstCount = AUDIO_FRAME_SAMPLES;
    }
    if (data) {
        memcpy(&audioRing[head], data, firstCount);
    } else {
        memset(&audioRing[head], 128, firstCount);
    }
    uint32_t remain = AUDIO_FRAME_SAMPLES - firstCount;
    if (remain > 0) {
        if (data) {
            memcpy(audioRing, data + firstCount, remain);
        } else {
            memset(audioRing, 128, remain);
        }
        head = remain;
    } else {
        head += firstCount;
        if (head == AUDIO_RINGBUF_SAMPLES) {
            head = 0;
        }
    }
    audioRingHead = head;
}

// Apply PB7 gating to one of the 370 PCM values. The raw value describes a
// high pulse inside this line: its high interval starts at lowCycles and ends
// at cycle 352. Only the part of that high interval covered by PB7 ON remains
// in the result; an OFF transition during the line therefore subtracts only
// the overlapping duty, not the whole PCM amplitude.
static uint8_t audioApplyGateToPcmSample(uint8_t sample,
                                         uint16_t line,
                                         bool *gateEnabled,
                                         const SndGateEvent *events,
                                         uint16_t eventCount,
                                         uint16_t *eventIndex) {
    const uint16_t boundedEventCount = eventCount > SND_GATE_MAX_EVENTS
        ? SND_GATE_MAX_EVENTS : eventCount;

    // Most lines have no PB7 transition. In that common case the gate state
    // is constant for the whole line, so avoid recomputing the PCM high run.
    while (*eventIndex < boundedEventCount && events[*eventIndex].line < line) {
        *gateEnabled = events[*eventIndex].state == SND_GATE_ENABLE;
        ++(*eventIndex);
    }
    if (*eventIndex >= boundedEventCount || events[*eventIndex].line > line) {
        return *gateEnabled ? sample : 0;
    }

    const uint16_t lowCycles = (uint16_t)(((uint32_t)(0xFF - sample) *
                                           AUDIO_MAC_LINE_CYCLES + 127) / 0xFF);
    uint16_t cycle = 0;
    uint16_t enabledHighCycles = 0;

    while (cycle < AUDIO_MAC_LINE_CYCLES) {
        uint16_t nextCycle = AUDIO_MAC_LINE_CYCLES;
        if (*eventIndex < boundedEventCount && events[*eventIndex].line == line) {
            nextCycle = events[*eventIndex].cycle > AUDIO_MAC_LINE_CYCLES
                ? AUDIO_MAC_LINE_CYCLES : events[*eventIndex].cycle;
            if (nextCycle < cycle) nextCycle = cycle;
        }
        if (*gateEnabled && nextCycle > lowCycles && nextCycle > cycle) {
            uint16_t highStart = cycle > lowCycles ? cycle : lowCycles;
            if (nextCycle > highStart) {
                enabledHighCycles += nextCycle - highStart;
            }
        }
        cycle = nextCycle;
        while (*eventIndex < boundedEventCount &&
               events[*eventIndex].line == line &&
               events[*eventIndex].cycle <= cycle) {
            *gateEnabled = events[*eventIndex].state == SND_GATE_ENABLE;
            ++(*eventIndex);
        }
    }

    return (uint8_t)((enabledHighCycles * 255U + AUDIO_MAC_LINE_CYCLES / 2) /
                     AUDIO_MAC_LINE_CYCLES);
}

// -----------------------------------------------------------------------------
// PWM frame conversion and ring-buffer submission
// -----------------------------------------------------------------------------
static bool audioWritePwmFrame(const uint8_t *rawSamples,
                               int volume,
                               bool frameStartGateEnabled,
                               const SndGateEvent *events,
                               uint16_t eventCount) {
    bool gateEnabled = frameStartGateEnabled;
    uint16_t eventIndex = 0;
    for (uint16_t i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
        uint8_t gatedSample = audioApplyGateToPcmSample(rawSamples[i],
                                                         i,
                                                         &gateEnabled,
                                                         events,
                                                         eventCount,
                                                         &eventIndex);
        pcmFrameBuffer[i] = audioApplyVolumeUint8t(gatedSample, volume);
    }

    audioSpaceSignaled = false;
    TickType_t startTicks = xTaskGetTickCount();

    while (audioRingFree() < AUDIO_FRAME_SAMPLES) {
        if (ulTaskNotifyTake(pdTRUE, AUDIO_WRITE_WAIT_TICKS) == 0) {
            return false;
        }
        if ((xTaskGetTickCount() - startTicks) >= AUDIO_WRITE_WAIT_TICKS) {
            return false;
        }
        audioSpaceSignaled = false;
    }

    audioRingWriteFrame(pcmFrameBuffer);
    return true;
}

// -----------------------------------------------------------------------------
// PDM frame conversion and I2S DMA submission
// -----------------------------------------------------------------------------
static esp_err_t audioInitPdm(void) {
    i2s_chan_config_t channelCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channelCfg.dma_desc_num = AUDIO_PDM_DMA_DESC;
    channelCfg.dma_frame_num = AUDIO_FRAME_SAMPLES;
    channelCfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&channelCfg, &pdmTxChannel, nullptr);
    if (err != ESP_OK) {
        return err;
    }

    i2s_pdm_tx_clk_config_t clkCfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
    i2s_pdm_tx_slot_config_t slotCfg =
        I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    i2s_pdm_tx_gpio_config_t gpioCfg = {};
    gpioCfg.clk = I2S_GPIO_UNUSED;
    gpioCfg.dout = (gpio_num_t)AUDIO_GPIO;
#if SOC_I2S_PDM_MAX_TX_LINES > 1
    gpioCfg.dout2 = I2S_GPIO_UNUSED;
#endif

    i2s_pdm_tx_config_t pdmCfg = {};
    pdmCfg.clk_cfg = clkCfg;
    pdmCfg.slot_cfg = slotCfg;
    pdmCfg.gpio_cfg = gpioCfg;

    err = i2s_channel_init_pdm_tx_mode(pdmTxChannel, &pdmCfg);
    if (err != ESP_OK) {
        i2s_del_channel(pdmTxChannel);
        pdmTxChannel = nullptr;
        return err;
    }
    err = i2s_channel_enable(pdmTxChannel);
    if (err != ESP_OK) {
        i2s_del_channel(pdmTxChannel);
        pdmTxChannel = nullptr;
    }
    return err;
}

static bool audioWritePdmFrame(const uint8_t *rawSamples,
                               int volume,
                               bool frameStartGateEnabled,
                               const SndGateEvent *events,
                               uint16_t eventCount) {
    bool gateEnabled = frameStartGateEnabled;
    uint16_t eventIndex = 0;
    for (uint16_t i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
        uint8_t gatedSample = audioApplyGateToPcmSample(rawSamples[i],
                                                         i,
                                                         &gateEnabled,
                                                         events,
                                                         eventCount,
                                                         &eventIndex);
        pdmFrameBuffer[i] = audioApplyVolumeInt16t(gatedSample, volume);
    }

    size_t bytesWritten = 0;
    esp_err_t err = i2s_channel_write(pdmTxChannel,
                                      pdmFrameBuffer,
                                      sizeof(pdmFrameBuffer),
                                      &bytesWritten,
                                      AUDIO_WRITE_WAIT_TICKS);
    if (err != ESP_OK || bytesWritten != sizeof(pdmFrameBuffer)) {
        printf("AUDIO: PDM DMA write failed, err=%s, %u/%u bytes\n",
            esp_err_to_name(err),
            (unsigned)bytesWritten,
            (unsigned)sizeof(pdmFrameBuffer));
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Public backend initialization and frame dispatch
// -----------------------------------------------------------------------------

// Initialize the sound front end used by the emulator core.
void sndInit(void) {
    activeBackend = appAudioBackend();

    if (activeBackend == AUDIO_BACKEND_RMT) {
        esp_err_t err = audioRmtInit();
        if (err == ESP_OK) {
            audioStarted = true;
            return;
        }
        printf("AUDIO: RMT init failed (%s), falling back to PWM\n", esp_err_to_name(err));
        activeBackend = AUDIO_BACKEND_PWM;
    }

    if (activeBackend == AUDIO_BACKEND_PDM) {
        esp_err_t err = audioInitPdm();
        if (err == ESP_OK) {
            printf("AUDIO: backend PDM per-scanline gate on GPIO%d, 16-bit PCM, %d Hz, PDM %d bits/sample\n",
                AUDIO_GPIO, AUDIO_SAMPLE_RATE, AUDIO_PDM_BITS_PER_SAMPLE);
            audioStarted = true;
            return;
        }
        printf("AUDIO: PDM init failed (%s), falling back to PWM\n", esp_err_to_name(err));
        activeBackend = AUDIO_BACKEND_PWM;
    }

    if (audioInitPwm() != ESP_OK) {
        printf("AUDIO: PWM init failed\n");
        return;
    }
    if (audioInitTimer() != ESP_OK) {
        printf("AUDIO: timer init failed\n");
        return;
    }

    const int updateRateMilliHz = ((int64_t)AUDIO_TIMER_HZ * 1000) / AUDIO_SAMPLE_TICKS;
    printf("AUDIO: backend PWM per-scanline gate on GPIO%d, 8-bit PCM, update %d.%03d Hz, carrier %d Hz\n",
        AUDIO_GPIO, updateRateMilliHz / 1000, updateRateMilliHz % 1000, AUDIO_PWM_RATE);
    audioStarted = true;
}

void sndSetEnabled(bool enabled) {
    if (audioEnabled == enabled) {
        return;
    }
    audioEnabled = enabled;
    if (!enabled) {
        audioRingTail = audioRingHead;
        audioSpaceSignaled = false;
        if (activeBackend == AUDIO_BACKEND_RMT) {
            audioRmtSetEnabled(false);
        }
        if (activeBackend == AUDIO_BACKEND_PWM) {
            audioSetDuty(0);
        }
        if (activeBackend == AUDIO_BACKEND_PDM && pdmTxChannel) {
            i2s_channel_disable(pdmTxChannel);
        }
    } else if (activeBackend == AUDIO_BACKEND_PDM && pdmTxChannel) {
        i2s_channel_enable(pdmTxChannel);
    } else if (activeBackend == AUDIO_BACKEND_RMT) {
        audioRmtSetEnabled(true);
    }
}

static bool isConstantFrame(const uint8_t *data) {
    uint8_t first = data[0];
    for (uint16_t i = 1; i < AUDIO_FRAME_SAMPLES; ++i) {
        if (data[i] != first) {
            return false;
        }
    }
    return true;
}

bool sndPushMacFrame(const uint8_t *rawSamples,
                     int volume,
                     bool frameStartGateEnabled,
                     const SndGateEvent *events,
                     uint16_t eventCount) {
    if (!audioStarted || !audioEnabled || rawSamples == nullptr) {
        return false;
    }

    if (!audioProducerTask) {
        audioProducerTask = xTaskGetCurrentTaskHandle();
    }

    if (volume < 0) volume = 0;
    if (volume > 7) volume = 7;

    if (!events) {
        eventCount = 0;
    }

    if (appTurboEnabled() &&
        (volume == 0 || (!frameStartGateEnabled && eventCount == 0) ||
         (eventCount == 0 && isConstantFrame(rawSamples)))) {
        return false;
    }

    if (activeBackend == AUDIO_BACKEND_PDM) {
        return audioWritePdmFrame(rawSamples,
                                  volume,
                                  frameStartGateEnabled,
                                  events,
                                  eventCount);
    }
    if (activeBackend == AUDIO_BACKEND_PWM) {
        return audioWritePwmFrame(rawSamples,
                                  volume,
                                  frameStartGateEnabled,
                                  events,
                                  eventCount);
    }
    if (activeBackend == AUDIO_BACKEND_RMT) {
        return audioRmtWriteFrame(rawSamples,
                                     volume,
                                     frameStartGateEnabled,
                                     events,
                                     eventCount);
    }
    return false;
}
