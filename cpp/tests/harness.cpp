#include "harness.hpp"

#include <cstdio>

namespace tst {
namespace {
int g_case_failures  = 0;
int g_total_failures = 0;
}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void report_failure(const char* file, int line, const std::string& what) {
    ++g_case_failures;
    ++g_total_failures;
    std::fprintf(stderr, "    %s:%d: %s\n", file, line, what.c_str());
}

int run_all() {
    int passed = 0;
    int failed = 0;

    for (const Case& c : registry()) {
        g_case_failures = 0;
        c.fn();
        if (g_case_failures == 0) {
            ++passed;
            std::printf("  ok    %s\n", c.name);
        } else {
            ++failed;
            std::printf("  FAIL  %s  (%d check%s)\n", c.name, g_case_failures,
                        g_case_failures == 1 ? "" : "s");
        }
    }

    std::printf("\n%d passed, %d failed, %d checks failed overall\n", passed, failed,
                g_total_failures);
    return failed == 0 ? 0 : 1;
}

}  // namespace tst

int main() {
    std::printf("running %zu tests\n", tst::registry().size());
    return tst::run_all();
}
