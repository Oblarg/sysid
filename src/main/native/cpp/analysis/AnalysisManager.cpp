// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "sysid/analysis/AnalysisManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <units/angle.h>
#include <wpi/StringMap.h>
#include <wpi/json.h>
#include <wpi/math>
#include <wpi/raw_istream.h>
#include <wpi/raw_ostream.h>

#include "sysid/analysis/AnalysisType.h"
#include "sysid/analysis/FilteringUtils.h"
#include "sysid/analysis/JSONConverter.h"
#include "sysid/analysis/TrackWidthAnalysis.h"

using namespace sysid;

/**
 * Concatenates a list of vectors to the end of a vector. The contents of the
 * source vectors are copied (not moved) into the new vector.
 */
static std::vector<PreparedData> Concatenate(
    std::vector<PreparedData> dest,
    std::initializer_list<const std::vector<PreparedData>*> srcs) {
  // Copy the contents of the source vectors into the dest vector.
  for (auto ptr : srcs) {
    dest.insert(dest.end(), ptr->cbegin(), ptr->cend());
  }

  // Return the dest vector.
  return dest;
}

/**
 * Computes acceleration from a vector of raw data and returns prepared data.
 *
 * @tparam S        The size of the raw data array.
 * @tparam Voltage  The index of the voltage entry in the raw data.
 * @tparam Position The index of the position entry in the raw data.
 * @tparam Velocity The index of the velocity entry in the raw data.
 *
 * @param data A reference to a vector of the raw data.
 */
template <size_t S, size_t Voltage, size_t Position, size_t Velocity>
std::vector<PreparedData> ComputeAcceleration(
    const std::vector<std::array<double, S>>& data, int window) {
  // Calculate the step size for acceleration data.
  size_t step = window / 2;

  // Create our prepared data vector.
  std::vector<PreparedData> prepared;
  if (data.size() <= static_cast<size_t>(window)) {
    throw std::runtime_error(
        "The data collected is too small! This can be caused by too high of a "
        "motion threshold or bad data collection.");
  }
  prepared.reserve(data.size());

  // Compute acceleration and add it to the vector.
  for (size_t i = step; i < data.size() - step; ++i) {
    const auto& pt = data[i];
    double acc = (data[i + step][Velocity] - data[i - step][Velocity]) /
                 (data[i + step][0] - data[i - step][0]);

    // Sometimes, if the encoder velocities are the same, it will register zero
    // acceleration. Do not include these values.
    if (acc != 0) {
      prepared.push_back(PreparedData{units::second_t{pt[0]}, pt[Voltage],
                                      pt[Position], pt[Velocity], acc, 0.0});
    }
  }
  return prepared;
}

/**
 * Calculates the cosine of the position data for single jointed arm analysis.
 *
 * @param data The data to calculate the cosine on.
 * @param unit The units that the data is in (rotations, radians, or degrees).
 */
static void CalculateCosine(std::vector<PreparedData>* data,
                            wpi::StringRef unit) {
  for (auto&& pt : *data) {
    if (unit == "Radians") {
      pt.cos = std::cos(pt.position);
    } else if (unit == "Degrees") {
      pt.cos = std::cos(pt.position * wpi::math::pi / 180.0);
    } else if (unit == "Rotations") {
      pt.cos = std::cos(pt.position * 2 * wpi::math::pi);
    }
  }
}

template <size_t S>
static units::second_t GetMaxTime(
    wpi::StringMap<std::vector<std::array<double, S>>> data, size_t timeCol) {
  return units::second_t{std::max(
      data["fast-forward"].back()[timeCol] - data["fast-forward"][0][timeCol],
      data["fast-backward"].back()[timeCol] -
          data["fast-backward"][0][timeCol])};
}

/**
 * Prepares data for general mechanisms (i.e. not drivetrain) and stores them
 * in the analysis manager dataset.
 *
 * @param json     A reference to the JSON containing all of the collected
 * data.
 * @param settings A reference to the settings being used by the analysis
 *                 manager instance.
 * @param factor   The units per rotation to multiply positions and velocities
 *                 by.
 * @param datasets A reference to the datasets object of the relevant analysis
 *                 manager instance.
 */
static void PrepareGeneralData(const wpi::json& json,
                               AnalysisManager::Settings& settings,
                               double factor, wpi::StringRef unit,
                               wpi::StringMap<Storage>& rawDatasets,
                               wpi::StringMap<Storage>& filteredDatasets,
                               std::array<units::second_t, 4>& startTimes,
                               units::second_t& minStepTime,
                               units::second_t& maxStepTime) {
  using Data = std::array<double, 4>;
  wpi::StringMap<std::vector<Data>> data;

  // Store the raw data columns.
  static constexpr size_t kTimeCol = 0;
  static constexpr size_t kVoltageCol = 1;
  static constexpr size_t kPosCol = 2;
  static constexpr size_t kVelCol = 3;

  // Get the major components from the JSON and store them inside a StringMap.
  for (auto&& key : AnalysisManager::kJsonDataKeys) {
    data[key] = json.at(key).get<std::vector<Data>>();
  }

  // Ensure that voltage and velocity have the same sign. Also multiply
  // positions and velocities by the factor.
  for (auto it = data.begin(); it != data.end(); ++it) {
    for (auto&& pt : it->second) {
      pt[kVoltageCol] = std::copysign(pt[kVoltageCol], pt[kVelCol]);
      pt[kPosCol] *= factor;
      pt[kVelCol] *= factor;
    }
  }

  // Trim quasistatic test data to remove all points where voltage is zero or
  // velocity < motion threshold.
  sysid::TrimQuasistaticData<4, kVoltageCol, kVelCol>(&data["slow-forward"],
                                                      settings.motionThreshold);
  sysid::TrimQuasistaticData<4, kVoltageCol, kVelCol>(&data["slow-backward"],
                                                      settings.motionThreshold);

  // Compute acceleration on raw data
  auto rawSf = ComputeAcceleration<4, kVoltageCol, kPosCol, kVelCol>(
      data["slow-forward"], settings.windowSize);
  auto rawSb = ComputeAcceleration<4, kVoltageCol, kPosCol, kVelCol>(
      data["slow-backward"], settings.windowSize);
  auto rawFf = ComputeAcceleration<4, kVoltageCol, kPosCol, kVelCol>(
      data["fast-forward"], settings.windowSize);
  auto rawFb = ComputeAcceleration<4, kVoltageCol, kPosCol, kVelCol>(
      data["fast-backward"], settings.windowSize);

  // Compute acceleration on median filtered data sets.
  auto sf = ComputeAcceleration<4, kVoltageCol, kPosCol, kVelCol>(
      sysid::ApplyMedianFilter<4, kVelCol>(data["slow-forward"],
                                           settings.windowSize),
      settings.windowSize);
  auto sb = ComputeAcceleration<4, kVoltageCol, kPosCol, kVelCol>(
      sysid::ApplyMedianFilter<4, kVelCol>(data["slow-backward"],
                                           settings.windowSize),
      settings.windowSize);
  auto ff = ComputeAcceleration<4, kVoltageCol, kPosCol, kVelCol>(
      sysid::ApplyMedianFilter<4, kVelCol>(data["fast-forward"],
                                           settings.windowSize),
      settings.windowSize);
  auto fb = ComputeAcceleration<4, kVoltageCol, kPosCol, kVelCol>(
      sysid::ApplyMedianFilter<4, kVelCol>(data["fast-backward"],
                                           settings.windowSize),
      settings.windowSize);

  // Calculate cosine of position data.
  CalculateCosine(&sf, unit);
  CalculateCosine(&sb, unit);
  CalculateCosine(&ff, unit);
  CalculateCosine(&fb, unit);

  // Find the maximum Step Test Duration
  maxStepTime = GetMaxTime<4>(data, kTimeCol);

  // Trim the raw step voltage data.
  auto tempTime = units::second_t{
      0.0};  // Raw data shouldn't be used to calculate mininum step test time
  sysid::TrimStepVoltageData(&rawFf, settings, tempTime, maxStepTime);
  sysid::TrimStepVoltageData(&rawFb, settings, tempTime, maxStepTime);

  // Trim the step voltage data.
  sysid::TrimStepVoltageData(&ff, settings, minStepTime, maxStepTime);
  sysid::TrimStepVoltageData(&fb, settings, minStepTime, maxStepTime);

  // Store the raw datasets
  rawDatasets["Forward"] = std::make_tuple(rawSf, rawFf);
  rawDatasets["Backward"] = std::make_tuple(rawSb, rawFb);
  rawDatasets["Combined"] = std::make_tuple(Concatenate(rawSf, {&rawSb}),
                                            Concatenate(rawFf, {&rawFb}));

  // Create the distinct datasets and store them in our StringMap.
  filteredDatasets["Forward"] = std::make_tuple(sf, ff);
  filteredDatasets["Backward"] = std::make_tuple(sb, fb);
  filteredDatasets["Combined"] =
      std::make_tuple(Concatenate(sf, {&sb}), Concatenate(ff, {&fb}));
  startTimes = {sf[0].timestamp, sb[0].timestamp, ff[0].timestamp,
                fb[0].timestamp};
}

/**
 * Prepares data for angular drivetrain test data and stores them in
 * the analysis manager dataset.
 *
 * @param json     A reference to the JSON containing all of the collected
 * data.
 * @param settings A reference to the settings being used by the analysis
 *                 manager instance.
 * @param factor   The units per rotation to multiply positions and velocities
 *                 by.
 * @param tw       A reference to the std::optional where the track width will
 *                 be stored.
 * @param datasets A reference to the datasets object of the relevant analysis
 *                 manager instance.
 */
static void PrepareAngularDrivetrainData(
    const wpi::json& json, AnalysisManager::Settings& settings, double factor,
    std::optional<double>& tw, wpi::StringMap<Storage>& rawDatasets,
    wpi::StringMap<Storage>& filteredDatasets,
    std::array<units::second_t, 4>& startTimes, units::second_t& minStepTime,
    units::second_t& maxStepTime) {
  using Data = std::array<double, 9>;
  wpi::StringMap<std::vector<Data>> data;

  // Store the relevant raw data columns.
  static constexpr size_t kTimeCol = 0;
  static constexpr size_t kVoltageCol = 1;
  static constexpr size_t kLPosCol = 3;
  static constexpr size_t kRPosCol = 4;
  static constexpr size_t kAngleCol = 7;
  static constexpr size_t kAngularRateCol = 8;

  // Get the major components from the JSON and store them inside a StringMap.
  for (auto&& key : AnalysisManager::kJsonDataKeys) {
    data[key] = json.at(key).get<std::vector<Data>>();
  }

  // Ensure that voltage and velocity have the same sign. Also multiply
  // positions and velocities by the factor.
  for (auto it = data.begin(); it != data.end(); ++it) {
    for (auto&& pt : it->second) {
      pt[kVoltageCol] = 2 * std::copysign(pt[kVoltageCol], pt[kAngularRateCol]);
      pt[kLPosCol] *= factor;
      pt[kRPosCol] *= factor;
    }
  }

  // Trim quasistatic test data to remove all points where voltage is zero or
  // velocity < motion threshold.
  sysid::TrimQuasistaticData<9, kVoltageCol, kAngularRateCol>(
      &data["slow-forward"], settings.motionThreshold);
  sysid::TrimQuasistaticData<9, kVoltageCol, kAngularRateCol>(
      &data["slow-backward"], settings.motionThreshold);

  // Compute acceleration on all data sets.
  auto sf = ComputeAcceleration<9, kVoltageCol, kAngleCol, kAngularRateCol>(
      data["slow-forward"], settings.windowSize);
  auto sb = ComputeAcceleration<9, kVoltageCol, kAngleCol, kAngularRateCol>(
      data["slow-backward"], settings.windowSize);
  auto ff = ComputeAcceleration<9, kVoltageCol, kAngleCol, kAngularRateCol>(
      data["fast-forward"], settings.windowSize);
  auto fb = ComputeAcceleration<9, kVoltageCol, kAngleCol, kAngularRateCol>(
      data["fast-backward"], settings.windowSize);

  // Get Max Time
  maxStepTime = GetMaxTime<9>(data, kTimeCol);

  // Trim the step voltage data.
  sysid::TrimStepVoltageData(&ff, settings, minStepTime, maxStepTime);
  sysid::TrimStepVoltageData(&fb, settings, maxStepTime, maxStepTime);

  // Calculate track width from the slow-forward raw data.
  auto& twd = data["slow-forward"];
  double l = twd.back()[kLPosCol] - twd.front()[kLPosCol];
  double r = twd.back()[kRPosCol] - twd.front()[kRPosCol];
  double a = twd.back()[kAngleCol] - twd.front()[kAngleCol];
  tw = sysid::CalculateTrackWidth(l, r, units::radian_t(a));

  // Create the distinct datasets and store them in our StringMap.
  filteredDatasets["Forward"] = std::make_tuple(sf, ff);
  filteredDatasets["Backward"] = std::make_tuple(sb, fb);
  filteredDatasets["Combined"] =
      std::make_tuple(Concatenate(sf, {&sb}), Concatenate(ff, {&fb}));
  startTimes = {sf[0].timestamp, sb[0].timestamp, ff[0].timestamp,
                fb[0].timestamp};
}

/**
 * Prepares data for linear drivetrain test data and stores them in
 * the analysis manager dataset.
 *
 * @param json     A reference to the JSON containing all of the collected
 * data.
 * @param settings A reference to the settings being used by the analysis
 *                 manager instance.
 * @param factor   The units per rotation to multiply positions and velocities
 *                 by.
 * @param datasets A reference to the datasets object of the relevant analysis
 *                 manager instance.
 */
static void PrepareLinearDrivetrainData(
    const wpi::json& json, AnalysisManager::Settings& settings, double factor,
    wpi::StringMap<Storage>& rawDatasets,
    wpi::StringMap<Storage>& filteredDatasets,
    std::array<units::second_t, 4>& startTimes, units::second_t& minStepTime,
    units::second_t& maxStepTime) {
  using Data = std::array<double, 9>;
  wpi::StringMap<std::vector<Data>> data;

  // Store the relevant raw data columns.
  static constexpr size_t kTimeCol = 0;
  static constexpr size_t kLVoltageCol = 1;
  static constexpr size_t kRVoltageCol = 2;
  static constexpr size_t kLPosCol = 3;
  static constexpr size_t kRPosCol = 4;
  static constexpr size_t kLVelCol = 5;
  static constexpr size_t kRVelCol = 6;

  // Get the major components from the JSON and store them inside a StringMap.
  for (auto&& key : AnalysisManager::kJsonDataKeys) {
    data[key] = json.at(key).get<std::vector<Data>>();
  }

  // Ensure that voltage and velocity have the same sign. Also multiply
  // positions and velocities by the factor.
  for (auto it = data.begin(); it != data.end(); ++it) {
    for (auto&& pt : it->second) {
      pt[kLVoltageCol] = std::copysign(pt[kLVoltageCol], pt[kLVelCol]);
      pt[kRVoltageCol] = std::copysign(pt[kRVoltageCol], pt[kRVelCol]);
      pt[kLPosCol] *= factor;
      pt[kRPosCol] *= factor;
      pt[kLVelCol] *= factor;
      pt[kRVelCol] *= factor;
    }
  }

  // Trim quasistatic test data to remove all points where voltage is zero or
  // velocity < motion threshold.
  sysid::TrimQuasistaticData<9, kLVoltageCol, kLVelCol>(
      &data["slow-forward"], settings.motionThreshold);
  sysid::TrimQuasistaticData<9, kLVoltageCol, kLVelCol>(
      &data["slow-backward"], settings.motionThreshold);
  sysid::TrimQuasistaticData<9, kRVoltageCol, kRVelCol>(
      &data["slow-forward"], settings.motionThreshold);
  sysid::TrimQuasistaticData<9, kRVoltageCol, kRVelCol>(
      &data["slow-backward"], settings.motionThreshold);

  // Compute acceleration on all raw data sets.
  auto rawSfl = ComputeAcceleration<9, kLVoltageCol, kLPosCol, kLVelCol>(
      data["slow-forward"], settings.windowSize);
  auto rawSbl = ComputeAcceleration<9, kLVoltageCol, kLPosCol, kLVelCol>(
      data["slow-backward"], settings.windowSize);
  auto rawFfl = ComputeAcceleration<9, kLVoltageCol, kLPosCol, kLVelCol>(
      data["fast-forward"], settings.windowSize);
  auto rawFbl = ComputeAcceleration<9, kLVoltageCol, kLPosCol, kLVelCol>(
      data["fast-backward"], settings.windowSize);
  auto rawSfr = ComputeAcceleration<9, kRVoltageCol, kRPosCol, kRVelCol>(
      data["slow-forward"], settings.windowSize);
  auto rawSbr = ComputeAcceleration<9, kRVoltageCol, kRPosCol, kRVelCol>(
      data["slow-backward"], settings.windowSize);
  auto rawFfr = ComputeAcceleration<9, kRVoltageCol, kRPosCol, kRVelCol>(
      data["fast-forward"], settings.windowSize);
  auto rawFbr = ComputeAcceleration<9, kRVoltageCol, kRPosCol, kRVelCol>(
      data["fast-backward"], settings.windowSize);

  // Compute acceleration on all data sets.
  auto sfl = ComputeAcceleration<9, kLVoltageCol, kLPosCol, kLVelCol>(
      sysid::ApplyMedianFilter<9, kLVelCol>(data["slow-forward"],
                                            settings.windowSize),
      settings.windowSize);
  auto sbl = ComputeAcceleration<9, kLVoltageCol, kLPosCol, kLVelCol>(
      sysid::ApplyMedianFilter<9, kLVelCol>(data["slow-backward"],
                                            settings.windowSize),
      settings.windowSize);
  auto ffl = ComputeAcceleration<9, kLVoltageCol, kLPosCol, kLVelCol>(
      sysid::ApplyMedianFilter<9, kLVelCol>(data["fast-forward"],
                                            settings.windowSize),
      settings.windowSize);
  auto fbl = ComputeAcceleration<9, kLVoltageCol, kLPosCol, kLVelCol>(
      sysid::ApplyMedianFilter<9, kLVelCol>(data["fast-backward"],
                                            settings.windowSize),
      settings.windowSize);
  auto sfr = ComputeAcceleration<9, kRVoltageCol, kRPosCol, kRVelCol>(
      sysid::ApplyMedianFilter<9, kRVelCol>(data["slow-forward"],
                                            settings.windowSize),
      settings.windowSize);
  auto sbr = ComputeAcceleration<9, kRVoltageCol, kRPosCol, kRVelCol>(
      sysid::ApplyMedianFilter<9, kRVelCol>(data["slow-backward"],
                                            settings.windowSize),
      settings.windowSize);
  auto ffr = ComputeAcceleration<9, kRVoltageCol, kRPosCol, kRVelCol>(
      sysid::ApplyMedianFilter<9, kRVelCol>(data["fast-forward"],
                                            settings.windowSize),
      settings.windowSize);
  auto fbr = ComputeAcceleration<9, kRVoltageCol, kRPosCol, kRVelCol>(
      sysid::ApplyMedianFilter<9, kRVelCol>(data["fast-backward"],
                                            settings.windowSize),
      settings.windowSize);

  // Get maximum dynamic test duration
  maxStepTime = GetMaxTime<9>(data, kTimeCol);

  // Trim raw step voltage data
  auto tempTime = units::second_t{
      0.0};  // Raw data shouldn't be used to calculate mininum step test time
  sysid::TrimStepVoltageData(&rawFfl, settings, tempTime, maxStepTime);
  sysid::TrimStepVoltageData(&rawFfr, settings, tempTime, maxStepTime);
  sysid::TrimStepVoltageData(&rawFbl, settings, tempTime, maxStepTime);
  sysid::TrimStepVoltageData(&rawFbr, settings, tempTime, maxStepTime);

  // Trim the step voltage data.
  sysid::TrimStepVoltageData(&ffl, settings, minStepTime, maxStepTime);
  sysid::TrimStepVoltageData(&ffr, settings, minStepTime, maxStepTime);
  sysid::TrimStepVoltageData(&fbl, settings, minStepTime, maxStepTime);
  sysid::TrimStepVoltageData(&fbr, settings, minStepTime, maxStepTime);

  // Create the distinct raw datasets and store them in our StringMap.
  auto raw_sf = Concatenate(rawSfl, {&rawSfr});
  auto raw_sb = Concatenate(rawSbl, {&rawSbr});
  auto raw_ff = Concatenate(rawFfl, {&rawFfr});
  auto raw_fb = Concatenate(rawFbl, {&rawFbr});

  rawDatasets["Forward"] = std::make_tuple(raw_sf, raw_ff);
  rawDatasets["Backward"] = std::make_tuple(raw_sb, raw_fb);
  rawDatasets["Combined"] = std::make_tuple(Concatenate(raw_sf, {&raw_sb}),
                                            Concatenate(raw_ff, {&raw_fb}));

  rawDatasets["Left Forward"] = std::make_tuple(rawSfl, rawFfl);
  rawDatasets["Left Backward"] = std::make_tuple(rawSbl, rawFbl);
  rawDatasets["Left Combined"] = std::make_tuple(
      Concatenate(rawSfl, {&rawSbl}), Concatenate(rawFfl, {&rawFbl}));

  rawDatasets["Right Forward"] = std::make_tuple(rawSfr, rawFfr);
  rawDatasets["Right Backward"] = std::make_tuple(rawSbr, rawFbr);
  rawDatasets["Right Combined"] = std::make_tuple(
      Concatenate(rawSfr, {&rawSbr}), Concatenate(rawFfr, {&rawFbr}));

  // Create the distinct datasets and store them in our StringMap.
  auto sf = Concatenate(sfl, {&sfr});
  auto sb = Concatenate(sbl, {&sbr});
  auto ff = Concatenate(ffl, {&ffr});
  auto fb = Concatenate(fbl, {&fbr});

  filteredDatasets["Forward"] = std::make_tuple(sf, ff);
  filteredDatasets["Backward"] = std::make_tuple(sb, fb);
  filteredDatasets["Combined"] =
      std::make_tuple(Concatenate(sf, {&sb}), Concatenate(ff, {&fb}));

  filteredDatasets["Left Forward"] = std::make_tuple(sfl, ffl);
  filteredDatasets["Left Backward"] = std::make_tuple(sbl, fbl);
  filteredDatasets["Left Combined"] =
      std::make_tuple(Concatenate(sfl, {&sbl}), Concatenate(ffl, {&fbl}));

  filteredDatasets["Right Forward"] = std::make_tuple(sfr, ffr);
  filteredDatasets["Right Backward"] = std::make_tuple(sbr, fbr);
  filteredDatasets["Right Combined"] =
      std::make_tuple(Concatenate(sfr, {&sbr}), Concatenate(ffr, {&fbr}));
  startTimes = {sf[0].timestamp, sb[0].timestamp, ff[0].timestamp,
                fb[0].timestamp};
}

AnalysisManager::AnalysisManager(wpi::StringRef path, Settings& settings,
                                 wpi::Logger& logger)
    : m_settings(settings), m_logger(logger) {
  // Read JSON from the specified path.
  std::error_code ec;
  wpi::raw_fd_istream is{path, ec};

  if (ec) {
    throw std::runtime_error("Unable to read: " + path.str());
  }

  is >> m_json;
  WPI_INFO(m_logger, "Read " << path);

  // Check that we have a sysid json.
  if (m_json.find("sysid") == m_json.end()) {
    throw std::runtime_error(
        "Incorrect JSON format detected. Please use the JSON Converter "
        "to convert a frc-char JSON to a sysid JSON.");
  } else {
    // Get the analysis type from the JSON.
    m_type = sysid::analysis::FromName(m_json.at("test").get<std::string>());

    // Get the rotation -> output units factor from the JSON.
    m_unit = m_json.at("units").get<std::string>();
    m_factor = m_json.at("unitsPerRotation").get<double>();

    // Reset settings for Dynamic Test Limits
    m_settings.stepTestDuration = units::second_t{0.0};
    m_minDuration = units::second_t{100000};

    // Prepare data.
    PrepareData();
  }
}

void AnalysisManager::PrepareData() {
  if (m_type == analysis::kDrivetrain) {
    PrepareLinearDrivetrainData(m_json, m_settings, m_factor, m_rawDatasets,
                                m_filteredDatasets, m_startTimes, m_minDuration,
                                m_maxDuration);
  } else if (m_type == analysis::kDrivetrainAngular) {
    PrepareAngularDrivetrainData(m_json, m_settings, m_factor, m_trackWidth,
                                 m_rawDatasets, m_filteredDatasets,
                                 m_startTimes, m_minDuration, m_maxDuration);
  } else {
    PrepareGeneralData(m_json, m_settings, m_factor, m_unit, m_rawDatasets,
                       m_filteredDatasets, m_startTimes, m_minDuration,
                       m_maxDuration);
  }
}

AnalysisManager::Gains AnalysisManager::Calculate() {
  // Calculate feedforward gains from the data.
  auto ff = sysid::CalculateFeedforwardGains(
      m_filteredDatasets[kDatasets[m_settings.dataset]], m_type);

  // Create the struct that we need for feedback analysis.
  auto& f = std::get<0>(ff);
  FeedforwardGains gains = {f[0], f[1], f[2]};

  // Calculate the appropriate gains.
  std::tuple<double, double> fb;
  if (m_settings.type == FeedbackControllerLoopType::kPosition) {
    fb = sysid::CalculatePositionFeedbackGains(
        m_settings.preset, m_settings.lqr, gains,
        m_settings.convertGainsToEncTicks
            ? m_settings.gearing * m_settings.cpr * m_factor
            : 1);
  } else {
    fb = sysid::CalculateVelocityFeedbackGains(
        m_settings.preset, m_settings.lqr, gains,
        m_settings.convertGainsToEncTicks
            ? m_settings.gearing * m_settings.cpr * m_factor
            : 1);
  }
  return {ff, fb, m_trackWidth};
}

void AnalysisManager::OverrideUnits(const std::string& unit,
                                    double unitsPerRotation) {
  m_unit = unit;
  m_factor = unitsPerRotation;
  PrepareData();
}

void AnalysisManager::ResetUnitsFromJSON() {
  m_unit = m_json.at("units").get<std::string>();
  m_factor = m_json.at("unitsPerRotation").get<double>();
  PrepareData();
}
