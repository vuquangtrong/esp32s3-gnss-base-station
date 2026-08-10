#!/usr/bin/env python3

import os
import random

from time import time
from datetime import datetime
from pathlib import Path
from flask import Flask, send_from_directory, jsonify, request

app = Flask(__name__)

WWW_DIR = Path(__file__).parent / "www"
HOST = "127.0.0.1"
PORT = 5000


GNSS_MODE = 0  # Options: 0 (ROVER), 1 (BASE), 2 (PPP)


class StatusResponse:
    def __init__(self):
        self._start_time = None

    def get_value(self):
        current_time = time()

        if self._start_time is None:
            self._start_time = current_time

        elapsed = current_time - self._start_time

        return {
            "sta_status": 0 if elapsed < 10 else (1 if elapsed < 20 else 2),
            "sta_ip": "" if elapsed < 20 else "192.168.1.100",

            "gnss_mode": GNSS_MODE,
            "gnss_date": datetime.utcnow().strftime("%Y-%m-%d"),
            "gnss_time": datetime.utcnow().strftime("%H:%M:%S"),
            "gnss_lat": f"{random.uniform(-90, 90):.7f}",
            "gnss_lon": f"{random.uniform(-180, 180):.7f}",
            "gnss_alt": f"{random.uniform(0, 1000):.3f}",
            "gnss_sat": random.randint(0, 20),
            "gnss_fix": random.choice([
                "DR ONLY",
                "TIME FIX",
                "GNSS+DR",
                "2D FIX",
                "3D FIX",
                "FLOAT RTK",
                "FIXED RTK"
            ]),
            "gnss_hacc": f"{random.uniform(0.01, 1.0):.3f}",
            "gnss_vacc": f"{random.uniform(0.01, 1.0):.3f}",
        }


statusResponse = StatusResponse()


@app.route("/config")
def get_config():
    return jsonify({
        "prj_version": "2.0",
        "build_date": "2026-01-01",
        "git_commit": "abcdef123",
        "wifi_ssid": "TrongIP",
        "wifi_password": "asdfghjkl"
    })


@app.route("/status")
def get_status():
    return jsonify(
        statusResponse.get_value()
    )


@app.route("/wifi", methods=["POST"])
def wifi():
    command = request.args.get("command")

    if command == "connect":
        ssid = request.args.get("ssid")
        password = request.args.get("password")

        if not ssid or not password:
            return "Missing SSID or password", 400

        return "WiFi connection initiated", 200

    return "Invalid command", 400


@app.route("/")
@app.route("/<path:path>")
def serve_static(path=""):
    if path == "":
        path = "index.html"

    file_path = WWW_DIR / path
    if file_path.is_file():
        return send_from_directory(WWW_DIR, path)

    return "Not found", 404


@app.errorhandler(404)
def not_found(e):
    return "Not found", 404


if __name__ == "__main__":
    print(f"Starting development server at http://{HOST}:{PORT}")
    print(f"Serving files from: {WWW_DIR}")
    app.run(host=HOST, port=PORT, debug=True, use_reloader=True)
