# Binance Futures Zorro Plugin
Binance Futures Plugin for Zorro Trader

# Instructions

- Build as the x86 (32 bit) architecture & place the output dynamic library into Zorro/Plugin folder.
- Plugin logs all issues into Zorro/Log/binance_futures.log file.

# Custom exported functions

- ```DLLFUNC_C int AssetMinuteCandle(char *Asset, int previous, T6 *candle);```
- ```DLLFUNC_C int AssetMinuteCandleREST(char *Asset, T6 *candles, int maxCandles);```
- ```DLLFUNC_C int GetLastFundingRate(char *Asset, double *fundingTime, double *fundingRate);```
- ```DLLFUNC_C int PreloadMinuteCandles(char **Assets, int numAssets, int numCandles);```
- ```DLLFUNC_C int GetPreloadedMinuteCandles(char *Asset, T6 *candles, int maxCandles, int &numRead);```
- ```DLLFUNC_C int GetMaxPositionValue(char *Asset, double *amount);```
- ```DLLFUNC_C int GetPositionLimits(char *Asset, double *lotSize, double *marketLotSize);```
- ```DLLFUNC_C int ChangeInitialLeverage(char *Asset, int targetLeverage, int *leverage, double *maxNotionalValue);```

# Dependencies

- https://github.com/gabime/spdlog
- https://github.com/vitakot/binance_cpp_api.git
- https://github.com/boostorg/boost
- https://github.com/nlohmann/json