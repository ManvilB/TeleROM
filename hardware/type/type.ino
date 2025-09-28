/*
  Tele-Rehab Potentiometer Tracker (sessions + motion)
  - median3 + linear gate + EMA
  - angle (deg), speed (deg/s)
  - auto session start/stop on motion/inactivity
  - per-rep summary during session
  - NDJSON output, non-blocking writes
*/

#include <Arduino.h>
#include <math.h>

const int POT_PIN = A0;

// ---- Optional manual control button (tie to GND when pressed). Set -1 to disable.
const int BTN_PIN = -1;  // e.g., 2 with pinMode(INPUT_PULLUP) if you wire a button

// ---- Calibration ----
int   potMin = 5, potMax = 1030;
float degMin = 0.0f, degMax = 240.0f;

// ---- Rates / filtering ----
const uint16_t LOOP_MS = 40;   // ~25 Hz
const int   DEADBAND = 3, MAX_STEP = 10, CONSISTENT_N = 3, CONSISTENT_TOL = 5, SAMPLE_US = 200;
const float OUT_EMA_ALPHA = 0.20f;
const float SPEED_ALPHA = 0.30f, SPEED_ZERO_EPS = 0.05f;

// ---- Lift detection ----
const float LIFT_POS_THR = 15.0f;   // start lift if speed > this
const float LIFT_NEG_THR = -5.0f;   // end lift if speed < this
const float LIFT_MIN_RANGE = 10.0f;
const unsigned long REP_COOLDOWN_MS = 600;
const bool  USE_TARGET_ANGLE = false;
const float TARGET_ANGLE = 90.0f;

// ---- Motion/session detection ----
const float MOTION_SPEED_THR       = 8.0f;    // deg/s considered "moving"
const float MOTION_ANGLE_THR       = 1.0f;    // deg fallback if speed noisy
const unsigned long MOTION_START_MS = 200;    // need >= this of movement to start session
const unsigned long INACTIVITY_MS   = 3000;   // stop session if idle this long

// ---- Output control ----
const uint8_t PRINT_DECIMATE = 1;   // 1 = print every frame while active
const int PWM_PIN = -1;             // -1 = off; else mirror to LED/motor

// ---- State ----
unsigned long lastLoopMs = 0;

// linear gate
int  lastGood = 0; bool inLargeMove = false; int largeRef = 0; int consistentCnt = 0;

// filtered output
float outEma = 0;

// angle/speed
unsigned long lastTs = 0; float lastAngleDeg = 0.0f; float speedEma = 0.0f;

// lift state
bool inLift = false; unsigned long tLiftStart = 0; float angleLiftStart = 0.0f;
float peakSpeedLift = 0.0f; unsigned long lastRepTime = 0; int repCount = 0;

// session state
bool sessionActive = false;
unsigned long sessionId = 0, sessionStartMs = 0, lastMotionMs = 0;
float sessionMaxAngle = 0.0f;

// TX buffer
char lineBuf[220];

// ---- Helpers ----
inline int median3(int a,int b,int c){ if(a>b){int t=a;a=b;b=t;} if(b>c){int t=b;b=c;c=t;} if(a>b){int t=a;a=b;b=t;} return b; }

int readMedian3(){
  analogRead(POT_PIN);
  int a=analogRead(POT_PIN); delayMicroseconds(SAMPLE_US);
  int b=analogRead(POT_PIN); delayMicroseconds(SAMPLE_US);
  int c=analogRead(POT_PIN);
  return median3(a,b,c);
}

int acceptLinearOnly(int raw){
  if (abs(raw - lastGood) <= DEADBAND){ inLargeMove=false; consistentCnt=0; return lastGood; }
  int d = raw - lastGood;
  if (abs(d) <= MAX_STEP){ inLargeMove=false; consistentCnt=0; lastGood=raw; return lastGood; }
  if (!inLargeMove){ inLargeMove=true; largeRef=raw; consistentCnt=1; return lastGood; }
  if (abs(raw - largeRef) <= CONSISTENT_TOL){ consistentCnt++; largeRef=(largeRef+raw)/2; }
  else { largeRef=raw; consistentCnt=1; }
  if (consistentCnt >= CONSISTENT_N){ lastGood=largeRef; inLargeMove=false; consistentCnt=0; }
  return lastGood;
}

inline float clampf(float v,float lo,float hi){ return v<lo?lo:(v>hi?hi:v); }

inline void writeLine(const char* fmt, ...){
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(lineBuf, sizeof(lineBuf), fmt, ap);
  va_end(ap);
  if (n > 0 && n < (int)sizeof(lineBuf) && Serial.availableForWrite() >= n) {
    Serial.write((const uint8_t*)lineBuf, n);
  }
}

inline void writeSample(unsigned long t,float angle,float speed){
  writeLine("{\"t\":%lu,\"angle\":%.2f,\"speed\":%.1f}\n",
            t, angle, (fabs(speed)<SPEED_ZERO_EPS?0.0f:speed));
}

inline void writeRep(unsigned long t,float angle,float speed,int rep,float liftRange,float avgLift,float peakLift){
  writeLine("{\"t\":%lu,\"event\":\"rep\",\"rep\":%d,"
            "\"angle\":%.2f,\"speed\":%.1f,"
            "\"liftRange\":%.1f,\"avgLiftSpeed\":%.1f,\"peakLiftSpeed\":%.1f}\n",
            t, rep, angle, (fabs(speed)<SPEED_ZERO_EPS?0.0f:speed), liftRange, avgLift, peakLift);
}

inline void writeSessionStart(unsigned long t, unsigned long sid){
  writeLine("{\"t\":%lu,\"event\":\"session_start\",\"sessionId\":%lu}\n", t, sid);
}

inline void writeSessionStop(unsigned long t, unsigned long sid, unsigned long durationMs, int reps, float maxAngle){
  writeLine("{\"t\":%lu,\"event\":\"session_stop\",\"sessionId\":%lu,"
            "\"duration_s\":%.2f,\"reps\":%d,\"maxAngle\":%.1f}\n",
            t, sid, durationMs/1000.0, reps, maxAngle);
}

// ---- Setup/loop ----
void setup(){
  Serial.begin(115200);
  if (PWM_PIN >= 0) pinMode(PWM_PIN, OUTPUT);
  if (BTN_PIN >= 0) pinMode(BTN_PIN, INPUT_PULLUP);

  lastGood = analogRead(POT_PIN);
  outEma   = lastGood;
  sessionMaxAngle = degMin;
}

void loop(){
  const unsigned long now = millis();
  if (now - lastLoopMs < LOOP_MS) return;
  lastLoopMs = now;

  // 1) Read & filter
  const int raw = readMedian3();
  const int out = acceptLinearOnly(raw);
  outEma = OUT_EMA_ALPHA*out + (1.0f-OUT_EMA_ALPHA)*outEma;

  if (PWM_PIN >= 0){
    const int pwm = map((int)outEma, 0, 1023, 0, 255);
    analogWrite(PWM_PIN, pwm);
  }

  // 2) Angle
  const float clamped = clampf(outEma, potMin, potMax);
  const float angleDeg = (clamped - potMin) * (degMax - degMin) / (float)(potMax - potMin) + degMin;

  // 3) Speed
  float speedDegS = 0.0f;
  float angleDelta = 0.0f;
  if (lastTs != 0){
    const float dt = (now - lastTs) * 0.001f;
    if (dt > 0.0f){
      const float rawSpeed = (angleDeg - lastAngleDeg) / dt;
      angleDelta = angleDeg - lastAngleDeg;
      speedEma = SPEED_ALPHA*rawSpeed + (1.0f - SPEED_ALPHA)*speedEma;
      speedDegS = speedEma;
    }
  }
  lastTs = now; lastAngleDeg = angleDeg;

  // ---- Motion detect (for session control) ----
  bool moving = (fabs(speedDegS) > MOTION_SPEED_THR) || (fabs(angleDelta) > MOTION_ANGLE_THR);
  if (moving) lastMotionMs = now;

  // Manual toggle (optional)
  if (BTN_PIN >= 0 && digitalRead(BTN_PIN) == LOW) {
    if (!sessionActive) {
      sessionActive   = true; sessionId++; sessionStartMs = now;
      repCount        = 0;    sessionMaxAngle = angleDeg;
      lastMotionMs    = now;  // start inactivity window from here
      writeSessionStart(now, sessionId);
    } else {
      sessionActive = false;
      inLift = false; // reset lift state
      writeSessionStop(now, sessionId, now - sessionStartMs, repCount, sessionMaxAngle);
    }
    delay(250); // crude debounce
  }

  // Auto start: sustained motion
  static unsigned long motionStartMs = 0;
  if (!sessionActive) {
    if (moving) {
      if (motionStartMs == 0) motionStartMs = now;
      if ((now - motionStartMs) >= MOTION_START_MS) {
        sessionActive   = true; sessionId++; sessionStartMs = now;
        repCount        = 0;    sessionMaxAngle = angleDeg;
        lastMotionMs    = now;
        writeSessionStart(now, sessionId);
      }
    } else {
      motionStartMs = 0;
    }
  } else {
    // Auto stop: inactivity timeout
    if ((now - lastMotionMs) >= INACTIVITY_MS) {
      sessionActive = false;
      inLift = false;
      writeSessionStop(now, sessionId, now - sessionStartMs, repCount, sessionMaxAngle);
      motionStartMs = 0;
    }
  }

  if (sessionActive && angleDeg > sessionMaxAngle) sessionMaxAngle = angleDeg;

  // 4) Lift detection (only while session active)
  if (sessionActive) {
    if (!inLift){
      if ((now - lastRepTime) > REP_COOLDOWN_MS && speedDegS > LIFT_POS_THR){
        inLift = true;
        tLiftStart = now;
        angleLiftStart = angleDeg;
        peakSpeedLift = speedDegS;
      }
    } else {
      if (speedDegS > peakSpeedLift) peakSpeedLift = speedDegS;

      const bool reachedTarget = (USE_TARGET_ANGLE && angleDeg >= TARGET_ANGLE);
      const bool falling = (speedDegS < LIFT_NEG_THR);

      if (falling || reachedTarget){
        const float liftRange = angleDeg - angleLiftStart;
        const float liftDur   = max((now - tLiftStart) * 0.001f, 1e-3f);
        const float avgLift   = liftRange / liftDur;

        if (liftRange >= LIFT_MIN_RANGE){
          repCount++;
          lastRepTime = now;
          writeRep(now, angleDeg, speedDegS, repCount, liftRange, avgLift, peakSpeedLift);
        }
        inLift = false;
      }
    }
  } else {
    inLift = false; // ensure we don't get stuck between sessions
  }

  // 5) Samples ONLY while session is active
  if (sessionActive) {
    static uint8_t decim = 0;
    if (++decim >= PRINT_DECIMATE){ decim = 0; writeSample(now, angleDeg, speedDegS); }
  }

  // OPTIONAL: idle heartbeat (debug)
  // static unsigned long lastBeat = 0;
  // if (!sessionActive && now - lastBeat > 1000) {
  //   lastBeat = now;
  //   writeLine("{\"t\":%lu,\"event\":\"idle\"}\n", now);
  // }
}
