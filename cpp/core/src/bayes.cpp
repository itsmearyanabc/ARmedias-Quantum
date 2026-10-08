#include "xau/bayes.hpp"

#include <cmath>
#include <random>

namespace xau {
namespace {

// Continued fraction for the regularized incomplete beta (Lentz's method).
double beta_cf(double a, double b, double x) noexcept {
    constexpr int    kMaxIter = 300;
    constexpr double kEps = 1e-14, kTiny = 1e-300;
    const double     qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double           c = 1.0, d = 1.0 - qab * x / qap;
    if (std::fabs(d) < kTiny) d = kTiny;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= kMaxIter; ++m) {
        const double m2 = 2.0 * m;
        double       aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < kTiny) d = kTiny;
        c = 1.0 + aa / c;
        if (std::fabs(c) < kTiny) c = kTiny;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < kTiny) d = kTiny;
        c = 1.0 + aa / c;
        if (std::fabs(c) < kTiny) c = kTiny;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < kEps) break;
    }
    return h;
}

double reg_inc_beta(double a, double b, double x) noexcept {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const double ln_front = std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) +
                            a * std::log(x) + b * std::log(1.0 - x);
    const double front = std::exp(ln_front);
    if (x < (a + 1.0) / (a + b + 2.0)) return front * beta_cf(a, b, x) / a;
    return 1.0 - front * beta_cf(b, a, 1.0 - x) / b;
}

}  // namespace

double student_t_cdf(double t, double dof) noexcept {
    if (!(dof > 0.0) || std::isnan(t)) return 0.5;
    if (std::isinf(t)) return t > 0.0 ? 1.0 : 0.0;
    const double x = dof / (dof + t * t);
    const double tail = 0.5 * reg_inc_beta(0.5 * dof, 0.5, x);   // P(T > |t|)
    return t >= 0.0 ? 1.0 - tail : tail;
}

EdgePosterior edge_posterior(std::span<const double> r, const EdgePrior& p) {
    const double k0 = p.strength > 0.0 ? p.strength : 1e-9;
    const double a0 = p.shape > 1.0 ? p.shape : 1.0 + 1e-9;
    const double b0 = (a0 - 1.0) * (p.var > 0.0 ? p.var : 1e-12);   // E[var] = var

    const auto n = static_cast<double>(r.size());
    double     mean = 0.0;
    for (double x : r) mean += x;
    if (!r.empty()) mean /= n;
    double ss = 0.0;
    for (double x : r) ss += (x - mean) * (x - mean);

    EdgePosterior out;
    out.n = r.size();
    const double kn = k0 + n;
    out.mean = (k0 * p.mean + n * mean) / kn;
    const double an = a0 + 0.5 * n;
    const double bn = b0 + 0.5 * ss + (r.empty() ? 0.0 : k0 * n * (mean - p.mean) * (mean - p.mean) / (2.0 * kn));
    out.dof = 2.0 * an;
    out.scale = std::sqrt(bn / (an * kn));
    out.p_edge = out.scale > 0.0 ? student_t_cdf(out.mean / out.scale, out.dof) : (out.mean > 0.0 ? 1.0 : 0.0);
    return out;
}

std::vector<double> prob_best(std::span<const EdgePosterior> posts, std::size_t draws,
                              std::uint64_t seed) {
    std::vector<double> wins(posts.size(), 0.0);
    if (posts.empty() || draws == 0) return wins;
    std::mt19937_64 rng(seed);
    std::vector<std::student_t_distribution<double>> t;
    t.reserve(posts.size());
    for (const EdgePosterior& q : posts) t.emplace_back(q.dof > 0.0 ? q.dof : 1.0);
    for (std::size_t d = 0; d < draws; ++d) {
        std::size_t best = 0;
        double      best_v = -INFINITY;
        for (std::size_t i = 0; i < posts.size(); ++i) {
            const double v = posts[i].mean + posts[i].scale * t[i](rng);
            if (v > best_v) {
                best_v = v;
                best = i;
            }
        }
        wins[best] += 1.0;
    }
    for (double& w : wins) w /= static_cast<double>(draws);
    return wins;
}

}  // namespace xau
