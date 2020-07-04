#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <EEPROM.h>

#define Seconds(s) s*1000
#define Minutes(m) m*Seconds(60)
#define MAX_MSG_LENGTH 120

//  Pin numbers of the sensor
#define Sensor1 D1
#define Sensor2 D2
#define Sensor3 D3

#define ON "ON"
#define ONs1s3 "ONs1s3"
#define ONs1 "ONs1"
#define ONs3 "ONs3"
#define OFF "OFF"
#define ON_WITH_TIMER "ONSolar"
#define STATUS "STATUS"
#define ALL "ALL"
#define TANK "TANK"
 

const char *ssid = "BCWifi";
const char *password = "Swamy";
const char *host_name = "hostname_goes_here";
const char *TOPIC_MainTankMid = "Sensor/MainMid";
const char *TOPIC_MainTankOVF = "Sensor/MainOVF";
const char *TOPIC_SolarTankMid = "Sensor/SolarMid";
const char *TOPIC_MotorChange = "MotorStatusChange";
const char *TOPIC_SensorMalfunction = "SensorMalfunction";
const char *TOPIC_SysKill = "SysKill";
const char *TOPIC_PingTank = "PingTank";
const char *TOPIC_TankResponse = "TankResponse";
const char *TOPIC_GroundReset = "GroundReset";
const char *TOPIC_SensorMalfunctionReset = "SensorMalfunctionReset";

const float timer_solar_seconds = 0;


/*
 * --Save sensor malfunction in EEPROM--
 * Add more delays or yields
 * Trigger solar tank timer when solar tank 0 to 1.
 * Change sleep time for when motor is ON on a timer
 * --Send on message repeatedly--
 * To check - persistence
 * When tank reset detach all tickers
 */


bool motor_state;   //Current state of the motor
bool on_timer;      //True indicates that the motor is on a pure timer
bool sensor_malfunction;
bool s1 = 1, s2 = 1, s3 = 1, s1prev = 1, s2prev = 1, s3prev = 1;  //Sensor values
WiFiClient wclient;
PubSubClient client(wclient);

Ticker timer_to_reset;
Ticker BlinkLED;

void setupWiFi() {

  delay(10);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED)
    delay(Seconds(1));
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
  on_timer = 0;
  sensor_malfunction = 0;
  
  pinMode(Sensor1, INPUT);
  pinMode(Sensor2, INPUT);
  pinMode(Sensor3, INPUT);

  setupWiFi();

  client.setServer(host_name, 1883);
  client.setCallback(callback);
  connectMQTT();

  if(EEPROM.read(0)) {
    client.publish(TOPIC_SensorMalfunction, ON);
    sensor_malfunction = 1;
  }
  
  BlinkLED.attach(10, blinkfun);
}

void connectMQTT() {

  while (!client.connected()) {
    String clientID = "BCterrace-";
    clientID += String(random(0xffff), HEX);    //Unique client ID each time

    if(client.connect(clientID.c_str())){   //Subscribe to required topics
      client.subscribe(TOPIC_GroundReset);
      client.subscribe(TOPIC_SysKill);
      client.subscribe(TOPIC_PingTank);
      client.subscribe(TOPIC_SensorMalfunctionReset);
    }
    else
      delay(Seconds(2.5));  //Try again after a while
  }
}

void callback(char *msgTopic, byte *msgPayload, unsigned int msgLength) {

  static char message[MAX_MSG_LENGTH + 1];

  memcpy(message, (char *) msgPayload, msgLength);
  message[msgLength] = '\0';

  if(!sensor_malfunction) {
    if(!strcmp(msgTopic, TOPIC_PingTank))
      if(!strcmp(message, STATUS))
        client.publish(TOPIC_TankResponse, ON);

    if(!strcmp(msgTopic, TOPIC_SensorMalfunctionReset))
      if(!strcmp(message, ON)) {
        sensor_malfunction = 0;
        EEPROM.write(0, 0);
      }
        
  }

  if(!strcmp(msgTopic, TOPIC_SysKill))
    if(!strcmp(message, TANK) || !strcmp(message, ALL))
      ESP.deepSleep(0);   //Disable system if asked to

  if(!strcmp(msgTopic, TOPIC_GroundReset))
    if(!strcmp(message, ON))
      {
        motor_state = 0;
        client.publish(TOPIC_MotorChange, OFF);
      }
}

void resetVar() {
  on_timer = 0;
  client.publish(TOPIC_MotorChange, OFF);
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

  if(!sensor_malfunction) {     //If sensor malfunction, do nothing

    if((!s2 && !s3) || (!s2 && !s1)) {
      //Send command to turn on motor
  
      //if(!motor_state) {    //Comment if ON message is to be sent multiple times
  
        if(!s1 && !s3)
          client.publish(TOPIC_MotorChange, ONs1s3);
        else if(!s1)
          client.publish(TOPIC_MotorChange, ONs1);
        else if(!s3)
          client.publish(TOPIC_MotorChange, ONs3);
  
        motor_state = 1;
      //}
    }
    
    else if(s2 && !s1) {
      //Sensor malfunction
  
      client.publish(TOPIC_SensorMalfunction, ON);
      motor_state = 0;
      client.publish(TOPIC_MotorChange, OFF);   //Safety
  
      EEPROM.write(0, 1);
      
      sensor_malfunction = 1;
      //ESP.deepSleep(0);
      
    }
    
    else if (s2 && !s3 && s1) {
  
      //Use timer to turn on
      //if(!motor_state) {
      client.publish(TOPIC_MotorChange, ON_WITH_TIMER);
      if(!motor_state) {
        motor_state = 1;
        timer_to_reset.once(timer_solar_seconds, resetVar);
      }
      //}
    }
  
    else if (s1 && !s2 && s3) {
      //Don't care
      //Send on if on or off if off
    } 
  
    else {
      client.publish(TOPIC_MotorChange, OFF);
      motor_state = 0;  
    }
  
    //Report changes to sensor values
    s1 ^ s1prev ? client.publish(TOPIC_MainTankMid, s1 ? ON : OFF) : 0;
    s2 ^ s2prev ? client.publish(TOPIC_MainTankOVF, s2 ? ON : OFF) : 0;
    s3 ^ s3prev ? client.publish(TOPIC_SolarTankMid, s3 ? ON : OFF) : 0;

  }
  
  client.loop();

  delay(Seconds(2));

}
