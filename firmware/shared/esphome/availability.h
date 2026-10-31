#pragma once
#include <string>
#include "esphome/components/mqtt/mqtt_client.h"

#ifdef USE_MQTT

struct AvailabilityTracker {
    bool stale = true;
    bool last_online = false;
    std::string status_topic;

    explicit AvailabilityTracker(const char* topic) : status_topic(topic) {}

    // handlers are invoked from BLE and UART callbacks that can fire before
    // MQTT setup completes; guard against a null client there
    void mark_online(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!mqtt) return;
        stale = false;
        if (!last_online) {
            mqtt->publish(status_topic, std::string("online"), (uint8_t)0, true);
            last_online = true;
        }
    }

    void mark_stale(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!mqtt) return;
        if (!stale) {
            stale = true;
            if (last_online) {
                mqtt->publish(status_topic, std::string("offline"), (uint8_t)0, true);
                last_online = false;
            }
        }
    }

    void on_connect(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!mqtt) return;
        if (!stale) {
            mqtt->publish(status_topic, std::string("online"), (uint8_t)0, true);
            last_online = true;
        } else {
            mqtt->publish(status_topic, std::string("offline"), (uint8_t)0, true);
            last_online = false;
        }
    }

    // republish the retained status even when unchanged - mark_online above
    // is a no-op once online, so periodic heartbeats must come through here
    void heartbeat(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!mqtt || stale) return;
        mqtt->publish(status_topic, std::string("online"), (uint8_t)0, true);
    }

    void reset() {
        stale = true;
        last_online = false;
    }
};

#endif
