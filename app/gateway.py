# gateway.py
import os, json, time, argparse, logging
from datetime import datetime, timezone
from collections import deque
import threading

import serial
import requests
import snowflake.connector

from dotenv import load_dotenv

load_dotenv()  # this loads variables from .env into os.environ

# ---------- CONFIG ----------
BATCH_SIZE = 50           # number of rows to insert per batch
FLUSH_INTERVAL = 1.0      # seconds (flush every second at most)
# near top, replace the DASHBOARD_URL line with:
DASHBOARD_URL_DEFAULT = os.getenv("DASHBOARD_URL", "http://127.0.0.1:5050/ingest")
# ----------------------------

log = logging.getLogger("gateway")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s: %(message)s")

def connect_snowflake():
    # Credentials via env vars (recommended)
    ctx = snowflake.connector.connect(
        user=os.environ['SNOW_USER'],
        password=os.environ.get('SNOW_PASSWORD'),     # or use keypair auth if you have one
        account=os.environ['SNOW_ACCOUNT'],           # e.g. 'abcd-xy123'
        warehouse=os.environ.get('SNOW_WAREHOUSE'),
        database=os.environ.get('SNOW_DATABASE'),
        schema=os.environ.get('SNOW_SCHEMA'),
        role=os.environ.get('SNOW_ROLE')
    )
    return ctx

def ensure_table(cursor):
    cursor.execute("""
    CREATE TABLE IF NOT EXISTS BRACE_SENSOR_READINGS (
      session_id VARCHAR,
      device_id VARCHAR,
      server_ts TIMESTAMP_NTZ,
      device_ts_ms BIGINT,
      angle DOUBLE,
      raw_adc INTEGER,
      sample_seq INTEGER,
      raw VARIANT
    );
    """)

class Sink:
    def __init__(self, conn):
        self.conn = conn
        self.cur = conn.cursor()
        ensure_table(self.cur)
        self.lock = threading.Lock()

    def insert_batch(self, rows):
        # rows: list of tuples matching columns below
        if not rows:
            return
        sql = """INSERT INTO BRACE_SENSOR_READINGS
        (session_id, device_id, server_ts, device_ts_ms, angle, raw_adc, sample_seq, raw)
        VALUES (%s,%s,%s,%s,%s,%s,%s,parse_json(%s))"""
        try:
            with self.lock:
                # executemany will be reasonably efficient for small batches
                self.cur.executemany(sql, rows)
                self.conn.commit()
            log.info("Inserted %d rows to Snowflake", len(rows))
        except Exception as e:
            log.exception("Snowflake insert failed; will retry later: %s", e)
            raise

def parse_line(line):
    # expects Arduino JSON -> {"t":1234,"a":38.7,"raw":512}
    try:
        obj = json.loads(line)
        return obj
    except Exception:
        return None

def run_serial_gateway(port, baud, session_id, device_id, dry_run=False, dashboard=False, dashboard_url=DASHBOARD_URL_DEFAULT):
    # Connect to Snowflake if not dry_run
    sf_conn = None
    sink = None
    if not dry_run:
        sf_conn = connect_snowflake()
        sink = Sink(sf_conn)

    ser = serial.Serial(port, baud, timeout=1)
    log.info("Opened serial port %s @ %d", port, baud)

    buffer = []
    seq = 0
    last_flush = time.time()

    try:
        while True:
            raw = ser.readline()
            if not raw:
                # periodic flush if needed
                if (time.time() - last_flush) >= FLUSH_INTERVAL and buffer:
                    try:
                        if not dry_run:
                            sink.insert_batch(buffer)
                        else:
                            log.info("Dry-run flush of %d rows", len(buffer))
                    except Exception:
                        # On insert failure, keep buffer and retry next iteration
                        log.exception("Insert failed, keeping buffer")
                    else:
                        buffer = []
                    last_flush = time.time()
                continue

            try:
                line = raw.decode('utf-8', errors='replace').strip()
            except Exception:
                continue

            obj = parse_line(line)
            if obj is None:
                log.debug("Non-JSON line: %s", line)
                continue

            server_ts = datetime.utcnow()

            # ---- Forward event frames to the dashboard, but do NOT insert into Snowflake ----
            # (Needed so the UI shows REC badge, rep logs, run cards, etc.)
            if 'event' in obj:
                if dashboard:
                    out = {
                        **obj,  # keep original keys: event, t, sessionId, etc.
                        "session_id": session_id,
                        "device_id": device_id,
                        "server_ts": server_ts.isoformat() + "Z",
                    }
                    try:
                        requests.post(dashboard_url, json=out, timeout=0.5)
                    except Exception:
                        pass
                continue  # skip Snowflake insert for event frames

            # ---- Normalize sample fields for Snowflake insert ----
            device_ts_ms = obj.get('t') or obj.get('ts') or obj.get('time_ms')

            # Accept either 'angle' or legacy 'a'
            angle = obj.get('angle', obj.get('a'))
            # Accept common raw/ADC keys
            raw_adc = obj.get('raw') or obj.get('adc') or obj.get('pot')
            # Keep speed if present (for dashboard)
            speed = obj.get('speed')

            # If there is no angle, skip this line as a malformed sample
            if angle is None:
                continue

            # Insert into Snowflake (raw VARIANT keeps the full original JSON, including speed)
            raw_json = json.dumps(obj)
            row = (session_id, device_id, server_ts, device_ts_ms, angle, raw_adc, seq, raw_json)
            buffer.append(row)
            seq += 1

            # ---- Post to dashboard: include original sample keys (t, angle, speed) ----
            if dashboard:
                out = {
                    **obj,  # preserves "t", "angle", "speed"
                    "session_id": session_id,
                    "device_id": device_id,
                    "server_ts": server_ts.isoformat() + "Z",
                    "seq": seq,
                }
                try:
                    requests.post(dashboard_url, json=out, timeout=0.5)
                except Exception:
                    pass



            # flush if buffer big or time elapsed
            if len(buffer) >= BATCH_SIZE or (time.time() - last_flush) >= FLUSH_INTERVAL:
                try:
                    if not dry_run:
                        sink.insert_batch(buffer)
                    else:
                        log.info("Dry-run would insert %d rows", len(buffer))
                except Exception:
                    log.exception("Failed inserting batch, keeping it for retry")
                else:
                    buffer = []
                    last_flush = time.time()

    except KeyboardInterrupt:
        log.info("Shutting down; flushing remaining %d rows", len(buffer))
        if buffer:
            try:
                if not dry_run:
                    sink.insert_batch(buffer)
                else:
                    log.info("Dry-run flush: %d rows", len(buffer))
            except Exception:
                log.exception("Final flush failed")
    finally:
        if sf_conn:
            sf_conn.close()
        if ser:
            ser.close()

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--port", required=True, help="Serial port e.g. COM3 or /dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--session", default="session_demo")
    p.add_argument("--device", default="brace01")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--dashboard", action="store_true", help="POST samples to local dashboard")
    p.add_argument("--dashboard-url", default=None, help="Override dashboard ingest URL (e.g., http://127.0.0.1:5050/ingest)")
    args = p.parse_args()
    dash_url = args.dashboard_url or DASHBOARD_URL_DEFAULT
    run_serial_gateway(
        args.port,
        args.baud,
        args.session,
        args.device,
        dry_run=args.dry_run,
        dashboard=args.dashboard,
        dashboard_url=dash_url
    )