// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#pragma once

#include <cstddef>
#include <string_view>

namespace sysid {

/**
 * Stores the type of Analysis and relevant information about the analysis.
 */
struct AnalysisType {
  /**
   * The number of independent variables for feedforward analysis.
   */
  size_t independentVariables;

  /**
   * The number of fields in the raw data within the mechanism's JSON.
   */
  size_t rawDataSize;

  /**
   * Display name for the analysis type.
   */
  const char* name;

  /**
   * Compares equality of two AnalysisType structs.
   *
   * @param rhs Another AnalysisType
   * @return True if the two analysis types are equal
   */
  constexpr bool operator==(const AnalysisType& rhs) const {
    return std::string_view{name} == rhs.name &&
           independentVariables == rhs.independentVariables &&
           rawDataSize == rhs.rawDataSize;
  }

  /**
   * Compares inequality of two AnalysisType structs.
   *
   * @param rhs Another AnalysisType
   * @return True if the two analysis types are not equal
   */
  constexpr bool operator!=(const AnalysisType& rhs) const {
    return !operator==(rhs);
  }
};

namespace analysis {
constexpr AnalysisType kDrivetrain{3, 9, "Drivetrain"};
constexpr AnalysisType kDrivetrainAngular{3, 9, "Drivetrain (Angular)"};
constexpr AnalysisType kElevator{4, 4, "Elevator"};
constexpr AnalysisType kArm{4, 4, "Arm"};
constexpr AnalysisType kSimple{3, 4, "Simple"};

AnalysisType FromName(std::string_view name);
}  // namespace analysis
}  // namespace sysid
