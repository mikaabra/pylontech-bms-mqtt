#pragma once
#include <string>
#include "esphome/components/mqtt/mqtt_client.h"
#include "breadcrumbs.h"

#ifdef USE_MQTT

struct AvailabilityTracker {
    bool stale = true;
    bool last_online = false;
    std::string status_topic;

    explicit AvailabilityTracker(const char* topic) : status_topic(topic) {}

    void publish_stamp_(esphome::mqtt::MQTTClientComponent* mqtt, const char* payload) {
        bc_pub_begin(BC_PUB_AVAIL);
        mqtt->publish(status_topic, std::string(payload), (uint8_t)0, true);
        bc_pub_end();
    }

    void mark_online(esphome::mqtt::MQTTClientComponent* mqtt) {
        stale = false;
        if (!last_online) {
            publish_stamp_(mqtt, "online");
            last_online = true;
        }
    }

    void mark_stale(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!stale) {
            stale = true;
            if (last_online) {
                publish_stamp_(mqtt, "offline");
                last_online = false;
            }
        }
    }

    void on_connect(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!stale) {
            publish_stamp_(mqtt, "online");
            last_online = true;
        } else {
            publish_stamp_(mqtt, "offline");
            last_online = false;
        }
    }

    // republish the retained status even when unchanged - mark_online above
    // is a no-op once online, so periodic heartbeats must come through here.
    // Publishes the CURRENT state either way: if a stale-transition's
    // "offline" was lost during a short disconnect, the retained topic
    // stays "online" and HA shows frozen data as live; repeating the
    // offline repairs it on the next heartbeat.
    void heartbeat(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!mqtt) return;
        publish_stamp_(mqtt, stale ? "offline" : "online");
    }

    void reset() {
        stale = true;
        last_online = false;
    }
};

#endif
