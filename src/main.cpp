#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <time.h>
#include "market_config.h"
#include "bitcoin_logo.h"
#include <math.h>

// Bitcoin-inspired UI palette (sRGB). LVGL converts these safely to RGB565.
#define COLOR_BTC_ORANGE 0xF7931A // Bitcoin orange
#define COLOR_BLACK      0x0D0D0D // Near-black background
#define COLOR_CARD_BG    0x1E222A // Charcoal cards
#define COLOR_WHITE      0xF5F5F5 // Warm white primary text
#define COLOR_MUTED_TEXT 0x9CA3AF // Cool grey secondary text
#define COLOR_DANGER     0xC62828 // Configuration / destructive actions

// ESP32-2432S028 ("CYD" 2.8 inch) wiring.
static constexpr uint16_t SCREEN_W = 240;
static constexpr uint16_t SCREEN_H = 320;
// ESP32-2432S028 has an XPT2046 on its own HSPI bus, not the LCD SPI bus.
static constexpr uint8_t TOUCH_CS = 33;
static constexpr uint8_t TOUCH_IRQ = 36;
static constexpr uint8_t TOUCH_SCLK = 25;
static constexpr uint8_t TOUCH_MISO = 39;
static constexpr uint8_t TOUCH_MOSI = 32;
static constexpr int16_t MIN_TOUCH_PRESSURE = 350;

struct TouchCalibration {
  int16_t left;
  int16_t right;
  int16_t top;
  int16_t bottom;
  bool swapAxes;
  bool valid;
};

struct RawTouch {
  int16_t x;
  int16_t y;
  int16_t pressure;
};

TFT_eSPI tft;
static SPIClass touchSpi(HSPI);
// Fixed portrait mapping for the ESP32-2432S028 XPT2046 controller.
// The touch controller axes are opposite the displayed X axis.
static constexpr TouchCalibration touchCalibration = {
  3700, 200, 600, 3700, true, true
};
static WiFiManager wifiPortal;
static lv_disp_draw_buf_t drawBuffer;
// Twelve scan lines keep the LVGL draw buffer below 6 KiB of static RAM.
static lv_color_t buf[SCREEN_W * 12];

static lv_obj_t *amountArea;
static lv_obj_t *resultLabel;
static lv_obj_t *modeButton;
static lv_obj_t *modeLabel;
static lv_obj_t *fiatButton;
static lv_obj_t *fiatLabel;
static lv_obj_t *unitButton;
static lv_obj_t *unitLabel;
static lv_obj_t *mainPanel;
static lv_obj_t *connectionPanel;
static lv_obj_t *configPanel;
static lv_obj_t *configInfoLabel;
static lv_obj_t *configStatsLabel;
static lv_obj_t *pricesPanel;
static lv_obj_t *customRatePanel;
static lv_obj_t *customRateArea;
static lv_obj_t *rateButtons[3];
static lv_obj_t *mainRateLabel;
static lv_obj_t *keyboard;
static lv_obj_t *updatingOverlay;
static lv_obj_t *updatingStatusLabel;
static bool fiatToBitcoin = true;
static bool useSats = false;
static uint8_t fiatIndex = 0;
static bool touchWasDown = false;
static uint32_t lastLvglTick = 0;
static float exchangeRates[MAX_MARKET_SOURCES];
static uint8_t activeSourceIndexes[MAX_MARKET_SOURCES];
static uint8_t activeSourceCount = 0;
static uint32_t lastRateUpdateMs = 0;
static time_t lastRateUpdateEpoch = 0;
static bool ntpRequested = false;
static bool startupRefreshDone = false;
static uint32_t lastClockCheckMs = 0;
enum class RateChoice : uint8_t { Average, Median, Custom };
static RateChoice rateChoice = RateChoice::Median;
static double customRate = 0.0;
static bool customRateConfigured = false;
static double lastAmount = 100.0;
static constexpr uint32_t RATE_CACHE_MAGIC = 0x53415452; // "SATR"
static constexpr uint8_t RATE_CACHE_VERSION = 1;
static constexpr uint8_t BOARD_STATE_VERSION = 1;

struct RateCache {
  uint32_t magic;
  uint8_t version;
  uint8_t sourceCount;
  uint16_t reserved;
  uint32_t updatedEpoch;
  float rates[MAX_MARKET_SOURCES];
};

static const FiatMarketConfig &selectedMarket() {
  return FIAT_MARKETS[fiatIndex];
}

static lv_color_t uiColor(uint32_t rgb) {
  return lv_color_hex(rgb);
}

static void applyButtonStyle(lv_obj_t *button) {
  lv_obj_set_style_bg_color(button, uiColor(COLOR_CARD_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(button, uiColor(COLOR_BTC_ORANGE), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(button, uiColor(COLOR_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(button, uiColor(COLOR_BLACK), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, uiColor(COLOR_BTC_ORANGE), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(button, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(button, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void applyRadioStyle(lv_obj_t *button) {
  applyButtonStyle(button);
  lv_obj_set_style_bg_color(button, uiColor(COLOR_BTC_ORANGE), LV_PART_MAIN | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(button, uiColor(COLOR_BLACK), LV_PART_MAIN | LV_STATE_CHECKED);
}

static void applyDangerStyle(lv_obj_t *button) {
  lv_obj_set_style_bg_color(button, uiColor(COLOR_DANGER), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(button, uiColor(COLOR_BTC_ORANGE), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(button, uiColor(COLOR_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(button, uiColor(COLOR_WHITE), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(button, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(button, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void loadZeroRates() {
  // Prices must represent persisted market data only.  A missing cache is zero,
  // never a plausible-looking prototype quote.
  const FiatMarketConfig &market = selectedMarket();
  activeSourceCount = 0;
  for (uint8_t i = 0; i < market.sourceCount; ++i) {
    if (market.sources[i].enabled) {
      activeSourceIndexes[activeSourceCount] = i;
      exchangeRates[activeSourceCount++] = 0;
    }
  }
  lastRateUpdateMs = 0;
  lastRateUpdateEpoch = 0;
}

static void cacheKey(uint8_t marketIndex, char *key, size_t keySize) {
  snprintf(key, keySize, "fiat%u", marketIndex);
}

static bool loadCachedRates(uint8_t marketIndex) {
  const FiatMarketConfig &market = FIAT_MARKETS[marketIndex];
  char key[12];
  cacheKey(marketIndex, key, sizeof(key));
  Preferences preferences;
  preferences.begin("sat-rates", true);
  RateCache cache = {};
  const size_t bytes = preferences.getBytes(key, &cache, sizeof(cache));
  preferences.end();
  if (bytes != sizeof(cache) || cache.magic != RATE_CACHE_MAGIC ||
      cache.version != RATE_CACHE_VERSION || cache.sourceCount != market.sourceCount) return false;

  activeSourceCount = 0;
  uint8_t valid = 0;
  for (uint8_t i = 0; i < market.sourceCount; ++i) {
    if (!market.sources[i].enabled) continue;
    activeSourceIndexes[activeSourceCount] = i;
    const float rate = cache.rates[i];
    exchangeRates[activeSourceCount++] = isfinite(rate) && rate > 0 ? rate : NAN;
    if (isfinite(rate) && rate > 0) ++valid;
  }
  if (!valid) return false;
  lastRateUpdateEpoch = cache.updatedEpoch > 1700000000 ? cache.updatedEpoch : 0;
  lastRateUpdateMs = 0;
  return true;
}

static void saveCachedRates(uint8_t marketIndex) {
  const FiatMarketConfig &market = FIAT_MARKETS[marketIndex];
  RateCache cache = {};
  cache.magic = RATE_CACHE_MAGIC;
  cache.version = RATE_CACHE_VERSION;
  cache.sourceCount = market.sourceCount;
  cache.updatedEpoch = lastRateUpdateEpoch;
  for (uint8_t i = 0; i < activeSourceCount; ++i) {
    cache.rates[activeSourceIndexes[i]] = isfinite(exchangeRates[i]) && exchangeRates[i] > 0 ? exchangeRates[i] : 0;
  }
  char key[12];
  cacheKey(marketIndex, key, sizeof(key));
  Preferences preferences;
  preferences.begin("sat-rates", false);
  preferences.putBytes(key, &cache, sizeof(cache));
  preferences.end();
}

static void loadSavedOrZeroRates() {
  if (!loadCachedRates(fiatIndex)) loadZeroRates();
}

static void eraseCachedRates() {
  Preferences preferences;
  preferences.begin("sat-rates", false);
  preferences.clear();
  preferences.end();
}

static size_t cachedRateBytesUsed() {
  Preferences preferences;
  preferences.begin("sat-rates", true);
  size_t bytes = 0;
  for (uint8_t i = 0; i < FIAT_MARKET_COUNT; ++i) {
    char key[12];
    cacheKey(i, key, sizeof(key));
    bytes += preferences.getBytesLength(key);
  }
  preferences.end();
  return bytes;
}

static size_t boardStateBytesUsed() {
  Preferences preferences;
  preferences.begin("sat-state", true);
  size_t bytes = 0;
  if (preferences.isKey("version")) bytes += sizeof(uint8_t);
  if (preferences.isKey("fiat")) bytes += sizeof(uint8_t);
  if (preferences.isKey("fiat-btc")) bytes += sizeof(bool);
  if (preferences.isKey("use-sats")) bytes += sizeof(bool);
  if (preferences.isKey("rate-mode")) bytes += sizeof(uint8_t);
  bytes += preferences.getBytesLength("custom");
  bytes += preferences.getBytesLength("amount");
  preferences.end();
  return bytes;
}

static void saveBoardState() {
  Preferences preferences;
  preferences.begin("sat-state", false);
  preferences.putUChar("version", BOARD_STATE_VERSION);
  preferences.putUChar("fiat", fiatIndex);
  preferences.putBool("fiat-btc", fiatToBitcoin);
  preferences.putBool("use-sats", useSats);
  preferences.putUChar("rate-mode", static_cast<uint8_t>(rateChoice));
  preferences.putBytes("custom", &customRate, sizeof(customRate));
  if (amountArea) {
    const char *entered = lv_textarea_get_text(amountArea);
    char *end = nullptr;
    const double parsed = strtod(entered, &end);
    if (end != entered && isfinite(parsed) && parsed >= 0) lastAmount = parsed;
  }
  preferences.putBytes("amount", &lastAmount, sizeof(lastAmount));
  preferences.end();
}

static void loadBoardState() {
  Preferences preferences;
  preferences.begin("sat-state", true);
  if (preferences.getUChar("version", 0) == BOARD_STATE_VERSION) {
    const uint8_t storedFiat = preferences.getUChar("fiat", 0);
    fiatIndex = storedFiat < FIAT_MARKET_COUNT ? storedFiat : 0;
    fiatToBitcoin = preferences.getBool("fiat-btc", true);
    useSats = preferences.getBool("use-sats", false);
    const uint8_t storedMode = preferences.getUChar("rate-mode", static_cast<uint8_t>(RateChoice::Median));
    rateChoice = storedMode <= static_cast<uint8_t>(RateChoice::Custom)
                   ? static_cast<RateChoice>(storedMode) : RateChoice::Median;
    double storedCustom = 0.0;
    if (preferences.getBytes("custom", &storedCustom, sizeof(storedCustom)) == sizeof(storedCustom) &&
        isfinite(storedCustom) && storedCustom > 0) {
      customRate = storedCustom;
      customRateConfigured = true;
    }
    double storedAmount = 0.0;
    if (preferences.getBytes("amount", &storedAmount, sizeof(storedAmount)) == sizeof(storedAmount) &&
        isfinite(storedAmount) && storedAmount >= 0) lastAmount = storedAmount;
  }
  preferences.end();
}

static double jsonNumberAfter(const String &json, const char *marker) {
  const char *cursor = strstr(json.c_str(), marker);
  if (!cursor) return NAN;
  cursor += strlen(marker);
  while (*cursor && !isdigit(static_cast<unsigned char>(*cursor)) && *cursor != '-' && *cursor != '.') ++cursor;
  char *end = nullptr;
  const double value = strtod(cursor, &end);
  return end != cursor && isfinite(value) && value > 0 ? value : NAN;
}

static double bitfinexLastPrice(const String &json) {
  const char *cursor = strchr(json.c_str(), '[');
  if (!cursor) return NAN;
  ++cursor;
  for (uint8_t index = 0; index < 7; ++index) {
    while (*cursor == ' ' || *cursor == ',') ++cursor;
    char *end = nullptr;
    const double value = strtod(cursor, &end);
    if (end == cursor) return NAN;
    if (index == 6) return value > 0 ? value : NAN;
    cursor = end;
  }
  return NAN;
}

static double parseLivePrice(const String &json, PriceParser parser) {
  switch (parser) {
    case PriceParser::Kraken: return jsonNumberAfter(json, "\"c\":[");
    case PriceParser::Coinbase: return jsonNumberAfter(json, "\"amount\":");
    case PriceParser::Bitstamp:
    case PriceParser::CexIo:
    case PriceParser::Coincheck:
    case PriceParser::Bitbank: return jsonNumberAfter(json, "\"last\":");
    case PriceParser::Bitfinex: return bitfinexLastPrice(json);
    case PriceParser::KuCoin:
    case PriceParser::Bybit:
    case PriceParser::Bitget: return jsonNumberAfter(json, "\"lastPrice\":");
    case PriceParser::Okx: return jsonNumberAfter(json, "\"last\":");
    case PriceParser::BitFlyer: return jsonNumberAfter(json, "\"ltp\":");
    case PriceParser::Gemini: return jsonNumberAfter(json, "\"close\":");
    case PriceParser::Binance: return jsonNumberAfter(json, "\"price\":");
  }
  return NAN;
}

static double fetchLivePrice(const MarketSourceConfig &source) {
  WiFiClientSecure client;
  client.setInsecure(); // Public rate APIs do not share a single root CA chain.
  HTTPClient request;
  request.setConnectTimeout(4500);
  request.setTimeout(4500);
  request.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!request.begin(client, source.url)) return NAN;
  const int status = request.GET();
  const String response = status == HTTP_CODE_OK ? request.getString() : String();
  request.end();
  return response.length() ? parseLivePrice(response, source.parser) : NAN;
}

static void displayFlush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color) {
  const uint32_t pixels = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
  // TFT_eSPI needs the 16-bit LVGL buffer byte-swapped for SPI transfer.
  tft.pushColors((uint16_t *)&color->full, pixels, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

static int16_t averageClosestPair(int16_t a, int16_t b, int16_t c) {
  const int16_t ab = abs(a - b);
  const int16_t ac = abs(a - c);
  const int16_t bc = abs(b - c);
  if (ab <= ac && ab <= bc) return (a + b) / 2;
  if (ac <= ab && ac <= bc) return (a + c) / 2;
  return (b + c) / 2;
}

static bool readRawTouch(RawTouch &raw) {
  touchSpi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  touchSpi.transfer(0xB1); // Z1
  const int16_t z1 = touchSpi.transfer16(0xC1) >> 3; // Z2
  const int16_t z2 = touchSpi.transfer16(0x91) >> 3;
  const int16_t pressure = z1 + 4095 - z2;
  int16_t values[6] = {};
  if (pressure >= MIN_TOUCH_PRESSURE) {
    touchSpi.transfer16(0x91); // discard the first noisy conversion
    values[0] = touchSpi.transfer16(0xD1) >> 3;
    values[1] = touchSpi.transfer16(0x91) >> 3;
    values[2] = touchSpi.transfer16(0xD1) >> 3;
    values[3] = touchSpi.transfer16(0x91) >> 3;
  }
  values[4] = touchSpi.transfer16(0xD0) >> 3;
  values[5] = touchSpi.transfer16(0) >> 3;
  digitalWrite(TOUCH_CS, HIGH);
  touchSpi.endTransaction();
  if (pressure < MIN_TOUCH_PRESSURE) return false;
  raw.x = averageClosestPair(values[0], values[2], values[4]);
  raw.y = averageClosestPair(values[1], values[3], values[5]);
  raw.pressure = pressure;
  return true;
}

static void touchRead(lv_indev_drv_t *indev, lv_indev_data_t *data) {
  RawTouch raw;
  if (!readRawTouch(raw)) {
    data->state = LV_INDEV_STATE_REL;
    touchWasDown = false;
    return;
  }

  const int16_t horizontal = touchCalibration.swapAxes ? raw.y : raw.x;
  const int16_t vertical = touchCalibration.swapAxes ? raw.x : raw.y;
  const int16_t x = constrain(map(horizontal, touchCalibration.left, touchCalibration.right,
                                  0, SCREEN_W - 1), 0, SCREEN_W - 1);
  const int16_t y = constrain(map(vertical, touchCalibration.top, touchCalibration.bottom,
                                  0, SCREEN_H - 1), 0, SCREEN_H - 1);
  data->state = LV_INDEV_STATE_PR;
  data->point.x = x;
  data->point.y = y;
  if (!touchWasDown) {
    Serial.printf("LVGL touch: raw=(%d,%d) mapped=(%d,%d)\n", raw.x, raw.y, x, y);
    touchWasDown = true;
  }
}

static double numericValue(lv_obj_t *area, double fallback) {
  const char *text = lv_textarea_get_text(area);
  char *end;
  const double value = strtod(text, &end);
  return end != text && value >= 0 ? value : fallback;
}

static uint8_t validRateCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < activeSourceCount; ++i) if (isfinite(exchangeRates[i]) && exchangeRates[i] > 0) ++count;
  return count;
}

static double averageRate() {
  double total = 0;
  uint8_t count = 0;
  for (uint8_t i = 0; i < activeSourceCount; ++i) {
    const float rate = exchangeRates[i];
    if (isfinite(rate) && rate > 0) {
      total += rate;
      ++count;
    }
  }
  return count ? total / count : 0;
}

static double medianRate() {
  float values[MAX_MARKET_SOURCES];
  uint8_t count = 0;
  for (uint8_t i = 0; i < activeSourceCount; ++i) {
    const float rate = exchangeRates[i];
    if (isfinite(rate) && rate > 0) values[count++] = rate;
  }
  for (uint8_t i = 0; i < count; ++i) {
    for (uint8_t j = i + 1; j < count; ++j) {
      if (values[j] < values[i]) {
        const float temp = values[i];
        values[i] = values[j];
        values[j] = temp;
      }
    }
  }
  return count ? values[count / 2] : 0;
}

static void updateRateLabels() {
  const FiatMarketConfig &market = selectedMarket();
  if (!mainRateLabel) return;
  const char *name = rateChoice == RateChoice::Average ? "Average" :
                     rateChoice == RateChoice::Median ? "Median" : "Custom";
  const double rate = rateChoice == RateChoice::Average ? averageRate() :
                      rateChoice == RateChoice::Median ? medianRate() : customRate;
  char text[96];
  if (rateChoice == RateChoice::Median || rateChoice == RateChoice::Average) {
    char timestamp[32];
    if (lastRateUpdateEpoch > 1700000000) {
      tm localTime;
      localtime_r(&lastRateUpdateEpoch, &localTime);
      strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &localTime);
    } else {
      const uint32_t seconds = lastRateUpdateMs / 1000U;
      snprintf(timestamp, sizeof(timestamp), "offline %02lu:%02lu:%02lu",
               static_cast<unsigned long>(seconds / 3600U),
               static_cast<unsigned long>((seconds / 60U) % 60U),
               static_cast<unsigned long>(seconds % 60U));
    }
    snprintf(text, sizeof(text), "%s: %.2f %s / BTC\nUpdated: %s", name, rate, market.code, timestamp);
  } else {
    snprintf(text, sizeof(text), "%s: %.2f %s / BTC", name, rate, market.code);
  }
  lv_label_set_text(mainRateLabel, text);
}

static double conversionRate() {
  if (rateChoice == RateChoice::Average) return averageRate();
  if (rateChoice == RateChoice::Custom) return customRate;
  return medianRate();
}

static void formatSats(double value, char *output, size_t outputSize) {
  const uint64_t rounded = value > 0 ? static_cast<uint64_t>(llround(value)) : 0;
  char digits[24];
  snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(rounded));
  const size_t length = strlen(digits);
  size_t out = 0;
  for (size_t i = 0; i < length && out + 1 < outputSize; ++i) {
    output[out++] = digits[i];
    const size_t remaining = length - i - 1;
    if (remaining && remaining % 3 == 0 && out + 1 < outputSize) output[out++] = ' ';
  }
  output[out] = '\0';
}

static void updateConversion() {
  const double amount = numericValue(amountArea, 0.0);
  const double rate = conversionRate(); // fiat currency per 1 BTC
  char mainText[64];

  if (fiatToBitcoin) {
    const double btc = rate > 0 ? amount / rate : 0;
    const double sats = btc * 100000000.0;
    char formattedSats[32];
    formatSats(sats, formattedSats, sizeof(formattedSats));
    if (useSats) {
      snprintf(mainText, sizeof(mainText), "%s sats", formattedSats);
    } else {
      snprintf(mainText, sizeof(mainText), "%.8f BTC", btc);
    }
  } else {
    const double btc = useSats ? amount / 100000000.0 : amount;
    const double fiat = btc * rate;
    snprintf(mainText, sizeof(mainText), "%.2f %s", fiat, selectedMarket().code);
  }
  lv_label_set_text(resultLabel, mainText);
}

static void hideKeyboard() {
  if (keyboard) lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *makeTextArea(lv_obj_t *parent, const char *placeholder, const char *initial);
static void showCustomRateScreen(lv_event_t *event);
static void showPricesScreen(lv_event_t *event);
static void refreshRatesEvent(lv_event_t *event);
static void refreshAllRatesEvent(lv_event_t *event);
static void updateConfigurationInfo(const char *notice = nullptr);
static void showConnectionScreen();
static void showConfigurationScreen(lv_event_t *event);

static void refreshUi() {
  // Label changes invalidate their own area; invalidate the full screen too so
  // every button state is visibly refreshed immediately after a press.
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);
}

static void amountEvent(lv_event_t *event) {
  lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(keyboard, (lv_obj_t *)lv_event_get_target(event));
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  } else if (code == LV_EVENT_VALUE_CHANGED) {
    updateConversion();
  } else if (code == LV_EVENT_DEFOCUSED) {
    updateConversion();
    saveBoardState();
  }
}

static void keyboardEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_READY || lv_event_get_code(event) == LV_EVENT_CANCEL) {
    saveBoardState();
    hideKeyboard();
  }
}

static void modeEvent(lv_event_t *) {
  Serial.println("Mode button pressed");
  fiatToBitcoin = !fiatToBitcoin;
  lv_label_set_text(modeLabel, fiatToBitcoin ? "->" : "<-");
  lv_textarea_set_placeholder_text(amountArea, fiatToBitcoin ? "Fiat amount" : (useSats ? "Satoshi amount" : "Bitcoin amount"));
  updateConversion();
  saveBoardState();
  refreshUi();
}

static void fiatEvent(lv_event_t *) {
  Serial.println("Fiat button pressed");
  fiatIndex = (fiatIndex + 1) % FIAT_MARKET_COUNT;
  char text[16];
  snprintf(text, sizeof(text), "%s", selectedMarket().code);
  lv_label_set_text(fiatLabel, text);
  loadSavedOrZeroRates();
  customRate = medianRate();
  customRateConfigured = true;
  updateRateLabels();
  updateConversion();
  saveBoardState();
  refreshUi();
}

static void unitEvent(lv_event_t *) {
  Serial.println("Unit button pressed");
  useSats = !useSats;
  lv_label_set_text(unitLabel, useSats ? "SATS" : "BTC");
  lv_textarea_set_placeholder_text(amountArea, fiatToBitcoin ? "Fiat amount" : (useSats ? "Satoshi amount" : "Bitcoin amount"));
  updateConversion();
  saveBoardState();
  refreshUi();
}

static void refreshRateChoiceButtons() {
  for (uint8_t i = 0; i < 3; ++i) {
    if (i == static_cast<uint8_t>(rateChoice)) lv_obj_add_state(rateButtons[i], LV_STATE_CHECKED);
    else lv_obj_clear_state(rateButtons[i], LV_STATE_CHECKED);
  }
  updateRateLabels();
  updateConversion();
  refreshUi();
}

static void rateChoiceEvent(lv_event_t *event) {
  const RateChoice selected = static_cast<RateChoice>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (selected == RateChoice::Custom) {
    showCustomRateScreen(nullptr);
    return;
  }
  rateChoice = selected;
  saveBoardState();
  refreshRateChoiceButtons();
}

static void showUpdatingOverlay() {
  if (updatingOverlay) return;
  updatingOverlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(updatingOverlay, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(updatingOverlay, 0, 0);
  lv_obj_set_style_bg_color(updatingOverlay, uiColor(COLOR_BLACK), 0);
  lv_obj_set_style_bg_opa(updatingOverlay, LV_OPA_90, 0);
  lv_obj_set_style_border_width(updatingOverlay, 0, 0);
  lv_obj_clear_flag(updatingOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(updatingOverlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *message = lv_label_create(updatingOverlay);
  char title[40];
  snprintf(title, sizeof(title), "Updating %s rates", selectedMarket().code);
  lv_label_set_text(message, title);
  lv_obj_set_style_text_font(message, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(message, uiColor(COLOR_BTC_ORANGE), 0);
  lv_obj_align(message, LV_ALIGN_CENTER, 0, -24);
  updatingStatusLabel = lv_label_create(updatingOverlay);
  lv_label_set_text(updatingStatusLabel, "Preparing requests...");
  lv_obj_set_style_text_color(updatingStatusLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_set_style_text_align(updatingStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(updatingStatusLabel, LV_ALIGN_CENTER, 0, 14);
  lv_obj_move_foreground(updatingOverlay);
  lv_refr_now(nullptr);
}

static void hideUpdatingOverlay() {
  if (!updatingOverlay) return;
  lv_obj_del(updatingOverlay);
  updatingOverlay = nullptr;
  updatingStatusLabel = nullptr;
}

static void refreshRatesEvent(lv_event_t *) {
  if (WiFi.status() != WL_CONNECTED) {
    loadSavedOrZeroRates();
    updateRateLabels();
    updateConversion();
    refreshUi();
    return;
  }

  showUpdatingOverlay();
  const FiatMarketConfig &market = selectedMarket();
  activeSourceCount = 0;
  uint8_t liveCount = 0;
  uint8_t enabledCount = 0;
  for (uint8_t i = 0; i < market.sourceCount; ++i) if (market.sources[i].enabled) ++enabledCount;
  for (uint8_t i = 0; i < market.sourceCount; ++i) {
    if (!market.sources[i].enabled) continue;
    activeSourceIndexes[activeSourceCount] = i;
    if (updatingStatusLabel) {
      char status[64];
      snprintf(status, sizeof(status), "Contacting %s\n%d of %d", market.sources[i].name,
               activeSourceCount + 1, enabledCount);
      lv_label_set_text(updatingStatusLabel, status);
      lv_refr_now(nullptr);
    }
    const double live = fetchLivePrice(market.sources[i]);
    exchangeRates[activeSourceCount++] = isfinite(live) && live > 0 ? live : NAN;
    if (isfinite(live) && live > 0) ++liveCount;
  }
  if (liveCount == 0) loadSavedOrZeroRates();
  else {
    lastRateUpdateMs = millis();
    const time_t now = time(nullptr);
    lastRateUpdateEpoch = now > 1700000000 ? now : 0;
    saveCachedRates(fiatIndex);
  }
  hideUpdatingOverlay();
  updateRateLabels();
  updateConversion();
  refreshUi();
}

static void refreshAllRatesEvent(lv_event_t *) {
  if (WiFi.status() != WL_CONNECTED) {
    updateConfigurationInfo("Wi-Fi is required to refresh all rates.");
    return;
  }
  const uint8_t originalFiat = fiatIndex;
  for (uint8_t i = 0; i < FIAT_MARKET_COUNT; ++i) {
    fiatIndex = i;
    refreshRatesEvent(nullptr);
  }
  fiatIndex = originalFiat;
  loadSavedOrZeroRates();
  updateRateLabels();
  updateConversion();
  updateConfigurationInfo("All supported fiat rates refreshed and saved.");
}

static void showMainScreen(lv_event_t *) {
  if (pricesPanel) {
    lv_obj_del(pricesPanel);
    pricesPanel = nullptr;
  }
  if (customRatePanel) {
    lv_obj_del(customRatePanel);
    customRatePanel = nullptr;
    customRateArea = nullptr;
  }
  if (connectionPanel) {
    lv_obj_del(connectionPanel);
    connectionPanel = nullptr;
  }
  if (configPanel) {
    lv_obj_del(configPanel);
    configPanel = nullptr;
    configInfoLabel = nullptr;
    configStatsLabel = nullptr;
  }
  hideKeyboard();
  lv_obj_clear_flag(mainPanel, LV_OBJ_FLAG_HIDDEN);
}

static void skipConnectionEvent(lv_event_t *) {
  showMainScreen(nullptr);
}

static void showConnectionScreen() {
  if (connectionPanel) return;
  lv_obj_add_flag(mainPanel, LV_OBJ_FLAG_HIDDEN);
  connectionPanel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(connectionPanel, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(connectionPanel, 0, 0);
  lv_obj_set_style_bg_color(connectionPanel, uiColor(COLOR_BLACK), 0);
  lv_obj_set_style_border_width(connectionPanel, 0, 0);
  lv_obj_set_style_pad_all(connectionPanel, 0, 0);
  lv_obj_clear_flag(connectionPanel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(connectionPanel);
  lv_label_set_text(title, "Connect to Wi-Fi");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, uiColor(COLOR_BTC_ORANGE), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);

  lv_obj_t *instructions = lv_label_create(connectionPanel);
  lv_label_set_text(instructions,
                    "1. Join Wi-Fi: SATulator-Setup\n"
                    "2. Open: 192.168.4.1\n"
                    "3. Choose your Wi-Fi network");
  lv_obj_set_style_text_color(instructions, uiColor(COLOR_WHITE), 0);
  lv_obj_set_style_text_align(instructions, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(instructions, LV_ALIGN_TOP_MID, 0, 95);

  lv_obj_t *hint = lv_label_create(connectionPanel);
  lv_label_set_text(hint, "You can continue offline at any time.");
  lv_obj_set_style_text_color(hint, uiColor(COLOR_MUTED_TEXT), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 177);

  lv_obj_t *skip = lv_btn_create(connectionPanel);
  lv_obj_set_size(skip, 130, 42);
  lv_obj_align(skip, LV_ALIGN_TOP_MID, 0, 210);
  applyButtonStyle(skip);
  lv_obj_add_event_cb(skip, skipConnectionEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *skipLabel = lv_label_create(skip);
  lv_label_set_text(skipLabel, "Skip / Offline");
  lv_obj_set_style_text_color(skipLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(skipLabel);
}

static void updateConfigurationInfo(const char *notice) {
  if (!configInfoLabel || !configStatsLabel) return;
  char info[160];
  if (notice) {
    snprintf(info, sizeof(info), "%s", notice);
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(info, sizeof(info), "Wi-Fi: connected\nSSID: %s\nIP: %s",
             WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else {
    snprintf(info, sizeof(info), "Wi-Fi: not connected\nPortal: SATulator-Setup\nOpen: 192.168.4.1");
  }
  lv_label_set_text(configInfoLabel, info);

  const uint32_t uptime = millis() / 1000U;
  char stats[160];
  const size_t cacheUsed = cachedRateBytesUsed();
  const size_t cacheMaximum = sizeof(RateCache) * FIAT_MARKET_COUNT;
  const size_t stateUsed = boardStateBytesUsed();
  const size_t savedTotal = cacheUsed + stateUsed;
  snprintf(stats, sizeof(stats),
           "Statistics\nUptime: %02lu:%02lu:%02lu\nFree heap: %lu bytes\nLive sources: %u / %u\nRate payload: %u / %u B\nState payload: %u B\nSaved total: %u B",
           static_cast<unsigned long>(uptime / 3600U),
           static_cast<unsigned long>((uptime / 60U) % 60U),
           static_cast<unsigned long>(uptime % 60U),
           static_cast<unsigned long>(ESP.getFreeHeap()),
           validRateCount(), activeSourceCount,
           static_cast<unsigned>(cacheUsed),
           static_cast<unsigned>(cacheMaximum),
           static_cast<unsigned>(stateUsed),
           static_cast<unsigned>(savedTotal));
  lv_label_set_text(configStatsLabel, stats);
}

static void forgetWifiEvent(lv_event_t *) {
  wifiPortal.resetSettings();
  WiFi.disconnect(false, true);
  ntpRequested = false;
  startupRefreshDone = false;
  updateConfigurationInfo("Saved Wi-Fi credentials erased.\nRestart to open SATulator-Setup.");
}

static void eraseCachedRatesEvent(lv_event_t *) {
  eraseCachedRates();
  loadZeroRates();
  customRate = medianRate();
  updateRateLabels();
  updateConversion();
  updateConfigurationInfo("Downloaded rates erased.\nMissing saved prices now show as zero.");
}

static void restartBoardEvent(lv_event_t *) {
  delay(100);
  ESP.restart();
}

static void showConfigurationScreen(lv_event_t *) {
  lv_obj_add_flag(mainPanel, LV_OBJ_FLAG_HIDDEN);
  if (configPanel) lv_obj_del(configPanel);

  configPanel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(configPanel, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(configPanel, 0, 0);
  lv_obj_set_style_bg_color(configPanel, uiColor(COLOR_BLACK), 0);
  lv_obj_set_style_border_width(configPanel, 0, 0);
  lv_obj_set_style_pad_all(configPanel, 0, 0);
  lv_obj_clear_flag(configPanel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_btn_create(configPanel);
  lv_obj_set_size(back, 58, 26);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 6);
  applyButtonStyle(back);
  lv_obj_add_event_cb(back, showMainScreen, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = lv_label_create(back);
  lv_label_set_text(backLabel, "Back");
  lv_obj_set_style_text_color(backLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(backLabel);

  lv_obj_t *title = lv_label_create(configPanel);
  lv_label_set_text(title, "Configuration");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, uiColor(COLOR_BTC_ORANGE), 0);
  lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -8, 8);

  configInfoLabel = lv_label_create(configPanel);
  lv_obj_set_style_text_color(configInfoLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_align(configInfoLabel, LV_ALIGN_TOP_LEFT, 14, 48);

  configStatsLabel = lv_label_create(configPanel);
  lv_obj_set_style_text_color(configStatsLabel, uiColor(COLOR_MUTED_TEXT), 0);
  lv_obj_align(configStatsLabel, LV_ALIGN_TOP_LEFT, 14, 112);
  updateConfigurationInfo();

  lv_obj_t *refreshAll = lv_btn_create(configPanel);
  lv_obj_set_size(refreshAll, 105, 30);
  lv_obj_align(refreshAll, LV_ALIGN_TOP_LEFT, 10, 215);
  applyButtonStyle(refreshAll);
  lv_obj_add_event_cb(refreshAll, refreshAllRatesEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *refreshAllLabel = lv_label_create(refreshAll);
  lv_label_set_text(refreshAllLabel, "Refresh all");
  lv_obj_set_style_text_color(refreshAllLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(refreshAllLabel);

  lv_obj_t *eraseRates = lv_btn_create(configPanel);
  lv_obj_set_size(eraseRates, 105, 30);
  lv_obj_align(eraseRates, LV_ALIGN_TOP_RIGHT, -10, 215);
  applyDangerStyle(eraseRates);
  lv_obj_add_event_cb(eraseRates, eraseCachedRatesEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *eraseRatesLabel = lv_label_create(eraseRates);
  lv_label_set_text(eraseRatesLabel, "Erase rates");
  lv_obj_set_style_text_color(eraseRatesLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(eraseRatesLabel);

  lv_obj_t *forget = lv_btn_create(configPanel);
  lv_obj_set_size(forget, 105, 30);
  lv_obj_align(forget, LV_ALIGN_TOP_LEFT, 10, 253);
  applyDangerStyle(forget);
  lv_obj_add_event_cb(forget, forgetWifiEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *forgetLabel = lv_label_create(forget);
  lv_label_set_text(forgetLabel, "Forget Wi-Fi");
  lv_obj_set_style_text_color(forgetLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(forgetLabel);

  lv_obj_t *restart = lv_btn_create(configPanel);
  lv_obj_set_size(restart, 105, 30);
  lv_obj_align(restart, LV_ALIGN_TOP_RIGHT, -10, 253);
  applyDangerStyle(restart);
  lv_obj_add_event_cb(restart, restartBoardEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *restartLabel = lv_label_create(restart);
  lv_label_set_text(restartLabel, "Restart board");
  lv_obj_set_style_text_color(restartLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(restartLabel);
}

static void priceFiatEvent(lv_event_t *event) {
  fiatIndex = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  char text[16];
  snprintf(text, sizeof(text), "%s", selectedMarket().code);
  lv_label_set_text(fiatLabel, text);
  loadSavedOrZeroRates();
  customRate = medianRate();
  customRateConfigured = true;
  updateRateLabels();
  updateConversion();
  saveBoardState();
  showPricesScreen(nullptr);
}

static void saveCustomRate(lv_event_t *) {
  const double entered = numericValue(customRateArea, medianRate());
  customRate = entered > 0 ? entered : medianRate();
  customRateConfigured = true;
  rateChoice = RateChoice::Custom;
  saveBoardState();
  showMainScreen(nullptr);
  refreshRateChoiceButtons();
}

static void showCustomRateScreen(lv_event_t *) {
  lv_obj_add_flag(mainPanel, LV_OBJ_FLAG_HIDDEN);
  if (customRatePanel) lv_obj_del(customRatePanel);

  customRatePanel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(customRatePanel, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(customRatePanel, 0, 0);
  lv_obj_set_style_bg_color(customRatePanel, uiColor(COLOR_BLACK), 0);
  lv_obj_set_style_border_width(customRatePanel, 0, 0);
  lv_obj_set_style_pad_all(customRatePanel, 0, 0);
  lv_obj_clear_flag(customRatePanel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_btn_create(customRatePanel);
  lv_obj_set_size(back, 64, 30);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 10, 8);
  applyButtonStyle(back);
  lv_obj_add_event_cb(back, showMainScreen, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = lv_label_create(back);
  lv_label_set_text(backLabel, "Cancel");
  lv_obj_set_style_text_color(backLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(backLabel);

  lv_obj_t *title = lv_label_create(customRatePanel);
  lv_label_set_text(title, "Custom BTC rate");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, uiColor(COLOR_BTC_ORANGE), 0);
  lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -10, 11);

  lv_obj_t *caption = lv_label_create(customRatePanel);
  char captionText[48];
  snprintf(captionText, sizeof(captionText), "%s per 1 BTC", selectedMarket().code);
  lv_label_set_text(caption, captionText);
  lv_obj_set_style_text_color(caption, uiColor(COLOR_MUTED_TEXT), 0);
  lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 55);

  char medianText[24];
  snprintf(medianText, sizeof(medianText), "%.2f", medianRate());
  customRateArea = makeTextArea(customRatePanel, "Custom rate", medianText);
  lv_obj_align(customRateArea, LV_ALIGN_TOP_MID, 0, 76);

  lv_obj_t *save = lv_btn_create(customRatePanel);
  lv_obj_set_size(save, 100, 34);
  lv_obj_align(save, LV_ALIGN_TOP_MID, 0, 126);
  applyButtonStyle(save);
  lv_obj_add_event_cb(save, saveCustomRate, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *saveLabel = lv_label_create(save);
  lv_label_set_text(saveLabel, "Save rate");
  lv_obj_set_style_text_color(saveLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(saveLabel);

  lv_keyboard_set_textarea(keyboard, customRateArea);
  lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(keyboard);
}

static void showPricesScreen(lv_event_t *) {
  lv_obj_add_flag(mainPanel, LV_OBJ_FLAG_HIDDEN);
  if (pricesPanel) lv_obj_del(pricesPanel);

  pricesPanel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(pricesPanel, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(pricesPanel, 0, 0);
  lv_obj_set_style_bg_color(pricesPanel, uiColor(COLOR_BLACK), 0);
  lv_obj_set_style_border_width(pricesPanel, 0, 0);
  lv_obj_set_style_pad_all(pricesPanel, 0, 0);
  lv_obj_clear_flag(pricesPanel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *back = lv_btn_create(pricesPanel);
  lv_obj_set_size(back, 58, 26);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 6);
  applyButtonStyle(back);
  lv_obj_add_event_cb(back, showMainScreen, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = lv_label_create(back);
  lv_label_set_text(backLabel, "Back");
  lv_obj_set_style_text_color(backLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(backLabel);

  char text[64];
  lv_obj_t *title = lv_label_create(pricesPanel);
  snprintf(text, sizeof(text), "%s BTC prices", selectedMarket().code);
  lv_label_set_text(title, text);
  lv_obj_set_style_text_color(title, uiColor(COLOR_BTC_ORANGE), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -8, 7);

  const FiatMarketConfig &market = selectedMarket();
  for (uint8_t i = 0; i < FIAT_MARKET_COUNT; ++i) {
    lv_obj_t *fiat = lv_btn_create(pricesPanel);
    lv_obj_set_size(fiat, 42, 24);
    lv_obj_align(fiat, LV_ALIGN_TOP_LEFT, 4 + i * 47, 38);
    applyRadioStyle(fiat);
    if (i == fiatIndex) lv_obj_add_state(fiat, LV_STATE_CHECKED);
    lv_obj_add_event_cb(fiat, priceFiatEvent, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    lv_obj_t *fiatLabel = lv_label_create(fiat);
    lv_label_set_text(fiatLabel, FIAT_MARKETS[i].code);
    lv_obj_set_style_text_color(fiatLabel, uiColor(COLOR_WHITE), 0);
    lv_obj_center(fiatLabel);
  }

  lv_obj_t *updated = lv_label_create(pricesPanel);
  char updatedText[48];
  if (lastRateUpdateEpoch > 1700000000) {
    tm localTime;
    localtime_r(&lastRateUpdateEpoch, &localTime);
    strftime(updatedText, sizeof(updatedText), "Updated: %Y-%m-%d %H:%M:%S", &localTime);
  } else {
    snprintf(updatedText, sizeof(updatedText), "Updated: no saved data");
  }
  lv_label_set_text(updated, updatedText);
  lv_obj_set_style_text_color(updated, uiColor(COLOR_MUTED_TEXT), 0);
  lv_obj_align(updated, LV_ALIGN_TOP_MID, 0, 66);

  lv_obj_t *table = lv_table_create(pricesPanel);
  lv_obj_set_size(table, SCREEN_W, SCREEN_H - 84);
  lv_obj_align(table, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_table_set_col_cnt(table, 2);
  lv_table_set_row_cnt(table, activeSourceCount + 1);
  lv_table_set_col_width(table, 0, 94);
  lv_table_set_col_width(table, 1, 146);
  lv_table_set_cell_value(table, 0, 0, "Source");
  snprintf(text, sizeof(text), "BTC price (%s)", market.code);
  lv_table_set_cell_value(table, 0, 1, text);
  for (uint8_t i = 0; i < activeSourceCount; ++i) {
    lv_table_set_cell_value(table, i + 1, 0, market.sources[activeSourceIndexes[i]].name);
    const float savedRate = isfinite(exchangeRates[i]) && exchangeRates[i] > 0 ? exchangeRates[i] : 0;
    snprintf(text, sizeof(text), "%.2f", savedRate);
    lv_table_set_cell_value(table, i + 1, 1, text);
  }
  lv_obj_set_style_bg_color(table, uiColor(COLOR_BLACK), LV_PART_MAIN);
  lv_obj_set_style_bg_color(table, uiColor(COLOR_CARD_BG), LV_PART_ITEMS);
  lv_obj_set_style_text_color(table, uiColor(COLOR_WHITE), LV_PART_ITEMS);
  lv_obj_set_style_border_color(table, uiColor(COLOR_BTC_ORANGE), LV_PART_ITEMS);
  lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
  lv_obj_set_style_pad_top(table, 1, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(table, 1, LV_PART_ITEMS);
  lv_obj_add_flag(table, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(table, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(table, LV_SCROLLBAR_MODE_AUTO);
}

static lv_obj_t *makeTextArea(lv_obj_t *parent, const char *placeholder, const char *initial) {
  lv_obj_t *area = lv_textarea_create(parent);
  lv_obj_set_width(area, 208);
  lv_textarea_set_one_line(area, true);
  lv_textarea_set_accepted_chars(area, "0123456789.");
  lv_textarea_set_placeholder_text(area, placeholder);
  lv_textarea_set_text(area, initial);
  lv_obj_set_style_bg_color(area, uiColor(COLOR_CARD_BG), 0);
  lv_obj_set_style_text_color(area, uiColor(COLOR_WHITE), 0);
  lv_obj_set_style_text_color(area, uiColor(COLOR_MUTED_TEXT), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_border_color(area, uiColor(COLOR_BTC_ORANGE), 0);
  lv_obj_set_style_border_width(area, 1, 0);
  lv_obj_set_style_radius(area, 6, 0);
  lv_obj_add_event_cb(area, amountEvent, LV_EVENT_ALL, nullptr);
  return area;
}

static void buildUi() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, uiColor(COLOR_BLACK), 0);
  lv_obj_set_style_text_color(screen, uiColor(COLOR_WHITE), 0);
  mainPanel = lv_obj_create(screen);
  lv_obj_set_size(mainPanel, SCREEN_W, SCREEN_H);
  lv_obj_set_pos(mainPanel, 0, 0);
  lv_obj_set_style_bg_color(mainPanel, uiColor(COLOR_BLACK), 0);
  lv_obj_set_style_border_width(mainPanel, 0, 0);
  lv_obj_set_style_pad_all(mainPanel, 0, 0);
  screen = mainPanel;

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "SATulator");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, uiColor(COLOR_BTC_ORANGE), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

  // Three direct controls on one row: fiat currency, conversion direction,
  // and the Bitcoin unit precision.
  fiatButton = lv_btn_create(screen);
  lv_obj_set_size(fiatButton, 70, 38);
  lv_obj_align(fiatButton, LV_ALIGN_TOP_LEFT, 8, 35);
  applyButtonStyle(fiatButton);
  lv_obj_add_event_cb(fiatButton, fiatEvent, LV_EVENT_CLICKED, nullptr);
  fiatLabel = lv_label_create(fiatButton);
  lv_label_set_text(fiatLabel, selectedMarket().code);
  lv_obj_center(fiatLabel);
  lv_obj_set_style_text_color(fiatLabel, uiColor(COLOR_WHITE), 0);

  modeButton = lv_btn_create(screen);
  lv_obj_set_size(modeButton, 70, 38);
  lv_obj_align(modeButton, LV_ALIGN_TOP_LEFT, 85, 35);
  applyButtonStyle(modeButton);
  lv_obj_add_event_cb(modeButton, modeEvent, LV_EVENT_CLICKED, nullptr);
  modeLabel = lv_label_create(modeButton);
  lv_label_set_text(modeLabel, fiatToBitcoin ? "->" : "<-");
  lv_obj_center(modeLabel);
  lv_obj_set_style_text_color(modeLabel, uiColor(COLOR_WHITE), 0);

  unitButton = lv_btn_create(screen);
  lv_obj_set_size(unitButton, 70, 38);
  lv_obj_align(unitButton, LV_ALIGN_TOP_LEFT, 162, 35);
  applyButtonStyle(unitButton);
  lv_obj_add_event_cb(unitButton, unitEvent, LV_EVENT_CLICKED, nullptr);
  unitLabel = lv_label_create(unitButton);
  lv_label_set_text(unitLabel, useSats ? "SATS" : "BTC");
  lv_obj_center(unitLabel);
  lv_obj_set_style_text_color(unitLabel, uiColor(COLOR_WHITE), 0);

  lv_obj_t *amountCaption = lv_label_create(screen);
  lv_label_set_text(amountCaption, "Amount");
  lv_obj_set_style_text_color(amountCaption, uiColor(COLOR_MUTED_TEXT), 0);
  lv_obj_align(amountCaption, LV_ALIGN_TOP_LEFT, 16, 82);
  char initialAmount[24];
  snprintf(initialAmount, sizeof(initialAmount), "%.8g", lastAmount);
  amountArea = makeTextArea(screen,
                            fiatToBitcoin ? "Fiat amount" : (useSats ? "Satoshi amount" : "Bitcoin amount"),
                            initialAmount);
  lv_obj_align(amountArea, LV_ALIGN_TOP_MID, 0, 96);

  lv_obj_t *rateCaption = lv_label_create(screen);
  lv_label_set_text(rateCaption, "Conversion rate");
  lv_obj_set_style_text_color(rateCaption, uiColor(COLOR_MUTED_TEXT), 0);
  lv_obj_align(rateCaption, LV_ALIGN_TOP_LEFT, 14, 143);
  static const char *RATE_LABELS[] = {"Avg", "Median", "Custom"};
  for (uint8_t i = 0; i < 3; ++i) {
    rateButtons[i] = lv_btn_create(screen);
    lv_obj_set_size(rateButtons[i], 70, 38);
    lv_obj_align(rateButtons[i], LV_ALIGN_TOP_LEFT, i == 0 ? 8 : i == 1 ? 85 : 162, 162);
    applyRadioStyle(rateButtons[i]);
    lv_obj_add_event_cb(rateButtons[i], rateChoiceEvent, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    lv_obj_t *label = lv_label_create(rateButtons[i]);
    lv_label_set_text(label, RATE_LABELS[i]);
    lv_obj_set_style_text_color(label, uiColor(COLOR_WHITE), 0);
    lv_obj_center(label);
  }

  mainRateLabel = lv_label_create(screen);
  lv_obj_set_style_text_color(mainRateLabel, uiColor(COLOR_MUTED_TEXT), 0);
  lv_obj_set_width(mainRateLabel, 224);
  lv_obj_set_style_text_align(mainRateLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(mainRateLabel, LV_ALIGN_TOP_MID, 0, 248);

  lv_obj_t *pricesButton = lv_btn_create(screen);
  lv_obj_set_size(pricesButton, 70, 38);
  lv_obj_align(pricesButton, LV_ALIGN_TOP_LEFT, 8, 207);
  applyButtonStyle(pricesButton);
  lv_obj_add_event_cb(pricesButton, showPricesScreen, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *pricesLabel = lv_label_create(pricesButton);
  lv_label_set_text(pricesLabel, "Prices");
  lv_obj_set_style_text_color(pricesLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(pricesLabel);

  lv_obj_t *refreshButton = lv_btn_create(screen);
  lv_obj_set_size(refreshButton, 70, 38);
  lv_obj_align(refreshButton, LV_ALIGN_TOP_LEFT, 85, 207);
  applyButtonStyle(refreshButton);
  lv_obj_add_event_cb(refreshButton, refreshRatesEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *refreshLabel = lv_label_create(refreshButton);
  lv_label_set_text(refreshLabel, "Refresh");
  lv_obj_set_style_text_color(refreshLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(refreshLabel);

  lv_obj_t *configButton = lv_btn_create(screen);
  lv_obj_set_size(configButton, 70, 38);
  lv_obj_align(configButton, LV_ALIGN_TOP_LEFT, 162, 207);
  applyDangerStyle(configButton);
  lv_obj_add_event_cb(configButton, showConfigurationScreen, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *configLabel = lv_label_create(configButton);
  lv_label_set_text(configLabel, "Config");
  lv_obj_set_style_text_color(configLabel, uiColor(COLOR_WHITE), 0);
  lv_obj_center(configLabel);

  resultLabel = lv_label_create(screen);
  lv_obj_set_style_text_font(resultLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(resultLabel, uiColor(COLOR_BTC_ORANGE), 0);
  lv_obj_align(resultLabel, LV_ALIGN_TOP_MID, 0, 292);

  // Keep the keypad at root level so it can be reused by the custom-rate page.
  keyboard = lv_keyboard_create(lv_scr_act());
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_set_size(keyboard, SCREEN_W, 130);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard, keyboardEvent, LV_EVENT_ALL, nullptr);
  loadSavedOrZeroRates();
  if (!customRateConfigured) customRate = medianRate();
  refreshRateChoiceButtons();
  updateRateLabels();
  updateConversion();
  if (WiFi.status() != WL_CONNECTED) showConnectionScreen();
}

static void drawBitcoinLogo(int16_t centerX, int16_t centerY) {
  tft.setSwapBytes(true);
  tft.pushImage(centerX - 56, centerY - 56, 112, 112,
                reinterpret_cast<const uint16_t *>(assets_bitcoin_logo_rgb565));
  tft.setSwapBytes(false);
}

static void showSplash() {
  tft.fillScreen(TFT_BLACK);
  const uint16_t bitcoinOrange = tft.color565(0xF7, 0x93, 0x1A);
  const uint16_t mutedText = tft.color565(0x9C, 0xA3, 0xAF);
  const uint16_t cardBackground = tft.color565(0x1E, 0x22, 0x2A);
  drawBitcoinLogo(SCREEN_W / 2, 78);
  tft.fillRoundRect(10, 140, SCREEN_W - 20, 137, 8, cardBackground);
  tft.setTextColor(bitcoinOrange, cardBackground);
  tft.drawCentreString("SATulator", SCREEN_W / 2, 146, 4);
  tft.setTextColor(TFT_WHITE, cardBackground);
  tft.drawCentreString("Bitcoin & Fiat Converter", SCREEN_W / 2, 178, 1);
  tft.drawCentreString("Multiple sources | Average | Median", SCREEN_W / 2, 194, 1);
  tft.setTextColor(mutedText, cardBackground);
  tft.drawCentreString("License: MIT", SCREEN_W / 2, 222, 1);
  tft.drawCentreString("Copyright (C) 2026 Valerio Vaccaro", SCREEN_W / 2, 238, 1);
  tft.drawCentreString("Connecting to Wi-Fi...", SCREEN_W / 2, 260, 1);
}

static void requestNtpSync() {
  if (WiFi.status() != WL_CONNECTED || ntpRequested) return;
  // Italy / Central European time, including daylight-saving transitions.
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
  ntpRequested = true;
  Serial.println("NTP synchronization requested");
}

static void updateClockIfSynced() {
  if (millis() - lastClockCheckMs < 1000) return;
  lastClockCheckMs = millis();
  requestNtpSync();
  if (WiFi.status() == WL_CONNECTED && connectionPanel) showMainScreen(nullptr);
  if (WiFi.status() == WL_CONNECTED && !startupRefreshDone) {
    refreshRatesEvent(nullptr);
    startupRefreshDone = true;
  }
  const time_t now = time(nullptr);
  if (now > 1700000000 && lastRateUpdateEpoch == 0) {
    // Repeat the startup refresh after NTP is valid, so its timestamp is real.
    refreshRatesEvent(nullptr);
  }
}

static void connectWiFi() {
  wifiPortal.setConfigPortalBlocking(false);
  wifiPortal.setConfigPortalTimeout(180);
  wifiPortal.setConnectTimeout(20);
  wifiPortal.setAPCallback([](WiFiManager *) {
    Serial.println("Captive portal started: connect to SATulator-Setup");
  });
  if (wifiPortal.autoConnect("SATulator-Setup")) {
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
    requestNtpSync();
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("SATulator boot");
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.init();
  tft.setRotation(0);
  touchSpi.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);
  loadBoardState();
  Serial.println("Using fixed touch mapping");
  tft.fillScreen(TFT_BLACK);
  showSplash();

  connectWiFi();
  delay(3500); // Keep the title and licensing information readable at startup.

  lv_init();
  lv_disp_draw_buf_init(&drawBuffer, buf, nullptr, SCREEN_W * 12);
  static lv_disp_drv_t display;
  lv_disp_drv_init(&display);
  display.hor_res = SCREEN_W;
  display.ver_res = SCREEN_H;
  display.flush_cb = displayFlush;
  display.draw_buf = &drawBuffer;
  lv_disp_drv_register(&display);

  static lv_indev_drv_t touch;
  lv_indev_drv_init(&touch);
  touch.type = LV_INDEV_TYPE_POINTER;
  touch.read_cb = touchRead;
  lv_indev_drv_register(&touch);
  buildUi();
  lastLvglTick = millis();
}

void loop() {
  wifiPortal.process();
  updateClockIfSynced();
  // LVGL's timers and input-device polling are driven by this monotonic tick.
  // Without it the initial page renders, but no later touch event is handled.
  const uint32_t now = millis();
  lv_tick_inc(now - lastLvglTick);
  lastLvglTick = now;
  lv_timer_handler();
  delay(5);
}
