import os
from flask import Flask, render_template, request
from random import randint, uniform

NEWLINE = "\n"
CARRET = "\r"

PARENT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

app = Flask(__name__,
            root_path=PARENT_DIR,
            static_url_path="",
            static_folder="data",
            template_folder="data"
            )


@app.route("/", methods=['GET'])
def index():
    return render_template("index.html")


def get_nmea_gga():
    return "$GNGGA,072446.00," + \
        str(uniform(0, 9000)) + "," + (['N', 'S'])[randint(0, 1)] + "," + \
        str(uniform(0, 18000)) + "," + (['E', 'W'])[randint(0, 1)] + "," + \
        str(randint(0, 9)) + "," + \
        str(randint(0, 9)) + "," + \
        "0.5" + "," + \
        str(uniform(-10, 10)) + ",M," + \
        str(uniform(-10, 10)) + ",M," + \
        "2.0,*44"


def get_nmea_gst():
    return "$GNGST,172814.0,0.006,0.023,0.020,273.6," + \
        str(uniform(0, 1)) + "," + \
        str(uniform(0, 1)) + "," + \
        str(uniform(0, 1)) + "," + \
        "*6A"


def get_mode():
    return (["Rover", "Base-Survey", "Base-Fixed"])[randint(0, 2)]


def get_ntrip_cli():
    return (["Unavailable", "Availabe", "Connecting", "Connected", "Disconnected"])[randint(0, 4)]


def get_ntrip_cas():
    return str(randint(0, 4))


def get_wifi():
    return (["Started", "Stopped", "Connected", "Disconnected", "192.168.5.249"])[randint(0, 4)]


@app.route("/status", methods=['GET'])
def status():
    return \
        get_nmea_gga() + NEWLINE + \
        get_nmea_gst() + NEWLINE + \
        get_mode() + NEWLINE + \
        get_ntrip_cli() + NEWLINE + \
        get_ntrip_cas() + NEWLINE + \
        get_wifi() + NEWLINE + \
        ""


@app.route("/config", methods=['GET'])
def config():
    print("query:", request.query_string)

    if str(request.query_string, 'ascii') == "ntrip_cli_get_mnts":
        return (["0\rABC\rDEF", "1\rABC\rDEF"])[randint(0, 1)]

    return \
        "hostname" + NEWLINE + \
        "version" + NEWLINE + \
        (["", "abcdefgh"])[randint(0, 1)] + NEWLINE + \
        (["", "12345678"])[randint(0, 1)] + NEWLINE + \
        "vngeonet.vn" + NEWLINE + \
        "2101" + NEWLINE + \
        "" + NEWLINE + \
        "" + NEWLINE + \
        (["", "ABC"])[randint(0, 1)] + NEWLINE + \
        str(uniform(0, 9000)) + NEWLINE + \
        str(uniform(0, 18000)) + NEWLINE + \
        str(uniform(-10, 10))


@app.route("/action", methods=['POST'])
def action():
    print(request.data)
    return "OK"


if __name__ == '__main__':
    app.run(host="0.0.0.0", debug=True)
