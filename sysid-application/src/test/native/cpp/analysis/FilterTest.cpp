// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include <array>
#include <cmath>
#include <vector>

#include "gtest/gtest.h"
#include "sysid/analysis/AnalysisManager.h"
#include "sysid/analysis/FeedforwardAnalysis.h"
#include "sysid/analysis/FilteringUtils.h"
#include "sysid/analysis/Storage.h"

TEST(FilterTest, MedianFilter) {
  std::vector<sysid::PreparedData> testData{
      sysid::PreparedData{0_s, 0, 0, 0},    sysid::PreparedData{0_s, 0, 0, 1},
      sysid::PreparedData{0_s, 0, 0, 10},   sysid::PreparedData{0_s, 0, 0, 5},
      sysid::PreparedData{0_s, 0, 0, 3},    sysid::PreparedData{0_s, 0, 0, 0},
      sysid::PreparedData{0_s, 0, 0, 1000}, sysid::PreparedData{0_s, 0, 0, 7},
      sysid::PreparedData{0_s, 0, 0, 6},    sysid::PreparedData{0_s, 0, 0, 5}};
  std::vector<sysid::PreparedData> expectedData{
      sysid::PreparedData{0_s, 0, 0, 0}, sysid::PreparedData{0_s, 0, 0, 1},
      sysid::PreparedData{0_s, 0, 0, 5}, sysid::PreparedData{0_s, 0, 0, 5},
      sysid::PreparedData{0_s, 0, 0, 3}, sysid::PreparedData{0_s, 0, 0, 3},
      sysid::PreparedData{0_s, 0, 0, 7}, sysid::PreparedData{0_s, 0, 0, 7},
      sysid::PreparedData{0_s, 0, 0, 6}, sysid::PreparedData{0_s, 0, 0, 5}};

  sysid::ApplyMedianFilter(&testData, 3);
  EXPECT_EQ(expectedData, testData);
}

TEST(FilterTest, NoiseFloor) {
  std::vector<sysid::PreparedData> testData = {
      {0_s, 1, 2, 3, 5_ms, 0, 0},    {1_s, 1, 2, 3, 5_ms, 1, 0},
      {2_s, 1, 2, 3, 5_ms, 2, 0},    {3_s, 1, 2, 3, 5_ms, 5, 0},
      {4_s, 1, 2, 3, 5_ms, 0.35, 0}, {5_s, 1, 2, 3, 5_ms, 0.15, 0},
      {6_s, 1, 2, 3, 5_ms, 0, 0},    {7_s, 1, 2, 3, 5_ms, 0.02, 0},
      {8_s, 1, 2, 3, 5_ms, 0.01, 0}, {9_s, 1, 2, 3, 5_ms, 0, 0}};
  double noiseFloor =
      GetNoiseFloor(testData, 2, [](auto&& pt) { return pt.acceleration; });
  EXPECT_NEAR(0.953, noiseFloor, 0.001);
}

TEST(FilterTest, StepTrim) {
  std::vector<sysid::PreparedData> testData = {
      {0_s, 1, 2, 3, 5_ms, 0, 0},    {1_s, 1, 2, 3, 5_ms, 0.25, 0},
      {2_s, 1, 2, 3, 5_ms, 0.5, 0},  {3_s, 1, 2, 3, 5_ms, 0.45, 0},
      {4_s, 1, 2, 3, 5_ms, 0.35, 0}, {5_s, 1, 2, 3, 5_ms, 0.15, 0},
      {6_s, 1, 2, 3, 5_ms, 0, 0},    {7_s, 1, 2, 3, 5_ms, 0.02, 0},
      {8_s, 1, 2, 3, 5_ms, 0.01, 0}, {9_s, 1, 2, 3, 5_ms, 0, 0},
  };

  std::vector<sysid::PreparedData> expectedData = {
      {2_s, 1, 2, 3, 5_ms, 0.5, 0},
      {3_s, 1, 2, 3, 5_ms, 0.45, 0},
      {4_s, 1, 2, 3, 5_ms, 0.35, 0},
      {5_s, 1, 2, 3, 5_ms, 0.15, 0}};

  auto maxTime = 9_s;
  auto minTime = maxTime;

  sysid::AnalysisManager::Settings settings;
  minTime = sysid::TrimStepVoltageData(&testData, &settings, minTime, maxTime);

  EXPECT_EQ(expectedData[0].acceleration, testData[0].acceleration);
  EXPECT_EQ(expectedData.back().acceleration, testData.back().acceleration);
  EXPECT_EQ(5, settings.stepTestDuration.value());
  EXPECT_EQ(2, minTime.value());
}

template <int Derivative, int Samples, typename F, typename DfDx>
void AssertCentralResults(F&& f, DfDx&& dfdx, units::second_t h, double min,
                          double max) {
  static_assert(Samples % 2 != 0, "Number of samples must be odd.");

  auto filter = sysid::CentralFiniteDifference<Derivative, Samples>(h);

  for (int i = min / h.value(); i < max / h.value(); ++i) {
    // Let filter initialize
    if (i < static_cast<int>(min / h.value()) + Samples) {
      filter.Calculate(f(i * h.value()));
      continue;
    }

    // For central finite difference, the derivative computed at this point is
    // half the window size in the past.
    // The order of accuracy is O(h^(N - d)) where N is number of stencil
    // points and d is order of derivative
    EXPECT_NEAR(dfdx((i - static_cast<int>((Samples - 1) / 2)) * h.value()),
                filter.Calculate(f(i * h.value())),
                std::pow(h.value(), Samples - Derivative));
  }
}

/**
 * Test central finite difference.
 */
TEST(LinearFilterOutputTest, CentralFiniteDifference) {
  constexpr auto h = 5_ms;

  AssertCentralResults<1, 3>(
      [](double x) {
        // f(x) = x²
        return x * x;
      },
      [](double x) {
        // df/dx = 2x
        return 2.0 * x;
      },
      h, -20.0, 20.0);

  AssertCentralResults<1, 3>(
      [](double x) {
        // f(x) = std::sin(x)
        return std::sin(x);
      },
      [](double x) {
        // df/dx = std::cos(x)
        return std::cos(x);
      },
      h, -20.0, 20.0);

  AssertCentralResults<1, 3>(
      [](double x) {
        // f(x) = ln(x)
        return std::log(x);
      },
      [](double x) {
        // df/dx = 1 / x
        return 1.0 / x;
      },
      h, 1.0, 20.0);

  AssertCentralResults<2, 5>(
      [](double x) {
        // f(x) = x^2
        return x * x;
      },
      [](double x) {
        // d²f/dx² = 2
        return 2.0;
      },
      h, -20.0, 20.0);

  AssertCentralResults<2, 5>(
      [](double x) {
        // f(x) = std::sin(x)
        return std::sin(x);
      },
      [](double x) {
        // d²f/dx² = -std::sin(x)
        return -std::sin(x);
      },
      h, -20.0, 20.0);

  AssertCentralResults<2, 5>(
      [](double x) {
        // f(x) = ln(x)
        return std::log(x);
      },
      [](double x) {
        // d²f/dx² = -1 / x²
        return -1.0 / (x * x);
      },
      h, 1.0, 20.0);
}
