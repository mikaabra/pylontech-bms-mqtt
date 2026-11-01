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

    // All stamps are edge-relevant and retained: an "online" accepted nowhere
    // means HA sees the channel dead, and a lost "offline" leaves frozen data
    // looking live. Every publish result is returned, and callers only flip
    // state on success; the both-state heartbeat repairs any dropped stamp.
    bool publish_stamp_(esphome::mqtt::MQTTClientComponent* mqtt, const char* payload) {
        bc_pub_begin(BC_PUB_AVAIL);
        bool ok = mqtt->publish(status_topic, std::string(payload), (uint8_t)0, true);
        bc_pub_end();
        return ok;
    }

    void mark_online(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (last_online) { stale = false; return; }  // no-op once confirmed
        if (!publish_stamp_(mqtt, "online")) return;  // keep last_online false: next rx retries
        last_online = true;
        stale = false;
    }

    void mark_stale(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (stale) return;  // already stale; the both-state heartbeat repairs a dropped stamp
        stale = true;       // data-age truth, independent of broker state
        if (last_online) {
            if (publish_stamp_(mqtt, "offline")) last_online = false;
        }
    }

    bool on_connect(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!stale) {
            if (!publish_stamp_(mqtt, "online")) return false;
            last_online = true;
        } else {
            if (!publish_stamp_(mqtt, "offline")) return false;
            last_online = false;
        }
        return true;
    }

    // republish the retained status even when unchanged - mark_online above
    // is a no-op once online, so periodic heartbeats must come through here.
    // Publishes the CURRENT state either way and runs in BOTH states: a
    // stale-transition's "offline" lost during a short disconnect leaves the
    // retained topic "online" (frozen data looking live) until this repairs
    // it, and a stale-channel "online" lost the same way is repaired too.
    bool heartbeat(esphome::mqtt::MQTTClientComponent* mqtt) {
        if (!mqtt) return false;
        bool online = !stale;
        if (!publish_stamp_(mqtt, online ? "online" : "offline")) return false;
        last_online = online;
        return true;
    }

    void reset() {
        stale = true;
        last_online = false;
    }
};

#endif
