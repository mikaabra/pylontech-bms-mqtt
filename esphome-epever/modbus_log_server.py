#!/usr/bin/env python3
"""
Simple HTTP server that serves the Modbus log viewer HTML
and proxies API requests to the ESP32.

This solves the SOCKS proxy issue by running the server locally
on the same machine that has the SSH tunnel to the ESP32.

Usage:
    ./modbus_log_server.py [--port 8080]

Then open in browser:
    http://localhost:8080/
"""

import http.server
import socketserver
import urllib.request
import json
import argparse
from pathlib import Path

ESP32_IP = '10.10.0.45'
ESP32_API_URL = f'http://{ESP32_IP}/text_sensor/zzz_modbus_interaction_log'

class ModbusLogHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/':
            # Serve the HTML viewer
            html_file = Path(__file__).parent / 'modbus_log_viewer.html'
            if html_file.exists():
                self.send_response(200)
                self.send_header('Content-type', 'text/html')
                self.end_headers()
                with open(html_file, 'rb') as f:
                    self.wfile.write(f.read())
            else:
                self.send_error(404, "modbus_log_viewer.html not found")

        elif self.path == '/api/log':
            # Proxy API request to ESP32
            try:
                with urllib.request.urlopen(ESP32_API_URL, timeout=5) as response:
                    data = response.read()
                    self.send_response(200)
                    self.send_header('Content-type', 'application/json')
                    self.send_header('Access-Control-Allow-Origin', '*')
                    self.end_headers()
                    self.wfile.write(data)
            except urllib.error.URLError as e:
                self.send_response(503)
                self.send_header('Content-type', 'application/json')
                self.end_headers()
                error_msg = json.dumps({
                    'error': f'Failed to connect to ESP32 at {ESP32_IP}',
                    'details': str(e)
                })
                self.wfile.write(error_msg.encode())
            except Exception as e:
                self.send_response(500)
                self.send_header('Content-type', 'application/json')
                self.end_headers()
                error_msg = json.dumps({
                    'error': 'Internal server error',
                    'details': str(e)
                })
                self.wfile.write(error_msg.encode())

        else:
            self.send_error(404, "File not found")

    def log_message(self, format, *args):
        # Custom logging format
        print(f"[{self.log_date_time_string()}] {format % args}")

def main():
    parser = argparse.ArgumentParser(description='Modbus Log Viewer Server')
    parser.add_argument('--port', type=int, default=8080,
                      help='Port to listen on (default: 8080)')
    args = parser.parse_args()

    # Change to the script's directory so we can find the HTML file
    script_dir = Path(__file__).parent
    import os
    os.chdir(script_dir)

    with socketserver.TCPServer(("", args.port), ModbusLogHandler) as httpd:
        print(f"Modbus Log Viewer Server")
        print(f"Serving at: http://localhost:{args.port}/")
        print(f"Proxying ESP32 API from: {ESP32_API_URL}")
        print(f"Press Ctrl+C to stop")
        print()

        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down server...")

if __name__ == "__main__":
    main()
