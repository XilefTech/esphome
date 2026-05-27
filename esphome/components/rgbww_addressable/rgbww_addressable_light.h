#pragma once

#ifdef USE_ESP32

#include "esphome/components/light/addressable_light.h"
#include "esphome/core/component.h"

#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <esp_err.h>
#include <esp_idf_version.h>

namespace esphome {
namespace rgbww_addressable {

enum RGBOrder : uint8_t {
  ORDER_RGB,
  ORDER_RBG,
  ORDER_GRB,
  ORDER_GBR,
  ORDER_BGR,
  ORDER_BRG,
};

struct LedParams {
  rmt_symbol_word_t bit0;
  rmt_symbol_word_t bit1;
  rmt_symbol_word_t reset;
};

class RGBWWAddressableLightOutput : public light::AddressableLight {
 public:
  void setup() override;
  void update_state(light::LightState *state) override;
  void write_state(light::LightState *state) override;
  void dump_config() override;

  int32_t size() const override { return this->num_leds_; }

  void clear_effect_data() override {
    for (int i = 0; i < this->size(); i++)
      this->effect_data_[i] = 0;
  }

  void set_pin(uint8_t pin) { this->pin_ = pin; }
  void set_inverted(bool inverted) { this->invert_out_ = inverted; }
  void set_num_leds(uint16_t num_leds) { this->num_leds_ = num_leds; }
  void set_rgb_order(RGBOrder rgb_order) { this->rgb_order_ = rgb_order; }
  void set_swap_white_channels(bool swap_white_channels) { this->swap_white_channels_ = swap_white_channels; }
  void set_cold_white_temperature(float cold_white_temperature) { cold_white_temperature_ = cold_white_temperature; }
  void set_warm_white_temperature(float warm_white_temperature) { warm_white_temperature_ = warm_white_temperature; }

  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::RGB_COLD_WARM_WHITE});
    traits.set_min_mireds(this->cold_white_temperature_);
    traits.set_max_mireds(this->warm_white_temperature_);
    return traits;
  }

 protected:
  light::ESPColorView get_view_internal(int32_t index) const override;
  size_t get_buffer_size_() const { return this->num_leds_ * BYTES_PER_LED; }

  static constexpr uint8_t BYTES_PER_LED = 6;

  uint8_t *buf_{nullptr};
  uint8_t *white_buf_{nullptr};
  uint8_t *effect_data_{nullptr};
  LedParams params_;
  rmt_channel_handle_t channel_{nullptr};
  rmt_encoder_handle_t encoder_{nullptr};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  uint8_t *rmt_buf_{nullptr};
#else
  rmt_symbol_word_t *rmt_buf_{nullptr};
#endif
  uint32_t rmt_symbols_{96};

  uint8_t pin_{0};
  uint16_t num_leds_{0};
  RGBOrder rgb_order_{ORDER_RGB};
  bool swap_white_channels_{false};
  bool invert_out_{false};
  float cold_white_temperature_{0};
  float warm_white_temperature_{0};
};

}  // namespace rgbww_addressable
}  // namespace esphome

#endif  // USE_ESP32
