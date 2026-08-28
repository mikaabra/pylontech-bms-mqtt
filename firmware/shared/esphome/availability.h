#pragma once
#include <string>
#include "esphome/components/mqtt/mqtt_client.h"

#ifdef USE_MQTT

struct AvailabilityTracker {
    bool stale = true;
    bool last_online = false;
    std::string status_topic;

    explicit AvailabilityTracker(const char* topic) : status_topic(topic) {}

    void mark_online(esphome::mqtt::MQTTClientComponent* mqtt) {
        stale = false;
        if (!last_online) {
            mqtt->publish(status_topic, std::string("online"), (uint8_t)0, true);
            last_online = true;
        }
    }

    void mark_stale(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!stale) {
            stale = true;
            if (last_online) {
                mqtt->publish(status_topic, std::string("offline"), (uint8_t)0, true);
                last_online = false;
            }
        }
    }

    void on_connect(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!stale) {
            mqtt->publish(status_topic, std::string("online"), (uint8_t)0, true);
            last_online = true;
        } else {
            mqtt->publish(status_topic, std::string("offline"), (uint8_t)0, true);
            last_online = false;
        }
    }

    void reset() {
        stale = true;
        last_online = false;
    }
};

#endif
