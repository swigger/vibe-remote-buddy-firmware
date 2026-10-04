#include "buddy_led.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_engine.h"
#include "nvs.h"
#include "rbp/defs.h"
#include <cstring>

namespace {
SemaphoreHandle_t mutex;
TaskHandle_t worker;
rmt_channel_handle_t channel;
rmt_encoder_handle_t encoder;
nvs_handle_t storage;
esp_err_t init_error = ESP_ERR_INVALID_STATE;
unsigned init_stage;

bool load(char *text, size_t capacity) {
  return nvs_get_str(storage, "effects", text, &capacity) == ESP_OK;
}
bool save(const char *text) {
  return nvs_set_str(storage, "effects", text) == ESP_OK &&
         nvs_commit(storage) == ESP_OK;
}
void output(uint32_t rgb) {
  // Preserve the actual board's RGB byte order from logled, GPIO 21.
  // A complete 24-bit frame plus reset fits in one hardware block: no refill
  // ISR.
  rmt_symbol_word_t symbols[25]{};
  for (unsigned i = 0; i < 24; ++i) {
    const bool one = (rgb & (1u << (23 - i))) != 0;
    symbols[i].level0 = 1;
    symbols[i].duration0 = one ? 9 : 3;
    symbols[i].level1 = 0;
    symbols[i].duration1 = one ? 3 : 9;
  }
  symbols[24].duration0 = 500;
  symbols[24].duration1 = 500;
  rmt_transmit_config_t config{};
  // Release the RMT power lock between color changes; WS2812 retains its state.
  if (rmt_enable(channel) == ESP_OK) {
    if (rmt_transmit(channel, encoder, symbols, sizeof symbols, &config) ==
        ESP_OK)
      rmt_tx_wait_all_done(channel, -1);
    rmt_disable(channel);
  }
  uint8_t r = rgb >> 16, g = rgb >> 8, b = rgb;
  bool yellow = r && g && b < r / 2 && b < g / 2;
  gpio_set_level(GPIO_NUM_7, !yellow && g > r && g > b);
  gpio_set_level(GPIO_NUM_8, yellow);
  gpio_set_level(GPIO_NUM_9, !yellow && r > g && r > b);
}
void run(void *) {
  uint32_t previous = UINT32_MAX;
  for (;;) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    auto frame = vibeled::tick(esp_timer_get_time() / 1000);
    if (frame.rgb != previous) {
      output(frame.rgb);
      previous = frame.rgb;
    }
    xSemaphoreGive(mutex);
    TickType_t wait = portMAX_DELAY;
    if (frame.wait_ms != UINT32_MAX) {
      uint64_t ticks =
          (uint64_t(frame.wait_ms) * configTICK_RATE_HZ + 999) / 1000;
      wait = ticks >= portMAX_DELAY ? portMAX_DELAY - 1 : ticks ? ticks : 1;
    }
    ulTaskNotifyTake(pdTRUE, wait);
  }
}
} // namespace

void buddy_led_init(void) {
#define LED_INIT(step, expression)                                             \
  do {                                                                         \
    init_stage = step;                                                         \
    init_error = (expression);                                                 \
    if (init_error != ESP_OK)                                                  \
      return;                                                                  \
  } while (0)
  // nvs_open is wrapped by buddy_update: presets follow the active data bank.
  LED_INIT(1, nvs_open("xgai", NVS_READWRITE, &storage));
  mutex = xSemaphoreCreateMutex();
  LED_INIT(2, mutex ? ESP_OK : ESP_ERR_NO_MEM);
  gpio_config_t gpio{};
  gpio.pin_bit_mask = (1ULL << 7) | (1ULL << 8) | (1ULL << 9);
  gpio.mode = GPIO_MODE_OUTPUT;
  LED_INIT(3, gpio_config(&gpio));
  rmt_tx_channel_config_t config{};
  config.gpio_num = GPIO_NUM_21;
  config.clk_src = RMT_CLK_SRC_DEFAULT;
  config.resolution_hz = 10000000;
  config.mem_block_symbols = 64;
  config.trans_queue_depth = 1;
  LED_INIT(4, rmt_new_tx_channel(&config, &channel));
  rmt_copy_encoder_config_t copy{};
  LED_INIT(5, rmt_new_copy_encoder(&copy, &encoder));
  vibeled::init({load, save});
  LED_INIT(6, xTaskCreatePinnedToCore(run, "vibeled", 4096, nullptr, 2, &worker,
                                      1) == pdPASS
                  ? ESP_OK
                  : ESP_ERR_NO_MEM);
#undef LED_INIT
}

uint16_t buddy_led_command(const cJSON *request, cJSON *response,
                           bool allow_save) {
  if (init_error != ESP_OK) {
    char error[96];
    snprintf(error, sizeof error,
             "error LED initialization stage=%u esp_error=%d", init_stage,
             init_error);
    cJSON_AddStringToObject(response, "text", error);
    return RBP_STATUS_OK;
  }
  const char *command = cJSON_GetStringValue(
      cJSON_GetObjectItemCaseSensitive(request, "command"));
  if (!command || !*command || strlen(command) >= 256)
    return RBP_STATUS_INVALID_ARGUMENT;
  char result[384]{};
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool handled = vibeled::execute_command(
      command, result, sizeof result, esp_timer_get_time() / 1000, allow_save);
  xSemaphoreGive(mutex);
  xTaskNotifyGive(worker);
  cJSON_AddStringToObject(response, "text",
                          handled ? result : "error empty command");
  return RBP_STATUS_OK;
}
