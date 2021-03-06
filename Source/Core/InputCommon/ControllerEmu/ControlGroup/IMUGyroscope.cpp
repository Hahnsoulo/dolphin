// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "InputCommon/ControllerEmu/ControlGroup/IMUGyroscope.h"

#include <memory>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Control/Input.h"

namespace ControllerEmu
{
// Maximum period for calculating an average stable value.
// Just to prevent failures due to timer overflow.
static constexpr auto MAXIMUM_CALIBRATION_DURATION = std::chrono::hours(1);

// If calibration updates do not happen at this rate, restart calibration period.
// This prevents calibration across periods of no regular updates. (e.g. between game sessions)
// This is made slightly lower than the UI update frequency of 30.
static constexpr auto WORST_ACCEPTABLE_CALIBRATION_UPDATE_FREQUENCY = 25;

IMUGyroscope::IMUGyroscope(std::string name, std::string ui_name)
    : ControlGroup(std::move(name), std::move(ui_name), GroupType::IMUGyroscope)
{
  AddInput(Translate, _trans("Pitch Up"));
  AddInput(Translate, _trans("Pitch Down"));
  AddInput(Translate, _trans("Roll Left"));
  AddInput(Translate, _trans("Roll Right"));
  AddInput(Translate, _trans("Yaw Left"));
  AddInput(Translate, _trans("Yaw Right"));

  AddSetting(&m_deadzone_setting,
             {_trans("Dead Zone"),
              // i18n: "°/s" is the symbol for degrees (angular measurement) divided by seconds.
              _trans("°/s"),
              // i18n: Refers to the dead-zone setting of gyroscope input.
              _trans("Angular velocity to ignore.")},
             2, 0, 180);

  AddSetting(&m_calibration_period_setting,
             {_trans("Calibration Period"),
              // i18n: "s" is the symbol for seconds.
              _trans("s"),
              // i18n: Refers to the "Calibration" setting of gyroscope input.
              _trans("Time period of stable input to trigger calibration. (zero to disable)")},
             3, 0, 30);
}

void IMUGyroscope::RestartCalibration() const
{
  m_calibration_period_start = Clock::now();
  m_running_calibration.Clear();
}

void IMUGyroscope::UpdateCalibration(const StateData& state) const
{
  const auto now = Clock::now();
  const auto calibration_period = m_calibration_period_setting.GetValue();

  // If calibration time is zero. User is choosing to not calibrate.
  if (!calibration_period)
  {
    // Set calibration to zero.
    m_calibration = {};
    RestartCalibration();
    return;
  }

  // If there is no running calibration a new gyro was just mapped or calibration was just enabled,
  // apply the current state as calibration, it's often better than zeros.
  if (!m_running_calibration.Count())
  {
    m_calibration = state;
  }
  else
  {
    const auto calibration_freq =
        m_running_calibration.Count() /
        std::chrono::duration_cast<std::chrono::duration<double>>(now - m_calibration_period_start)
            .count();

    const auto potential_calibration = m_running_calibration.Mean();
    const auto current_difference = state - potential_calibration;
    const auto deadzone = GetDeadzone();

    // Check for required calibration update frequency
    // and if current data is within deadzone distance of mean stable value.
    if (calibration_freq < WORST_ACCEPTABLE_CALIBRATION_UPDATE_FREQUENCY ||
        std::any_of(current_difference.data.begin(), current_difference.data.end(),
                    [&](auto c) { return std::abs(c) > deadzone; }))
    {
      RestartCalibration();
    }
  }

  // Update running mean stable value.
  m_running_calibration.Push(state);

  // Apply calibration after configured time.
  const auto calibration_duration = now - m_calibration_period_start;
  if (calibration_duration >= std::chrono::duration<double>(calibration_period))
  {
    m_calibration = m_running_calibration.Mean();

    if (calibration_duration >= MAXIMUM_CALIBRATION_DURATION)
    {
      RestartCalibration();
      m_running_calibration.Push(m_calibration);
    }
  }
}

auto IMUGyroscope::GetRawState() const -> StateData
{
  return StateData(controls[1]->GetState() - controls[0]->GetState(),
                   controls[2]->GetState() - controls[3]->GetState(),
                   controls[4]->GetState() - controls[5]->GetState());
}

std::optional<IMUGyroscope::StateData> IMUGyroscope::GetState() const
{
  if (controls[0]->control_ref->BoundCount() == 0)
  {
    // Set calibration to zero.
    m_calibration = {};
    RestartCalibration();
    return std::nullopt;
  }

  auto state = GetRawState();

  // If the input gate is disabled, miscalibration to zero values would occur.
  if (ControlReference::GetInputGate())
    UpdateCalibration(state);

  state -= m_calibration;

  // Apply "deadzone".
  for (auto& c : state.data)
    c *= std::abs(c) > GetDeadzone();

  return state;
}

ControlState IMUGyroscope::GetDeadzone() const
{
  return m_deadzone_setting.GetValue() / 360 * MathUtil::TAU;
}

bool IMUGyroscope::IsCalibrating() const
{
  const auto calibration_period = m_calibration_period_setting.GetValue();
  return calibration_period && (Clock::now() - m_calibration_period_start) >=
                                   std::chrono::duration<double>(calibration_period);
}

}  // namespace ControllerEmu
