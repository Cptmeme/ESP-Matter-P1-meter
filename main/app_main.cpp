/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <new>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>
#include <log_heap_numbers.h>

#include <app_priv.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>

#include "electrical_measurement.h"
#include "p1_reader.h"
#include "dsmr_parser.h"

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
#include <esp_matter_providers.h>
#include <lib/support/Span.h>
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
#include <platform/ESP32/ESP32SecureCertDACProvider.h>
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif
using namespace chip::DeviceLayer;
#endif

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;

/* The Electrical Sensor endpoint that mirrors the smart meter's P1 data. */
static uint16_t s_electrical_endpoint_id = 0;
/* PowerTopology delegate for the electrical sensor (reports a flat topology). */
static chip::app::Clusters::PowerTopology::PowerTopologyDelegate s_power_topology_delegate;

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
extern const uint8_t cd_start[] asm("_binary_certification_declaration_der_start");
extern const uint8_t cd_end[] asm("_binary_certification_declaration_der_end");

const chip::ByteSpan cdSpan(cd_start, static_cast<size_t>(cd_end - cd_start));
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

#if CONFIG_ENABLE_ENCRYPTED_OTA
extern const char decryption_key_start[] asm("_binary_esp_image_encryption_key_pem_start");
extern const char decryption_key_end[] asm("_binary_esp_image_encryption_key_pem_end");

static const char *s_decryption_key = decryption_key_start;
static const uint16_t s_decryption_key_len = decryption_key_end - decryption_key_start;
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning window opened");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        {
            ESP_LOGI(TAG, "Fabric removed successfully");
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0)
            {
                chip::CommissioningWindowManager & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
                constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
                if (!commissionMgr.IsCommissioningWindowOpen())
                {
                    /* After removing last fabric, this example does not remove the Wi-Fi credentials
                     * and still has IP connectivity so, only advertising on DNS-SD.
                     */
                    CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                    chip::CommissioningWindowAdvertisement::kDnssdOnly);
                    if (err != CHIP_NO_ERROR)
                    {
                        ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                    }
                }
            }
        break;
        }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        MEMORY_PROFILER_DUMP_HEAP_STAT("BLE deinitialized");
        break;

    default:
        break;
    }
}

// This callback is invoked when clients interact with the Identify Cluster.
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// Attribute updates are driven from the meter, not from controllers, so there is
// nothing to push to a local driver here. Strictly return ESP_OK for attributes
// we don't handle.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK;
}

// Runs on the Matter task: bring up the electrical measurement clusters.
static void electrical_init_work(intptr_t arg)
{
    electrical_measurement_init(s_electrical_endpoint_id);
}

// Runs on the Matter task: apply one parsed reading (ownership of the copy is
// transferred here and freed once applied).
static void electrical_apply_work(intptr_t arg)
{
    dsmr_data_t *data = reinterpret_cast<dsmr_data_t *>(arg);
    electrical_measurement_apply(s_electrical_endpoint_id, data);
    delete data;
}

// Called from the P1 reader task for each valid telegram. Marshal the data onto
// the Matter task -- CHIP attribute/event APIs are not thread-safe.
static void on_p1_telegram(const dsmr_data_t *data)
{
    dsmr_data_t *copy = new (std::nothrow) dsmr_data_t(*data);
    if (!copy) {
        ESP_LOGE(TAG, "Out of memory copying telegram");
        return;
    }
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(electrical_apply_work, reinterpret_cast<intptr_t>(copy)) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to schedule electrical update");
        delete copy;
    }
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    nvs_flash_init();

    MEMORY_PROFILER_DUMP_HEAP_STAT("Bootup");

    /* Matter reset button (GPIO23): hold 10 s to erase the Matter pairing */
    app_driver_button_init();

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    /* Default friendly name (NodeLabel) shown by controllers; user-editable and
     * persisted once changed. Vendor/Product name come from CHIPProjectConfig.h. */
    snprintf(node_config.root_node.basic_information.node_label,
             sizeof(node_config.root_node.basic_information.node_label), "P1-meter");
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    MEMORY_PROFILER_DUMP_HEAP_STAT("node created");

    /* Electrical Sensor endpoint: Power Topology + Electrical Power Measurement,
     * plus an Electrical Energy Measurement cluster for cumulative import/export. */
    endpoint::electrical_sensor::config_t electrical_sensor_config;
    /* Do NOT set a topology feature here: electrical_sensor::add() already ORs in
     * NodeTopology, and the power_topology cluster asserts unless exactly one of
     * Node/Tree/SetTopology is selected. NodeTopology (power applies to the whole
     * node) is the right choice for a single aggregate sensor. */
    electrical_sensor_config.power_topology.delegate = &s_power_topology_delegate;
    electrical_sensor_config.electrical_power_measurement.feature_flags =
        cluster::electrical_power_measurement::feature::direct_current::get_id() |
        cluster::electrical_power_measurement::feature::alternating_current::get_id();

    endpoint_t *endpoint = endpoint::electrical_sensor::create(node, &electrical_sensor_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create electrical sensor endpoint"));

    cluster::electrical_energy_measurement::config_t energy_config;
    energy_config.feature_flags =
        cluster::electrical_energy_measurement::feature::imported_energy::get_id() |
        cluster::electrical_energy_measurement::feature::exported_energy::get_id() |
        cluster::electrical_energy_measurement::feature::cumulative_energy::get_id() |
        cluster::electrical_energy_measurement::feature::periodic_energy::get_id();
    cluster_t *energy_cluster = cluster::electrical_energy_measurement::create(endpoint, &energy_config, CLUSTER_FLAG_SERVER);
    ABORT_APP_ON_FAILURE(energy_cluster != nullptr, ESP_LOGE(TAG, "Failed to create energy measurement cluster"));

    /* Expose the optional Voltage and ActiveCurrent attributes on the power cluster */
    cluster_t *power_cluster = cluster::get(endpoint, ElectricalPowerMeasurement::Id);
    if (power_cluster) {
        cluster::electrical_power_measurement::attribute::create_voltage(power_cluster, nullable<int64_t>());
        cluster::electrical_power_measurement::attribute::create_active_current(power_cluster, nullable<int64_t>());
    }

    s_electrical_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Electrical Sensor created on endpoint %d", s_electrical_endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    // Enable secondary network interface
    secondary_network_interface::config_t secondary_network_interface_config;
    endpoint = endpoint::secondary_network_interface::create(node, &secondary_network_interface_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create secondary network interface endpoint"));
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
    auto * dac_provider = get_dac_provider();
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
    static_cast<ESP32SecureCertDACProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
    static_cast<ESP32FactoryDataProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#endif
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    MEMORY_PROFILER_DUMP_HEAP_STAT("matter started");

    /* Initialize the electrical measurement clusters on the Matter task */
    chip::DeviceLayer::PlatformMgr().ScheduleWork(electrical_init_work, reinterpret_cast<intptr_t>(nullptr));

    /* Start reading the smart meter's P1 port */
    err = p1_reader_start(on_p1_telegram);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start P1 reader, err:%d", err));

#if CONFIG_ENABLE_ENCRYPTED_OTA
    err = esp_matter_ota_requestor_encrypted_init(s_decryption_key, s_decryption_key_len);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialized the encrypted OTA, err: %d", err));
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::attribute_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif

    while (true) {
        MEMORY_PROFILER_DUMP_HEAP_STAT("Idle");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}
