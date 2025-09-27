# app/test_conn.py
import os
from dotenv import load_dotenv
import snowflake.connector as sf

# Load environment variables from .env
load_dotenv()

# Connect to Snowflake
conn = sf.connect(
    user=os.getenv("SNOW_USER"),
    password=os.getenv("SNOW_PASSWORD"),
    account=os.getenv("SNOW_ACCOUNT"),
    warehouse=os.getenv("SNOW_WAREHOUSE"),
    database=os.getenv("SNOW_DATABASE"),
    schema=os.getenv("SNOW_SCHEMA"),
    role=os.getenv("SNOW_ROLE"),
)

with conn.cursor() as cur:
    # Insert a dummy row
    cur.execute("""
        INSERT INTO HACKGT.RAW.BRACE_SENSOR_READINGS
        (session_id, device_id, server_ts, device_ts_ms, angle, raw_adc, sample_seq, raw)
        SELECT 'smoke','dev0',CURRENT_TIMESTAMP,123,42.0,512,1,PARSE_JSON('{"ok":true}')
    """)
    conn.commit()

    # Count rows to confirm insert worked
    cur.execute("SELECT COUNT(*) FROM HACKGT.RAW.BRACE_SENSOR_READINGS;")
    print("Row count:", cur.fetchone()[0])

conn.close()
print("Connection + insert successful")