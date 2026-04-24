/**
Binance Futures Zorro Plugin

Licensed under the MIT License <http://opensource.org/licenses/MIT>.
SPDX-License-Identifier: MIT
Copyright (c) 2025 Vitezslav Kot <vitezslav.kot@stonky.cz>, Stonky s.r.o.
*/

#include "stonky/binance/binance_futures_rest_client.h"
#include "stonky/binance/binance_ws_stream_manager.h"
#include "stonky/binance/binance.h"
#include "stonky/utils/utils.h"
#include "stonky/utils/registry.h"
#include "binance_futures.h"
#include <wtypes.h>
#include <string>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <algorithm>
#include <fstream>
#include "magic_enum/magic_enum.hpp"

#define PLUGIN_VERSION    2
#define PLUGIN_VERSION_STR "1.0.0"
#define PLUGIN_VERSION_RELEASE_DATE "13-november-2025"
#undef min

#define ZORRO_REG_KEY "SOFTWARE\\Zorro"
#define LAST_ORDER_ID_KEY "BinanceLastOrderId"
#define OPEN_TRADES_FILE R"(./Data/binance_open_trades.json)"

using namespace std::chrono_literals;
using namespace stonky::binance;

static std::string currentSymbol;
static std::string accountCurrency;
static int lastOrderId = 0;
static int orderType = 0;
static bool hedge = false;
static double lotAmount = 1.0;
static int loopMs = 50; // Actually unused
static int waitMs = 30000; // Actually unused
std::atomic exchangeUpdaterRunning = false;
std::thread exchangeUpdater;

std::shared_ptr<futures::RESTClient> restClient;
std::unique_ptr<futures::WSStreamManager> streamManager;

std::map<std::string, futures::EventCandlestick> lastBars;
std::map<std::string, std::vector<Candle> > lastCandles;

// #define EXPERIMENTAL

#ifdef _WIN64
std::map<std::string, double> lotAmounts;
#endif

enum ExchangeStatus {
	Unavailable = 0,
	Closed,
	Open
};

__int64 convertTime(const DATE date) {
	return static_cast<__int64>((date - 25569.) * 24. * 60. * 60.);
}

DATE convertTime(const __int64 t64) {
	if (t64 == 0) return 0.;
	return (25569. + static_cast<double>(t64 / 1000) / (24. * 60. * 60.));
}

DLLFUNC_C int BrokerOpen(char *Name, FARPROC fpError, FARPROC fpProgress) {
	strcpy_s(Name, 32, "BinanceFutures");
	reinterpret_cast<FARPROC &>(BrokerError) = fpError;
	reinterpret_cast<FARPROC &>(BrokerProgress) = fpProgress;

	return PLUGIN_VERSION;
}

void logFunction(const stonky::LogSeverity severity, const std::string &errmsg) {
	switch (severity) {
		case stonky::LogSeverity::Info:
			spdlog::info(errmsg);
			break;
		case stonky::LogSeverity::Warning:
			spdlog::warn(errmsg);
			break;
		case stonky::LogSeverity::Critical:
			spdlog::critical(errmsg);
			break;
		case stonky::LogSeverity::Error:
			spdlog::error(errmsg);
			break;
		case stonky::LogSeverity::Debug:
			spdlog::debug(errmsg);
			break;
		case stonky::LogSeverity::Trace:
			spdlog::trace(errmsg);
			break;
	}
}

void exchangeUpdaterFunc() {
	exchangeUpdaterRunning = true;
	int numPass = 60;

	std::unique_ptr<futures::RESTClient> restClientUpdater;

	while (exchangeUpdaterRunning) {
		if (numPass == 60) {
			numPass = 0;
			try {
				if (!restClientUpdater) {
					restClientUpdater = std::make_unique<futures::RESTClient>("", "");
				}

				if (restClientUpdater) {
					restClient->setExchangeInfo(restClientUpdater->getExchangeInfo(true));
				}
			} catch (std::exception &e) {
				spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
				spdlog::info(fmt::format("Resetting restClientUpdater, {}", MAKE_FILELINE));
				restClientUpdater = std::make_unique<futures::RESTClient>("", "");
			}
		}
		std::this_thread::sleep_for(1s);
		numPass++;
	}
}

void startExchangeUpdater() {
	exchangeUpdater = std::thread(&exchangeUpdaterFunc);
}

void stopExchangeUpdater() {
	exchangeUpdaterRunning = false;

	if (exchangeUpdater.joinable()) {
		exchangeUpdater.join();
	}
}

void writeLastOrderId() {
	if (const bool success = stonky::writeInRegistry(HKEY_CURRENT_USER, ZORRO_REG_KEY, LAST_ORDER_ID_KEY, lastOrderId); !
		success) {
		spdlog::error("Cannot store BinanceLastOrderId");
	}
}

void readLastOrderId() {
	DWORD bnbLastOrderId;
	const bool success = stonky::readDwordValueRegistry(HKEY_CURRENT_USER, ZORRO_REG_KEY, LAST_ORDER_ID_KEY,
	                                                &bnbLastOrderId);
	if (success && bnbLastOrderId != 0) {
		lastOrderId = static_cast<int>(bnbLastOrderId);
	} else {
		time_t Time;
		time(&Time);
		lastOrderId = static_cast<int>(Time);
		writeLastOrderId();
	}
}

std::string findAssetForTradeId(int tradeId, bool erase = true) {
	if (std::ifstream ifs(OPEN_TRADES_FILE); ifs.is_open()) {
		nlohmann::json json;
		json = nlohmann::json::parse(ifs);
		ifs.close();

		if (auto it = json.find("openTrades"); it != json.end()) {
			std::map<int, std::string> openTrades;
			openTrades = it->get<std::map<int, std::string> >();

			if (const auto tradeIt = openTrades.find(tradeId); tradeIt != openTrades.end()) {
				auto retVal = tradeIt->second;

				if (erase) {
					openTrades.erase(tradeIt);

					json["openTrades"] = openTrades;

					if (std::ofstream ofs(OPEN_TRADES_FILE); ofs.is_open()) {
						ofs << json.dump(4);
						ofs.close();
					} else {
						spdlog::error(fmt::format("Couldn't save json file, path: {}, {}", OPEN_TRADES_FILE, MAKE_FILELINE));
					}
				}

				return retVal;
			}
		}
	}
	spdlog::error(fmt::format("Could not find Asset for trade id: {}, {}", tradeId, MAKE_FILELINE));
	return {};
}

void saveAssetForTradeId(const std::string &asset, int tradeId) {
	try {
		std::ifstream ifs(OPEN_TRADES_FILE);
		nlohmann::json json;
		std::map<int, std::string> openTrades;

		if (ifs.is_open()) {
			json = nlohmann::json::parse(ifs);
			ifs.close();
		}

		if (auto it = json.find("openTrades"); it != json.end()) {
			openTrades = it->get<std::map<int, std::string> >();
			openTrades.insert_or_assign(tradeId, asset);
		} else {
			openTrades.insert_or_assign(tradeId, asset);
		}
		json["openTrades"] = openTrades;

		if (std::ofstream ofs(OPEN_TRADES_FILE); ofs.is_open()) {
			ofs << json.dump(4);
			ofs.close();
		} else {
			spdlog::error(fmt::format("Couldn't save json file, path: {}, {}", OPEN_TRADES_FILE, MAKE_FILELINE));
		}
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
	}
}

DLLFUNC_C int BrokerLogin(char *User, char *Pwd, char *Type, char *Account) {
	if (!User) {
		stopExchangeUpdater();
		streamManager.reset();
		restClient.reset();
		spdlog::info("Logout");
		spdlog::shutdown();
		return 1;
	}
	if (static_cast<std::string>(Type) == "Demo") {
		const auto msg = "Demo mode not supported by this plugin.";
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
		BrokerError(msg);
		return 0;
	}
	if (!restClient) {
		const auto logger = spdlog::basic_logger_mt("binance_logger", R"(./Log/binance_futures.log)");
		spdlog::set_default_logger(logger);
		spdlog::flush_on(spdlog::level::info);
		logger->set_pattern("%+", spdlog::pattern_time_type::utc);

		if (!std::string_view(User).empty() && !std::string_view(Pwd).empty()) {
			restClient = std::make_shared<futures::RESTClient>(User, Pwd);
			startExchangeUpdater();
			readLastOrderId();
			spdlog::info("Logged into account: " + std::string(Account));
			const std::string msg = "Plugin version: " + std::string(PLUGIN_VERSION_STR) + ",  release date: " +
			                        std::string(PLUGIN_VERSION_RELEASE_DATE);
			spdlog::info("Plugin version: " + msg);
			BrokerError(msg.c_str());
		} else {
			const auto msg = "Missing or Incomplete Account credentials.";
			spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
			BrokerError(msg);
			return 0;
		}
	} else {
		restClient->setCredentials(User, Pwd);
	}
	try {
		if (!streamManager) {
			streamManager = std::make_unique<futures::WSStreamManager>(restClient);
			streamManager->setLoggerCallback(&logFunction);
		}
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
		return 0;
	}

	try {
		const auto account = restClient->getAccountInfo();

		if (const auto positionMode = restClient->getPositionMode(); positionMode == PositionMode::Hedge) {
			hedge = true;
		}

		return 1;
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
		return 0;
	}
}

DLLFUNC_C int
BrokerAsset(char *Asset, double *pPrice, double *pSpread, double *pVolume, double *pPip, double *pPipCost,
            double *pLotAmount, double *pMarginCost, double *pRollLong, double *pRollShort) {
	const auto currentS = std::time(nullptr);
	const auto currentMinute = stonky::getMsTimestamp(stonky::currentTime()).count() / 60000;

	/// NOTE: Do not log normal state, this function is called ver often!
	if (!streamManager) {
		spdlog::critical(fmt::format("{}: {}", MAKE_FILELINE, "Binance WS stream manager instance not initialized."));
		return 0;
	}

	if (pPip != nullptr) {
		try {
			for (const auto exchangeInfo = restClient->getExchangeInfo(); const auto &el: exchangeInfo.symbols) {
				if (el.symbol == Asset) {
					for (const auto &fEl: el.filters) {
						if (fEl.filterType == futures::SymbolFilter::LOT_SIZE) {
							if (pLotAmount) {
								*pLotAmount = fEl.stepSize;
#ifdef _WIN64
								lotAmounts.insert_or_assign(Asset, fEl.stepSize);
#endif
							}
						} else if (fEl.filterType == futures::SymbolFilter::PRICE_FILTER) {
							*pPip = fEl.tickSize;
						}
					}

					if (pPipCost && *pPip != 0.0 && *pLotAmount != 0.0) {
						*pPipCost = *pPip * *pLotAmount;
					}
				}
			}
		} catch (std::exception &e) {
			spdlog::error(fmt::format("{}: {}\n", MAKE_FILELINE, e.what()));
			BrokerError("Cannot acquire asset info from server.");
		}
	}

	try {
		/// Check if the Book Ticker Stream is subscribed for the Asset
		streamManager->subscribeBookTickerStream(Asset);

#ifdef EXPERIMENTAL
		/// TODO: Candle interval must correspond to BAR size (but how to set it?)
		streamManager->subscribeCandlestickStream(Asset, CandleInterval::_1m);

		auto candlestick = streamManager->readEventCandlestick(Asset, CandleInterval::_1m);

		if (candlestick) {
			const auto candleMinute = candlestick.value().m_k.m_t / 60000;
			auto timeStruct = std::gmtime(&currentS);
			if (timeStruct->tm_sec == 0) {
				if (currentMinute == candleMinute) {
					/// Time mismatch - take previous candle
					const auto it = lastBars.find(Asset);
					if (it != lastBars.end()) {
						candlestick.emplace(it->second);
					}
				}
			} else {
				if (currentMinute == candleMinute) {
					lastBars.insert_or_assign(Asset, candlestick.value());
				}
			}
		}

#endif

		if (const auto tickPrice = streamManager->readEventTickPrice(Asset)) {
			const auto &tickerPrice = *tickPrice;

			if (tickerPrice.a == 0.0 || tickerPrice.b == 0.0) {
				return 0;
			}

			if (pPrice) {
				*pPrice = tickerPrice.a;
			}
			if (pSpread) {
				*pSpread = tickerPrice.a - tickerPrice.b;
			}
			if (pVolume) {
#ifdef EXPERIMENTAL
				if (candlestick) {
					*pVolume = candlestick->m_k.m_v;
				} else {
					*pVolume = tickerPrice.m_A + tickerPrice.m_B;
				}
#else
				*pVolume = tickerPrice.A + tickerPrice.B;
#endif
			}

			return 1;
		}
		const auto msg = "Could not read Book Ticker Stream for Asset: " + std::string(Asset) + ", reading timeout";
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
		BrokerError("Cannot acquire asset info from server.");
	}

	return 0;
}

DLLFUNC_C int BrokerAccount(char *Account, double *pdBalance, double *pdTradeVal, double *pdMarginVal) {
	if (!restClient) {
		spdlog::critical(fmt::format("{}: {}", MAKE_FILELINE, "Binance Rest Client instance not initialized."));
		return 0;
	}

	if (!Account || !*Account) {
		accountCurrency = "USDT";
	} else {
		accountCurrency = Account;
	}

	try {
		const auto accountBalances = restClient->getAccountBalances();

		if (pdBalance) {
			for (const auto &el: accountBalances) {
				if (el.asset == accountCurrency) {
					*pdBalance = std::round(el.balance);
					return 1;
				}
			}
		}
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
		BrokerError("Cannot acquire account info from server.");
	}

	return 0;
}

DLLFUNC_C int BrokerTime(DATE *pTimeGMT) {
	if (!restClient) {
		spdlog::critical(fmt::format("{}: {}", MAKE_FILELINE, "Binance Rest Client instance not initialized."));
		return ExchangeStatus::Unavailable;
	}

#ifdef ENABLE_SERVER_TIME
	// Off by default, to save time, the response takes roughly 0.35 s
	std::int64_t timeInMs = restClient->getServerTime();

	if (pTimeGMT) {
		*pTimeGMT = convertTime(timeInMs);
	}
#endif

	/// Binance never closes
	return ExchangeStatus::Open;
}

DLLFUNC_C int GetLastFundingRate(char *Asset, double *fundingTime, double *fundingRate) {
	try {
		const auto rate = restClient->getLastFundingRate(Asset);
		if (fundingRate && fundingTime) {
			*fundingTime = convertTime(rate.fundingTime);
			*fundingRate = rate.fundingRate;
			return 1;
		}
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
	}
	return 0;
}

DLLFUNC_C int AssetMinuteCandleREST(char *Asset, T6 *candles, int maxCandles) {
	if (candles) {
		const std::time_t from = std::time(nullptr) * 1000 - maxCandles * 60000;
		const std::time_t to = std::time(nullptr) * 1000;

		try {
			const auto bnbCandles = restClient->getHistoricalPrices(Asset, CandleInterval::_1m, from, to, maxCandles);
			const auto maxElements = std::min(maxCandles, static_cast<int>(bnbCandles.size()));

			for (auto i = 0; i < maxElements; i++) {
				candles[i].fOpen = bnbCandles[i].open;
				candles[i].fHigh = bnbCandles[i].high;
				candles[i].fLow = bnbCandles[i].low;
				candles[i].fClose = bnbCandles[i].close;
				candles[i].fVol = bnbCandles[i].volume;
				candles[i].time = convertTime(bnbCandles[i].closeTime);
			}
			return 1;
		} catch (std::exception &e) {
			spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
		}
	}

	return 0;
}

DLLFUNC_C int AssetMinuteCandle(char *Asset, int previous, T6 *candle) {
	if (candle) {
		if (const auto candleEvent = streamManager->readEventCandlestick(
			Asset, CandleInterval::_1m, static_cast<bool>(previous))) {
			candle->fVol = candleEvent->k.v;
			candle->fOpen = candleEvent->k.o;
			candle->fHigh = candleEvent->k.h;
			candle->fLow = candleEvent->k.l;
			candle->fClose = candleEvent->k.c;
			candle->time = convertTime(candleEvent->k.T);
			return 1;
		}
	}

	return 0;
}

DLLFUNC_C int PreloadMinuteCandles(char **Assets, int numAssets, int numCandles) {
	if (!Assets) {
		return 0;
	}

	if (!restClient) {
		spdlog::critical(fmt::format("{}: {}", MAKE_FILELINE, "Binance Rest Client instance not initialized."));
		return 0;
	}

	std::vector<std::string> symbols;

	for (auto i = 0; i < numAssets; i++) {
		symbols.emplace_back(Assets[i]);
	}

	const std::time_t from = std::time(nullptr) * 1000 - numCandles * 60000;
	const std::time_t to = std::time(nullptr) * 1000;

	try {
		lastCandles = restClient->getHistoricalPrices(symbols, CandleInterval::_1m, from, to, numCandles);
		return 1;
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
	}
	return 0;
}

DLLFUNC_C int GetPreloadedMinuteCandles(char *Asset, T6 *candles, int maxCandles, int &numRead) {
	if (const auto it = lastCandles.find(Asset); it != lastCandles.end()) {
		const auto maxElements = std::min(maxCandles, static_cast<int>(it->second.size()));
		numRead = maxElements;

		for (auto i = 0; i < maxElements; i++) {
			candles[i].fOpen = it->second[i].open;
			candles[i].fHigh = it->second[i].high;
			candles[i].fLow = it->second[i].low;
			candles[i].fClose = it->second[i].close;
			candles[i].fVol = it->second[i].volume;
			candles[i].time = convertTime(it->second[i].closeTime);
		}
		return 1;
	}
	return 0;
}

DLLFUNC_C int GetMaxPositionValue(char *Asset, double *amount) {
	try {
		const auto positionRisk = restClient->getPositionRisk(Asset);

		if (positionRisk.empty()) {
			spdlog::critical(fmt::format("{}: {}\n", MAKE_FILELINE, "Unknown error"));
			return 0;
		}

		if (amount) {
			*amount = positionRisk[0].maxNotionalValue;
			return 1;
		}
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}\n", MAKE_FILELINE, e.what()));
	}

	return 0;
}

DLLFUNC_C int GetPositionLimits(char *Asset, double *lotSize, double *marketLotSize) {
	try {
		for (const auto exchangeInfo = restClient->getExchangeInfo(); const auto &el: exchangeInfo.symbols) {
			if (el.symbol == Asset) {
				for (const auto &fEl: el.filters) {
					if (fEl.filterType == futures::SymbolFilter::LOT_SIZE) {
						if (lotSize) {
							*lotSize = fEl.maxQty;
						}
					} else if (fEl.filterType == futures::SymbolFilter::MARKET_LOT_SIZE) {
						if (marketLotSize) {
							*marketLotSize = fEl.maxQty;
						}
					}
				}
			}
		}
		return 1;
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}\n", MAKE_FILELINE, e.what()));
		BrokerError("Cannot acquire asset info from server.");
	}

	return 0;
}

DLLFUNC_C int ChangeInitialLeverage(char *Asset, int targetLeverage, int *leverage, double *maxNotionalValue) {
	try {
		const auto [fst, snd] = restClient->changeInitialLeverage(Asset, targetLeverage);

		if (leverage) {
			*leverage = fst;
		}

		if (maxNotionalValue) {
			*maxNotionalValue = snd;
		}

		return 1;
	} catch (std::exception &e) {
		std::string msg = "Cannot change initial leverage";
		spdlog::error(fmt::format("{}, {}: {}\n", msg, MAKE_FILELINE, e.what()));
		BrokerError(msg.c_str());
	}

	return 0;
}

DLLFUNC_C int BrokerHistory2(char *Asset, DATE tStart, DATE tEnd, int nTickMinutes, int nTicks, T6 *ticks) {
	if (!Asset || !ticks || !nTicks) {
		return 0;
	}

	if (!restClient) {
		spdlog::critical(fmt::format("{}: {}", MAKE_FILELINE, "Binance Rest Client instance not initialized."));
		return 0;
	}

	try {
		auto candleInterval = CandleInterval::_1m;

		if (!Binance::isValidCandleResolution(nTickMinutes, candleInterval)) {
			std::string msg = "Invalid data resolution: " + std::to_string(nTickMinutes) + " minutes.";
			spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
			BrokerError(msg.c_str());
			return 0;
		}

		const auto msInInterval = Binance::numberOfMsForCandleInterval(candleInterval);
		auto candles = restClient->getHistoricalPrices(Asset, candleInterval, convertTime(tStart) * 1000 - msInInterval,
		                                               convertTime(tEnd) * 1000 - msInInterval, -1);

		if (candles.empty()) {
			std::string msg = "No historical data.";
			spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
			BrokerError(msg.c_str());
			return 0;
		}

		const auto maxCandles = std::min(nTicks, static_cast<int>((candles).size()));
		std::ranges::reverse(candles);

		/// From most recent to oldest.
		for (int i = 0; i < maxCandles; i++, ticks++) {
			ticks->fOpen = static_cast<float>(candles[i].open);
			ticks->fHigh = static_cast<float>(candles[i].high);
			ticks->fLow = static_cast<float>(candles[i].low);
			ticks->fClose = static_cast<float>(candles[i].close);
			ticks->fVol = static_cast<float>(candles[i].volume);

			/// Zorro uses reversed order in time series so that's why...
			ticks->time = convertTime(candles[i].openTime + msInInterval);
		}

		return maxCandles;
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
		BrokerError("Cannot acquire historical data from server.");
	}

	return 0;
}

DLLFUNC_C int BrokerBuy2(char *Asset, int Amount, double dStopDist, double Limit, double *pPrice, int *pFill) {
	if (!restClient) {
		spdlog::critical(fmt::format("{}: {}", MAKE_FILELINE, "Binance Rest Client instance not initialized."));
		return 0;
	}

	try {
#ifdef _WIN64
		if (auto it = lotAmounts.find(Asset); it != lotAmounts.end()) {
			lotAmount = it->second;
		} else {
			std::string msg = "Cannot find lot amount size for asset: " + std::string(Asset);
			spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
			return 0;
		}
#endif
		spdlog::info("New Order for asset: " + std::string(Asset) + ", amount: " + std::to_string(Amount) + ", size: " +
		             std::to_string(lotAmount * std::abs(Amount)) + ", stop dist:" + std::to_string(Limit) +
		             ", limit: " +
		             std::to_string(Limit));

		futures::Order order;
		order.symbol = Asset;

		if (Amount > 0) {
			order.side = Side::BUY;
		} else {
			order.side = Side::SELL;
		}

		hedge
			? (Amount > 0
				   ? order.positionSide = futures::PositionSide::LONG
				   : order.positionSide = futures::PositionSide::SHORT)
			: order.positionSide = futures::PositionSide::BOTH;

		if (orderType == 1) {
			order.timeInForce = TimeInForce::IOC;
		} else if (orderType == 2) {
			order.timeInForce = TimeInForce::GTC;
		} else {
			order.timeInForce = TimeInForce::FOK;
		}

		if (Limit > 0.) {
			order.price = Limit;
			order.type = futures::OrderType::LIMIT;
		} /*else if (dStopDist != 0.0 && dStopDist != -1) {
            order.m_price = Limit;
            order.m_stopPrice = dStopDist;
            order.m_type = futures::OrderType::STOP;
        } */ else {
			order.type = futures::OrderType::MARKET;
		}

		order.quantity = lotAmount * std::abs(Amount);
		order.newOrderRespType = OrderRespType::RESULT;

		readLastOrderId();
		order.newClientOrderId = std::to_string(lastOrderId++);
		writeLastOrderId();

		if (futures::OrderResponse orderResponse = restClient->sendOrder(order);
			orderResponse.orderStatus == futures::OrderStatus::FILLED) {
			if (pPrice) {
				*pPrice = orderResponse.avgPrice;
			}

			if (pFill) {
				*pFill = std::round(orderResponse.executedQty / lotAmount);
			}

			spdlog::info("Order placed for asset: " + std::string(Asset) + ", filled size: " +
			             std::to_string(orderResponse.executedQty / lotAmount) + ", price: " +
			             std::to_string(orderResponse.avgPrice) + ", clientId: " + orderResponse.clientOrderId);

			saveAssetForTradeId(Asset, stoi(orderResponse.clientOrderId));
			return std::stoi(orderResponse.clientOrderId);
		} else {
			std::string msg =
					"Cannot place order: " + std::string(Asset) + ", size: " + std::to_string(Amount) +
					", reason: " + std::string(magic_enum::enum_name(orderResponse.orderStatus));
			spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
			BrokerError(msg.c_str());
		}
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
		BrokerError("Cannot send order to server.");
	}

	return 0;
}

DLLFUNC_C int
BrokerSell2(int nTradeId, int nAmount, double Limit, double *pClose, double *pCost, double *pProfit, int *pFill) {
	if (!restClient) {
		spdlog::critical(fmt::format("{}: {}", MAKE_FILELINE, "Bybit Rest Client instance not initialized."));
		return 0;
	}

	try {
		auto asset = findAssetForTradeId(nTradeId);

		if (asset.empty()) {
			return 0;
		}

#ifdef _WIN64
		if (auto it = lotAmounts.find(asset); it != lotAmounts.end()) {
			lotAmount = it->second;
		} else {
			std::string msg = "Cannot find lot amount size for asset: " + std::string(asset);
			spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
			return 0;
		}
#endif
		futures::Order order;
		order.symbol = asset;

		if (nAmount > 0) {
			order.side = Side::SELL;
			order.positionSide = futures::PositionSide::LONG;
		} else {
			order.side = Side::BUY;
			order.positionSide = futures::PositionSide::SHORT;
		}

		if (Limit > 0.) {
			order.price = Limit;
			order.type = futures::OrderType::LIMIT;
		} else {
			order.type = futures::OrderType::MARKET;
		}

		order.quantity = lotAmount * std::abs(nAmount);
		order.newOrderRespType = OrderRespType::RESULT;

		readLastOrderId();
		order.newClientOrderId = std::to_string(lastOrderId++);
		writeLastOrderId();

		order.timeInForce = TimeInForce::GTC;

		if (auto orderResponse = restClient->sendOrder(order);
			orderResponse.orderStatus == futures::OrderStatus::FILLED) {
			if (pFill) {
				*pFill = std::round(orderResponse.executedQty / lotAmount);
			}

			spdlog::info("Order placed for asset: " + std::string(asset) + ", filled size: " +
			             std::to_string(orderResponse.executedQty / lotAmount) + ", price: " +
			             std::to_string(orderResponse.avgPrice) + ", clientId: " + orderResponse.clientOrderId);

			saveAssetForTradeId(asset, stoi(orderResponse.clientOrderId));
			return std::stoi(orderResponse.clientOrderId);
		} else {
			std::string msg =
					"Cannot place order: " + std::string(asset) + ", size: " + std::to_string(nAmount) +
					", reason: " + std::string(magic_enum::enum_name(orderResponse.orderStatus));
			spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
			BrokerError(msg.c_str());
		}
	} catch (std::exception &e) {
		spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
		BrokerError("Cannot close trade.");
	}

	return 0;
}

DLLFUNC_C double BrokerCommand(int Command, DWORD dwParameter) {
	switch (Command) {
		case SET_ORDERTYPE:
			return orderType = dwParameter;
		case SET_DELAY:
			loopMs = dwParameter;
		case GET_DELAY:
			return loopMs;
		case SET_AMOUNT:
#ifndef _WIN64
			lotAmount = *(double *) dwParameter;
#endif
			return 1;
		case SET_WAIT:
			waitMs = dwParameter;
		case GET_WAIT:
			return waitMs;
		case SET_SYMBOL:
			currentSymbol = reinterpret_cast<char *>(dwParameter);
			return 1;
		case GET_POSITION:
			if (restClient) {
				const char *symbol = reinterpret_cast<char *>(dwParameter);
				try {
					const auto positions = restClient->getPosition(symbol);

					double totalPositionAmt = 0;

					for (const auto &position: positions) {
						totalPositionAmt += position.positionAmt;
					}

					/// Return real position size instead of lot amount
					/// totalPositionAmt = totalPositionAmt / lotAmount;

					return totalPositionAmt;
				} catch (std::exception &e) {
					spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
					BrokerError(std::string("Cannot get position of " + std::string(symbol)).c_str());
				}
			}
			break;
		case GET_BROKERZONE:
			return 0; //return 0 for UTC
		case GET_MAXREQUESTS:
			return 10;
		case GET_MAXTICKS:
			return 250;
		case DO_CANCEL:
			if (restClient) {
				try {
					futures::OrderResponse orderResponse = restClient->cancelOrder(currentSymbol,
						std::to_string(dwParameter));

					if (orderResponse.orderStatus == futures::OrderStatus::CANCELED) {
						spdlog::info("Order canceled for asset: " + std::string(currentSymbol) + ", order id: " +
						             orderResponse.clientOrderId);
						return 1;
					}
					std::string msg =
							"Cannot cancel order for asset: " + std::string(currentSymbol) + ", order id: " +
							orderResponse.clientOrderId + ", reason: " + std::string(magic_enum::enum_name(orderResponse.orderStatus));

					spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, msg));
					BrokerError(msg.c_str());
					return 0;
				} catch (std::exception &e) {
					spdlog::error(fmt::format("{}: {}", MAKE_FILELINE, e.what()));
					BrokerError(std::string(
						"Cannot cancel order id " + std::to_string(dwParameter)).c_str());
				}
			}
			return 0;

		default:
			return 0;
	}

	return 0;
}
