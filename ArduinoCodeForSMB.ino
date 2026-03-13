#define BLYNK_TEMPLATE_ID "I_put_my_blynk_template_ID_here"
#define BLYNK_TEMPLATE_NAME "Smart Mailbox"
#define BLYNK_AUTH_TOKEN "I_put_my_blynk_auth_tkn_here"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <ESP32Servo.h>

char ssid[] = "my_Wifi_SSID_here";
char pass[] = "my_Wifi_pwd_here";
// ---------------- PINS ----------------
#define TRIG_PIN 5
#define ECHO_PIN 18
#define BLUE_PIN 32
#define PRESSURE_PIN 35
#define MAG_PIN 25
#define SERVO_PIN 13
#define BUZZER_PIN 27
// ---------------- SERVO ----------------
#define SERVO_LOCKED 20
#define SERVO_UNLOCKED 100
// ---------------- THRESHOLDS ----------------
#define DIST_TRIGGER 80
#define BLUE_INSERT_DELTA 15
#define BLUE_REMOVE_DELTA 15
#define PRESSURE_TRIGGER 50   // lower if handle press is not detected
// ---------------- TIMING ----------------
#define LOOP_MS 300
#define ACTIVE_WINDOW_MS 10000
#define TEMP_UNLOCK_MS 10000
#define FACE_RESULT_WINDOW_MS 8000
#define FACE_REQUEST_TIMEOUT_MS 5000
#define ACCESS_WINDOW_MS 7000
#define EVENT_COOLDOWN_MS 2500

BlynkTimer timer;
Servo lockServo;

bool ownerMode = false;
bool faceKnown = false;
bool waitingForFace = false;
bool captureRequested = false;
bool buzzerOn = false;
bool accessAttemptActive = false;
bool lastDoorOpen = false;

unsigned long activeUntil = 0;
unsigned long tempUnlockUntil = 0;
unsigned long faceResultUntil = 0;
unsigned long faceRequestStarted = 0;
unsigned long accessAttemptUntil = 0;
unsigned long lastEvent = 0;

int blueBaseline = 0;
int pressureBaseline = 0;

String lastStatus = "";
// ---------------- HELPERS ----------------
void setStatus(String msg) 
{
  if (msg != lastStatus) 
  {
    Blynk.virtualWrite(V5, msg);
    lastStatus = msg;
  }
}
void setBuzzer(bool on) 
{
  if (ownerMode) 
  {
    buzzerOn = false;
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }
  buzzerOn = on;
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
}
void stopBuzzer() 
{
  buzzerOn = false;
  digitalWrite(BUZZER_PIN, LOW);
}
void setLock(bool locked) 
{
  lockServo.write(locked ? SERVO_LOCKED : SERVO_UNLOCKED);
}
int readAvg(int pin, int samples = 10) 
{
  long sum = 0;
  for (int i = 0; i < samples; i++) 
  {
    sum += analogRead(pin);
    delay(2);
  }
  return sum / samples;
}
long readDistanceCm() 
{
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;
  return duration / 58;
}
void calibrateSensors() 
{
  delay(500);
  blueBaseline = readAvg(BLUE_PIN, 25);
  pressureBaseline = readAvg(PRESSURE_PIN, 25);
  Serial.print("Blue baseline: ");
  Serial.println(blueBaseline);
  Serial.print("Pressure baseline: ");
  Serial.println(pressureBaseline);
}
bool isAuthorized() 
{
  bool tempUnlock = millis() <= tempUnlockUntil;
  bool faceWindow = faceKnown && millis() <= faceResultUntil;
  return ownerMode || tempUnlock || faceWindow;
}
void requestFaceScan(unsigned long now) 
{
  if (!captureRequested) 
  {
    Blynk.virtualWrite(V10, 1);
    captureRequested = true;
    waitingForFace = true;
    faceRequestStarted = now;
    accessAttemptActive = true;
    accessAttemptUntil = now + ACCESS_WINDOW_MS;
    setStatus("Checking face");
    Serial.println("Face scan requested");
  }
}
void clearAccessAttempt() 
{
  waitingForFace = false;
  captureRequested = false;
  accessAttemptActive = false;
}
// ---------------- BLYNK INPUTS ----------------
BLYNK_WRITE(V0) // silence buzzer
{   
  if (param.asInt() == 1) 
  {
    stopBuzzer();
    setStatus("Buzzer silenced");
  }
}
BLYNK_WRITE(V1) // temp unlock
{   
  if (param.asInt() == 1) 
  {
    tempUnlockUntil = millis() + TEMP_UNLOCK_MS;
    clearAccessAttempt();
    setStatus("Temp unlock on");
  }
}
BLYNK_WRITE(V2) // owner mode
{   
  ownerMode = (param.asInt() == 1);
  if (ownerMode) 
  {
    stopBuzzer();
    clearAccessAttempt();
  }
  setStatus(ownerMode ? "Owner mode on" : "Owner mode off");
}
BLYNK_WRITE(V11)  // face result from laptop 
{  
  int result = param.asInt();
  waitingForFace = false;
  captureRequested = false;
  if (result == 1) 
  {
    faceKnown = true;
    faceResultUntil = millis() + FACE_RESULT_WINDOW_MS;
    setStatus("Face matched");
    stopBuzzer();
    Serial.println("Face matched");
  } 
  else 
    {
    faceKnown = false;
    faceResultUntil = 0;
    setStatus("Face not matched");
      if (accessAttemptActive) 
      {
        setBuzzer(true);
      }
    Serial.println("Face not matched");
    }
}
// ---------------- MAIN LOGIC ----------------
void mailboxLogic() 
{
  unsigned long now = millis();
  long dist = readDistanceCm();
  int blueVal = readAvg(BLUE_PIN);
  int pressureVal = readAvg(PRESSURE_PIN);
  int blueDelta = blueVal - blueBaseline;
  int pressureDelta = pressureVal - pressureBaseline;
  bool doorNowOpen = digitalRead(MAG_PIN) == HIGH;   // HIGH=open, LOW=closed
  bool authorized = isAuthorized();
  // ultrasonic arms system
  if (dist > 0 && dist <= DIST_TRIGGER) 
  {
    activeUntil = now + ACTIVE_WINDOW_MS;
  }
  bool active = (now <= activeUntil);
  // lock follows authorization
  setLock(!authorized);
  // start a real access attempt only when:
  // person is near + not authorized + handle pressed
  if (active && !authorized && pressureDelta >= PRESSURE_TRIGGER && !waitingForFace && !captureRequested) 
  {
    requestFaceScan(now);
  }
  // face request timeout
  if (waitingForFace && (now - faceRequestStarted > FACE_REQUEST_TIMEOUT_MS)) 
  {
    waitingForFace = false;
    captureRequested = false;
    faceKnown = false;
    setStatus("Face check timed out");
    if (accessAttemptActive) 
    {
      setBuzzer(true);
    }
  }
  // access attempt expires automatically
  if (accessAttemptActive && now > accessAttemptUntil) 
  {
    accessAttemptActive = false;
  }
  // door status display only
  if (doorNowOpen && authorized) 
  {
    setStatus("Door open");
  } 
  else if (!doorNowOpen && authorized) 
  {
    setStatus("Door closed");
  }
  // only buzz for door opening if there was a real access attempt
  if (doorNowOpen && !lastDoorOpen && !authorized && accessAttemptActive) 
  {
    setStatus("Unauthorized door open");
    setBuzzer(true);
  }
  lastDoorOpen = doorNowOpen;
  if (now - lastEvent < EVENT_COOLDOWN_MS) return;
  // mail inserted
  if (blueDelta > BLUE_INSERT_DELTA) 
  {
    lastEvent = now;
    blueBaseline = readAvg(BLUE_PIN, 20);
    setStatus("Mail inserted");
    return;
  }
  // mail removed
  if (blueDelta < -BLUE_REMOVE_DELTA) 
  {
    lastEvent = now;
    blueBaseline = readAvg(BLUE_PIN, 20);
    if (authorized) 
    {
      setStatus("Mail removed by owner");
      stopBuzzer();
    } 
    else 
    {
      setStatus("Unauthorized removal");
      if (accessAttemptActive) 
      {
        setBuzzer(true);
      }
      Blynk.logEvent("theft_alert", "Unauthorized mail removal attempt.");
    }
    return;
  }
  // clear face permission after window expires
  if (faceKnown && now > faceResultUntil) 
  {
    faceKnown = false;
  }
  Serial.print("Dist=");
  Serial.print(dist);
  Serial.print("  Blue=");
  Serial.print(blueVal);
  Serial.print(" d=");
  Serial.print(blueDelta);
  Serial.print("  Pressure35=");
  Serial.print(pressureVal);
  Serial.print(" d=");
  Serial.print(pressureDelta);
  Serial.print("  DoorOpen=");
  Serial.print(doorNowOpen);
  Serial.print("  ownerMode=");
  Serial.print(ownerMode);
  Serial.print("  waiting=");
  Serial.print(waitingForFace);
  Serial.print("  captureReq=");
  Serial.print(captureRequested);
  Serial.print("  accessAttempt=");
  Serial.print(accessAttemptActive);
  Serial.print("  faceKnown=");
  Serial.println(faceKnown);
}
void setup() 
{
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MAG_PIN, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetPinAttenuation(BLUE_PIN, ADC_11db);
  analogSetPinAttenuation(PRESSURE_PIN, ADC_11db);

  lockServo.attach(SERVO_PIN);
  setLock(true);
  stopBuzzer();

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  calibrateSensors();
  timer.setInterval(LOOP_MS, mailboxLogic);
  setStatus("System ready");
}
void loop() 
{
  Blynk.run();
  timer.run();
}