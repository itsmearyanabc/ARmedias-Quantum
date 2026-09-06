// The strategy zoo.
//
// Phases 3-5 tested four rules and all four lost. The frictionless runs said
// why: their gross edge is about 0.06 USD a trade against 0.68 USD of cost, and
// it is spread evenly rather than concentrated, so no meta-label gate could
// rescue them. That is a verdict on those four rules, not on the instrument.
//
// So: more hypotheses, and deliberately DIFFERENT ones. Four variations on
// "breakout" would just be one bet wearing four hats. These span mean
// reversion, momentum, carry-of-the-session, microstructure and time-of-day,
// which is what makes a regime-aware selector possible -- a selector can only
// pick a strategy suited to conditions if the strategies actually disagree.
//
// Every one is honest about direction: none peeks past the closing bar, all
// size in ATR units, all respect the session guards.

#ifndef XAU_ZOO_HPP
#define XAU_ZOO_HPP

#include "xau/indicators.hpp"
#include "xau/rules.hpp"
#include "xau/regime.hpp"
#include "xau/strategy.hpp"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>

namespace xau {

// ---------------------------------------------------------------------------
// 5. Bollinger mean reversion
//
// Gold spends most of its life ranging. Fade a stretch beyond N standard
// deviations, targeting the middle. The opposite bet to every breakout rule in
// rules.hpp, which is the point.
// ---------------------------------------------------------------------------
class BollingerReversion final : public Strategy {
public:
    struct Config {
        std::size_t period = 20;
        double      entry_sd = 2.0;
        double      sl_atr = 1.5;
        double      tp_r = 1.0;      // mean reversion pays small and often
        std::size_t atr_period = 14;
        int         trade_from_hour = 7;
        int         trade_until_hour = 20;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    BollingerReversion();
    explicit BollingerReversion(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "BollingerReversion"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
};

// ---------------------------------------------------------------------------
// 6. RSI-2 extreme
//
// Connors' short-lookback RSI. A 2-period RSI below 5 or above 95 is a
// genuinely rare event; the bet is that such an extreme resolves. Distinct from
// Bollinger because it keys on the RUN of closes, not the distance travelled.
// ---------------------------------------------------------------------------
class Rsi2Extreme final : public Strategy {
public:
    struct Config {
        std::size_t rsi_period = 2;
        double      oversold = 5.0;
        double      overbought = 95.0;
        std::size_t exit_ema = 5;
        double      sl_atr = 2.0;
        double      tp_r = 1.0;
        std::size_t atr_period = 14;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    Rsi2Extreme();
    explicit Rsi2Extreme(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "Rsi2Extreme"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
};

// ---------------------------------------------------------------------------
// 7. Momentum continuation
//
// Buy strength, sell weakness: the return over N bars predicts the next few.
// The cleanest possible momentum bet, included precisely because it is the
// null that "trend following works on gold" has to beat.
// ---------------------------------------------------------------------------
class MomentumContinuation final : public Strategy {
public:
    struct Config {
        std::size_t lookback = 24;
        double      min_move_atr = 1.5;   // how big a move counts as momentum
        double      sl_atr = 1.5;
        double      tp_r = 1.5;
        std::size_t atr_period = 14;
        int         trade_from_hour = 7;
        int         trade_until_hour = 19;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    MomentumContinuation();
    explicit MomentumContinuation(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "MomentumContinuation"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
};

// ---------------------------------------------------------------------------
// 8. Session-open drift
//
// Gold has a documented tendency to drift in the direction of the first move
// after a major session opens. No indicator at all -- pure time-of-day, which
// makes it the cheapest hypothesis in the zoo to falsify.
// ---------------------------------------------------------------------------
class SessionDrift final : public Strategy {
public:
    struct Config {
        int         session_hour = 13;   // NY open, UTC
        std::size_t confirm_bars = 2;    // direction of the first N bars
        double      sl_atr = 1.0;
        double      tp_r = 1.5;
        std::size_t atr_period = 14;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    SessionDrift();
    explicit SessionDrift(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "SessionDrift"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
    TimeUs armed_day_ = -1;
};

// ---------------------------------------------------------------------------
// 9. Spread-aware liquidity fade
//
// Microstructure rather than price. A spread spike with no matching range
// expansion is liquidity withdrawing, not information arriving -- and price
// dislocations on thin books tend to revert. This is the only strategy here
// that reads the book rather than the chart, which is why it is worth having:
// it can be right when every price rule is wrong.
// ---------------------------------------------------------------------------
class LiquidityFade final : public Strategy {
public:
    struct Config {
        std::size_t lookback = 20;
        double      spread_spike = 2.0;   // multiples of the recent mean spread
        double      max_range_expansion = 1.2;   // ... with range NOT expanding
        double      sl_atr = 1.0;
        double      tp_r = 1.0;
        std::size_t atr_period = 14;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    LiquidityFade();
    explicit LiquidityFade(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "LiquidityFade"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
};

// ---------------------------------------------------------------------------
// 10. Inside-bar continuation
//
// An inside bar is a one-bar compression. Trade the break of the mother bar's
// range. Related to VolatilityCompression but far more selective, and it fires
// on a two-bar pattern rather than a seven-bar statistic.
// ---------------------------------------------------------------------------
class InsideBarBreak final : public Strategy {
public:
    struct Config {
        double      sl_atr = 1.0;
        double      tp_r = 2.0;
        std::size_t atr_period = 14;
        std::size_t expiry_bars = 3;
        int         trade_from_hour = 6;
        int         trade_until_hour = 20;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    InsideBarBreak();
    explicit InsideBarBreak(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "InsideBarBreak"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config      cfg_;
    Atr         atr_;
    bool        armed_ = false;
    std::size_t bars_since_arm_ = 0;
    Points      mother_high_ = 0;
    Points      mother_low_ = 0;
};

// ---------------------------------------------------------------------------
// 11. Gap fade
//
// The Sunday open gap after a weekend of news. Gold gaps and then, more often
// than not, fills. Fires at most once a week, so it is thin on data -- reported
// separately rather than blended, because 500 trades and 50 trades do not
// deserve the same confidence.
// ---------------------------------------------------------------------------
class WeekendGapFade final : public Strategy {
public:
    struct Config {
        double      min_gap_atr = 0.75;
        double      sl_atr = 1.5;
        double      tp_r = 1.0;
        std::size_t atr_period = 14;
        std::size_t max_hold_bars = 32;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    WeekendGapFade();
    explicit WeekendGapFade(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "WeekendGapFade"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
    TimeUs last_bar_us_ = 0;
};

// ---------------------------------------------------------------------------
// 12. Dual-EMA regime filter with ATR channel
//
// Trend following that only trades when volatility is EXPANDING. Most trend
// systems bleed in quiet chop; this one is built to sit out, which makes it the
// natural complement to the mean-reversion entries above.
// ---------------------------------------------------------------------------
class AdaptiveTrend final : public Strategy {
public:
    struct Config {
        std::size_t fast_ema = 21;
        std::size_t slow_ema = 55;
        std::size_t atr_period = 14;
        std::size_t vol_lookback = 50;
        double      min_vol_ratio = 1.1;   // ATR must exceed its own average
        double      sl_atr = 2.0;
        double      tp_r = 2.0;
        int         trade_from_hour = 7;
        int         trade_until_hour = 20;
        double      lots = 0.01;
        ExitRules   exits{};
    };

    AdaptiveTrend();
    explicit AdaptiveTrend(const Config& c);

    [[nodiscard]] const char* name() const noexcept override { return "AdaptiveTrend"; }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;

private:
    Config cfg_;
    Atr    atr_;
    Ema    fast_;
    Ema    slow_;
    bool   was_above_ = false;
    bool   primed_ = false;
};

// ---------------------------------------------------------------------------
// 13. Regime-gated wrapper
//
// Wraps any strategy and suppresses its entries outside a chosen set of
// regimes. Exits are never suppressed -- a position opened in one regime must
// still be allowed to close when the market changes underneath it, or the
// gate would strand trades it opened.
//
// The regime set is a decision about WHEN a hypothesis applies, and should be
// justified by the hypothesis rather than by the backtest. Mean reversion
// losing in trends is theory; picking the six best-looking cells out of a
// table of seventy-two is curve fitting that will not survive contact with
// next year.
// ---------------------------------------------------------------------------
class RegimeGated final : public Strategy {
public:
    RegimeGated(std::unique_ptr<Strategy> inner, unsigned allowed_mask, const char* label);

    [[nodiscard]] const char* name() const noexcept override { return label_.c_str(); }
    [[nodiscard]] std::size_t warmup_bars() const noexcept override;
    [[nodiscard]] Decision on_bar(const BarContext& c) override;
    void on_trade_closed(const Trade& t) override { inner_->on_trade_closed(t); }
    void on_finish() override { inner_->on_finish(); }

    // Bit k is set when regime index k is tradeable.
    static constexpr unsigned mask_of(std::initializer_list<std::size_t> idx) noexcept {
        unsigned m = 0;
        for (std::size_t i : idx) m |= (1u << i);
        return m;
    }

private:
    std::unique_ptr<Strategy> inner_;
    unsigned                  allowed_ = ~0u;
    std::string               label_;
};

}  // namespace xau

#endif  // XAU_ZOO_HPP
