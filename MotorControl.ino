#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>

#define Seconds(s) s*1000
#define Minutes(m) m*Seconds(60)
#define MAX_MSG_LENGTH 120

#define Motor LED_BUILTIN
#define ManualOverride D1
#define ManualControl D2

#define ON "ON"
#define ONs1s3 "ONs1s3"
#define ONs1 "ONs1"
#define ONs3 "ONs3"
#define OFF "OFF"
#define STATUS "STATUS"
#define ON_WITH_TIMER "ONSolar"
#define ALL "ALL"
#define MOTOR "MOTOR"

const char *ssid = "BCWifi";
const char *password = "Swamy";
const char *host_name = "hostname_goes_here";
const char *TOPIC_MotorChange = "MotorStatusChange";
const char *TOPIC_PingGround = "PingGround";
const char *TOPIC_SysKill = "SysKill";
const char *TOPIC_PingTank = "PingTank";
const char *TOPIC_TankResponse = "TankResponse";
const char *TOPIC_ManualOverride = "ManualOverride";
const char *TOPIC_MotorReset = "MotorReset";
const char *TOPIC_CurrentMotorState = "CurrentMotorState";


bool motor_state;
bool manualEnable;
bool manual_state;
bool tank_responsive;
int no_response_count = 0;

unsigned long lastTankResponse = 4294967294;
unsigned long pingTime;
Ticker ping_tank;
Ticker tank_response;
Ticker BlinkLED;
Ticker water_timer;
WiFiClient wclient;
PubSubClient client(wclient);

const float timer_pure_seconds = 0;
const float timer_s1s3 = 0;
const float timer_s1 = 0;
const float timer_s3 = 0;

/*
 * Add delay everywhere we turn off motor to avoid immediate turn on
 * Maybe send manualOverride message less frequently to avoid wasting time at both ends.
 */

void setupWiFi() {

  bool manual;
  delay(10);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED) {
    manual = digitalRead(ManualOverride);
    if(manual)
      break;
    delay(300);
  }  
}

void tankresponsefun() {

  unsigned long elapsed = lastTankResponse - pingTime;
  if(abs(elapsed) < Minutes(48 * 60 /* 2 days */)) {       //Handle millis overflow
    if(elapsed <= Seconds(14)) 
      no_response_count = 0;
    else {
      if(no_response_count < 3)
        ++no_response_count;
    }
  }
}


void pingNow() {
  client.publish(TOPIC_PingTank, STATUS);
  pingTime = millis();
  tank_response.once(14, tankresponsefun);
}


void waterTimer() {

  if(motor_state) {
    digitalWrite(Motor, LOW);
    client.publish(TOPIC_CurrentMotorState, OFF);
    motor_state = 0;
  }
}


void callback(char *msgTopic, byte *msgPayload, unsigned int msgLength) {

  static char message[MAX_MSG_LENGTH + 1];

  memcpy(message, (char *) msgPayload, msgLength);
  message[msgLength] = '\0';

  if(!strcmp(msgTopic, TOPIC_TankResponse))
    if(!strcmp(message, ON))
      lastTankResponse = millis();


  if(!strcmp(msgTopic, TOPIC_SysKill))
    if(!strcmp(message, MOTOR) | !strcmp(message, ALL)) {
      if(motor_state)
        {
          digitalWrite(Motor, LOW);
          motor_state = 0;
        }
      ESP.deepSleep(0);
    }

  if(!strcmp(msgTopic, TOPIC_MotorChange) && !tank_responsive) {

    if(!strcmp(message, OFF)) {
      if(motor_state) {
        digitalWrite(Motor, LOW);
        motor_state = 0;
        client.publish(TOPIC_CurrentMotorState, OFF);
      }  
    }
    
    if(!strcmp(message, ON_WITH_TIMER)) {
      if(!motor_state) {
        motor_state = 1;
        digitalWrite(Motor, HIGH);
        client.publish(TOPIC_CurrentMotorState, ON);
        water_timer.once(timer_pure_seconds, waterTimer);
      }
    }

    if(!strcmp(message, ONs1s3)) {
      if(!motor_state) {
        motor_state = 1;
        digitalWrite(Motor, HIGH);
        client.publish(TOPIC_CurrentMotorState, ON);
        water_timer.once(timer_s1s3, waterTimer);
      }
    }

    if(!strcmp(message, ONs1)) {
      if(!motor_state) {
        motor_state = 1;
        digitalWrite(Motor, HIGH);
        client.publish(TOPIC_CurrentMotorState, ON);
        water_timer.once(timer_s1, waterTimer);
      }
    }

    if(!strcmp(message, ONs3)) {
      if(!motor_state) {
        motor_state = 1;
        digitalWrite(Motor, HIGH);
        client.publish(TOPIC_CurrentMotorState, ON);
        water_timer.once(timer_s3, waterTimer);
      }
    }
  }
}

void blinkfun() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(200);
  digitalWrite(LED_BUILTIN, LOW);
}

void setup() {
  // put your setup code here, to run once:

  Serial.begin(115200);
  motor_state = 0;
  tank_responsive = 1;
  
  pinMode(Motor, OUTPUT);
  pinMode(ManualOverride, INPUT);
  pinMode(ManualControl, INPUT);

  bool manual = digitalRead(ManualOverride);

  if(!manual)
    digitalWrite(Motor, LOW);

  setupWiFi();

  client.setServer(host_name, 1883);
  client.setCallback(callback);

  connectMQTT();

  client.publish(TOPIC_CurrentMotorState, OFF);
  ping_tank.attach(10, pingNow);
  BlinkLED.attach(10, blinkfun);
}


void connectMQTT() {

  bool manual;
  
  while (!client.connected()) {

    manual = digitalRead(ManualOverride);

    if(manual)
      break;
    String clientID = "BCground-";
    clientID += String(random(0xffff), HEX);    //Unique client ID each time

    if(client.connect(clientID.c_str())){   //Subscribe to required topics
      client.subscribe(TOPIC_TankResponse);
      client.subscribe(TOPIC_SysKill);
      client.subscribe(TOPIC_MotorChange);
    }
    else
      delay(300);  //Try again after a while
  }
}


void loop() {
  // put your main code here, to run repeatedly:
  manualEnable = digitalRead(ManualOverride);
  if(manualEnable) {
    client.publish(TOPIC_ManualOverride, ON);
    manual_state = digitalRead(ManualControl);
    
    if(manual_state) {
      motor_state = 1;
      digitalWrite(Motor, HIGH);
      client.publish(TOPIC_CurrentMotorState, ON);
    }
    else {
      motor_state = 0;
      digitalWrite(Motor, LOW);  
      client.publish(TOPIC_CurrentMotorState, OFF);
    }
  }
  else {
    if(WiFi.status() != WL_CONNECTED) 
      setupWiFi();

    if(!client.connected())   //Make sure MQTT is connected
      connectMQTT();

    client.publish(TOPIC_ManualOverride, OFF);
    client.loop();
  }
  delay(Seconds(1));
}
