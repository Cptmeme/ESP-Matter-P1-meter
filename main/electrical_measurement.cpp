/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.

   The ElectricalPowerMeasurement delegate/instance and accuracy tables are
   adapted from the esp-matter all_device_types_app example. The init/apply
   entry points feed live P1 (DSMR) readings into the clusters.
*/

#include "electrical_measurement.h"

#include <cmath>
#include <memory>

#include <esp_log.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Events.h>
#include <app/clusters/electrical-energy-measurement-server/ElectricalEnergyMeasurementCluster.h>
#include <app/clusters/electrical-power-measurement-server/electrical-power-measurement-server.h>
#include <app/reporting/reporting.h>
#include <app/data-model/Nullable.h>
#include <system/SystemClock.h>
#include <app/data-model/List.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::DataModel;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::ElectricalEnergyMeasurement;
using namespace chip::app::Clusters::ElectricalEnergyMeasurement::Attributes;
using namespace chip::app::Clusters::ElectricalEnergyMeasurement::Structs;
using namespace chip::app::Clusters::ElectricalPowerMeasurement;
using namespace chip::app::Clusters::ElectricalPowerMeasurement::Attributes;
using namespace chip::app::Clusters::ElectricalPowerMeasurement::Structs;

static const char *TAG = "electrical_measurement";

// Attribute access object for the Electrical Energy Measurement cluster
static std::unique_ptr<ElectricalEnergyMeasurementAttrAccess> gEEMAttrAccess;
// Delegate + instance for the Electrical Power Measurement cluster
static std::unique_ptr<ElectricalPowerMeasurementDelegate> gEPMDelegate;
static std::unique_ptr<ElectricalPowerMeasurementInstance> gEPMInstance;

CHIP_ERROR PowerTopology::PowerTopologyDelegate::GetAvailableEndpointAtIndex(size_t index, EndpointId & endpointId)
{
    return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
}

CHIP_ERROR PowerTopology::PowerTopologyDelegate::GetActiveEndpointAtIndex(size_t index, EndpointId & endpointId)
{
    return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
}

esp_err_t electrical_measurement_init(uint16_t endpoint_id)
{
    ESP_LOGI(TAG, "Initializing Electrical Energy Measurement cluster for endpoint %d", endpoint_id);

    if (gEEMAttrAccess) {
        ESP_LOGW(TAG, "Electrical measurement already initialized");
        return ESP_OK;
    }

    gEEMAttrAccess = std::make_unique<ElectricalEnergyMeasurementAttrAccess>(
        BitMask<ElectricalEnergyMeasurement::Feature, uint32_t>(
            ElectricalEnergyMeasurement::Feature::kImportedEnergy,
            ElectricalEnergyMeasurement::Feature::kExportedEnergy,
            ElectricalEnergyMeasurement::Feature::kCumulativeEnergy,
            ElectricalEnergyMeasurement::Feature::kPeriodicEnergy),
        BitMask<ElectricalEnergyMeasurement::OptionalAttributes, uint32_t>(
            ElectricalEnergyMeasurement::OptionalAttributes::kOptionalAttributeCumulativeEnergyReset));

    MeasurementAccuracyRangeStruct::Type energyAccuracyRanges[] = {
        {
            .rangeMin   = 0,
            .rangeMax   = 1000000000000000, // 1 million MWh
            .percentMax = MakeOptional(static_cast<chip::Percent100ths>(500)),
            .percentMin = MakeOptional(static_cast<chip::Percent100ths>(50))
        }
    };

    MeasurementAccuracyStruct::Type accuracy = {
        .measurementType  = MeasurementTypeEnum::kElectricalEnergy,
        .measured         = true,
        .minMeasuredValue = 0,
        .maxMeasuredValue = 1000000000000000, // 1 million MWh
        .accuracyRanges   = chip::app::DataModel::List<const MeasurementAccuracyRangeStruct::Type>(energyAccuracyRanges, 1)
    };

    CumulativeEnergyResetStruct::Type resetStruct = {
        .importedResetTimestamp = MakeOptional(MakeNullable(static_cast<uint32_t>(0))),
        .exportedResetTimestamp = MakeOptional(MakeNullable(static_cast<uint32_t>(0))),
        .importedResetSystime   = MakeOptional(MakeNullable(static_cast<uint64_t>(0))),
        .exportedResetSystime   = MakeOptional(MakeNullable(static_cast<uint64_t>(0)))
    };

    gEEMAttrAccess->Init();

    CHIP_ERROR err = SetMeasurementAccuracy(endpoint_id, accuracy);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to set measurement accuracy: %" CHIP_ERROR_FORMAT, err.Format());
        return ESP_FAIL;
    }

    err = SetCumulativeReset(endpoint_id, MakeOptional(resetStruct));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to set cumulative reset: %" CHIP_ERROR_FORMAT, err.Format());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Initializing Electrical Power Measurement cluster for endpoint %d", endpoint_id);

    gEPMDelegate = std::make_unique<ElectricalPowerMeasurementDelegate>();
    if (!gEPMDelegate) {
        ESP_LOGE(TAG, "Failed to allocate EPM delegate");
        return ESP_FAIL;
    }

    gEPMInstance = std::make_unique<ElectricalPowerMeasurementInstance>(
        EndpointId(endpoint_id), *gEPMDelegate,
        BitMask<ElectricalPowerMeasurement::Feature, uint32_t>(
            ElectricalPowerMeasurement::Feature::kDirectCurrent,
            ElectricalPowerMeasurement::Feature::kAlternatingCurrent),
        BitMask<ElectricalPowerMeasurement::OptionalAttributes, uint32_t>(
            ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeVoltage,
            ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeActiveCurrent));

    if (!gEPMInstance) {
        ESP_LOGE(TAG, "Failed to allocate EPM instance");
        gEPMDelegate.reset();
        return ESP_FAIL;
    }

    err = gEPMInstance->Init();
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Init failed on gEPMInstance: %" CHIP_ERROR_FORMAT, err.Format());
        gEPMInstance.reset();
        gEPMDelegate.reset();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Electrical Energy and Power Measurement clusters initialized");
    return ESP_OK;
}

void electrical_measurement_apply(uint16_t endpoint_id, const dsmr_data_t *data)
{
    if (!gEPMDelegate || !gEEMAttrAccess) {
        ESP_LOGW(TAG, "apply() before init - skipping");
        return;
    }

    // Net active power (import positive, export negative), kW -> mW.
    if (data->has_power_import || data->has_power_export) {
        double net_kw = (data->has_power_import ? data->power_import_kw : 0.0) -
                        (data->has_power_export ? data->power_export_kw : 0.0);
        gEPMDelegate->SetActivePower(MakeNullable(static_cast<int64_t>(llround(net_kw * 1000000.0))));
    }

    // Representative phase voltage / current for the aggregate sensor (L1), V/A -> mV/mA.
    if (data->has_voltage[0]) {
        gEPMDelegate->SetVoltage(MakeNullable(static_cast<int64_t>(llround(data->voltage_v[0] * 1000.0))));
    }
    if (data->has_current[0]) {
        gEPMDelegate->SetActiveCurrent(MakeNullable(static_cast<int64_t>(llround(data->current_a[0] * 1000.0))));
    }

    // Cumulative imported/exported energy, kWh -> mWh. Updates the cluster
    // attributes and emits a CumulativeEnergyMeasured event.
    if (data->has_energy_import || data->has_energy_export) {
        uint64_t sys = static_cast<uint64_t>(System::SystemClock().GetMonotonicTimestamp().count());

        Optional<EnergyMeasurementStruct::Type> imported;
        Optional<EnergyMeasurementStruct::Type> exported;

        if (data->has_energy_import) {
            EnergyMeasurementStruct::Type e = {
                .energy     = static_cast<int64_t>(llround(data->energy_import_kwh * 1000000.0)),
                .endSystime = MakeOptional(sys),
            };
            imported = MakeOptional(e);
        }
        if (data->has_energy_export) {
            EnergyMeasurementStruct::Type e = {
                .energy     = static_cast<int64_t>(llround(data->energy_export_kwh * 1000000.0)),
                .endSystime = MakeOptional(sys),
            };
            exported = MakeOptional(e);
        }

        if (!NotifyCumulativeEnergyMeasured(endpoint_id, imported, exported)) {
            ESP_LOGW(TAG, "Failed to report cumulative energy");
        }
    }
}

//
// Implementation of ElectricalPowerMeasurementDelegate methods
//
namespace chip {
namespace app {
namespace Clusters {
namespace ElectricalPowerMeasurement {

const MeasurementAccuracyRangeStruct::Type activePowerAccuracyRanges[] = {
    {
        .rangeMin       = -50'000'000, // -50kW
        .rangeMax       = -10'000'000, // -10kW
        .percentMax     = MakeOptional(static_cast<chip::Percent100ths>(5000)),
        .percentMin     = MakeOptional(static_cast<chip::Percent100ths>(2000)),
        .percentTypical = MakeOptional(static_cast<chip::Percent100ths>(3000)),
    },
    {
        .rangeMin       = -9'999'999, // -9.999kW
        .rangeMax       = 9'999'999,  //  9.999kW
        .percentMax     = MakeOptional(static_cast<chip::Percent100ths>(1000)),
        .percentMin     = MakeOptional(static_cast<chip::Percent100ths>(100)),
        .percentTypical = MakeOptional(static_cast<chip::Percent100ths>(500)),
    },
    {
        .rangeMin       = 10'000'000, // 10 kW
        .rangeMax       = 50'000'000, // 50 kW
        .percentMax     = MakeOptional(static_cast<chip::Percent100ths>(5000)),
        .percentMin     = MakeOptional(static_cast<chip::Percent100ths>(2000)),
        .percentTypical = MakeOptional(static_cast<chip::Percent100ths>(3000)),
    },
};

const MeasurementAccuracyRangeStruct::Type activeCurrentAccuracyRanges[] = {
    {
        .rangeMin       = -100'000, // -100A
        .rangeMax       = -5'000,   // -5A
        .percentMax     = MakeOptional(static_cast<chip::Percent100ths>(5000)),
        .percentMin     = MakeOptional(static_cast<chip::Percent100ths>(2000)),
        .percentTypical = MakeOptional(static_cast<chip::Percent100ths>(3000)),
    },
    {
        .rangeMin       = -4'999, // -4.999A
        .rangeMax       = 4'999,  //  4.999A
        .percentMax     = MakeOptional(static_cast<chip::Percent100ths>(1000)),
        .percentMin     = MakeOptional(static_cast<chip::Percent100ths>(100)),
        .percentTypical = MakeOptional(static_cast<chip::Percent100ths>(500)),
    },
    {
        .rangeMin       = 5'000,   // 5A
        .rangeMax       = 100'000, // 100 A
        .percentMax     = MakeOptional(static_cast<chip::Percent100ths>(5000)),
        .percentMin     = MakeOptional(static_cast<chip::Percent100ths>(2000)),
        .percentTypical = MakeOptional(static_cast<chip::Percent100ths>(3000)),
    },
};

const MeasurementAccuracyRangeStruct::Type voltageAccuracyRanges[] = {
    {
        .rangeMin       = -500'000, // -500V
        .rangeMax       = -100'000, // -100V
        .percentMax     = MakeOptional(static_cast<chip::Percent100ths>(5000)),
        .percentMin     = MakeOptional(static_cast<chip::Percent100ths>(2000)),
        .percentTypical = MakeOptional(static_cast<chip::Percent100ths>(3000)),
    },
    {
        .rangeMin       = -99'999, // -99.999V
        .rangeMax       = 99'999,  //  99.999V
        .percentMax     = MakeOptional(static_cast<chip::Percent100ths>(1000)),
        .percentMin     = MakeOptional(static_cast<chip::Percent100ths>(100)),
        .percentTypical = MakeOptional(static_cast<chip::Percent100ths>(500)),
    },
    {
        .rangeMin       = 100'000, // 100 V
        .rangeMax       = 500'000, // 500 V
        .percentMax     = MakeOptional(static_cast<chip::Percent100ths>(5000)),
        .percentMin     = MakeOptional(static_cast<chip::Percent100ths>(2000)),
        .percentTypical = MakeOptional(static_cast<chip::Percent100ths>(3000)),
    }
};

static const Structs::MeasurementAccuracyStruct::Type kMeasurementAccuracies[] = {
    {
        .measurementType  = MeasurementTypeEnum::kActivePower,
        .measured         = true,
        .minMeasuredValue = -50'000'000, // -50 kW
        .maxMeasuredValue = 50'000'000,  //  50 kW
        .accuracyRanges   = DataModel::List<const MeasurementAccuracyRangeStruct::Type>(activePowerAccuracyRanges),
    },
    {
        .measurementType  = MeasurementTypeEnum::kActiveCurrent,
        .measured         = true,
        .minMeasuredValue = -100'000, // -100A
        .maxMeasuredValue = 100'000,  //  100A
        .accuracyRanges   = DataModel::List<const MeasurementAccuracyRangeStruct::Type>(activeCurrentAccuracyRanges),
    },
    {
        .measurementType  = MeasurementTypeEnum::kVoltage,
        .measured         = true,
        .minMeasuredValue = -500'000, // -500V
        .maxMeasuredValue = 500'000,  //  500V
        .accuracyRanges   = DataModel::List<const MeasurementAccuracyRangeStruct::Type>(voltageAccuracyRanges),
    },
};

static const Structs::HarmonicMeasurementStruct::Type kHarmonicCurrentMeasurements[] = {
    { .order = 1, .measurement = MakeNullable(static_cast<int64_t>(100000)) }
};

static const Structs::HarmonicMeasurementStruct::Type kHarmonicPhaseMeasurements[] = {
    { .order = 1, .measurement = MakeNullable(static_cast<int64_t>(100000)) }
};

uint8_t ElectricalPowerMeasurementDelegate::GetNumberOfMeasurementTypes()
{
    return MATTER_ARRAY_SIZE(kMeasurementAccuracies);
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::StartAccuracyRead()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::GetAccuracyByIndex(uint8_t index, Structs::MeasurementAccuracyStruct::Type & accuracy)
{
    if (index >= MATTER_ARRAY_SIZE(kMeasurementAccuracies)) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    accuracy = kMeasurementAccuracies[index];
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::EndAccuracyRead()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::StartRangesRead()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::GetRangeByIndex(uint8_t index, Structs::MeasurementRangeStruct::Type & range)
{
    return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::EndRangesRead()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::StartHarmonicCurrentsRead()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::GetHarmonicCurrentsByIndex(uint8_t index, Structs::HarmonicMeasurementStruct::Type & harmonics)
{
    if (index >= MATTER_ARRAY_SIZE(kHarmonicCurrentMeasurements)) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    harmonics = kHarmonicCurrentMeasurements[index];
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::EndHarmonicCurrentsRead()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::StartHarmonicPhasesRead()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::GetHarmonicPhasesByIndex(uint8_t index, Structs::HarmonicMeasurementStruct::Type & harmonics)
{
    if (index >= MATTER_ARRAY_SIZE(kHarmonicPhaseMeasurements)) {
        return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
    }
    harmonics = kHarmonicPhaseMeasurements[index];
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::EndHarmonicPhasesRead()
{
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetPowerMode(PowerModeEnum newValue)
{
    PowerModeEnum oldValue = mPowerMode;
    if (EnsureKnownEnumValue(newValue) == PowerModeEnum::kUnknownEnumValue) {
        return CHIP_IM_GLOBAL_STATUS(ConstraintError);
    }
    mPowerMode = newValue;
    if (oldValue != newValue) {
        ChipLogDetail(AppServer, "mPowerMode updated to %d", static_cast<int>(mPowerMode));
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, PowerMode::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetVoltage(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mVoltage;
    mVoltage = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, Voltage::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetActiveCurrent(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mActiveCurrent;
    mActiveCurrent = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, ActiveCurrent::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetReactiveCurrent(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mReactiveCurrent;
    mReactiveCurrent = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, ReactiveCurrent::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetApparentCurrent(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mApparentCurrent;
    mApparentCurrent = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, ApparentCurrent::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetActivePower(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mActivePower;
    mActivePower = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, ActivePower::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetReactivePower(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mReactivePower;
    mReactivePower = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, ReactivePower::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetApparentPower(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mApparentPower;
    mApparentPower = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, ApparentPower::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetRMSVoltage(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mRMSVoltage;
    mRMSVoltage = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, RMSVoltage::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetRMSCurrent(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mRMSCurrent;
    mRMSCurrent = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, RMSCurrent::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetRMSPower(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mRMSPower;
    mRMSPower = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, RMSPower::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetFrequency(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mFrequency;
    mFrequency = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, Frequency::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetPowerFactor(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mPowerFactor;
    mPowerFactor = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, PowerFactor::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementDelegate::SetNeutralCurrent(DataModel::Nullable<int64_t> newValue)
{
    DataModel::Nullable<int64_t> oldValue = mNeutralCurrent;
    mNeutralCurrent = newValue;
    if (oldValue != newValue) {
        MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, NeutralCurrent::Id);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR ElectricalPowerMeasurementInstance::Init()
{
    return Instance::Init();
}

void ElectricalPowerMeasurementInstance::Shutdown()
{
    Instance::Shutdown();
}

} // namespace ElectricalPowerMeasurement
} // namespace Clusters
} // namespace app
} // namespace chip
