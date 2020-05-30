#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>

#define Seconds(s) s*1000
#define Minutes(m) m*Seconds(60)
#define MAX_MSG_LENGTH 120

//  Pin numbers of the sensor
#define Sensor1 D1
#define Sensor2 D2
#define Sensor3 D3

#define ON "ON"
#define ONs1s2 "ONs1s2"
#define ONs1 "ONs1"
#define ONs2 "ONs2"
#define OFF "OFF"
#define ON_WITH_TIMER "ON_WITH_TIMER"
#define STATUS "STATUS"
#define GROUNDDEAD "GROUNDDEAD"
 

const char *ssid = "BCWifi";
const char *password = "Swamy";
const char *host_name = "hostname_goes_here";
const char *TOPIC_MainTankMid = "Sensor/MainMid";
const char *TOPIC_MainTankOVF = "Sensor/MainOVF";
const char *TOPIC_SolarTankMid = "Sensor/SolarMid";
const char *TOPIC_MotorChange = "MotorStatusChange";
const char *TOPIC_SensorMalfunction = "SensorMalfunction";
const char *TOPIC_PingGround = "PingGround";
const char *TOPIC_SysKill = "SysKill";
const char *TOPIC_PingTank = "PingTank";
const char *TOPIC_TankAwake = "TankAwake";
const char *TOPIC_GroundResponse = "GroundResponse";
const char *TOPIC_TankResponse = "TankResponse";

const float timer_pure_seconds = 0;


/*
 * May need to implement acknowedgement for each TOPIC especially MotorChange
 * Add more delays or yields
 * Change sleep time for when motor is ON on a timer
 * Add feedback if WiFi disconnects and reconnects
 */


bool motor_state;   //Current state of the motor
bool s1 = 1, s2 = 1, s3 = 1, s1prev = 1, s2prev = 1, s3prev = 1;  //Sensor values
unsigned long lastGroundResponse = 4294967294; //Store last response received from ground
Ticker ping_ground;
Ticker ground_response;
Ticker notify_active_state;
WiFiClient wclient;
PubSubClient client(wclient);


void setupWiFi() {

  delay(10);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED)
    delay(1000);
}

void notifyActive() {
  client.publish(TOPIC_TankAwake, ON);
}

void setup() {
  // put your setup code here, to run once:

  Serial.begin(115200);

  motor_state = 0;
  
  pinMode(Sensor1, INPUT);
  pinMode(Sensor2, INPUT);
  pinMode(Sensor3, INPUT);

  setupWiFi();

  client.setServer(host_name, 1883);
  client.setCallback(callback);

  ping_ground.attach(10 * 60, pingNow);
  notify_active_state.attach(30 * 60, notifyActive);
}

void checkGroundResponse() {

  unsigned long time_elapsed = abs(lastGroundResponse - millis());
  if (time_elapsed > Seconds(30) && time_elapsed < Minutes(60) * 24 * 40 /* Making sure we don't run into trouble when millis overflows */) {
    client.publish(TOPIC_PingGround, GROUNDDEAD);
    ESP.deepSleep(0);
  }
}


void pingNow() {
  client.publish(TOPIC_PingGround, STATUS);
  ground_response.once(20, checkGroundResponse);
}


void connectMQTT() {

  while (!client.connected()) {
    String clientID = "BCterrace-";
    clientID += String(random(0xffff), HEX);    //Unique client ID each time

    if(client.connect(clientID.c_str())){   //Subscribe to required topics
      client.subscribe(TOPIC_GroundResponse);
      client.subscribe(TOPIC_SysKill);
      client.subscribe(TOPIC_PingTank);
    }
    else
      delay(Seconds(2.5));  //Try again after a while
  }
}

void callback(char *msgTopic, byte *msgPayload, unsigned int msgLength) {

  static char message[MAX_MSG_LENGTH + 1];

  memcpy(message, (char *) msgPayload, msgLength);
  message[msgLength] = '\0';

  if(!strcmp(msgTopic, TOPIC_GroundResponse))
    if(!strcmp(message, ON))
      lastGroundResponse = millis();

  if(!strcmp(msgTopic, TOPIC_PingTank))
    if(!strcmp(message, STATUS))
      client.publish(TOPIC_TankResponse, ON);

  if(!strcmp(msgTopic, TOPIC_SysKill))
    if(!strcmp(message, OFF))
      ESP.deepSleep(0);   //Disable system if asked to   
}

void loop() {
  // put your main code here, to run repeatedly:

  if(WiFi.status() != WL_CONNECTED) 
    setupWiFi();

  if(!client.connected())   //Make sure MQTT is connected
    connectMQTT();

  s1prev = s1;
  s2prev = s2;
  s3prev = s3;

  s1 = digitalRead(Sensor1);
  s2 = digitalRead(Sensor2);
  s3 = digitalRead(Sensor3);

  if((!s3 && !s2) || (!s3 && !s1)) {
    //Send command to turn on motor

    if(!motor_state) 

      if(s1 && s2)
        client.publish(TOPIC_MotorChange, ONs1s2);
      else if(s1)
        client.publish(TOPIC_MotorChange, ONs1);
      else if(s2)
        client.publish(TOPIC_MotorChange, ONs2);

      motor_state = 1;
    }
  }
  
  else if(s2 && !s1) {
    //Sensor malfunction

    client.publish(TOPIC_SensorMalfunction, ON);
    motor_state = 0;
    client.publish(TOPIC_MotorChange, OFF);   //Safety
    
  }
  
  else if (s2 && !s3 && s1) {

    //Use timer to turn on
    if(!motor_state) {
      client.publish(TOPIC_MotorChange, ON_WITH_TIMER);
      motor_state = 1;
      delay(Seconds(timer_pure_seconds));    //Sleep for the time motor is ON.
      motor_state = 0;
    }
  }

  else if (motor_state) {
    client.publish(TOPIC_MotorChange, OFF);
    motor_state = 0;  
  }
  
  client.loop();

  //Report changes to sensor values
  s1 ^ s1prev ? client.publish(TOPIC_MainTankMid, s1 ? ON : OFF) : 0;
  s2 ^ s2prev ? client.publish(TOPIC_MainTankOVF, s2 ? ON : OFF) : 0;
  s3 ^ s3prev ? client.publish(TOPIC_SolarTankMid, s3 ? ON : OFF) : 0;

  delay(motor_state ? Seconds(10) : Minutes(20));    //Different delay based on whether the motor is on or off

}
