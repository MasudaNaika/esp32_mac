#include "snd_rmt.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_private/rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr gpio_num_t AUDIO_GPIO = GPIO_NUM_16;
constexpr int AUDIO_FRAME_SAMPLES = 370;
constexpr int AUDIO_MAC_CPU_HZ = 7833600;
constexpr int AUDIO_MAC_LINE_CYCLES = 352;
constexpr int AUDIO_RMT_REQUESTED_RESOLUTION_HZ = 40000000;
constexpr int AUDIO_RMT_MEM_BLOCK_SYMBOLS = 512;
constexpr int AUDIO_RMT_DMA_CHUNK_SYMBOLS = AUDIO_RMT_MEM_BLOCK_SYMBOLS / 2;
constexpr int AUDIO_RMT_RING_CHUNKS = 5;
constexpr int AUDIO_RMT_DMA_NODES = 2;
constexpr TickType_t AUDIO_RMT_WRITE_WAIT_TICKS = pdMS_TO_TICKS(75);

typedef enum {
    AUDIO_RMT_CHUNK_FREE = 0,
    AUDIO_RMT_CHUNK_FILLING,
    AUDIO_RMT_CHUNK_READY,
    AUDIO_RMT_CHUNK_ACTIVE,
} AudioRmtChunkState;

typedef struct {
    rmt_symbol_word_t symbols[AUDIO_RMT_DMA_CHUNK_SYMBOLS];
    volatile uint8_t state;
} AudioRmtChunk;

typedef struct {
    AudioRmtChunk *chunk;
    rmt_symbol_word_t *symbols;
    uint16_t symbolCount;
    rmt_symbol_word_t pendingSymbol;
    bool hasPendingSymbol;
} AudioRmtStreamWriter;

static rmt_channel_handle_t rmtTxChannel = nullptr;
static rmt_encoder_handle_t rmtStreamEncoder = nullptr;
static uint8_t rmtStreamToken = 0;
static DRAM_ATTR AudioRmtChunk rmtChunks[AUDIO_RMT_RING_CHUNKS];
static DRAM_ATTR volatile uint8_t rmtPendingChunkIndexes[AUDIO_RMT_RING_CHUNKS];
static volatile uint32_t rmtPendingHead = 0;
static volatile uint32_t rmtPendingTail = 0;
static bool rmtStreamStarted = false;
static int rmtStreamChunkIndex = -1;
static uint16_t rmtStreamSymbolOffset = 0;
static uint8_t rmtStreamLastLevel = 0;
static uint32_t rmtResolutionHz = AUDIO_RMT_REQUESTED_RESOLUTION_HZ;
static uint32_t rmtQueueWaits = 0;
static uint32_t rmtUnderflows = 0;
static TaskHandle_t rmtProducerTask = nullptr;
static AudioRmtStreamWriter rmtProducerWriter = {};

static inline uint32_t IRAM_ATTR audioRmtAbsoluteCycleToTick(uint64_t frameCycle) {
    return (uint32_t)((frameCycle * rmtResolutionHz + (AUDIO_MAC_CPU_HZ / 2)) /
                      AUDIO_MAC_CPU_HZ);
}

static void IRAM_ATTR audioRmtSignalChunkFree(uint8_t chunkIndex) {
    __atomic_store_n(&rmtChunks[chunkIndex].state,
                     AUDIO_RMT_CHUNK_FREE,
                     __ATOMIC_RELEASE);
    TaskHandle_t task = rmtProducerTask;
    if (!task) return;
    if (xPortInIsrContext()) {
        BaseType_t taskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &taskWoken);
        if (taskWoken == pdTRUE) portYIELD_FROM_ISR();
    } else {
        xTaskNotifyGive(task);
    }
}

static bool IRAM_ATTR audioRmtSelectNextChunk(void) {
    uint32_t tail = __atomic_load_n(&rmtPendingTail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&rmtPendingHead, __ATOMIC_ACQUIRE);
    if (tail == head) return false;
    uint8_t index = rmtPendingChunkIndexes[tail % AUDIO_RMT_RING_CHUNKS];
    __atomic_store_n(&rmtPendingTail, tail + 1, __ATOMIC_RELEASE);
    rmtStreamChunkIndex = index;
    rmtStreamSymbolOffset = 0;
    __atomic_store_n(&rmtChunks[index].state,
                     AUDIO_RMT_CHUNK_ACTIVE,
                     __ATOMIC_RELEASE);
    return true;
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
        if (rmtStreamChunkIndex < 0 && !audioRmtSelectNextChunk()) {
            ++rmtUnderflows;
            uint32_t lineTicks = audioRmtAbsoluteCycleToTick(AUDIO_MAC_LINE_CYCLES);
            rmt_symbol_word_t hold = {};
            uint32_t firstTicks = lineTicks / 2;
            hold.level0 = rmtStreamLastLevel;
            hold.duration0 = firstTicks > 0x7FFF ? 0x7FFF : firstTicks;
            hold.level1 = rmtStreamLastLevel;
            hold.duration1 = lineTicks - firstTicks > 0x7FFF ? 0x7FFF : lineTicks - firstTicks;
            while (written < symbolsFree) symbols[written++] = hold;
            break;
        }
        AudioRmtChunk *chunk = &rmtChunks[rmtStreamChunkIndex];
        if (rmtStreamSymbolOffset == 0 &&
            symbolsFree - written >= AUDIO_RMT_DMA_CHUNK_SYMBOLS) {
            memcpy(&symbols[written], chunk->symbols,
                   AUDIO_RMT_DMA_CHUNK_SYMBOLS * sizeof(rmt_symbol_word_t));
            rmtStreamLastLevel = chunk->symbols[AUDIO_RMT_DMA_CHUNK_SYMBOLS - 1].level1;
            written += AUDIO_RMT_DMA_CHUNK_SYMBOLS;
            uint8_t completed = (uint8_t)rmtStreamChunkIndex;
            rmtStreamChunkIndex = -1;
            rmtStreamSymbolOffset = 0;
            audioRmtSignalChunkFree(completed);
            continue;
        }
        while (written < symbolsFree &&
               rmtStreamSymbolOffset < AUDIO_RMT_DMA_CHUNK_SYMBOLS) {
            rmt_symbol_word_t symbol = chunk->symbols[rmtStreamSymbolOffset++];
            symbols[written++] = symbol;
            rmtStreamLastLevel = symbol.duration1 > 0 ? symbol.level1 : symbol.level0;
        }
        if (rmtStreamSymbolOffset == AUDIO_RMT_DMA_CHUNK_SYMBOLS) {
            uint8_t completed = (uint8_t)rmtStreamChunkIndex;
            rmtStreamChunkIndex = -1;
            rmtStreamSymbolOffset = 0;
            audioRmtSignalChunkFree(completed);
        }
    }
    return written;
}

static bool audioRmtAcquireChunk(AudioRmtStreamWriter *writer) {
    if (writer->chunk) return true;
    for (;;) {
        for (uint8_t i = 0; i < AUDIO_RMT_RING_CHUNKS; ++i) {
            uint8_t expected = AUDIO_RMT_CHUNK_FREE;
            if (__atomic_compare_exchange_n(&rmtChunks[i].state, &expected,
                                            AUDIO_RMT_CHUNK_FILLING, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                writer->chunk = &rmtChunks[i];
                writer->symbols = rmtChunks[i].symbols;
                writer->symbolCount = 0;
                return true;
            }
        }
        ++rmtQueueWaits;
        if (ulTaskNotifyTake(pdTRUE, AUDIO_RMT_WRITE_WAIT_TICKS) == 0) {
            printf("AUDIO: RMT chunk wait timeout, waits=%u\n", (unsigned)rmtQueueWaits);
            return false;
        }
    }
}

static bool audioRmtStartStream(void);

static bool audioRmtPublishChunk(AudioRmtStreamWriter *writer) {
    if (!writer->chunk || writer->symbolCount != AUDIO_RMT_DMA_CHUNK_SYMBOLS) return false;
    uint8_t index = (uint8_t)(writer->chunk - rmtChunks);
    __atomic_store_n(&writer->chunk->state, AUDIO_RMT_CHUNK_READY, __ATOMIC_RELEASE);
    uint32_t head = __atomic_load_n(&rmtPendingHead, __ATOMIC_RELAXED);
    rmtPendingChunkIndexes[head % AUDIO_RMT_RING_CHUNKS] = index;
    __atomic_store_n(&rmtPendingHead, head + 1, __ATOMIC_RELEASE);
    writer->chunk = nullptr;
    writer->symbols = nullptr;
    writer->symbolCount = 0;
    uint32_t tail = __atomic_load_n(&rmtPendingTail, __ATOMIC_ACQUIRE);
    if (!rmtStreamStarted && head + 1 - tail >= AUDIO_RMT_DMA_NODES) {
        return audioRmtStartStream();
    }
    return true;
}

static bool audioRmtFlushPendingSymbol(AudioRmtStreamWriter *writer) {
    if (!writer->hasPendingSymbol || writer->pendingSymbol.duration1 == 0) return true;
    if (!audioRmtAcquireChunk(writer)) return false;
    writer->symbols[writer->symbolCount++] = writer->pendingSymbol;
    writer->hasPendingSymbol = false;
    if (writer->symbolCount == AUDIO_RMT_DMA_CHUNK_SYMBOLS) return audioRmtPublishChunk(writer);
    return true;
}

static bool audioRmtAppendRun(AudioRmtStreamWriter *stream, uint8_t level, uint32_t ticks) {
    while (ticks > 0) {
        uint16_t part = ticks > 0x7FFF ? 0x7FFF : (uint16_t)ticks;
        if (!stream->hasPendingSymbol) {
            stream->pendingSymbol = {};
            stream->pendingSymbol.level0 = level;
            stream->pendingSymbol.duration0 = part;
            stream->pendingSymbol.level1 = level;
            stream->hasPendingSymbol = true;
            ticks -= part;
            continue;
        }
        rmt_symbol_word_t *symbol = &stream->pendingSymbol;
        if (symbol->duration1 == 0) {
            if (symbol->level0 == level && symbol->duration0 + part <= 0x7FFF) {
                symbol->duration0 += part;
            } else {
                symbol->level1 = level;
                symbol->duration1 = part;
            }
            ticks -= part;
            continue;
        }
        if (symbol->level1 == level && symbol->duration1 + part <= 0x7FFF) {
            symbol->duration1 += part;
            ticks -= part;
            continue;
        }
        if (!audioRmtFlushPendingSymbol(stream)) return false;
        stream->pendingSymbol = {};
        stream->pendingSymbol.level0 = level;
        stream->pendingSymbol.duration0 = part;
        stream->pendingSymbol.level1 = level;
        stream->hasPendingSymbol = true;
        ticks -= part;
    }
    return true;
}

static uint32_t audioRmtFrameCycleToTick(uint16_t line, uint16_t cycle) {
    if (line >= AUDIO_FRAME_SAMPLES) { line = AUDIO_FRAME_SAMPLES; cycle = 0; }
    else if (cycle > AUDIO_MAC_LINE_CYCLES) cycle = AUDIO_MAC_LINE_CYCLES;
    uint64_t frameCycle = (uint32_t)line * AUDIO_MAC_LINE_CYCLES + cycle;
    return audioRmtAbsoluteCycleToTick(frameCycle);
}

static bool audioRmtEmitEnabledTicks(AudioRmtStreamWriter *writer,
                                     uint16_t line, uint16_t lowCycles,
                                     uint16_t startCycle, uint16_t endCycle,
                                     uint32_t startTick, uint32_t endTick) {
    if (endCycle <= lowCycles) return endTick > startTick ? audioRmtAppendRun(writer, 0, endTick - startTick) : true;
    if (startCycle >= lowCycles) return endTick > startTick ? audioRmtAppendRun(writer, 1, endTick - startTick) : true;
    uint32_t lowTick = audioRmtFrameCycleToTick(line, lowCycles);
    return (lowTick > startTick ? audioRmtAppendRun(writer, 0, lowTick - startTick) : true) &&
        (endTick > lowTick ? audioRmtAppendRun(writer, 1, endTick - lowTick) : true);
}

static bool audioRmtRenderLine(AudioRmtStreamWriter *writer, int volume,
                               const SndGateEvent *events, uint16_t eventCount,
                               uint16_t line, uint16_t *eventIndex,
                               bool *gateEnabled, uint16_t lowCycles,
                               uint32_t lineStartTick, uint32_t lineEndTick) {
    if (eventCount == 0) {
        if (volume == 0 || !*gateEnabled) {
            return lineEndTick > lineStartTick ? audioRmtAppendRun(writer, 0, lineEndTick - lineStartTick) : true;
        }
        return audioRmtEmitEnabledTicks(writer, line, lowCycles, 0, AUDIO_MAC_LINE_CYCLES,
                                        lineStartTick, lineEndTick);
    }
    uint16_t cycle = 0;
    uint32_t startTick = lineStartTick;
    while (cycle < AUDIO_MAC_LINE_CYCLES) {
        uint16_t nextCycle = AUDIO_MAC_LINE_CYCLES;
        if (*eventIndex < eventCount && events[*eventIndex].line == line) {
            nextCycle = events[*eventIndex].cycle;
            if (nextCycle > AUDIO_MAC_LINE_CYCLES) nextCycle = AUDIO_MAC_LINE_CYCLES;
            if (nextCycle < cycle) nextCycle = cycle;
        }
        uint32_t endTick = nextCycle == AUDIO_MAC_LINE_CYCLES ? lineEndTick : audioRmtFrameCycleToTick(line, nextCycle);
        bool ok = volume == 0 || !*gateEnabled
            ? (endTick > startTick ? audioRmtAppendRun(writer, 0, endTick - startTick) : true)
            : audioRmtEmitEnabledTicks(writer, line, lowCycles, cycle, nextCycle, startTick, endTick);
        if (!ok) return false;
        cycle = nextCycle;
        startTick = endTick;
        while (*eventIndex < eventCount && events[*eventIndex].line == line &&
               events[*eventIndex].cycle <= cycle) {
            *gateEnabled = events[*eventIndex].state == SND_GATE_ENABLE;
            ++(*eventIndex);
        }
    }
    return true;
}

static esp_err_t audioRmtInitImpl(void) {
    rmt_tx_channel_config_t channelCfg = {};
    channelCfg.gpio_num = AUDIO_GPIO;
    channelCfg.clk_src = RMT_CLK_SRC_DEFAULT;
    channelCfg.resolution_hz = AUDIO_RMT_REQUESTED_RESOLUTION_HZ;
    channelCfg.mem_block_symbols = AUDIO_RMT_MEM_BLOCK_SYMBOLS;
    channelCfg.trans_queue_depth = 1;
    channelCfg.flags.with_dma = true;
    channelCfg.flags.init_level = 0;
    esp_err_t err = rmt_new_tx_channel(&channelCfg, &rmtTxChannel);
    if (err != ESP_OK) return err;
    rmt_simple_encoder_config_t encoderCfg = {};
    encoderCfg.callback = audioRmtStreamEncode;
    encoderCfg.min_chunk_size = AUDIO_RMT_DMA_CHUNK_SYMBOLS;
    err = rmt_new_simple_encoder(&encoderCfg, &rmtStreamEncoder);
    if (err != ESP_OK) return err;
    uint32_t realResolution = 0;
    err = rmt_get_channel_resolution(rmtTxChannel, &realResolution);
    rmtResolutionHz = err == ESP_OK && realResolution > 0 ? realResolution : AUDIO_RMT_REQUESTED_RESOLUTION_HZ;
    err = rmt_enable(rmtTxChannel);
    if (err == ESP_OK) {
        printf("AUDIO: backend RMT symbol stream on GPIO%d, real %u Hz, DMA %d symbols, ring %d chunks\n",
               AUDIO_GPIO, (unsigned)rmtResolutionHz,
               AUDIO_RMT_DMA_CHUNK_SYMBOLS, AUDIO_RMT_RING_CHUNKS);
    }
    return err;
}

static bool audioRmtStartStream(void) {
    rmt_transmit_config_t transmitCfg = {};
    transmitCfg.loop_count = 0;
    transmitCfg.flags.eot_level = 0;
    transmitCfg.flags.queue_nonblocking = false;
    rmtStreamStarted = true;
    esp_err_t err = rmt_transmit(rmtTxChannel, rmtStreamEncoder, &rmtStreamToken,
                                 sizeof(rmtStreamToken), &transmitCfg);
    if (err != ESP_OK) {
        rmtStreamStarted = false;
        printf("AUDIO: RMT continuous stream start failed, err=%s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool audioRmtBuildFrame(AudioRmtStreamWriter *writer,
                               const uint8_t *rawSamples, int volume,
                               bool frameStartGateEnabled,
                               const SndGateEvent *events, uint16_t eventCount) {
    bool gateEnabled = frameStartGateEnabled;
    uint16_t eventIndex = 0;
    uint16_t boundedEventCount = eventCount > SND_GATE_MAX_EVENTS ? SND_GATE_MAX_EVENTS : eventCount;
    uint64_t lineNumerator = (uint64_t)AUDIO_MAC_LINE_CYCLES * rmtResolutionHz;
    uint32_t lineTickWhole = (uint32_t)(lineNumerator / AUDIO_MAC_CPU_HZ);
    uint32_t lineTickRemainder = (uint32_t)(lineNumerator % AUDIO_MAC_CPU_HZ);
    uint32_t lineTick = 0;
    uint32_t lineRemainder = AUDIO_MAC_CPU_HZ / 2;
    for (uint16_t line = 0; line < AUDIO_FRAME_SAMPLES; ++line) {
        uint8_t sample = rawSamples[line];
        uint16_t lowCycles = (uint16_t)(((uint32_t)(0xFF - sample) * AUDIO_MAC_LINE_CYCLES + 127) / 0xFF);
        uint32_t lineStartTick = lineTick;
        lineTick += lineTickWhole;
        lineRemainder += lineTickRemainder;
        if (lineRemainder >= AUDIO_MAC_CPU_HZ) { lineRemainder -= AUDIO_MAC_CPU_HZ; ++lineTick; }
        if (!audioRmtRenderLine(writer, volume, events, boundedEventCount, line,
                                &eventIndex, &gateEnabled, lowCycles,
                                lineStartTick, lineTick)) return false;
    }
    return audioRmtFlushPendingSymbol(writer);
}

static bool audioRmtWriteFrameImpl(const uint8_t *rawSamples, int volume,
                                   bool frameStartGateEnabled,
                                   const SndGateEvent *events, uint16_t eventCount) {
    if (!rmtProducerTask) rmtProducerTask = xTaskGetCurrentTaskHandle();
    return audioRmtBuildFrame(&rmtProducerWriter, rawSamples, volume,
                              frameStartGateEnabled, events, eventCount);
}

} // namespace

extern "C" esp_err_t audioRmtInit(void) { return audioRmtInitImpl(); }

extern "C" bool audioRmtWriteFrame(const uint8_t *rawSamples, int volume,
                                    bool frameStartGateEnabled,
                                    const SndGateEvent *events, uint16_t eventCount) {
    return audioRmtWriteFrameImpl(rawSamples, volume, frameStartGateEnabled, events, eventCount);
}
