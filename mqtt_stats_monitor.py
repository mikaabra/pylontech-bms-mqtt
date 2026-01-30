#!/usr/bin/env python3
"""
MQTT Statistics Monitor - Tracks message rates and identifies top talkers.

Usage:
    ./mqtt_stats_monitor.py [--duration 3600] [--top 10] [--interval 10] [--quiet]
    
    --duration: Monitoring duration in seconds (default: 3600 = 1 hour)
    --top: Number of top talkers to display (default: 10)
    --interval: Display update interval in seconds (default: 10)
    --quiet: Suppress periodic updates, only show final summary
    
Example:
    ./mqtt_stats_monitor.py --duration 1800 --top 5
    ./mqtt_stats_monitor.py --quiet  # Quiet mode
    """

import paho.mqtt.client as mqtt
import yaml
import time
import argparse
import os
import sys
from collections import defaultdict
from datetime import datetime

def load_secrets(secrets_path='secrets.yaml'):
    """Load MQTT credentials from secrets.yaml"""
    # Try to find secrets.yaml in common locations
    search_paths = [
        secrets_path,
        'esphome/secrets.yaml',
        '../esphome/secrets.yaml',
        os.path.join(os.path.dirname(__file__), 'esphome', 'secrets.yaml')
    ]
    
    found_path = None
    for path in search_paths:
        if os.path.exists(path):
            found_path = path
            break
    
    if not found_path:
        print(f"Error: secrets.yaml not found. Tried: {', '.join(search_paths)}")
        sys.exit(1)
    
    try:
        with open(found_path, 'r') as f:
            secrets = yaml.safe_load(f)
        return {
            'host': secrets.get('mqtt_host', 'localhost'),
            'user': secrets.get('mqtt_user'),
            'password': secrets.get('mqtt_password'),
            'port': int(secrets.get('mqtt_port', 1883))
        }
    except Exception as e:
        print(f"Error loading secrets from {found_path}: {e}")
        sys.exit(1)

class MQTTStatsMonitor:
    def __init__(self, duration=3600, top_n=10, update_interval=10, quiet=False):
        self.duration = duration
        self.top_n = top_n
        self.update_interval = update_interval
        self.quiet = quiet
        self.message_counts = defaultdict(int)
        self.message_sizes = defaultdict(int)
        self.start_time = None
        self.last_update = 0
        
        # Load secrets
        self.secrets = load_secrets()
        
        # Setup MQTT client
        self.client = mqtt.Client()
        if self.secrets['user'] and self.secrets['password']:
            self.client.username_pw_set(self.secrets['user'], self.secrets['password'])
        
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.on_subscribe = self.on_subscribe
    
    def on_connect(self, client, userdata, flags, rc):
        """Callback for when the client connects to the broker"""
        if rc == 0:
            print(f"Connected to MQTT broker at {self.secrets['host']}")
            # Subscribe to all topics
            client.subscribe("#")
            print("Subscribed to all topics (#)")
        else:
            print(f"Connection failed with result code {rc}")
            sys.exit(1)
    
    def on_subscribe(self, client, userdata, mid, granted_qos):
        """Callback for when subscription is acknowledged"""
        print(f"Subscription acknowledged (QOS: {granted_qos})")
        self.start_time = time.time()
    
    def on_message(self, client, userdata, msg):
        """Callback for when a message is received"""
        topic = msg.topic
        payload_size = len(msg.payload)
        
        self.message_counts[topic] += 1
        self.message_sizes[topic] += payload_size
        
        # Periodically display stats (unless in quiet mode)
        current_time = time.time()
        if not self.quiet and current_time - self.last_update >= self.update_interval:
            self.display_stats()
            self.last_update = current_time
            
            # Check if we've reached the monitoring duration
            if current_time - self.start_time >= self.duration:
                print(f"\nMonitoring completed after {self.duration} seconds")
                self.display_final_stats()
                client.disconnect()
    
    def display_stats(self):
        """Display current statistics"""
        elapsed = time.time() - self.start_time
        print(f"\n--- MQTT Statistics ({elapsed:.0f}s elapsed) ---")
        
        if not self.message_counts:
            print("No messages received yet")
            return
        
        # Sort by message count (descending)
        sorted_topics = sorted(self.message_counts.items(), 
                              key=lambda x: x[1], reverse=True)
        
        print(f"Top {self.top_n} talkers by message count:")
        print("-" * 80)
        print(f"{'Topic':<60} {'Count':<10} {'Rate (msg/s)':<15} {'Avg Size':<10}")
        print("-" * 80)
        
        for topic, count in sorted_topics[:self.top_n]:
            rate = count / elapsed if elapsed > 0 else 0
            avg_size = self.message_sizes[topic] / count if count > 0 else 0
            print(f"{topic[:57]:<60} {count:<10} {rate:<15.2f} {avg_size:<10.0f}")
        
        # Show summary
        total_messages = sum(self.message_counts.values())
        total_size = sum(self.message_sizes.values())
        print("-" * 80)
        print(f"Total messages: {total_messages}")
        print(f"Total data: {total_size / 1024:.2f} KB")
        print(f"Overall rate: {total_messages / elapsed:.2f} msg/s")
    
    def display_final_stats(self):
        """Display final comprehensive statistics"""
        elapsed = time.time() - self.start_time
        print("\n" + "="*80)
        print("FINAL MQTT STATISTICS SUMMARY")
        print("="*80)
        
        if not self.message_counts:
            print("No messages received during monitoring period")
            return
        
        # Sort by message count (descending)
        sorted_topics = sorted(self.message_counts.items(), 
                              key=lambda x: x[1], reverse=True)
        
        print(f"\nTop {self.top_n} talkers by message count:")
        print("-" * 80)
        print(f"{'Rank':<5} {'Topic':<55} {'Count':<10} {'% Total':<10} {'Rate':<12}")
        print("-" * 80)
        
        total_messages = sum(self.message_counts.values())
        for rank, (topic, count) in enumerate(sorted_topics[:self.top_n], 1):
            percentage = (count / total_messages * 100) if total_messages > 0 else 0
            rate = count / elapsed if elapsed > 0 else 0
            print(f"{rank:<5} {topic[:52]:<55} {count:<10} {percentage:.1f}% {rate:<12.2f}")
        
        # Additional statistics
        print(f"\nDetailed Statistics:")
        print(f"  Monitoring duration: {elapsed:.0f} seconds ({elapsed/60:.1f} minutes)")
        print(f"  Total messages received: {total_messages}")
        print(f"  Total data received: {sum(self.message_sizes.values()) / 1024:.2f} KB")
        print(f"  Average message rate: {total_messages / elapsed:.2f} msg/s")
        print(f"  Unique topics: {len(self.message_counts)}")
        
        # Find most active topics
        if self.message_counts:
            max_count = max(self.message_counts.values())
            most_active = [t for t, c in self.message_counts.items() if c == max_count]
            print(f"  Most active topic(s): {', '.join(most_active[:3])}")
            print(f"  Max messages from single topic: {max_count}")
    
    def run(self):
        """Start the MQTT monitoring"""
        print("Starting MQTT Statistics Monitor")
        print(f"Monitoring duration: {self.duration} seconds ({self.duration/60:.1f} minutes)")
        print(f"Update interval: {self.update_interval} seconds")
        print(f"Top N talkers to display: {self.top_n}")
        print(f"Connecting to: {self.secrets['host']}:{self.secrets['port']}")
        
        try:
            self.client.connect(self.secrets['host'], self.secrets['port'], 60)
            self.client.loop_forever()
        except KeyboardInterrupt:
            print("\nMonitoring interrupted by user")
            self.display_final_stats()
        except Exception as e:
            print(f"Error: {e}")
            sys.exit(1)

def main():
    parser = argparse.ArgumentParser(
        description='MQTT Statistics Monitor - Track message rates and identify top talkers'
    )
    parser.add_argument('--duration', type=int, default=3600,
                       help='Monitoring duration in seconds (default: 3600 = 1 hour)')
    parser.add_argument('--top', type=int, default=10,
                       help='Number of top talkers to display (default: 10)')
    parser.add_argument('--interval', type=int, default=10,
                       help='Display update interval in seconds (default: 10)')
    parser.add_argument('--quiet', action='store_true',
                       help='Quiet mode - suppress periodic updates, only show final summary')
    parser.add_argument('--secrets', type=str, default='secrets.yaml',
                       help='Path to secrets.yaml file (default: secrets.yaml)')
    
    args = parser.parse_args()
    
    # Create and run monitor
    monitor = MQTTStatsMonitor(
        duration=args.duration,
        top_n=args.top,
        update_interval=args.interval,
        quiet=args.quiet
    )
    monitor.run()

if __name__ == "__main__":
    main()