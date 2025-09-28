# app/dashboard_server.py
from flask import Flask, request, jsonify, send_from_directory
from collections import deque
from datetime import datetime
import argparse

app = Flask(__name__, static_folder="static", static_url_path="")

BUFFER_SZ = 1000
buf = deque(maxlen=BUFFER_SZ)

@app.route("/")
def root():
    # Serve app/static/index.html
    return send_from_directory(app.static_folder, "index.html")

@app.route("/ingest", methods=["POST"])
def ingest():
    j = request.get_json(silent=True) or {}
    j["_received_at"] = datetime.utcnow().isoformat() + "Z"
    buf.append(j)
    return ("", 204)

@app.route("/recent", methods=["GET"])
def recent():
    n = int(request.args.get("n", 200))
    return jsonify(list(buf)[-n:])

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5000)
    args = parser.parse_args()
    app.run(host=args.host, port=args.port)