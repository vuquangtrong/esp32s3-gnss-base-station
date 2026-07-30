#!/usr/bin/env python3

import os
import random

from flask import Flask, send_from_directory, jsonify, request
from pathlib import Path

app = Flask(__name__)

WWW_DIR = Path(__file__).parent / "www"
HOST = "127.0.0.1"
PORT = 5000


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
    return jsonify({
        "sta_status": random.choice(['0', '1', '2']),
    })


@app.route("/wifi/connect", methods=["POST"])
def wifi_connect():
    ssid = request.args.get("ssid")
    password = request.args.get("password")

    if not ssid or not password:
        return "Missing SSID or password", 400

    return "WiFi connection initiated", 200


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
