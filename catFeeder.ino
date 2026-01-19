#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <time.h>

// ======= WIFI SETTINGS =======
const char* WIFI_SSID = "DIGI-E01B";
const char* WIFI_PASS = "alexandra";
// ============================

// Bucharest timezone rule (DST aware)
const char* TZ_INFO = "EET-2EEST,M3.5.0/3,M10.5.0/4";
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";

// Pins
const int SERVO_PIN  = 12;   // if boot issues, change to 13
const int PIR_PIN    = 27;
const int BUZZER_PIN = 14;

// Servo angles
const int SERVO_CLOSED = 0;
const int SERVO_OPEN   = 90;

// FEED cycle
const unsigned long FEED_OPEN_MS = 10000; // 10 seconds

// CONFIRM motion filter
const unsigned long MOTION_CONFIRM_MS = 250; // PIR must be HIGH this long to count

// SLOW BUZZER pattern (reliable)
// (slower than your previous fast beeps)
const unsigned long BUZZ_ON_MS  = 500;
const unsigned long BUZZ_OFF_MS = 500;

WebServer server(80);
Servo myServo;

// ----- State -----
bool systemEnabled = true;

bool pirRaw = false;
bool motionConfirmed = false;

bool servoOpen = false;
bool buzzerOn = false;

enum Mode { MODE_AUTO, MODE_FEED };
Mode mode = MODE_AUTO;

// timers
unsigned long feedStartMs = 0;
unsigned long pirHighSinceMs = 0;

// buzzer timing
unsigned long buzzToggleMs = 0;

// graph logging
struct MoveEvent {
  uint32_t startEpoch;
  uint32_t durationSec;
};
const int MAX_EVENTS = 160;
MoveEvent events[MAX_EVENTS];
int eventCount = 0;
uint32_t openStartEpoch = 0;

//  Time helpers 
uint32_t nowEpoch() {
  time_t t;
  time(&t);
  if (t < 100000) return 0;
  return (uint32_t)t;
}

String fmtTime(uint32_t epoch) {
  if (epoch == 0) return "NO_TIME";
  time_t t = (time_t)epoch;
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  return String(buf);
}

void addEvent(uint32_t startEpoch, uint32_t durationSec) {
  if (startEpoch == 0 || durationSec == 0) return;

  if (eventCount < MAX_EVENTS) {
    events[eventCount++] = { startEpoch, durationSec };
  } else {
    for (int i = 1; i < MAX_EVENTS; i++) events[i - 1] = events[i];
    events[MAX_EVENTS - 1] = { startEpoch, durationSec };
  }
}

//  Servo helpers 
void setServo(bool open) {
  if (open == servoOpen) return;
  servoOpen = open;
  myServo.write(servoOpen ? SERVO_OPEN : SERVO_CLOSED);
}

void closeServoAndLogIfNeeded() {
  if (!servoOpen) return;

  setServo(false);
  uint32_t endEpoch = nowEpoch();
  if (openStartEpoch != 0 && endEpoch != 0 && endEpoch >= openStartEpoch) {
    addEvent(openStartEpoch, endEpoch - openStartEpoch);
  }
  openStartEpoch = 0;
}

void openServoAndMarkStartIfNeeded() {
  if (servoOpen) return;

  setServo(true);
  openStartEpoch = nowEpoch();
}

//  Buzzer (simple ON/OFF) 
void buzzerWrite(bool on) {
  buzzerOn = on;
  digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);
}

void buzzerStop() {
  buzzerWrite(false);
}

// During FEED mode: slow beep pattern
void buzzerUpdateFeedPattern() {
  unsigned long now = millis();

  if (buzzerOn) {
    if (now - buzzToggleMs >= BUZZ_ON_MS) {
      buzzToggleMs = now;
      buzzerWrite(false);
    }
  } else {
    if (now - buzzToggleMs >= BUZZ_OFF_MS) {
      buzzToggleMs = now;
      buzzerWrite(true);
    }
  }
}

//  Motion filter 
void updateMotionFilter() {
  pirRaw = (digitalRead(PIR_PIN) == HIGH);
  unsigned long now = millis();

  if (pirRaw) {
    if (pirHighSinceMs == 0) pirHighSinceMs = now;
    motionConfirmed = (now - pirHighSinceMs >= MOTION_CONFIRM_MS);
  } else {
    pirHighSinceMs = 0;
    motionConfirmed = false;
  }
}

//  Control logic 
void forceStopAll() {
  mode = MODE_AUTO;
  buzzerStop();
  closeServoAndLogIfNeeded();
}

void autoControlFromMotion(bool motion) {
  if (motion) openServoAndMarkStartIfNeeded();
  else closeServoAndLogIfNeeded();
}

// Feed cycle logic with your rule:
// after 10s, close ONLY if motion is DOWN; otherwise keep open and return to AUTO
void startFeedCycle() {
  if (!systemEnabled) return;

  mode = MODE_FEED;
  feedStartMs = millis();

  openServoAndMarkStartIfNeeded();

  // start slow buzzer pattern
  buzzerWrite(true);
  buzzToggleMs = millis();
}

void updateFeedCycle() {
  unsigned long now = millis();

  // slow beeps while feeding
  buzzerUpdateFeedPattern();

  // after 10 seconds:
  if (now - feedStartMs >= FEED_OPEN_MS) {
    // stop buzzer
    buzzerStop();

    // close only if motion is DOWN
    if (!motionConfirmed) {
      closeServoAndLogIfNeeded();
    }

    // return to AUTO
    mode = MODE_AUTO;
  }
}

//  Web UI 
String pageHtml() {
  return R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>Cat Feeder</title>
  <style>
    :root{
      --bg1:#ff4da6;
      --bg2:#ff8fd1;
      --bg3:#ffd1ea;
      --card: rgba(255,255,255,.18);
      --card2: rgba(255,255,255,.12);
      --border: rgba(255,255,255,.35);
      --text:#1b1020;
      --muted: rgba(27,16,32,.72);
      --shadow: 0 18px 50px rgba(0,0,0,.18);
      --btnShadow: 0 10px 24px rgba(0,0,0,.18);
      --radius: 18px;
    }

    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial, sans-serif;
      color: var(--text);
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 18px;
      background: radial-gradient(1100px 600px at 20% 20%, var(--bg3), transparent 60%),
                  radial-gradient(900px 520px at 90% 10%, rgba(255,255,255,.45), transparent 55%),
                  linear-gradient(135deg, var(--bg1), var(--bg2));
    }

    .wrap { width: min(980px, 100%); }

    .topbar{
      display:flex;
      align-items:flex-end;
      justify-content:space-between;
      gap: 16px;
      margin-bottom: 14px;
    }

    h1{
      margin:0;
      font-size: 22px;
      letter-spacing: .2px;
    }

    .sub{
      margin-top: 6px;
      color: var(--muted);
      font-size: 13px;
      line-height: 1.35;
    }

    .clock{
      padding: 10px 12px;
      border-radius: 999px;
      background: rgba(255,255,255,.25);
      border: 1px solid rgba(255,255,255,.35);
      backdrop-filter: blur(10px);
      box-shadow: 0 10px 28px rgba(0,0,0,.10);
      font-size: 13px;
      color: rgba(27,16,32,.85);
      white-space: nowrap;
    }

    .card{
      background: var(--card);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      box-shadow: var(--shadow);
      backdrop-filter: blur(14px);
      overflow: hidden;
    }

    .cardInner{
      padding: 16px;
      background: linear-gradient(180deg, rgba(255,255,255,.20), rgba(255,255,255,.10));
    }

    .grid{
      display:grid;
      grid-template-columns: 1fr 1fr;
      gap: 12px;
    }

    .panel{
      border-radius: 14px;
      background: rgba(255,255,255,.16);
      border: 1px solid rgba(255,255,255,.25);
      padding: 12px;
    }

    .row{
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap: 10px;
      padding: 8px 0;
      border-bottom: 1px dashed rgba(255,255,255,.25);
    }
    .row:last-child{ border-bottom: 0; }

    .label{
      font-size: 13px;
      color: rgba(27,16,32,.80);
    }

    .pill{
      display:inline-flex;
      align-items:center;
      gap: 8px;
      padding: 8px 12px;
      border-radius: 999px;
      font-size: 13px;
      border: 1px solid rgba(255,255,255,.35);
      background: rgba(255,255,255,.22);
      white-space: nowrap;
      min-width: 148px;
      justify-content:center;
    }

    .dot{
      width: 9px;
      height: 9px;
      border-radius: 50%;
      background: rgba(255,255,255,.8);
      box-shadow: 0 0 0 3px rgba(255,255,255,.25);
    }

    .ok .dot{ background: #19c37d; }
    .warn .dot{ background: #ffb020; }
    .bad .dot{ background: #ff3b6b; }

    .pillText{ font-weight: 650; letter-spacing: .2px; }

    .controls{
      display:flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-top: 12px;
    }

    button{
      border: 0;
      padding: 11px 14px;
      border-radius: 14px;
      cursor: pointer;
      font-weight: 700;
      font-size: 14px;
      letter-spacing: .2px;
      box-shadow: var(--btnShadow);
      transition: transform .08s ease, filter .12s ease;
      color: rgba(27,16,32,.92);
      background: rgba(255,255,255,.75);
    }
    button:hover{ filter: brightness(1.02); }
    button:active{ transform: translateY(1px); }

    .primary{
      background: linear-gradient(135deg, rgba(255,255,255,.92), rgba(255,255,255,.55));
    }
    .danger{
      background: linear-gradient(135deg, rgba(255,59,107,.95), rgba(255,140,180,.75));
      color: white;
    }
    .success{
      background: linear-gradient(135deg, rgba(25,195,125,.95), rgba(160,255,216,.70));
      color: #08301f;
    }
    .ghost{
      background: rgba(255,255,255,.45);
    }

    .chartWrap{
      margin-top: 12px;
      border-radius: 16px;
      background: rgba(255,255,255,.16);
      border: 1px solid rgba(255,255,255,.25);
      padding: 12px;
    }

    canvas{
      width: 100%;
      height: 290px;
      display:block;
      border-radius: 12px;
      background: rgba(255,255,255,.40);
    }

    .hint{
      margin-top: 10px;
      font-size: 12px;
      color: rgba(27,16,32,.72);
      line-height: 1.35;
    }

    @media (max-width: 820px){
      .grid{ grid-template-columns: 1fr; }
      .pill{ min-width: 0; width: 100%; }
    }
  </style>
</head>

<body>
  <div class="wrap">
    <div class="topbar">
      <div>
        <h1>ESP32 Cat Feeder</h1>
        <div class="sub">
          Auto mode uses motion confirmation. Feed Now opens for 10s, then closes only if motion is LOW.
        </div>
      </div>
      <div class="clock">
        Time: <span id="time">--:--:--</span>
      </div>
    </div>

    <div class="card">
      <div class="cardInner">
        <div class="grid">
          <div class="panel">
            <div class="row">
              <div class="label">System</div>
              <div id="sysPill" class="pill">
                <span class="dot"></span>
                <span class="pillText" id="sys">...</span>
              </div>
            </div>

            <div class="row">
              <div class="label">Mode</div>
              <div id="modePill" class="pill">
                <span class="dot"></span>
                <span class="pillText" id="mode">...</span>
              </div>
            </div>

            <div class="row">
              <div class="label">PIR raw</div>
              <div id="pirRawPill" class="pill">
                <span class="dot"></span>
                <span class="pillText" id="pirraw">...</span>
              </div>
            </div>

            <div class="row">
              <div class="label">Motion confirmed</div>
              <div id="motionPill" class="pill">
                <span class="dot"></span>
                <span class="pillText" id="motion">...</span>
              </div>
            </div>
          </div>

          <div class="panel">
            <div class="row">
              <div class="label">Gate / Servo</div>
              <div id="servoPill" class="pill">
                <span class="dot"></span>
                <span class="pillText" id="servo">...</span>
              </div>
            </div>

            <div class="row">
              <div class="label">Buzzer</div>
              <div id="buzzPill" class="pill">
                <span class="dot"></span>
                <span class="pillText" id="buzz">...</span>
              </div>
            </div>

            <div class="controls">
              <button class="success" onclick="startSys()">START</button>
              <button class="danger" onclick="stopSys()">STOP</button>
              <button class="primary" onclick="feedNow()">FEED NOW (10s)</button>
              <button class="ghost" onclick="closeNow()">CLOSE NOW</button>
            </div>

            <div class="hint">
              STOP disables the system and forces the gate closed. CLOSE NOW closes the gate immediately in any mode.
            </div>
          </div>
        </div>

        <div class="chartWrap">
          <canvas id="chart" width="1200" height="320"></canvas>
          <div class="hint">
            Graph: x = time of day, y = seconds the gate stayed open (each bar = one session).
          </div>
        </div>

      </div>
    </div>
  </div>

<script>
  async function getStatus(){
    const r = await fetch('/status', {cache:'no-store'});
    return await r.json();
  }
  async function getLog(){
    const r = await fetch('/log', {cache:'no-store'});
    return await r.json();
  }

  function setPill(pillId, textId, text, level){
    const pill = document.getElementById(pillId);
    pill.classList.remove('ok','warn','bad');
    pill.classList.add(level);
    document.getElementById(textId).textContent = text;
  }

  function hmsToSec(hms){
    const parts = hms.split(':').map(Number);
    if (parts.length !== 3) return 0;
    return parts[0]*3600 + parts[1]*60 + parts[2];
  }

  function drawChart(events) {
    const c = document.getElementById('chart');
    const ctx = c.getContext('2d');

    ctx.clearRect(0,0,c.width,c.height);

    const padL=62,padR=18,padT=16,padB=46;
    const w=c.width-padL-padR, h=c.height-padT-padB;

    let maxDur=1;
    for(const e of events) maxDur=Math.max(maxDur, e.duration_sec);

    // soft grid
    ctx.strokeStyle='rgba(0,0,0,0.08)';
    ctx.lineWidth=1;
    for(let i=0;i<=5;i++){
      const y=padT+(h*i/5);
      ctx.beginPath(); ctx.moveTo(padL,y); ctx.lineTo(padL+w,y); ctx.stroke();
    }

    // axes
    ctx.strokeStyle='rgba(0,0,0,0.25)';
    ctx.lineWidth=2;
    ctx.beginPath();
    ctx.moveTo(padL,padT);
    ctx.lineTo(padL,padT+h);
    ctx.lineTo(padL+w,padT+h);
    ctx.stroke();

    // y labels
    ctx.fillStyle='rgba(0,0,0,0.65)';
    ctx.font='14px Arial';
    ctx.fillText('sec', 14, padT+14);

    ctx.fillStyle='rgba(0,0,0,0.55)';
    ctx.font='12px Arial';
    for(let i=0;i<=5;i++){
      const val=Math.round(maxDur*(5-i)/5);
      const y=padT+(h*i/5);
      ctx.fillText(String(val), 20, y+4);
    }

    // x ticks
    const ticks=['00:00','06:00','12:00','18:00','24:00'];
    ctx.fillStyle='rgba(0,0,0,0.55)';
    ctx.font='12px Arial';
    for(let i=0;i<5;i++){
      const x=padL+(w*i/4);
      ctx.fillText(ticks[i], x-18, padT+h+30);
    }

    // bars
    ctx.fillStyle='rgba(255,77,166,0.85)';
    const barW=Math.max(3, Math.floor(w/160));
    for(const e of events){
      const xSec=hmsToSec(e.start_hms);
      const x=padL+(xSec/86400)*w;
      const barH=(e.duration_sec/maxDur)*h;
      const y=padT+h-barH;
      ctx.fillRect(x, y, barW, barH);
    }
  }

  async function refresh(){
    try{
      const s = await getStatus();
      document.getElementById('time').textContent = s.local_time || '--:--:--';

      setPill('sysPill','sys', s.enabled ? 'ENABLED' : 'STOPPED', s.enabled ? 'ok' : 'bad');
      setPill('modePill','mode', s.mode || '...', (s.mode && s.mode.indexOf('FEED')>=0) ? 'warn' : 'ok');

      setPill('pirRawPill','pirraw', s.pir_raw ? 'HIGH' : 'LOW', s.pir_raw ? 'warn' : 'ok');
      setPill('motionPill','motion', s.motion ? 'YES' : 'NO', s.motion ? 'ok' : 'ok');

      setPill('servoPill','servo', s.servo_open ? 'OPEN (90°)' : 'CLOSED (0°)', s.servo_open ? 'warn' : 'ok');
      setPill('buzzPill','buzz', s.buzzer ? 'ON' : 'OFF', s.buzzer ? 'warn' : 'ok');

      const log = await getLog();
      drawChart(log.events || []);
    } catch(e) {
      // If unreachable, show stopped-ish UI
      setPill('sysPill','sys', 'OFFLINE', 'bad');
    }
  }

  async function startSys(){ await fetch('/start', {cache:'no-store'}); refresh(); }
  async function stopSys(){ await fetch('/stop', {cache:'no-store'}); refresh(); }
  async function feedNow(){ await fetch('/feed', {cache:'no-store'}); refresh(); }
  async function closeNow(){ await fetch('/close', {cache:'no-store'}); refresh(); }

  setInterval(refresh, 1200);
  refresh();
</script>
</body>
</html>
)rawliteral";
}


//  Web handlers 
void handleRoot() { server.send(200, "text/html", pageHtml()); }

void handleStatus() {
  String t = fmtTime(nowEpoch());
  String m = (mode == MODE_FEED) ? "FEED (10s)" : "AUTO (PIR)";

  String json = "{";
  json += "\"enabled\":" + String(systemEnabled ? "true" : "false") + ",";
  json += "\"mode\":\"" + m + "\",";
  json += "\"pir_raw\":" + String(pirRaw ? "true" : "false") + ",";
  json += "\"motion\":" + String(motionConfirmed ? "true" : "false") + ",";
  json += "\"servo_open\":" + String(servoOpen ? "true" : "false") + ",";
  json += "\"buzzer\":" + String(buzzerOn ? "true" : "false") + ",";
  json += "\"local_time\":\"" + t + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handleLog() {
  String json = "{";
  json += "\"count\":" + String(eventCount) + ",";
  json += "\"events\":[";
  for (int i = 0; i < eventCount; i++) {
    if (i) json += ",";
    json += "{";
    json += "\"start_epoch\":" + String(events[i].startEpoch) + ",";
    json += "\"start_hms\":\"" + fmtTime(events[i].startEpoch) + "\",";
    json += "\"duration_sec\":" + String(events[i].durationSec);
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleStart() { systemEnabled = true; server.send(200, "text/plain", "OK"); }

void handleStop() {
  systemEnabled = false;
  forceStopAll(); // closes servo + buzzer off
  server.send(200, "text/plain", "OK");
}

void handleFeed() {
  if (systemEnabled) startFeedCycle();
  server.send(200, "text/plain", "OK");
}

void handleClose() {
  // FORCE CLOSE NOW
  mode = MODE_AUTO;
  buzzerStop();
  closeServoAndLogIfNeeded();
  server.send(200, "text/plain", "OK");
}

//  Time setup 
void setupTime() {
  setenv("TZ", TZ_INFO, 1);
  tzset();
  configTime(0, 0, NTP1, NTP2);

  Serial.print("Syncing time");
  for (int i = 0; i < 30; i++) {
    if (nowEpoch() != 0) {
      Serial.println("\nTime synced.");
      return;
    }
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nTime not synced yet (timestamps may show NO_TIME until sync).");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerStop();

  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.write(SERVO_CLOSED);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 30000) break;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi FAILED (check SSID/PASS and 2.4GHz).");
  }

  setupTime();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/start", HTTP_GET, handleStart);
  server.on("/stop", HTTP_GET, handleStop);
  server.on("/feed", HTTP_GET, handleFeed);
  server.on("/close", HTTP_GET, handleClose);

  server.begin();
  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();

  // update PIR + confirmed motion
  updateMotionFilter();

  if (!systemEnabled) return;

  if (mode == MODE_FEED) {
    updateFeedCycle();
  } else {
    autoControlFromMotion(motionConfirmed);
  }
}
