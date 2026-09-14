/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Matter device identity overrides. Pulled into the CHIP build via
   CONFIG_CHIP_PROJECT_CONFIG="main/CHIPProjectConfig.h" (set in sdkconfig).
   These override the CHIP defaults (TEST_VENDOR / TEST_PRODUCT) defined with
   #ifndef guards in connectedhomeip's CHIPDeviceConfig.h.
*/

#pragma once

#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME  "Cptmeme"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "P1-Meter"

/* Hardware version string, shown as "Hardware:" by controllers. The numeric
 * HardwareVersion attribute (set to 1) is driven by Kconfig on esp32
 * (CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION in sdkconfig), so it must NOT be
 * #defined here. */
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "1.0"

/* Note: CHIP_DEVICE_CONFIG_DEFAULT_NODE_LABEL is not honored by this
 * esp-matter / CHIP version (nothing reads it), so the default NodeLabel
 * ("P1-meter") is set on the Basic Information cluster in app_main() via
 * node_config.root_node.basic_information.node_label instead. */
