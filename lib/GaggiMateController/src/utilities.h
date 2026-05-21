#ifndef UTILITIES_H
#define UTILITIES_H
#include "ControllerConfig.h"
#include <Arduino.h>
#include <ArduinoJson.h>

inline String make_system_info(ControllerConfig config, String version) {
    JsonDocument doc;
    doc["hw"] = config.name;
    doc["v"] = version;
    JsonDocument capabilities;
    capabilities["ps"] = config.capabilites.pressure;
    capabilities["dm"] = config.capabilites.dimming;
    capabilities["led"] = config.capabilites.ledControls;
    capabilities["tof"] = config.capabilites.tof;
    // Firmware-version capability: this build emits heater/pump output in
    // `sendSensorData` for `.slog` v6+ recording. Always true on this firmware
    // version; absent (and parsed as false) on older Controller firmware.
    capabilities["xs"] = true;
    doc["cp"] = capabilities;
    return doc.as<String>();
}

#endif // UTILITIES_H
