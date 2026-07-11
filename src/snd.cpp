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
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_private/rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// #include "soc/ledc_struct.h" // Direct register timing experiments.
#include "app_settings.h"

extern "C" {
#include "tme/snd.h"
}

constexpr gpio_num_t AUDIO_GPIO = (gpio_num_t) 16;
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
// Use a higher RMT resolution and convert all Mac cycles through the actual
// channel clock. Event positions and frame duration therefore remain correct
// when the resolution changes.
constexpr int AUDIO_RMT_REQUESTED_RESOLUTION_HZ = 40000000;
constexpr int AUDIO_RMT_MEM_BLOCK_SYMBOLS = 512;
// One frame can be filled by the emulator while the stream encoder drains the
// other. The RMT driver already owns the separate DMA ping-pong staging area.
constexpr int AUDIO_RMT_PAYLOAD_BUFFERS = 3;
constexpr int AUDIO_RMT_STREAM_PREFILL_FRAMES = AUDIO_RMT_PAYLOAD_BUFFERS;
constexpr int AUDIO_RMT_MAX_SYMBOLS_PER_FRAME = 512;
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
constexpr TickType_t AUDIO_RMT_WRITE_WAIT_TICKS = pdMS_TO_TICKS(75);

static AudioBackend activeBackend = AUDIO_BACKEND_PWM;
static bool audioStarted = false;
static uint8_t audioRing[AUDIO_RINGBUF_SAMPLES];
static volatile uint32_t audioRingHead = 0;
static volatile uint32_t audioRingTail = 0;
static volatile bool audioSpaceSignaled = false;
static gptimer_handle_t audioTimer = nullptr;
static TaskHandle_t audioProducerTask = nullptr;
// static decltype(&LEDC.channel_group[LEDC_LOW_SPEED_MODE].channel[LEDC_CHANNEL_0]) audioLedcChannel = nullptr;

typedef struct {
    rmt_symbol_word_t symbols[AUDIO_RMT_MAX_SYMBOLS_PER_FRAME];
    size_t symbolCount;
    volatile bool busy;
} AudioRmtPayloadBuffer;

static uint8_t pcmFrameBuffer[AUDIO_FRAME_SAMPLES];
static int16_t pdmFrameBuffer[AUDIO_FRAME_SAMPLES];
static i2s_chan_handle_t pdmTxChannel = nullptr;
static rmt_channel_handle_t rmtTxChannel = nullptr;
static rmt_encoder_handle_t rmtStreamEncoder = nullptr;
static uint8_t rmtStreamToken = 0;
static AudioRmtPayloadBuffer rmtPayloadBuffers[AUDIO_RMT_PAYLOAD_BUFFERS];
static volatile uint8_t rmtPendingBufferIndexes[AUDIO_RMT_PAYLOAD_BUFFERS];
static volatile uint32_t rmtPendingHead = 0;
static volatile uint32_t rmtPendingTail = 0;
static int rmtStreamBufferIndex = -1;
static size_t rmtStreamSymbolOffset = 0;
static bool rmtStreamStarted = false;
static uint32_t rmtResolutionHz = AUDIO_RMT_REQUESTED_RESOLUTION_HZ;
static uint16_t maxEventsSeen = 0;
static size_t maxRmtSymbols = 0;
static uint32_t rmtQueueWaits = 0;

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
// RMT stream callbacks (declared before channel initialization)
// -----------------------------------------------------------------------------
static inline uint32_t IRAM_ATTR audioRmtAbsoluteCycleToTick(uint64_t frameCycle) {
    return (uint32_t)((frameCycle * rmtResolutionHz + (AUDIO_MAC_CPU_HZ / 2)) /
                      AUDIO_MAC_CPU_HZ);
}

static void IRAM_ATTR audioRmtSignalPayloadFree(uint8_t bufferIndex) {
    __atomic_store_n(&rmtPayloadBuffers[bufferIndex].busy, false, __ATOMIC_RELEASE);
    if (!audioProducerTask) {
        return;
    }
    if (xPortInIsrContext()) {
        BaseType_t taskWoken = pdFALSE;
        audioWakeProducerFromIsr(&taskWoken);
        if (taskWoken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    } else {
        audioWakeProducer();
    }
}

static size_t IRAM_ATTR audioRmtStreamEncode(const void *data,
                                             size_t dataSize,
                                             size_t symbolsWritten,
                                             size_t symbolsFree,
                                             rmt_symbol_word_t *symbols,
                                             bool *done,
                                             void *arg) {
    (void)data;
    (void)dataSize;
    (void)symbolsWritten;
    (void)arg;
    *done = false;

    size_t written = 0;
    while (written < symbolsFree) {
        if (rmtStreamBufferIndex < 0) {
            uint32_t tail = __atomic_load_n(&rmtPendingTail, __ATOMIC_RELAXED);
            uint32_t head = __atomic_load_n(&rmtPendingHead, __ATOMIC_ACQUIRE);
            if (tail == head) {
                // Keep the PCM midpoint (sample 128) while the producer is
                // late.  Driving the pin low for the whole gap creates a
                // large step from the active PWM waveform and can be heard
                // as a click when the stream resumes.
                rmt_symbol_word_t neutral = {};
                uint32_t lineTicks = audioRmtAbsoluteCycleToTick(AUDIO_MAC_LINE_CYCLES);
                neutral.duration0 = lineTicks >> 1;
                neutral.level0 = 0;
                neutral.duration1 = lineTicks - neutral.duration0;
                neutral.level1 = 1;
                while (written < symbolsFree) {
                    symbols[written++] = neutral;
                }
                break;
            }
            rmtStreamBufferIndex = rmtPendingBufferIndexes[tail % AUDIO_RMT_PAYLOAD_BUFFERS];
            rmtStreamSymbolOffset = 0;
            __atomic_store_n(&rmtPendingTail, tail + 1, __ATOMIC_RELEASE);
        }

        AudioRmtPayloadBuffer *payload = &rmtPayloadBuffers[rmtStreamBufferIndex];
        while (written < symbolsFree && rmtStreamSymbolOffset < payload->symbolCount) {
            symbols[written++] = payload->symbols[rmtStreamSymbolOffset++];
        }
        if (rmtStreamSymbolOffset == payload->symbolCount) {
            uint8_t completedIndex = (uint8_t)rmtStreamBufferIndex;
            rmtStreamBufferIndex = -1;
            rmtStreamSymbolOffset = 0;
            audioRmtSignalPayloadFree(completedIndex);
        }
    }
    return written;
}

// -----------------------------------------------------------------------------
// PWM backend
// -----------------------------------------------------------------------------

static inline void IRAM_ATTR audioSetDuty(uint8_t duty) {
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
    if (audioRingRead(&sample)) {
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

// Shared scanline gate reconstruction used by PWM and PDM.
static inline bool isFrameEnabled(uint16_t line,
                                  bool *gateEnabled,
                                  const SndGateEvent *events,
                                  uint16_t eventCount,
                                  uint16_t *eventIndex) {
    // PWM/PDM have one output value per scanline. Fold all edge events up to
    // this line into the same gate state that RMT applies at cycle precision.
    // The last edge on a line wins, preserving event order for rapid toggles.
    while (*eventIndex < eventCount && events[*eventIndex].line <= line) {
        *gateEnabled = events[*eventIndex].state == SND_GATE_ENABLE;
        ++(*eventIndex);
    }
    return *gateEnabled;
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
        int vol = isFrameEnabled(i,
                                 &gateEnabled,
                                 events,
                                 eventCount,
                                 &eventIndex) ? volume : -1;
        pcmFrameBuffer[i] = audioApplyVolumeUint8t(rawSamples[i], vol);
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
        int vol = isFrameEnabled(i,
                                 &gateEnabled,
                                 events,
                                 eventCount,
                                 &eventIndex) ? volume : -1;
        pdmFrameBuffer[i] = audioApplyVolumeInt16t(rawSamples[i], vol);
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
// RMT backend initialization, timing conversion, and edge-to-symbol encoding
// -----------------------------------------------------------------------------

static esp_err_t audioInitRmt(void) {
    rmt_tx_channel_config_t channelCfg = {};
    channelCfg.gpio_num = AUDIO_GPIO;
    channelCfg.clk_src = RMT_CLK_SRC_DEFAULT;
    channelCfg.resolution_hz = AUDIO_RMT_REQUESTED_RESOLUTION_HZ;
    channelCfg.mem_block_symbols = AUDIO_RMT_MEM_BLOCK_SYMBOLS;
    channelCfg.trans_queue_depth = 1;
    channelCfg.flags.with_dma = true;
    channelCfg.flags.init_level = 0;

    esp_err_t err = rmt_new_tx_channel(&channelCfg, &rmtTxChannel);
    if (err != ESP_OK) {
        return err;
    }

    rmt_simple_encoder_config_t encoderCfg = {};
    encoderCfg.callback = audioRmtStreamEncode;
    encoderCfg.min_chunk_size = 1;
    err = rmt_new_simple_encoder(&encoderCfg, &rmtStreamEncoder);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t realResolution = 0;
    err = rmt_get_channel_resolution(rmtTxChannel, &realResolution);
    if (err == ESP_OK && realResolution > 0) {
        rmtResolutionHz = realResolution;
    } else {
        rmtResolutionHz = AUDIO_RMT_REQUESTED_RESOLUTION_HZ;
    }
    return rmt_enable(rmtTxChannel);
}

static uint32_t audioRmtFrameCycleToTick(uint16_t line, uint16_t cycle) {
    if (line >= AUDIO_FRAME_SAMPLES) {
        line = AUDIO_FRAME_SAMPLES;
        cycle = 0;
    } else if (cycle > AUDIO_MAC_LINE_CYCLES) {
        cycle = AUDIO_MAC_LINE_CYCLES;
    }

    uint64_t frameCycle = (uint32_t)line * AUDIO_MAC_LINE_CYCLES + cycle;
    return audioRmtAbsoluteCycleToTick(frameCycle);
}

static bool audioRmtAppendRun(AudioRmtPayloadBuffer *payload, uint8_t level, uint32_t ticks) {
    if (ticks == 0) {
        return true;
    }
    while (ticks > 0) {
        uint16_t chunkTicks = ticks > 0x7FFF ? 0x7FFF : (uint16_t)ticks;
        if (payload->symbolCount > 0) {
            rmt_symbol_word_t *last = &payload->symbols[payload->symbolCount - 1];
            if (last->duration1 > 0) {
                if (last->level1 == level && last->duration1 + chunkTicks <= 0x7FFF) {
                    last->duration1 += chunkTicks;
                    ticks -= chunkTicks;
                    continue;
                }
            } else if (last->level0 == level && last->duration0 + chunkTicks <= 0x7FFF) {
                last->duration0 += chunkTicks;
                ticks -= chunkTicks;
                continue;
            } else {
                last->level1 = level;
                last->duration1 = chunkTicks;
                ticks -= chunkTicks;
                continue;
            }
        }

        if (payload->symbolCount >= AUDIO_RMT_MAX_SYMBOLS_PER_FRAME) {
            return false;
        }
        rmt_symbol_word_t *symbol = &payload->symbols[payload->symbolCount++];
        symbol->level0 = level;
        symbol->duration0 = chunkTicks;
        symbol->level1 = level;
        symbol->duration1 = 0;
        ticks -= chunkTicks;
    }
    return true;
}

static bool audioRmtEmitEnabledTicks(AudioRmtPayloadBuffer *payload,
                                     uint16_t line,
                                     uint8_t sample,
                                     uint16_t startCycle,
                                     uint16_t endCycle,
                                     uint32_t startTick,
                                     uint32_t endTick) {
    if (sample == 0xFF) {
        return endTick > startTick ? audioRmtAppendRun(payload, 1, endTick - startTick) : true;
    }

    // Mac sound samples are unsigned PCM: 128 is the neutral midpoint,
    // matching the 50% carrier duty used by PWM/PDM. Scale the 8-bit sample
    // to the complete 352-cycle scanline instead of using 255 as the period.
    uint16_t lowCycles = (uint16_t)(((uint32_t)(0xFF - sample) *
                                     AUDIO_MAC_LINE_CYCLES + 127) / 0xFF);
    if (endCycle <= lowCycles) {
        return endTick > startTick ? audioRmtAppendRun(payload, 0, endTick - startTick) : true;
    }
    if (startCycle >= lowCycles) {
        return endTick > startTick ? audioRmtAppendRun(payload, 1, endTick - startTick) : true;
    }

    uint32_t lowTick = audioRmtFrameCycleToTick(line, lowCycles);
    return (lowTick > startTick ? audioRmtAppendRun(payload, 0, lowTick - startTick) : true) &&
        (endTick > lowTick ? audioRmtAppendRun(payload, 1, endTick - lowTick) : true);
}

static bool audioRmtRenderLine(AudioRmtPayloadBuffer *payload,
                               const uint8_t *rawSamples,
                               int volume,
                               const SndGateEvent *events,
                               uint16_t eventCount,
                               uint16_t line,
                               uint16_t *eventIndex,
                               bool *gateEnabled,
                               uint32_t lineStartTick,
                               uint32_t lineEndTick) {
    uint16_t cycle = 0;
    uint32_t startTick = lineStartTick;
    while (cycle < AUDIO_MAC_LINE_CYCLES) {
        uint16_t nextCycle = AUDIO_MAC_LINE_CYCLES;
        if (*eventIndex < eventCount && events[*eventIndex].line == line) {
            nextCycle = events[*eventIndex].cycle;
            if (nextCycle > AUDIO_MAC_LINE_CYCLES) {
                nextCycle = AUDIO_MAC_LINE_CYCLES;
            }
            if (nextCycle < cycle) {
                nextCycle = cycle;
            }
        }

        uint32_t endTick = nextCycle == AUDIO_MAC_LINE_CYCLES
            ? lineEndTick
            : audioRmtFrameCycleToTick(line, nextCycle);
        bool ok = true;
        if (volume == 0 || !*gateEnabled) {
            ok = endTick > startTick ? audioRmtAppendRun(payload, 0, endTick - startTick) : true;
        } else {
            ok = audioRmtEmitEnabledTicks(payload,
                                          line,
                                          rawSamples[line],
                                          cycle,
                                          nextCycle,
                                          startTick,
                                          endTick);
        }
        if (!ok) {
            return false;
        }

        cycle = nextCycle;
        startTick = endTick;
        while (*eventIndex < eventCount &&
               events[*eventIndex].line == line &&
               events[*eventIndex].cycle <= cycle) {
            *gateEnabled = events[*eventIndex].state == SND_GATE_ENABLE;
            ++(*eventIndex);
        }
    }
    return true;
}

// RMT payload ownership and continuous stream submission.
static AudioRmtPayloadBuffer *audioRmtFindFreePayload(void) {
    for (uint8_t i = 0; i < AUDIO_RMT_PAYLOAD_BUFFERS; ++i) {
        if (!__atomic_load_n(&rmtPayloadBuffers[i].busy, __ATOMIC_ACQUIRE)) {
            bool expected = false;
            if (__atomic_compare_exchange_n(&rmtPayloadBuffers[i].busy,
                                            &expected,
                                            true,
                                            false,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                rmtPayloadBuffers[i].symbolCount = 0;
                return &rmtPayloadBuffers[i];
            }
        }
    }
    return nullptr;
}

static AudioRmtPayloadBuffer *audioRmtAcquirePayload(void) {
    AudioRmtPayloadBuffer *payload = audioRmtFindFreePayload();
    if (payload) {
        return payload;
    }

    ++rmtQueueWaits;
    if (ulTaskNotifyTake(pdTRUE, AUDIO_RMT_WRITE_WAIT_TICKS) == 0) {
        printf("AUDIO: RMT payload wait timeout, waits=%u\n", (unsigned)rmtQueueWaits);
        return nullptr;
    }
    return audioRmtFindFreePayload();
}

static void audioRmtReleasePayload(AudioRmtPayloadBuffer *payload) {
    if (!payload) {
        return;
    }
    __atomic_store_n(&payload->busy, false, __ATOMIC_RELEASE);
}

static bool audioRmtStartStream(void) {
    rmt_transmit_config_t transmitCfg = {};
    transmitCfg.loop_count = 0;
    transmitCfg.flags.eot_level = 0;
    transmitCfg.flags.queue_nonblocking = false;

    rmtStreamStarted = true;
    esp_err_t err = rmt_transmit(rmtTxChannel,
                                 rmtStreamEncoder,
                                 &rmtStreamToken,
                                 sizeof(rmtStreamToken),
                                 &transmitCfg);
    if (err != ESP_OK) {
        rmtStreamStarted = false;
        printf("AUDIO: RMT continuous stream start failed, err=%s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool audioRmtSubmitPayload(AudioRmtPayloadBuffer *payload) {
    if (!payload || payload->symbolCount == 0) {
        audioRmtReleasePayload(payload);
        return true;
    }

    // A zero duration is an RMT end marker. Complete a half-used final symbol
    // without changing its level or total duration before joining frames.
    rmt_symbol_word_t *last = &payload->symbols[payload->symbolCount - 1];
    if (last->duration1 == 0) {
        if (last->duration0 <= 1) {
            printf("AUDIO: RMT invalid final run duration=%u\n", (unsigned)last->duration0);
            audioRmtReleasePayload(payload);
            return false;
        }
        last->duration0 -= 1;
        last->level1 = last->level0;
        last->duration1 = 1;
    }

    uint32_t head = __atomic_load_n(&rmtPendingHead, __ATOMIC_RELAXED);
    uint8_t bufferIndex = (uint8_t)(payload - rmtPayloadBuffers);
    rmtPendingBufferIndexes[head % AUDIO_RMT_PAYLOAD_BUFFERS] = bufferIndex;
    __atomic_store_n(&rmtPendingHead, head + 1, __ATOMIC_RELEASE);

    uint32_t tail = __atomic_load_n(&rmtPendingTail, __ATOMIC_ACQUIRE);
    if (!rmtStreamStarted && (head + 1 - tail) >= AUDIO_RMT_STREAM_PREFILL_FRAMES) {
        if (!audioRmtStartStream()) {
            // The queued payloads remain owned by the RMT path. Do not reuse
            // their memory after a partially attempted stream start.
            return false;
        }
    }
    return true;
}

static bool audioWriteRmtFrame(const uint8_t *rawSamples,
                               int volume,
                               bool frameStartGateEnabled,
                               const SndGateEvent *events,
                               uint16_t eventCount) {
    bool gateEnabled = frameStartGateEnabled;
    uint16_t eventIndex = 0;

    if (eventCount > maxEventsSeen) {
        maxEventsSeen = eventCount;
        printf("AUDIO: RMT max gate events=%u\n", (unsigned)maxEventsSeen);
    }

    AudioRmtPayloadBuffer *payload = audioRmtAcquirePayload();
    if (!payload) {
        return false;
    }

    bool ok = true;
    // Advance rounded scanline boundaries without repeating a 64-bit
    // cycle-to-tick division for every line.  The half-denominator offset
    // preserves the same round(frameCycle * resolution / CPU_Hz) result.
    const uint64_t lineNumerator = (uint64_t)AUDIO_MAC_LINE_CYCLES * rmtResolutionHz;
    const uint32_t lineTickWhole = (uint32_t)(lineNumerator / AUDIO_MAC_CPU_HZ);
    const uint32_t lineTickRemainder = (uint32_t)(lineNumerator % AUDIO_MAC_CPU_HZ);
    uint32_t lineTick = 0;
    uint32_t lineRemainder = AUDIO_MAC_CPU_HZ / 2;
    for (uint16_t line = 0; line < AUDIO_FRAME_SAMPLES; ++line) {
        uint32_t lineStartTick = lineTick;
        lineTick += lineTickWhole;
        lineRemainder += lineTickRemainder;
        if (lineRemainder >= AUDIO_MAC_CPU_HZ) {
            lineRemainder -= AUDIO_MAC_CPU_HZ;
            ++lineTick;
        }

        ok = audioRmtRenderLine(payload,
                                rawSamples,
                                volume,
                                events,
                                eventCount,
                                line,
                                &eventIndex,
                                &gateEnabled,
                                lineStartTick,
                                lineTick);
        if (!ok) {
            audioRmtReleasePayload(payload);
            printf("AUDIO: RMT frame payload overflow at line %u, symbols=%u/%u\n",
                (unsigned)line,
                (unsigned)payload->symbolCount,
                (unsigned)AUDIO_RMT_MAX_SYMBOLS_PER_FRAME);
            return false;
        }
    }

    if (payload->symbolCount > maxRmtSymbols) {
        maxRmtSymbols = payload->symbolCount;
        printf("AUDIO: RMT max frame symbols=%u\n", (unsigned)maxRmtSymbols);
    }
    return audioRmtSubmitPayload(payload);
}

// -----------------------------------------------------------------------------
// Public backend initialization and frame dispatch
// -----------------------------------------------------------------------------

// Initialize the sound front end used by the emulator core.
void sndInit(void) {
    activeBackend = appAudioBackend();

    if (activeBackend == AUDIO_BACKEND_RMT) {
        esp_err_t err = audioInitRmt();
        if (err == ESP_OK) {
            uint32_t frameTicks = audioRmtFrameCycleToTick(AUDIO_FRAME_SAMPLES, 0);
            printf("AUDIO: backend RMT continuous DMA stream on GPIO%d, requested %d Hz, real %u Hz, %d ticks/line, %u ticks/frame, %d DMA-memory symbols, %d software frames, %d symbols/frame\n",
                AUDIO_GPIO,
                AUDIO_RMT_REQUESTED_RESOLUTION_HZ,
                (unsigned)rmtResolutionHz,
                (unsigned)audioRmtFrameCycleToTick(1, 0),
                (unsigned)frameTicks,
                AUDIO_RMT_MEM_BLOCK_SYMBOLS,
                AUDIO_RMT_PAYLOAD_BUFFERS,
                AUDIO_RMT_MAX_SYMBOLS_PER_FRAME);
            audioStarted = true;
            return;
        }
        printf("AUDIO: RMT init failed (%s), falling back to PWM\n", esp_err_to_name(err));
        activeBackend = AUDIO_BACKEND_PWM;
    }

    if (activeBackend == AUDIO_BACKEND_PDM) {
        esp_err_t err = audioInitPdm();
        if (err == ESP_OK) {
            printf("AUDIO: backend PDM frame-gate on GPIO%d, 16-bit PCM, %d Hz, PDM %d bits/sample\n",
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
    printf("AUDIO: backend PWM frame-gate on GPIO%d, 8-bit PCM, update %d.%03d Hz, carrier %d Hz\n",
        AUDIO_GPIO, updateRateMilliHz / 1000, updateRateMilliHz % 1000, AUDIO_PWM_RATE);
    audioStarted = true;
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
    if (!audioStarted || rawSamples == nullptr) {
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
        return audioWriteRmtFrame(rawSamples,
                                  volume,
                                  frameStartGateEnabled,
                                  events,
                                  eventCount);
    }
    return false;
}
