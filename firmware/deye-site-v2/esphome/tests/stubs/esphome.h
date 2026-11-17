// Host-side umbrella for the deye gateway handler headers
// (pylontech_can.h / pylontech_rs485.h), whose first include is "esphome.h".
// Pulls the existing host stubs together and adds nothing else - device
// builds never see this file (tests/stubs is only ever on host -I paths).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_system.h"
#include "esp_attr.h"
#include "esphome/core/application.h"
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/components/uart/uart.h"
