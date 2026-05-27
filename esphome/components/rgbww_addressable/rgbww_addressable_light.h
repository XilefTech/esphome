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

struct RGBWWColor {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t cold_white;
  uint8_t warm_white;

  bool operator==(const RGBWWColor &rhs) const {
    return this->red == rhs.red && this->green == rhs.green && this->blue == rhs.blue &&
           this->cold_white == rhs.cold_white && this->warm_white == rhs.warm_white;
  }
  bool operator!=(const RGBWWColor &rhs) const { return !(*this == rhs); }
  RGBWWColor &operator*=(uint8_t scale) {
    this->red = esp_scale8(this->red, scale);
    this->green = esp_scale8(this->green, scale);
    this->blue = esp_scale8(this->blue, scale);
    this->cold_white = esp_scale8(this->cold_white, scale);
    this->warm_white = esp_scale8(this->warm_white, scale);
    return *this;
  }
};

class RGBWWColorView {
 public:
  RGBWWColorView(uint8_t *red, uint8_t *green, uint8_t *blue, uint8_t *cold_white, uint8_t *warm_white,
                 uint8_t *effect_data, const light::ESPColorCorrection *color_correction)
      : red_(red),
        green_(green),
        blue_(blue),
        cold_white_(cold_white),
        warm_white_(warm_white),
        effect_data_(effect_data),
        color_correction_(color_correction) {}

  void set_red(uint8_t red) { *this->red_ = this->color_correction_->color_correct_red(red); }
  void set_green(uint8_t green) { *this->green_ = this->color_correction_->color_correct_green(green); }
  void set_blue(uint8_t blue) { *this->blue_ = this->color_correction_->color_correct_blue(blue); }
  void set_cold_white(uint8_t cold_white) {
    *this->cold_white_ = this->color_correction_->color_correct_white(cold_white);
  }
  void set_warm_white(uint8_t warm_white) {
    *this->warm_white_ = this->color_correction_->color_correct_white(warm_white);
  }
  void set_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    this->set_red(red);
    this->set_green(green);
    this->set_blue(blue);
  }
  void set_rgbww(uint8_t red, uint8_t green, uint8_t blue, uint8_t cold_white, uint8_t warm_white) {
    this->set_rgb(red, green, blue);
    this->set_cold_white(cold_white);
    this->set_warm_white(warm_white);
  }
  void set_effect_data(uint8_t effect_data) {
    if (this->effect_data_ == nullptr)
      return;
    *this->effect_data_ = effect_data;
  }

  uint8_t get_red() const { return this->color_correction_->color_uncorrect_red(*this->red_); }
  uint8_t get_green() const { return this->color_correction_->color_uncorrect_green(*this->green_); }
  uint8_t get_blue() const { return this->color_correction_->color_uncorrect_blue(*this->blue_); }
  uint8_t get_cold_white() const { return this->color_correction_->color_uncorrect_white(*this->cold_white_); }
  uint8_t get_warm_white() const { return this->color_correction_->color_uncorrect_white(*this->warm_white_); }
  RGBWWColor get() const {
    return {this->get_red(), this->get_green(), this->get_blue(), this->get_cold_white(), this->get_warm_white()};
  }

 protected:
  uint8_t *const red_;
  uint8_t *const green_;
  uint8_t *const blue_;
  uint8_t *const cold_white_;
  uint8_t *const warm_white_;
  uint8_t *const effect_data_;
  const light::ESPColorCorrection *color_correction_;
};

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

class RGBWWAddressableLightTransformer;

class RGBWWAddressableLightOutput : public light::AddressableLight {
 public:
  void setup() override;
  void update_state(light::LightState *state) override;
  void write_state(light::LightState *state) override;
  void dump_config() override;
  std::unique_ptr<light::LightTransformer> create_default_transition() override;

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
  friend class RGBWWAddressableLightTransformer;

  light::ESPColorView get_view_internal(int32_t index) const override;
  RGBWWColorView get_rgbww_view_internal(int32_t index) const;
  void set_combined_white_(int32_t index, uint8_t cold_white, uint8_t warm_white) {
    const uint16_t combined = cold_white + warm_white;
    this->white_buf_[index] = combined > 255 ? 255 : combined;
  }
  size_t get_buffer_size_() const { return this->num_leds_ * BYTES_PER_LED; }

  static constexpr uint8_t BYTES_PER_LED = 6;

  uint8_t *buf_{nullptr};
  uint8_t *white_buf_{nullptr};
  uint8_t *cold_white_buf_{nullptr};
  uint8_t *warm_white_buf_{nullptr};
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
