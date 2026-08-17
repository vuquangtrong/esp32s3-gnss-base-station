#!/usr/bin/env python3

import random
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, jsonify, request, send_from_directory

app = Flask(__name__)

WWW_DIR = Path(__file__).parent / "www"
HOST = "127.0.0.1"
PORT = 5000

# Simulated logger state
logger_state = {
    "status": 0,  # 0=stopped, 1=running, 2=error
    "file": "",
    "size": 0,
}

# Simulated NTRIP client state
ntrip_client_state = {
    "status": 0,  # 0=disconnected, 1=connecting, 2=connected
    "received_bytes": 0,
}


@app.route("/sysinfo")
def sysinfo():
    info_type = request.args.get("type")
    if info_type == "config":
        return jsonify(
            {
                # Project
                "version": "0.1",
                "build_time": "2026-08-16 12:00:00",
                "git_commit": "abc1234",
                # WiFi
                "wifi_ssid": "TrongIP",
                "wifi_password": "asdfghjkl",
                # NTRIP Client
                "ntrip_server": "vngeonet.vn",
                "ntrip_port": "2101",
                "ntrip_mountpoint": "VRS.105M6",
                "ntrip_username": "",
                "ntrip_password": "",
            }
        )
    elif info_type == "status":
        now = datetime.now(timezone.utc).strftime("%Y-%m-%d_%H-%M-%S")
        # Simulate growing file size when logger is running
        if logger_state["status"] == 1:
            logger_state["size"] += random.randint(10000, 50000)
        # Simulate growing received bytes when ntrip client is connected
        if ntrip_client_state["status"] == 2:
            ntrip_client_state["received_bytes"] += random.randint(500, 5000)
        return jsonify(
            {
                # Battery
                "bat_volt": random.randint(1650, 2100),
                # WiFi
                "wifi_status": 2,
                "wifi_ip_addr": "192.168.1.100",
                # GNSS Mode
                "gnss_mode": 0,
                # GNSS Position
                "gnss_time": now,
                "gnss_lat": 210285110 + random.randint(-1000, 1000),
                "gnss_lon": 1058048170 + random.randint(-1000, 1000),
                "gnss_alt": 12340 + random.randint(-100, 100),
                "gnss_sat": random.randint(8, 24),
                "gnss_hacc": random.randint(0, 10000),
                "gnss_vacc": random.randint(0, 10000),
                "gnss_fix": random.choice(
                    ["NO_FIX", "3D_FIX", "FLOAT RTK", "FIXED RTK"]
                ),
                # SDCard
                "sdcard_status": 1,
                # Logger
                "logger_status": logger_state["status"],
                "logger_file": logger_state["file"],
                "logger_size": logger_state["size"],
                # NTRIP Client
                "ntrip_client_status": ntrip_client_state["status"],
                "ntrip_received_bytes": ntrip_client_state["received_bytes"],
            }
        )
    else:
        return "Missing or invalid type parameter", 400


@app.route("/wifi", methods=["POST"])
def wifi():
    data = request.get_json(silent=True)
    print(f"POST /wifi body: {data}")
    if data is None:
        return "Invalid JSON", 400

    command = data.get("command")
    if command == "connect":
        ssid = data.get("ssid", "")
        password = data.get("password", "")
        print(f"WiFi connect: ssid={ssid}, password={password}")
        return jsonify({"status": "ok"})

    return "Unknown command", 400


@app.route("/logger", methods=["POST"])
def logger():
    data = request.get_json(silent=True)
    print(f"POST /logger body: {data}")
    if data is None:
        return "Invalid JSON", 400

    command = data.get("command")
    if command == "start":
        now = datetime.now(timezone.utc).strftime("%Y-%m-%d_%H-%M-%S")
        prefix = data.get("prefix", "")
        if prefix:
            filename = f"{prefix}_{now}.ubx"
        else:
            filename = f"{now}.ubx"
        logger_state["status"] = 1
        logger_state["file"] = filename
        logger_state["size"] = 0
        print(f"Logger started: {logger_state['file']}")
        return jsonify({"status": "ok"})
    elif command == "stop":
        print(
            f"Logger stopped: {logger_state['file']} ({logger_state['size']} bytes)")
        logger_state["status"] = 0
        # logger_state["file"] = ""
        # logger_state["size"] = 0
        return jsonify({"status": "ok"})
    elif command == "list":
        files = [
            {"name": "2026-08-10_08-30-00.ubx", "size": 5242880},
            {"name": "2026-08-12_14-15-30.ubx", "size": 12582912},
            {"name": "survey_2026-08-14_09-00-00.ubx", "size": 8388608},
            {"name": "base_2026-08-15_16-45-12.ubx", "size": 3145728},
        ]
        # Include the current logger file if it exists
        if logger_state["file"]:
            existing = [f["name"] for f in files]
            if logger_state["file"] not in existing:
                files.append(
                    {"name": logger_state["file"], "size": logger_state["size"]})
        files.sort(key=lambda f: f["name"])
        return jsonify({"files": files})

    return "Unknown command", 400


@app.route("/ntripclient", methods=["POST"])
def ntripclient_post():
    data = request.get_json(silent=True)
    print(f"POST /ntripclient body: {data}")
    if data is None:
        return "Invalid JSON", 400

    command = data.get("command")
    if command == "list":
        return jsonify(
            {
                "mountpoints": [
                    "VRS.105M6",
                    "VRS.105M7",
                    "VRS.105M8",
                ]
            }
        )
    elif command == "query":
        host = data.get("host", "")
        port = data.get("port", 0)
        print(f"NTRIP query mountpoints: host={host}, port={port}")
        return jsonify({"status": "ok"})
    elif command == "connect":
        host = data.get("host", "")
        port = data.get("port", 0)
        mountpoint = data.get("mountpoint", "")
        username = data.get("username", "")
        print(
            f"NTRIP connect: host={host}, port={port}, "
            f"mountpoint={mountpoint}, username={username}"
        )
        ntrip_client_state["status"] = 2
        ntrip_client_state["received_bytes"] = 0
        return jsonify({"status": "ok"})
    elif command == "disconnect":
        print("NTRIP disconnect")
        ntrip_client_state["status"] = 0
        ntrip_client_state["received_bytes"] = 0
        return jsonify({"status": "ok"})

    return "Unknown command", 400


@app.route("/")
@app.route("/<path:path>")
def serve_static(path=""):
    if path == "":
        path = "index.html"

    file_path = WWW_DIR / path
    if file_path.is_file():
        response = send_from_directory(WWW_DIR, path)

        # Development: don't let the browser cache static files.
        response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
        response.headers["Pragma"] = "no-cache"
        response.headers["Expires"] = "0"

        return response

    return "Not found", 404


@app.errorhandler(404)
def not_found(e):
    return "Not found", 404


if __name__ == "__main__":
    print(f"Starting development server at http://{HOST}:{PORT}")
    print(f"Serving files from: {WWW_DIR}")
    app.run(host=HOST, port=PORT, debug=True, use_reloader=True)
