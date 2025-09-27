# dashboard_server.py
from flask import Flask, request, jsonify
from collections import deque
from datetime import datetime
app = Flask(__name__)

BUFFER_SZ = 1000
buf = deque(maxlen=BUFFER_SZ)

@app.route("/ingest", methods=['POST'])
def ingest():
    j = request.get_json()
    j['_received_at'] = datetime.utcnow().isoformat() + "Z"
    buf.append(j)
    return ("", 204)

@app.route("/recent", methods=['GET'])
def recent():
    # return last N samples
    n = int(request.args.get("n", 200))
    return jsonify(list(buf)[-n:])

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=5000)