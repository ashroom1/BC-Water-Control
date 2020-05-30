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
#define ONs1s2 "ONs1s2"
#define ONs1 "ONs1"
#define ONs2 "ONs2"
#define OFF "OFF"
#define STATUS "STATUS"
#define TANKDEAD "TANKDEAD"
#define ON_WITH_TIMER "ON_WITH_TIMER"

const char *ssid = "BCWifi";
const char *password = "Swamy";
const char *host_name = "hostname_goes_here";
const char *TOPIC_MotorChange = "MotorStatusChange";
const char *TOPIC_SensorMalfunction = "SensorMalfunction";
const char *TOPIC_PingGround = "PingGround";
const char *TOPIC_SysKill = "SysKill";
const char *TOPIC_PingTank = "PingTank";
const char *TOPIC_GroundAwake = "GroundAwake";
const char *TOPIC_GroundResponse = "GroundResponse";
const char *TOPIC_TankResponse = "TankResponse";
const char *TOPIC_ManualOverride = "ManualOverride";


bool motor_state;
unsigned long lastTankResponse = 4294967294;
Ticker ping_tank;
Ticker tank_response;
Ticker notify_active_state;
Ticker water_timer;
WiFiClient wclient;
PubSubClient client(wclient);

const float timer_pure_seconds = 0;
const float timer_s1s2 = 0;
const float timer_s1 = 0;
const float timer_s2 = 0;

/*
 * Add delay everywhere we turn off motor to avoid immediate turn on
 * Maybe send manualOverride message less frequently to avoid wasting time at both ends.
 */

void setupWiFi() {

  delay(10);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED)
    delay(1000);
}


void notifyActive() {
  client.publish(TOPIC_GroundAwake, ON);
}


void checkTankResponse() {

  unsigned long time_elapsed = abs(lastTankResponse - millis());
  if (time_elapsed > Seconds(30) && time_elapsed < Minutes(60) * 24 * 40 /* Making sure we don't run into trouble when millis overflows */) {
    client.publish(TOPIC_PingTank, TANKDEAD);
    ESP.deepSleep(0);
  }
}


void pingNow() {
  client.publish(TOPIC_PingTank, STATUS);
  tank_response.once(20, checkTankResponse);
}


void waterTimer() {

  if(motor_state) {
    digitalWrite(Motor, LOW);
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

  if(!strcmp(msgTopic, TOPIC_PingGround))
    if(!strcmp(message, STATUS))
      client.publish(TOPIC_GroundResponse, ON);

  if(!strcmp(msgTopic, TOPIC_SysKill))
    if(!strcmp(message, OFF)) {
      if(motor_state)
        {
          digitalWrite(Motor, LOW);
          motor_state = 0;
        }
      ESP.deepSleep(0);   //TODO: Also turn off motor
    }

   if(!strcmp(msgTopic, TOPIC_SensorMalfunction))
    if(!strcmp(message, ON))  {
      if(motor_state)
      {
        digitalWrite(Motor, LOW);
        motor_state = 0;
      }
      ESP.deepSleep(0);
    }


  if(!strcmp(msgTopic, TOPIC_MotorChange)) {

    if(!strcmp(message, OFF)) {
      if(motor_state) {
        digitalWrite(Motor, LOW);
        motor_state = 0;   
      }  
    }
    
    if(!strcmp(message, ON_WITH_TIMER)) {
      if(!motor_state) {
        motor_state = 1;
        digitalWrite(Motor, HIGH);
        water_timer.once(timer_pure_seconds, waterTimer);
      }
    }

    if(!strcmp(message, ONs1s2)) {
      if(!motor_state) {
        motor_state = 1;
        digitalWrite(Motor, HIGH);
        water_timer.once(timer_s1s2, waterTimer);
      }
    }

    if(!strcmp(message, ONs1)) {
      if(!motor_state) {
        motor_state = 1;
        digitalWrite(Motor, HIGH);
        water_timer.once(timer_s1, waterTimer);
      }
    }

    if(!strcmp(message, ONs2)) {
      if(!motor_state) {
        motor_state = 1;
        digitalWrite(Motor, HIGH);
        water_timer.once(timer_s2, waterTimer);
      }
    }
  }
}


void setup() {
  // put your setup code here, to run once:

  Serial.begin(115200);
  motor_state = 0;
  pinMode(Motor, OUTPUT);
  pinMode(ManualOverride, INPUT);
  pinMode(ManualControl, INPUT);
  
  digitalWrite(Motor, LOW);

  setupWiFi();

  client.setServer(host_name, 1883);
  client.setCallback(callback);

  ping_tank.attach(10 * 60, pingNow);
  notify_active_state.attach(30 * 60, notifyActive);
}


void connectMQTT() {

  while (!client.connected()) {
    String clientID = "BCground-";
    clientID += String(random(0xffff), HEX);    //Unique client ID each time

    if(client.connect(clientID.c_str())){   //Subscribe to required topics
      client.subscribe(TOPIC_TankResponse);
      client.subscribe(TOPIC_SysKill);
      client.subscribe(TOPIC_PingGround);
      client.subscribe(TOPIC_MotorChange);
    }
    else
      delay(Seconds(2.5));  //Try again after a while
  }
}


void loop() {
  // put your main code here, to run repeatedly:
  bool manualEnable = digitalRead(ManualOverride);
  bool manual_state;
  if(manualEnable) {
    client.publish(TOPIC_ManualOverride, ON);
    manual_state = digitalRead(ManualControl);
    if(client.connected()) {    //Disconnect MQTT during manual control
      client.disconnect();
    }
    
    if(manual_state) {
      digitalWrite(Motor, HIGH);
    }
    else {
      digitalWrite(Motor, LOW);  
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
  delay(motor_state ? Seconds(10) : Seconds(30));    //Different delay based on whether the motor is on or off
}
