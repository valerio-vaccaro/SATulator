#pragma once

#include <Arduino.h>

// Live exchange configuration derived from the fiat-converter project.
// Toggle `enabled` to select the sources used for average and median rates.
enum class PriceParser : uint8_t {
  Kraken, Coinbase, Bitstamp, Bitfinex, CexIo, KuCoin, Okx, BitFlyer,
  Gemini, Binance, Bybit, Bitget, Coincheck, Bitbank
};

struct MarketSourceConfig {
  const char *name;
  bool enabled;
  const char *url;
  PriceParser parser;
  float prototypeOffset; // Offline fallback only.
};

struct FiatMarketConfig {
  const char *code;
  float prototypeBaseRate;
  const MarketSourceConfig *sources;
  uint8_t sourceCount;
};

static constexpr MarketSourceConfig EUR_SOURCES[] = {
  {"Kraken", true, "https://api.kraken.com/0/public/Ticker?pair=XBTEUR", PriceParser::Kraken, 0.0000F},
  {"Coinbase", true, "https://api.coinbase.com/v2/prices/BTC-EUR/spot", PriceParser::Coinbase, -0.0018F},
  {"Bitstamp", true, "https://www.bitstamp.net/api/v2/ticker/btceur/", PriceParser::Bitstamp, 0.0012F},
  {"Bitfinex", true, "https://api-pub.bitfinex.com/v2/ticker/tBTCEUR", PriceParser::Bitfinex, -0.0025F},
  {"CEX.IO", true, "https://cex.io/api/ticker/BTC/EUR", PriceParser::CexIo, 0.0031F},
  {"KuCoin", true, "https://api.kucoin.com/api/ua/v1/market/ticker?tradeType=SPOT&symbol=BTC-EUR", PriceParser::KuCoin, -0.0011F},
  {"OKX", true, "https://www.okx.com/api/v5/market/ticker?instId=BTC-EUR", PriceParser::Okx, 0.0007F},
  {"bitFlyer", true, "https://api.bitflyer.com/v1/ticker?product_code=BTC_EUR", PriceParser::BitFlyer, -0.0033F},
};

static constexpr MarketSourceConfig USD_SOURCES[] = {
  {"Kraken", true, "https://api.kraken.com/0/public/Ticker?pair=XBTUSD", PriceParser::Kraken, 0.0000F},
  {"Coinbase", true, "https://api.coinbase.com/v2/prices/BTC-USD/spot", PriceParser::Coinbase, -0.0018F},
  {"Bitstamp", true, "https://www.bitstamp.net/api/v2/ticker/btcusd/", PriceParser::Bitstamp, 0.0012F},
  {"Gemini", true, "https://api.gemini.com/v2/ticker/btcusd", PriceParser::Gemini, -0.0022F},
  {"Bitfinex", true, "https://api-pub.bitfinex.com/v2/ticker/tBTCUSD", PriceParser::Bitfinex, 0.0026F},
  {"CEX.IO", true, "https://cex.io/api/ticker/BTC/USD", PriceParser::CexIo, 0.0031F},
  {"Binance", true, "https://data-api.binance.vision/api/v3/ticker/price?symbol=BTCUSDT", PriceParser::Binance, -0.0011F},
  {"KuCoin", true, "https://api.kucoin.com/api/ua/v1/market/ticker?tradeType=SPOT&symbol=BTC-USDT", PriceParser::KuCoin, 0.0019F},
  {"Bybit", true, "https://api.bybit.com/v5/market/tickers?category=spot&symbol=BTCUSDT", PriceParser::Bybit, -0.0008F},
  {"OKX", true, "https://www.okx.com/api/v5/market/ticker?instId=BTC-USD", PriceParser::Okx, 0.0007F},
  {"Bitget", true, "https://api.bitget.com/api/v3/market/tickers?category=SPOT&symbol=BTCUSDT", PriceParser::Bitget, -0.0033F},
};

static constexpr MarketSourceConfig CHF_SOURCES[] = {
  {"Kraken", true, "https://api.kraken.com/0/public/Ticker?pair=XBTCHF", PriceParser::Kraken, 0.0000F},
  {"Coinbase", true, "https://api.coinbase.com/v2/prices/BTC-CHF/spot", PriceParser::Coinbase, -0.0018F},
};

static constexpr MarketSourceConfig GBP_SOURCES[] = {
  {"Kraken", true, "https://api.kraken.com/0/public/Ticker?pair=XBTGBP", PriceParser::Kraken, 0.0000F},
  {"Coinbase", true, "https://api.coinbase.com/v2/prices/BTC-GBP/spot", PriceParser::Coinbase, -0.0018F},
  {"Bitstamp", true, "https://www.bitstamp.net/api/v2/ticker/btcgbp/", PriceParser::Bitstamp, 0.0012F},
  {"Bitfinex", true, "https://api-pub.bitfinex.com/v2/ticker/tBTCGBP", PriceParser::Bitfinex, -0.0025F},
  {"CEX.IO", true, "https://cex.io/api/ticker/BTC/GBP", PriceParser::CexIo, 0.0031F},
};

static constexpr MarketSourceConfig JPY_SOURCES[] = {
  {"Kraken", true, "https://api.kraken.com/0/public/Ticker?pair=XBTJPY", PriceParser::Kraken, 0.0000F},
  {"bitFlyer", true, "https://api.bitflyer.com/v1/ticker?product_code=BTC_JPY", PriceParser::BitFlyer, -0.0018F},
  {"Coincheck", true, "https://coincheck.com/api/ticker?pair=btc_jpy", PriceParser::Coincheck, 0.0012F},
  {"bitbank", true, "https://public.bitbank.cc/btc_jpy/ticker", PriceParser::Bitbank, -0.0025F},
};

static constexpr FiatMarketConfig FIAT_MARKETS[] = {
  {"EUR", 95100.0F, EUR_SOURCES, 8},
  {"USD", 103420.0F, USD_SOURCES, 11},
  {"CHF", 92700.0F, CHF_SOURCES, 2},
  {"GBP", 81400.0F, GBP_SOURCES, 5},
  {"JPY", 15960000.0F, JPY_SOURCES, 4},
};

static constexpr uint8_t FIAT_MARKET_COUNT = sizeof(FIAT_MARKETS) / sizeof(FIAT_MARKETS[0]);
// Each supported fiat can persist up to 21 configured exchange rates plus
// its own last-update timestamp in board flash.
static constexpr uint8_t MAX_MARKET_SOURCES = 21;
