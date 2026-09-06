#include "xau/validation.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace xau {

namespace {

// Standard normal CDF. std::erfc is accurate in the far tail, which matters:
// deflated Sharpe lives out at three and four sigma, and the naive
// 0.5*(1+erf(x/sqrt2)) form loses its significant digits exactly there.
double norm_cdf(double z) noexcept { return 0.5 * std::erfc(-z * std::numbers::sqrt2 / 2.0); }

// Inverse standard normal, Acklam's rational approximation. Accurate to about
// 1.15e-9 across the range, which is far better than the inputs here deserve.
double norm_ppf(double p) noexcept {
    if (p <= 0.0) return -40.0;
    if (p >= 1.0) return 40.0;

    static constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                                   -2.759285104469687e+02, 1.383577518672690e+02,
                                   -3.066479806614716e+01, 2.506628277459239e+00};
    static constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                                   -1.556989798598866e+02, 6.680131188771972e+01,
                                   -1.328068155288572e+01};
    static constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                                   -2.400758277161838e+00, -2.549732539343734e+00,
                                   4.374664141464968e+00,  2.938163982698783e+00};
    static constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01,
                                   2.445134137142996e+00, 3.754408661907416e+00};
    static constexpr double plow = 0.02425;

    if (p < plow) {
        const double q = std::sqrt(-2.0 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > 1.0 - plow) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

}  // namespace

Moments moments_of(std::span<const double> xs) noexcept {
    Moments m;
    m.n = xs.size();
    if (xs.size() < 2) return m;

    m.mean = std::accumulate(xs.begin(), xs.end(), 0.0) / static_cast<double>(xs.size());

    double m2 = 0.0, m3 = 0.0, m4 = 0.0;
    for (double x : xs) {
        const double d = x - m.mean;
        const double d2 = d * d;
        m2 += d2;
        m3 += d2 * d;
        m4 += d2 * d2;
    }
    const double n = static_cast<double>(xs.size());
    m2 /= n;
    m3 /= n;
    m4 /= n;

    m.stdev = std::sqrt(m2);
    if (m2 > 1e-18) {
        m.skew = m3 / std::pow(m2, 1.5);
        m.kurtosis = m4 / (m2 * m2);
    }
    return m;
}

double sharpe_ratio(std::span<const double> returns) noexcept {
    const Moments m = moments_of(returns);

    // The guard has to be RELATIVE to the size of the returns, not an absolute
    // epsilon. A constant series of 0.004 leaves floating-point dust of about
    // 1e-18 in its standard deviation, which cleared a 1e-18 absolute floor and
    // produced a Sharpe of 2.3e15. That is not a rounding artefact anyone would
    // notice downstream -- it is an equity curve so smooth it looks perfect,
    // which is exactly the shape a subtly broken backtest produces.
    const double scale = std::max(std::abs(m.mean), 1e-12);
    if (!(m.stdev > scale * 1e-9)) return 0.0;
    return m.mean / m.stdev;
}

double expected_max_sharpe(double trial_sr_variance, std::size_t n_trials) noexcept {
    if (n_trials < 2 || trial_sr_variance <= 0.0) return 0.0;

    // Euler-Mascheroni. The expected maximum of n draws from a normal sits at
    // this particular blend of the (1 - 1/n) and (1 - 1/(n*e)) quantiles.
    constexpr double gamma = 0.5772156649015329;
    const double     n = static_cast<double>(n_trials);
    const double     q1 = norm_ppf(1.0 - 1.0 / n);
    const double     q2 = norm_ppf(1.0 - 1.0 / (n * std::numbers::e));
    return std::sqrt(trial_sr_variance) * ((1.0 - gamma) * q1 + gamma * q2);
}

double deflated_sharpe(double observed_sr, double benchmark_sr, std::size_t n_obs, double skew,
                       double kurtosis) noexcept {
    if (n_obs < 3) return 0.0;

    // The variance of an estimated Sharpe depends on the shape of the returns,
    // not just their spread. Negative skew and fat tails -- which is every
    // trading strategy that sells volatility, and most that do not -- inflate
    // it, so a Sharpe measured on such returns is less trustworthy than the
    // same number measured on normal ones.
    const double t = static_cast<double>(n_obs);
    double       denom = 1.0 - skew * observed_sr +
                   ((kurtosis - 1.0) / 4.0) * observed_sr * observed_sr;
    if (denom <= 1e-12) denom = 1e-12;

    const double z = (observed_sr - benchmark_sr) * std::sqrt(t - 1.0) / std::sqrt(denom);
    return norm_cdf(z);
}

PboResult probability_of_backtest_overfitting(const std::vector<std::vector<double>>& perf) {
    PboResult out;
    if (perf.size() < 2) return out;
    const std::size_t n_strat = perf.size();
    const std::size_t n_blocks = perf[0].size();
    if (n_blocks < 4 || n_blocks % 2 != 0) return out;

    const std::size_t half = n_blocks / 2;

    // Every way of choosing `half` blocks as in-sample. The complement is out
    // of sample, which is what makes this COMBINATORIALLY SYMMETRIC: each
    // split is evaluated in both directions by construction.
    std::vector<bool> pick(n_blocks, false);
    std::fill(pick.begin(), pick.begin() + static_cast<long>(half), true);
    std::vector<std::size_t> idx(n_blocks);
    std::iota(idx.begin(), idx.end(), 0);

    std::size_t below_median = 0;
    std::size_t splits = 0;

    // std::prev_permutation over a sorted-descending bool vector walks every
    // combination exactly once.
    std::vector<bool> mask = pick;
    do {
        std::vector<double> is_perf(n_strat, 0.0), oos_perf(n_strat, 0.0);
        for (std::size_t s = 0; s < n_strat; ++s) {
            for (std::size_t b = 0; b < n_blocks; ++b) {
                if (mask[b]) is_perf[s] += perf[s][b];
                else oos_perf[s] += perf[s][b];
            }
        }

        const std::size_t best =
            static_cast<std::size_t>(std::max_element(is_perf.begin(), is_perf.end()) -
                                     is_perf.begin());

        // Rank the in-sample winner among all strategies out of sample.
        std::vector<double> sorted = oos_perf;
        const double        med = median_of(sorted);
        if (oos_perf[best] < med) ++below_median;
        out.oos_of_is_best.push_back(oos_perf[best]);
        ++splits;
    } while (std::prev_permutation(mask.begin(), mask.end()));

    out.splits = splits;
    out.pbo = splits > 0 ? static_cast<double>(below_median) / static_cast<double>(splits) : 1.0;
    return out;
}

std::vector<std::vector<std::size_t>> cpcv_test_combinations(std::size_t groups,
                                                             std::size_t test_groups) {
    std::vector<std::vector<std::size_t>> out;
    if (test_groups == 0 || test_groups > groups) return out;

    std::vector<bool> mask(groups, false);
    std::fill(mask.begin(), mask.begin() + static_cast<long>(test_groups), true);
    do {
        std::vector<std::size_t> combo;
        combo.reserve(test_groups);
        for (std::size_t i = 0; i < groups; ++i) {
            if (mask[i]) combo.push_back(i);
        }
        out.push_back(std::move(combo));
    } while (std::prev_permutation(mask.begin(), mask.end()));
    return out;
}

std::size_t cpcv_path_count(std::size_t groups, std::size_t test_groups) noexcept {
    if (test_groups == 0 || test_groups > groups) return 0;
    // C(groups, test_groups) * test_groups / groups, computed without
    // overflowing on the intermediate binomial.
    double c = 1.0;
    for (std::size_t i = 0; i < test_groups; ++i) {
        c = c * static_cast<double>(groups - i) / static_cast<double>(i + 1);
    }
    return static_cast<std::size_t>(c * static_cast<double>(test_groups) /
                                    static_cast<double>(groups));
}

}  // namespace xau
