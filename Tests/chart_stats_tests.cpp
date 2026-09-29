#include "gui_core.hpp"
#include <cstdlib>
#include <iostream>

namespace {
int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void Near(double actual, double expected, const char* message, double tolerance = 1e-11) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr << "Actual: " << actual << ", expected: " << expected << '\n';
        Check(false, message);
    }
    Check(true, message);
}

PullBucket Bucket(std::initializer_list<int> intervals) {
    PullBucket bucket{std::pmr::polymorphic_allocator<std::byte>{}};
    for (int interval : intervals) {
        for (int x = 1; x <= interval; ++x) {
            bucket.push_back(x == interval ? RankType::Rank6 : RankType::Rank3,
                             x == interval ? "UP" : "Other", "Pool", 0);
        }
    }
    return bucket;
}

// Check that the exported marker is a point on the actual plotted ECDF and its
// theoretical reference, not merely a second copy of the reported D.
void CheckMarker(const KSLocation& point, double d,
                 const std::array<int, 260>& freq, int count,
                 std::span<const double> cdf) {
    Check(point.x >= 0 && point.x < (int)freq.size(), "marker has a valid frequency index");
    const int cumulative = std::accumulate(freq.begin(), freq.begin() + point.x + 1, 0);
    Near(point.empirical, (double)cumulative / count, "marker lies on empirical CDF");
    Near(point.theory, cdf[(std::min)(point.x, FindCDFLastValid(cdf))],
         "marker uses valid theoretical endpoint");
    Near(std::abs(point.empirical - point.theory), d, "marker spans the reported D");
}

void TestEffectiveEndpoint() {
    const std::array<double, 5> trailing_zero{0.0, 0.2, 0.7, 0.0, 0.0};
    const std::array<double, 4> saturated{0.0, 0.4, 1.0, 0.0};
    Check(FindCDFLastValid(trailing_zero) == 2, "ignore unused zero suffix");
    Check(FindCDFLastValid(saturated) == 2, "stop at saturation");
    Check(FindCDFLastValid(g_cdf_joint_up) == 240, "joint theory ends at 240");
    Check(FindCDFLastValid(g_cdf_wep_up) == 80, "weapon UP theory ends at 80");
    Check(FindCDFLastValid(g_cdf_char_up) == 120, "character UP theory ends at 120");

    std::array<int, 260> frequency{};
    frequency[3] = 1;
    KSLocation point;
    Near(ComputeKS(frequency, 3, 1, trailing_zero, &point), 0.7,
         "KS must not read the unused suffix within a table's allocation");
    CheckMarker(point, 0.7, frequency, 1, trailing_zero);
}

void TestWeaponClaimFrequency() {
    const StatsResult first = Calculate(Bucket({1}), true, {}, {});
    const StatsResult tenth = Calculate(Bucket({10}), true, {}, {});
    const StatsResult guaranteed = Calculate(Bucket({71}), true, {}, {});
    Check(first.count_up == 1 && tenth.count_up == 1 && guaranteed.count_up == 1,
          "weapon fixtures contain one UP");
    Check(first.freq_up[1] == 1 && first.freq_ecdf_up[1] == 0 && first.freq_ecdf_up[10] == 1,
          "weapon ECDF exports the claim boundary, preserving raw landing point");
    Check(first.freq_ecdf_up == tenth.freq_ecdf_up, "landing points 1 and 10 share one claim");
    Check(guaranteed.freq_up[71] == 1 && guaranteed.freq_ecdf_up[80] == 1,
          "guaranteed weapon claim maps 71 to 80");
    Near(first.ks_d_up, 0.9043820750088044, "weapon first-claim KS regression");
    Near(first.ks_d_up, tenth.ks_d_up, "weapon KS ignores within-claim landing point");
    Near(guaranteed.ks_d_up, g_cdf_wep_up[70], "eighth-claim KS uses pre-jump CDF");
    Near(first.avg_up, 1.0, "weapon mean keeps raw pull units");
    Near(tenth.avg_up, 10.0, "weapon tenth-pull mean remains ten");
    Near(guaranteed.avg_up, 71.0, "weapon guaranteed mean is not rounded");
    Near(first.hazard_up[1], 1.0, "weapon hazard keeps raw pull units");
    Near(first.hazard_up[10], 0.0, "claim aggregation does not move hazard events");
    CheckMarker(first.ks_location_up, first.ks_d_up, first.freq_ecdf_up, 1, g_cdf_wep_up);
    CheckMarker(guaranteed.ks_location_up, guaranteed.ks_d_up,
                guaranteed.freq_ecdf_up, 1, g_cdf_wep_up);
    CheckMarker(first.ks_location_all, first.ks_d_all, first.freq_all, 1, g_cdf_wep);
}

void TestJointTail() {
    // Previously cdf[241] was read as zero: text reported D=1 while the chart,
    // stopping at 240, reported about 0.04964. Both now use this same result.
    const StatsResult joint = Calculate(Bucket({13, 31, 62, 69, 71, 75, 110, 140, 179, 241}),
                                        false, {}, {}, true);
    Check(g_cdf_joint_up[241] == 0.0, "fixture exercises the unfilled sentinel");
    Near(joint.ks_d_up, 0.0692239566959276, "joint tail KS golden regression");
    Check(joint.ks_location_up.x == 241, "maximum after the theory endpoint is retained");
    Check(joint.ks_is_normal_up, "corrected KS no longer spuriously rejects this sample");
    CheckMarker(joint.ks_location_up, joint.ks_d_up, joint.freq_ecdf_up,
                joint.count_up, g_cdf_joint_up);

    const StatsResult last_slot = Calculate(Bucket({13, 31, 62, 69, 71, 75, 110, 140, 179, 259}),
                                            false, {}, {}, true);
    Near(last_slot.ks_d_up, joint.ks_d_up, "joint tail uses constant valid endpoint through 259");
    Check(last_slot.ks_location_up.x == 259, "maximum at final frequency slot is retained");
    CheckMarker(last_slot.ks_location_up, last_slot.ks_d_up, last_slot.freq_ecdf_up,
                last_slot.count_up, g_cdf_joint_up);
}

void TestOtherPoolsAndEmpty() {
    const StatsResult character = Calculate(Bucket({1, 10, 71}), false, {}, {});
    const StatsResult joint = Calculate(Bucket({1, 10, 71}), false, {}, {}, true);
    const StatsResult refactor = Calculate(Bucket({1, 10, 71}), false, {}, {}, false, true);
    Check(character.freq_ecdf_up == character.freq_up, "character ECDF remains per pull");
    Check(joint.freq_ecdf_up == joint.freq_up, "joint ECDF remains per pull");
    Check(refactor.freq_ecdf_up == refactor.freq_up, "refactor ECDF remains per pull");
    Check(!character.ks_up_mixed && !joint.ks_up_mixed && refactor.ks_up_mixed,
          "mixed-distribution flag is preserved for refactor chart gating");
    Check(!Calculate(Bucket({1}), false, {}, {}, false, true).ks_up_mixed,
          "single refactor UP remains eligible for comparison");
    CheckMarker(character.ks_location_up, character.ks_d_up, character.freq_ecdf_up,
                character.count_up, g_cdf_char_up);
    CheckMarker(refactor.ks_location_up, refactor.ks_d_up, refactor.freq_ecdf_up,
                refactor.count_up, g_cdf_refactor_up);

    const StatsResult empty = Calculate(Bucket({}), true, {}, {});
    Check(empty.count_up == 0 && empty.count_all == 0, "empty analysis has no events");
    Check(empty.freq_ecdf_up == std::array<int, 260>{}, "empty claim frequency is zero");
    Near(empty.ks_d_up, 0.0, "empty analysis has no UP deviation");
    Near(empty.ks_d_all, 0.0, "empty analysis has no overall deviation");
    KSLocation point = character.ks_location_up;
    Near(ComputeKS(empty.freq_up, 259, 0, g_cdf_wep_up, &point), 0.0,
         "empty KS returns zero");
    Check(point.x == 0 && point.empirical == 0.0 && point.theory == 0.0,
          "empty KS resets a reused output marker");
}

void TestFrequencyBounds() {
    const StatsResult weapon = Calculate(Bucket({259}), true, {}, {});
    Check(weapon.freq_up[259] == 1 && weapon.freq_ecdf_up[259] == 1,
          "rounding a final-slot weapon sample does not overflow frequency storage");
    Check(std::accumulate(weapon.freq_ecdf_up.begin(), weapon.freq_ecdf_up.end(), 0) == 1,
          "claim rounding preserves event count at the storage limit");
    CheckMarker(weapon.ks_location_up, weapon.ks_d_up, weapon.freq_ecdf_up, 1, g_cdf_wep_up);
    std::array<int, 260> frequency{};
    frequency[259] = 1;
    KSLocation point;
    Near(ComputeKS(frequency, 10000, 1, g_cdf_wep_up, &point), 1.0,
         "KS clamps oversized max_pity to available frequency storage");
}
} // namespace

int main() {
    InitCDFTables();
    TestEffectiveEndpoint();
    TestWeaponClaimFrequency();
    TestJointTail();
    TestOtherPoolsAndEmpty();
    TestFrequencyBounds();
    std::cout << "PASS: " << checks << " chart statistics checks\n";
}
