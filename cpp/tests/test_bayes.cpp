// The live evolution's arithmetic: the Student-t tail and the posterior.

#include "harness.hpp"

#include "xau/bayes.hpp"

#include <vector>

XAU_TEST(student_t_cdf_matches_known_values) {
    CHECK_NEAR(xau::student_t_cdf(0.0, 5.0), 0.5, 1e-12);
    CHECK_NEAR(xau::student_t_cdf(1.0, 1.0), 0.75, 1e-10);            // Cauchy
    CHECK_NEAR(xau::student_t_cdf(2.0, 10.0), 0.963305982, 1e-8);
    CHECK_NEAR(xau::student_t_cdf(-2.0, 10.0), 1.0 - 0.963305982, 1e-8);
    CHECK_NEAR(xau::student_t_cdf(1.96, 1e6), 0.975002105, 1e-6);     // -> normal
    CHECK_NEAR(xau::student_t_cdf(3.0, 4.0), 0.980029, 1e-6);
}

XAU_TEST(no_trades_leaves_the_skeptical_prior_at_even_odds) {
    const xau::EdgePosterior p = xau::edge_posterior({}, xau::EdgePrior{});
    CHECK_EQ(p.n, std::size_t{0});
    CHECK_NEAR(p.mean, 0.0, 1e-15);
    CHECK_NEAR(p.p_edge, 0.5, 1e-12);
}

XAU_TEST(evidence_moves_belief_and_more_evidence_moves_it_further) {
    // +0.2% a trade with 1% noise: a real but small edge.
    std::vector<double> r;
    std::uint64_t       s = 7;
    for (int i = 0; i < 400; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const double u = static_cast<double>(s >> 11) / 9007199254740992.0;   // [0,1)
        r.push_back(0.002 + (u - 0.5) * 0.0346);   // uniform, sd ~ 1%
    }
    xau::EdgePrior pr;
    pr.var = 1e-4;
    const auto p20 = xau::edge_posterior(std::span<const double>(r.data(), 20), pr);
    const auto p400 = xau::edge_posterior(r, pr);
    CHECK(p400.p_edge > 0.99);
    CHECK(p400.p_edge > p20.p_edge);
    // The skeptical prior shrinks toward zero: the estimate sits below the
    // sample mean, never above it.
    double mean = 0.0;
    for (double x : r) mean += x;
    mean /= static_cast<double>(r.size());
    CHECK(p400.mean < mean && p400.mean > 0.0);

    // A losing strategy is believed to be one.
    std::vector<double> neg;
    for (double x : r) neg.push_back(-x);
    CHECK(xau::edge_posterior(neg, pr).p_edge < 0.01);
}

XAU_TEST(prob_best_favours_the_better_posterior_and_is_seeded) {
    xau::EdgePosterior a, b;
    a.mean = 0.002; a.scale = 0.0005; a.dof = 50; a.n = 100;
    b.mean = 0.000; b.scale = 0.0005; b.dof = 50; b.n = 100;
    const std::vector<xau::EdgePosterior> v = {a, b};
    const auto p = xau::prob_best(v, 20'000, 42);
    REQUIRE(p.size() == 2);
    CHECK(p[0] > 0.99);
    CHECK_NEAR(p[0] + p[1], 1.0, 1e-12);
    CHECK(xau::prob_best(v, 20'000, 42) == p);
}
