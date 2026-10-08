// EXAMPLE custom strategy -- the shape every file in this folder takes.
// Delete it or keep it; it is here to be copied. See docs/CUSTOM-STRATEGIES.md.
//
// Idea: follow the trend of the 20-bar EMA against the 100-bar EMA on H1.
// Long when the fast crosses above the slow, short when it crosses below, out
// when it crosses back. A 3-ATR stop rides on every entry. Untuned.

#include "xau/custom.hpp"
#include "xau/indicators.hpp"

namespace {   // everything but the registration line stays file-local

class ExampleEmaTrend final : public xau::Strategy {
public:
    explicit ExampleEmaTrend(double lots) : lots_(lots) {}

    const char* name() const noexcept override { return "ExampleEmaTrend"; }

    xau::Decision on_bar(const xau::BarContext& c) override {
        const xau::Bar& b = c.bar();
        fast_.update(static_cast<double>(b.close));
        slow_.update(static_cast<double>(b.close));
        atr_.update(b);
        if (!fast_.ready() || !slow_.ready() || !atr_.ready()) return xau::Decision::hold();

        const int  side = fast_.value() > slow_.value() ? 1 : -1;
        const bool crossed = prev_side_ != 0 && side != prev_side_;
        prev_side_ = side;

        if (c.position.is_open()) {
            const bool against = (c.position.side == xau::Side::Long) != (side > 0);
            return against ? xau::Decision::close("ema_cross_back") : xau::Decision::hold();
        }
        if (!crossed) return xau::Decision::hold();

        const auto stop = static_cast<xau::Points>(3.0 * atr_.value());
        if (stop <= 0) return xau::Decision::hold();   // no volatility, no stop, no trade
        xau::Decision d = xau::Decision::enter(side > 0 ? xau::Side::Long : xau::Side::Short,
                                               stop, 0, side > 0 ? "ema_up" : "ema_dn");
        d.lots = lots_;   // 0 hands sizing to the risk layer
        return d;
    }

private:
    double    lots_;
    xau::Ema  fast_{20};
    xau::Ema  slow_{100};
    xau::Atr  atr_{14};
    int       prev_side_ = 0;
};

}  // namespace

XAU_CUSTOM_STRATEGY(ExampleEmaTrend, H1,
                    "EXAMPLE: 20/100 EMA cross on H1, 3-ATR stop, out on the cross back.")
