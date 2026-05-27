#include "rgbww_addressable_light.h"

#include <cinttypes>
#include <cmath>

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_attr.h>
#include <esp_clk_tree.h>

namespace esphome::rgbww_addressable {

static const char *const TAG = "rgbww_addressable";

static const size_t RMT_SYMBOLS_PER_BYTE = 8;

static uint32_t rmt_resolution_hz() {
  uint32_t freq;
  esp_clk_tree_src_get_freq_hz((soc_module_clk_t) RMT_CLK_SRC_DEFAULT, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &freq);
  return freq;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
static size_t IRAM_ATTR HOT encoder_callback(const void *data, size_t size, size_t symbols_written, size_t symbols_free,
                                             rmt_symbol_word_t *symbols, bool *done, void *arg) {
  auto *params = static_cast<LedParams *>(arg);
  const auto *bytes = static_cast<const uint8_t *>(data);
  size_t index = symbols_written / RMT_SYMBOLS_PER_BYTE;

  if (index < size) {
    if (symbols_free < RMT_SYMBOLS_PER_BYTE) {
      return 0;
    }
    for (size_t i = 0; i < RMT_SYMBOLS_PER_BYTE; i++) {
      if (bytes[index] & (1 << (7 - i))) {
        symbols[i] = params->bit1;
      } else {
        symbols[i] = params->bit0;
      }
    }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 1)
    if ((index + 1) >= size && params->reset.duration0 == 0 && params->reset.duration1 == 0) {
      *done = true;
    }
#endif
    return RMT_SYMBOLS_PER_BYTE;
  }

  if (symbols_free < 1) {
    return 0;
  }
  symbols[0] = params->reset;
  *done = true;
  return 1;
}
#endif

static void set_led_params(LedParams *params, uint32_t bit0_high, uint32_t bit0_low, uint32_t bit1_high,
                           uint32_t bit1_low, uint32_t reset_time_high, uint32_t reset_time_low) {
  float ratio = (float) rmt_resolution_hz() / 1e09f;

  params->bit0.duration0 = (uint32_t) (ratio * bit0_high);
  params->bit0.level0 = 1;
  params->bit0.duration1 = (uint32_t) (ratio * bit0_low);
  params->bit0.level1 = 0;

  params->bit1.duration0 = (uint32_t) (ratio * bit1_high);
  params->bit1.level0 = 1;
  params->bit1.duration1 = (uint32_t) (ratio * bit1_low);
  params->bit1.level1 = 0;

  params->reset.duration0 = (uint32_t) (ratio * reset_time_high);
  params->reset.level0 = 1;
  params->reset.duration1 = (uint32_t) (ratio * reset_time_low);
  params->reset.level1 = 0;
}

void RGBWWAddressableLightOutput::setup() {
  const size_t buffer_size = this->get_buffer_size_();

  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_INTERNAL);
  this->buf_ = allocator.allocate(buffer_size);
  if (this->buf_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate LED buffer!");
    this->mark_failed();
    return;
  }
  memset(this->buf_, 0, buffer_size);

  this->white_buf_ = allocator.allocate(this->num_leds_);
  if (this->white_buf_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate white buffer!");
    this->mark_failed();
    return;
  }
  memset(this->white_buf_, 0, this->num_leds_);

  this->effect_data_ = allocator.allocate(this->num_leds_);
  if (this->effect_data_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate effect data!");
    this->mark_failed();
    return;
  }
  memset(this->effect_data_, 0, this->num_leds_);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  this->rmt_buf_ = allocator.allocate(buffer_size);
#else
  RAMAllocator<rmt_symbol_word_t> rmt_allocator(RAMAllocator<rmt_symbol_word_t>::ALLOC_INTERNAL);
  this->rmt_buf_ = rmt_allocator.allocate(buffer_size * 8 + 1);
#endif

  if (this->rmt_buf_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate RMT buffer!");
    this->mark_failed();
    return;
  }

  // WS2811 default timings in nanoseconds
  set_led_params(&this->params_, 300, 1090, 1090, 320, 0, 300000);

  rmt_tx_channel_config_t channel;
  memset(&channel, 0, sizeof(channel));
  channel.clk_src = RMT_CLK_SRC_DEFAULT;
  channel.resolution_hz = rmt_resolution_hz();
  channel.gpio_num = gpio_num_t(this->pin_);
  channel.mem_block_symbols = this->rmt_symbols_;
  channel.trans_queue_depth = 1;
  channel.flags.invert_out = this->invert_out_;
  channel.flags.with_dma = false;
  channel.intr_priority = 0;
  if (rmt_new_tx_channel(&channel, &this->channel_) != ESP_OK) {
    ESP_LOGE(TAG, "Channel creation failed");
    this->mark_failed();
    return;
  }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  rmt_simple_encoder_config_t encoder;
  memset(&encoder, 0, sizeof(encoder));
  encoder.callback = encoder_callback;
  encoder.arg = &this->params_;
  encoder.min_chunk_size = RMT_SYMBOLS_PER_BYTE;
  if (rmt_new_simple_encoder(&encoder, &this->encoder_) != ESP_OK) {
    ESP_LOGE(TAG, "Encoder creation failed");
    this->mark_failed();
    return;
  }
#else
  rmt_copy_encoder_config_t encoder;
  memset(&encoder, 0, sizeof(encoder));
  if (rmt_new_copy_encoder(&encoder, &this->encoder_) != ESP_OK) {
    ESP_LOGE(TAG, "Encoder creation failed");
    this->mark_failed();
    return;
  }
#endif

  if (rmt_enable(this->channel_) != ESP_OK) {
    ESP_LOGE(TAG, "Enabling channel failed");
    this->mark_failed();
    return;
  }
}

void RGBWWAddressableLightOutput::update_state(light::LightState *state) {
  auto val = state->current_values;
  auto max_brightness = to_uint8_scale(val.get_brightness() * val.get_state());
  this->correction_.set_local_brightness(max_brightness);

  if (this->is_effect_active()) {
    return;
  }

  float combined = 0.0f;
  if (val.get_color_mode() & light::ColorCapability::COLD_WARM_WHITE) {
    combined = val.get_cold_white() + val.get_warm_white();
    if (combined > 1.0f) {
      combined = 1.0f;
    }
  }

  auto r = to_uint8_scale(val.get_color_brightness() * val.get_red());
  auto g = to_uint8_scale(val.get_color_brightness() * val.get_green());
  auto b = to_uint8_scale(val.get_color_brightness() * val.get_blue());
  auto w = to_uint8_scale(combined);

  this->all() = Color(r, g, b, w);
  this->schedule_show();
}

void RGBWWAddressableLightOutput::write_state(light::LightState *state) {
  if (this->is_failed()) {
    ESP_LOGW(TAG, "Light is in failed state, not writing state.");
    return;
  }
  if (this->buf_ == nullptr || this->white_buf_ == nullptr) {
    ESP_LOGW(TAG, "Buffers are null, not writing state.");
    return;
  }

  float cold_white = 0.0f;
  float warm_white = 0.0f;
  state->current_values_as_cwww(&cold_white, &warm_white, false);
  const float sum = cold_white + warm_white;
  const float cw_ratio = sum > 0.0f ? cold_white / sum : 0.5f;

  for (int32_t i = 0; i < this->size(); i++) {
    const uint8_t combined = this->white_buf_[i];
    const uint8_t cold = static_cast<uint8_t>(roundf(combined * cw_ratio));
    const uint8_t warm = combined - cold;
    const size_t base = i * BYTES_PER_LED;

    if (this->swap_white_channels_) {
      this->buf_[base + 3] = warm;
      this->buf_[base + 4] = cold;
    } else {
      this->buf_[base + 3] = cold;
      this->buf_[base + 4] = warm;
    }
    this->buf_[base + 5] = 0;
  }

  this->mark_shown_();

  esp_err_t error = rmt_tx_wait_all_done(this->channel_, 1000);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "RMT TX timeout");
    this->status_set_warning();
    return;
  }
  delayMicroseconds(50);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  memcpy(this->rmt_buf_, this->buf_, this->get_buffer_size_());
#else
  size_t buffer_size = this->get_buffer_size_();
  size_t size = 0;
  size_t len = 0;
  uint8_t *psrc = this->buf_;
  rmt_symbol_word_t *pdest = this->rmt_buf_;
  while (size < buffer_size) {
    uint8_t b = *psrc;
    for (int i = 0; i < 8; i++) {
      pdest->val = b & (1 << (7 - i)) ? this->params_.bit1.val : this->params_.bit0.val;
      pdest++;
      len++;
    }
    size++;
    psrc++;
  }

  if (this->params_.reset.duration0 > 0 || this->params_.reset.duration1 > 0) {
    pdest->val = this->params_.reset.val;
    pdest++;
    len++;
  }
#endif

  rmt_transmit_config_t config;
  memset(&config, 0, sizeof(config));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  error = rmt_transmit(this->channel_, this->encoder_, this->rmt_buf_, this->get_buffer_size_(), &config);
#else
  error = rmt_transmit(this->channel_, this->encoder_, this->rmt_buf_, len * sizeof(rmt_symbol_word_t), &config);
#endif
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "RMT TX error");
    this->status_set_warning();
    return;
  }
  this->status_clear_warning();
}

light::ESPColorView RGBWWAddressableLightOutput::get_view_internal(int32_t index) const {
  const size_t base = index * BYTES_PER_LED;
  uint8_t *red = nullptr;
  uint8_t *green = nullptr;
  uint8_t *blue = nullptr;
  switch (this->rgb_order_) {
    case ORDER_RGB:
      red = &this->buf_[base + 0];
      green = &this->buf_[base + 1];
      blue = &this->buf_[base + 2];
      break;
    case ORDER_RBG:
      red = &this->buf_[base + 0];
      green = &this->buf_[base + 2];
      blue = &this->buf_[base + 1];
      break;
    case ORDER_GRB:
      red = &this->buf_[base + 1];
      green = &this->buf_[base + 0];
      blue = &this->buf_[base + 2];
      break;
    case ORDER_GBR:
      red = &this->buf_[base + 2];
      green = &this->buf_[base + 0];
      blue = &this->buf_[base + 1];
      break;
    case ORDER_BGR:
      red = &this->buf_[base + 2];
      green = &this->buf_[base + 1];
      blue = &this->buf_[base + 0];
      break;
    case ORDER_BRG:
      red = &this->buf_[base + 1];
      green = &this->buf_[base + 2];
      blue = &this->buf_[base + 0];
      break;
  }
  return {red, green, blue, &this->white_buf_[index], &this->effect_data_[index], &this->correction_};
}

void RGBWWAddressableLightOutput::dump_config() {
  ESP_LOGCONFIG(TAG, "RGBWW Addressable Light:");
  ESP_LOGCONFIG(TAG, "  Pin: %" PRIu8, this->pin_);
  ESP_LOGCONFIG(TAG, "  Number of LEDs: %" PRIu16, this->num_leds_);
  ESP_LOGCONFIG(TAG, "  Swap white channels: %s", YESNO(this->swap_white_channels_));
  ESP_LOGCONFIG(TAG, "  Inverted: %s", YESNO(this->invert_out_));
  ESP_LOGCONFIG(TAG, "  Cold white color temperature: %.1f mireds", this->cold_white_temperature_);
  ESP_LOGCONFIG(TAG, "  Warm white color temperature: %.1f mireds", this->warm_white_temperature_);
}

}  // namespace esphome::rgbww_addressable

#endif  // USE_ESP32
