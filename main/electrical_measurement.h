/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.

   Electrical Power / Energy Measurement glue for the Matter Electrical Sensor
   endpoint. The delegate/instance classes below are adapted from the esp-matter
   all_device_types_app example.
*/

#pragma once

#include <esp_err.h>
#include <app/clusters/electrical-power-measurement-server/electrical-power-measurement-server.h>
#include <app/util/af-types.h>
#include <lib/core/CHIPError.h>

#include "dsmr_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the Electrical Power + Energy Measurement clusters on the given
 * endpoint (accuracy ranges, delegate/instance, attribute access). Must run on
 * the Matter task. */
esp_err_t electrical_measurement_init(uint16_t endpoint_id);

/* Push one parsed P1 reading into the Matter attributes/events. Must run on the
 * Matter task (schedule it with PlatformMgr().ScheduleWork). */
void electrical_measurement_apply(uint16_t endpoint_id, const dsmr_data_t *data);

#ifdef __cplusplus
}
#endif

#include <app/clusters/power-topology-server/power-topology-server.h>

using namespace chip;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::PowerTopology;

namespace chip {
namespace app {
namespace Clusters {
namespace PowerTopology {

class PowerTopologyDelegate : public Delegate
{
public:
    ~PowerTopologyDelegate() = default;

    CHIP_ERROR GetAvailableEndpointAtIndex(size_t index, EndpointId & endpointId) override;
    CHIP_ERROR GetActiveEndpointAtIndex(size_t index, EndpointId & endpointId) override;
};

} // namespace PowerTopology
} // namespace Clusters
} // namespace app
} // namespace chip

namespace chip {
namespace app {
namespace Clusters {
namespace ElectricalPowerMeasurement {

class ElectricalPowerMeasurementDelegate : public ElectricalPowerMeasurement::Delegate
{
public:
    ~ElectricalPowerMeasurementDelegate() = default;

    static constexpr uint8_t kMaxNumberOfMeasurementTypes     = 14; // From spec
    static constexpr uint8_t kDefaultNumberOfMeasurementTypes = 1;

    // Attribute Accessors
    PowerModeEnum GetPowerMode() override { return mPowerMode; }
    uint8_t GetNumberOfMeasurementTypes() override;

    CHIP_ERROR StartAccuracyRead() override;
    CHIP_ERROR GetAccuracyByIndex(uint8_t, Structs::MeasurementAccuracyStruct::Type &) override;
    CHIP_ERROR EndAccuracyRead() override;

    CHIP_ERROR StartRangesRead() override;
    CHIP_ERROR GetRangeByIndex(uint8_t, Structs::MeasurementRangeStruct::Type &) override;
    CHIP_ERROR EndRangesRead() override;

    CHIP_ERROR StartHarmonicCurrentsRead() override;
    CHIP_ERROR GetHarmonicCurrentsByIndex(uint8_t, Structs::HarmonicMeasurementStruct::Type &) override;
    CHIP_ERROR EndHarmonicCurrentsRead() override;

    CHIP_ERROR StartHarmonicPhasesRead() override;
    CHIP_ERROR GetHarmonicPhasesByIndex(uint8_t, Structs::HarmonicMeasurementStruct::Type &) override;
    CHIP_ERROR EndHarmonicPhasesRead() override;

    DataModel::Nullable<int64_t> GetVoltage() override { return mVoltage; }
    DataModel::Nullable<int64_t> GetActiveCurrent() override { return mActiveCurrent; }
    DataModel::Nullable<int64_t> GetReactiveCurrent() override { return mReactiveCurrent; }
    DataModel::Nullable<int64_t> GetApparentCurrent() override { return mApparentCurrent; }
    DataModel::Nullable<int64_t> GetActivePower() override { return mActivePower; }
    DataModel::Nullable<int64_t> GetReactivePower() override { return mReactivePower; }
    DataModel::Nullable<int64_t> GetApparentPower() override { return mApparentPower; }
    DataModel::Nullable<int64_t> GetRMSVoltage() override { return mRMSVoltage; }
    DataModel::Nullable<int64_t> GetRMSCurrent() override { return mRMSCurrent; }
    DataModel::Nullable<int64_t> GetRMSPower() override { return mRMSPower; }
    DataModel::Nullable<int64_t> GetFrequency() override { return mFrequency; }
    DataModel::Nullable<int64_t> GetPowerFactor() override { return mPowerFactor; }
    DataModel::Nullable<int64_t> GetNeutralCurrent() override { return mNeutralCurrent; };

    // Internal Application API to set attribute values
    CHIP_ERROR SetPowerMode(PowerModeEnum);
    CHIP_ERROR SetVoltage(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetActiveCurrent(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetReactiveCurrent(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetApparentCurrent(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetActivePower(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetReactivePower(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetApparentPower(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetRMSVoltage(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetRMSCurrent(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetRMSPower(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetFrequency(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetPowerFactor(DataModel::Nullable<int64_t>);
    CHIP_ERROR SetNeutralCurrent(DataModel::Nullable<int64_t>);

private:
    PowerModeEnum mPowerMode = PowerModeEnum::kUnknown;
    DataModel::Nullable<int64_t> mVoltage;
    DataModel::Nullable<int64_t> mActiveCurrent;
    DataModel::Nullable<int64_t> mReactiveCurrent;
    DataModel::Nullable<int64_t> mApparentCurrent;
    DataModel::Nullable<int64_t> mActivePower;
    DataModel::Nullable<int64_t> mReactivePower;
    DataModel::Nullable<int64_t> mApparentPower;
    DataModel::Nullable<int64_t> mRMSVoltage;
    DataModel::Nullable<int64_t> mRMSCurrent;
    DataModel::Nullable<int64_t> mRMSPower;
    DataModel::Nullable<int64_t> mFrequency;
    DataModel::Nullable<int64_t> mPowerFactor;
    DataModel::Nullable<int64_t> mNeutralCurrent;
};

class ElectricalPowerMeasurementInstance : public Instance
{
public:
    ElectricalPowerMeasurementInstance(EndpointId aEndpointId, ElectricalPowerMeasurementDelegate & aDelegate, Feature aFeature,
                                      OptionalAttributes aOptionalAttributes) :
        ElectricalPowerMeasurement::Instance(aEndpointId, aDelegate, aFeature, aOptionalAttributes)
    {
        mDelegate = &aDelegate;
    }

    ElectricalPowerMeasurementInstance(const ElectricalPowerMeasurementInstance &)             = delete;
    ElectricalPowerMeasurementInstance(const ElectricalPowerMeasurementInstance &&)            = delete;
    ElectricalPowerMeasurementInstance & operator=(const ElectricalPowerMeasurementInstance &) = delete;

    CHIP_ERROR Init();
    void Shutdown();

    ElectricalPowerMeasurementDelegate * GetDelegate() { return mDelegate; };

private:
    ElectricalPowerMeasurementDelegate * mDelegate;
};

} // namespace ElectricalPowerMeasurement
} // namespace Clusters
} // namespace app
} // namespace chip
