# MQTT Statistics Monitor

A Python script to monitor MQTT traffic and identify "top talkers" - topics with the highest message rates.

## Features

- **Real-time monitoring**: Tracks message counts and rates in real-time
- **Top talkers identification**: Shows which topics are most active
- **Comprehensive statistics**: Provides detailed metrics including message rates, data sizes, and percentages
- **Configurable**: Adjust monitoring duration, update interval, and number of top talkers to display
- **Seamless integration**: Uses existing `secrets.yaml` for MQTT credentials

## Installation

No additional installation needed if you already have the required dependencies:
- Python 3.x
- `paho-mqtt` package
- `PyYAML` package

If you need to install dependencies:
```bash
pip install paho-mqtt pyyaml
```

## Usage

```bash
# Basic usage (1 hour monitoring, top 10 talkers, 10s updates)
./mqtt_stats_monitor.py

# Custom duration (30 minutes = 1800 seconds)
./mqtt_stats_monitor.py --duration 1800

# Show only top 5 talkers
./mqtt_stats_monitor.py --top 5

# Custom update interval (every 5 seconds)
./mqtt_stats_monitor.py --interval 5

# Quiet mode (only show final summary, no periodic updates)
./mqtt_stats_monitor.py --quiet

# Custom secrets file path
./mqtt_stats_monitor.py --secrets /path/to/secrets.yaml

# Combined options
./mqtt_stats_monitor.py --duration 3600 --top 15 --interval 5 --quiet
```

## Example Output

```
Starting MQTT Statistics Monitor
Monitoring duration: 3600 seconds (60.0 minutes)
Update interval: 60 seconds
Top N talkers to display: 10
Connecting to: 192.168.200.217:1883
Connected to MQTT broker at 192.168.200.217
Subscribed to all topics (#)
Subscription acknowledged (QOS: 0)

--- MQTT Statistics (60s elapsed) ---
Top 10 talkers by message count:
--------------------------------------------------------------------------------
Topic                                                  Count     Rate (msg/s)   Avg Size
--------------------------------------------------------------------------------
deye_bms/cell_voltages                                 120       2.00           45
deye_bms/temperatures                                  60        1.00           32
deye_bms/status                                       30        0.50           15
...
--------------------------------------------------------------------------------
Total messages: 210
Total data: 4.25 KB
Overall rate: 3.50 msg/s

[Additional updates every 60 seconds...]

================================================================================
FINAL MQTT STATISTICS SUMMARY
================================================================================

Top 10 talkers by message count:
--------------------------------------------------------------------------------
Rank  Topic                                                  Count     % Total   Rate
--------------------------------------------------------------------------------
1     deye_bms/cell_voltages                                 7200      35.3%     2.00/s
2     deye_bms/temperatures                                  3600      17.6%     1.00/s
3     deye_bms/status                                       1800      8.8%      0.50/s
...

Detailed Statistics:
  Monitoring duration: 3600 seconds (60.0 minutes)
  Total messages received: 20400
  Total data received: 425.32 KB
  Average message rate: 5.67 msg/s
  Unique topics: 42
  Most active topic(s): deye_bms/cell_voltages
  Max messages from single topic: 7200
```

## How It Works

1. **Connection**: Connects to your MQTT broker using credentials from `secrets.yaml`
2. **Subscription**: Subscribes to all topics using the wildcard `#`
3. **Tracking**: For each message received:
   - Increments message count for the topic
   - Tracks total payload size
   - Updates statistics in real-time
4. **Reporting**: 
   - **Periodic updates**: Displays statistics every 10 seconds by default (configurable)
   - **Quiet mode**: Suppresses periodic updates when `--quiet` flag is used
   - **Final summary**: Always shows comprehensive statistics at the end
   - Shows top N most active topics, message counts, rates, data sizes, and percentages

## Configuration

The script uses your existing `secrets.yaml` file. Ensure it contains:
```yaml
mqtt_host: "your.mqtt.broker.ip"
mqtt_user: "your_username"
mqtt_password: "your_password"
mqtt_port: 1883  # optional, defaults to 1883
```

## Requirements

- Python 3.6+
- `paho-mqtt` package
- `PyYAML` package
- Access to MQTT broker
- Read permissions for `secrets.yaml`

## Troubleshooting

**Issue**: `ModuleNotFoundError: No module named 'paho'`
**Solution**: Install the required package: `pip install paho-mqtt`

**Issue**: `ModuleNotFoundError: No module named 'yaml'`
**Solution**: Install PyYAML: `pip install pyyaml`

**Issue**: Connection refused/timeout
**Solution**: Check your MQTT broker is running and the credentials in `secrets.yaml` are correct

**Issue**: No messages appearing
**Solution**: Verify topics are being published and your MQTT client has proper permissions

## License

This script is provided as-is and can be freely used and modified.

## Future Enhancements

Possible improvements for future versions:
- CSV/JSON export of statistics
- Historical trend analysis
- Topic filtering/blacklisting
- Message content analysis
- Integration with monitoring systems (Prometheus, etc.)