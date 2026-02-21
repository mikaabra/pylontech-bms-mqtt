#!/bin/bash
#
# Terminal-based viewer for EPever Modbus interaction log
# Fetches and displays the log from ESP32 with syntax highlighting
#
# Usage:
#   ./modbus_log_tail.sh           # Show once
#   ./modbus_log_tail.sh -f        # Follow mode (refresh every 5 seconds)
#   ./modbus_log_tail.sh -w        # Watch mode (refresh every 2 seconds)
#

ESP32_IP="10.10.0.45"
API_URL="http://${ESP32_IP}/text_sensor/zzz_modbus_interaction_log"

# ANSI color codes
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# Function to fetch and display log
show_log() {
    # Fetch JSON from ESP32
    local json=$(curl -s "$API_URL" 2>/dev/null)

    if [ $? -ne 0 ] || [ -z "$json" ]; then
        echo -e "${RED}✗ Failed to connect to ESP32 at ${ESP32_IP}${NC}"
        echo "Make sure:"
        echo "  1. ESP32 is powered on and connected to WiFi"
        echo "  2. IP address is correct: ${ESP32_IP}"
        echo "  3. Firmware with Modbus log buffer is uploaded"
        return 1
    fi

    # Extract value field (handle both "value" and "state" fields)
    local log_content=$(echo "$json" | jq -r '.value // .state // empty' 2>/dev/null)

    if [ -z "$log_content" ] || [ "$log_content" = "null" ]; then
        echo -e "${YELLOW}No Modbus interactions logged yet${NC}"
        return 0
    fi

    # Display header
    echo -e "${BOLD}${CYAN}═══════════════════════════════════════════════════════════${NC}"
    echo -e "${BOLD}${CYAN}  Modbus Interaction Log - EPever CAN Bridge${NC}"
    echo -e "${BOLD}${CYAN}  ESP32: ${ESP32_IP}${NC}"
    echo -e "${BOLD}${CYAN}═══════════════════════════════════════════════════════════${NC}"
    echo

    # Apply syntax highlighting and display
    echo "$log_content" | while IFS= read -r line; do
        # Skip empty lines
        [ -z "$line" ] && continue

        # Apply highlighting using printf
        line=$(printf "%s\n" "$line" | \
            sed -E \
                -e 's/(\[[0-9]+\])/\x1b[0;32m\1\x1b[0m/g' \
                -e 's/TX:/\x1b[0;34mTX:\x1b[0m/g' \
                -e 's/RX:/\x1b[0;36mRX:\x1b[0m/g' \
                -e 's/✓/\x1b[0;32m✓\x1b[0m/g' \
                -e 's/✗/\x1b[0;31m✗\x1b[0m/g' \
                -e 's/(Mode change needed|Auto check|Triggered update)/\x1b[0;33m\1\x1b[0m/g' \
                -e 's/(Exception|timeout|failed)/\x1b[0;31m\1\x1b[0m/g')

        printf "%b\n" "$line"
    done

    # Display entry count
    local count=$(echo "$log_content" | grep -c "^\[")
    echo
    echo -e "${GRAY}─────────────────────────────────────────────────────────${NC}"
    echo -e "${GRAY}${count} log entries (buffer limit: 50)${NC}"
    echo -e "${GRAY}Last updated: $(date '+%Y-%m-%d %H:%M:%S')${NC}"
}

# Parse command line arguments
FOLLOW_MODE=0
INTERVAL=5

while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--follow)
            FOLLOW_MODE=1
            INTERVAL=5
            shift
            ;;
        -w|--watch)
            FOLLOW_MODE=1
            INTERVAL=2
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo
            echo "Options:"
            echo "  -f, --follow    Follow mode (refresh every 5 seconds)"
            echo "  -w, --watch     Watch mode (refresh every 2 seconds)"
            echo "  -h, --help      Show this help message"
            echo
            echo "Examples:"
            echo "  $0              # Show log once"
            echo "  $0 -f           # Follow mode (Ctrl+C to exit)"
            echo "  $0 -w           # Watch mode (faster refresh)"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use -h for help"
            exit 1
            ;;
    esac
done

# Main execution
if [ $FOLLOW_MODE -eq 1 ]; then
    echo -e "${BOLD}Follow mode enabled (refresh every ${INTERVAL}s) - Press Ctrl+C to exit${NC}"
    echo

    while true; do
        clear
        show_log
        sleep $INTERVAL
    done
else
    show_log
fi
