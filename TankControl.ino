#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>   
#include <EEPROM.h>

#define SECONDS(s) s*1000
#define MINUTES(m) m*SECONDS(60) //Currently not used
#define OFF_MESSAGE_FREQ_MILLISEC SECONDS(5)
#define MAX_MSG_LENGTH 25

//  Pin numbers of the sensor
#define SENSOR1 D5
#define SENSOR2 D6
#define SENSOR3 D7
#define EEPROM_INIT_PIN D1

#define ON "ON"
#define ONs1s3 "ONs1s3"
#define ONs1 "ONs1"
#define ONs3 "ONs3"
#define OFF "OFF"
#define ON_WITH_TIMER "ONSolar"
#define STATUS "STATUS"
#define ALL "ALL"
#define TANK "TANK"
 

const char *ssid = "likith12345";
const char *password = "*druthi#";
const char *host_name = "192.168.0.100";
const char *TOPIC_MainTankMid = "Sensor/MainMid";
const char *TOPIC_MainTankOVF = "Sensor/MainOVF";
const char *TOPIC_SolarTankMid = "Sensor/SolarMid";
const char *TOPIC_MotorChange = "MotorStatusChange";
const char *TOPIC_SensorMalfunction = "SensorMalfunction";
const char *TOPIC_SysKill = "SysKill";
const char *TOPIC_PingTank = "PingTank";
const char *TOPIC_TankResponse = "TankResponse";
const char *TOPIC_GroundReset = "GroundReset"; //To be checked Motor ON/OFF Condition- Toggle
const char *TOPIC_SensorMalfunctionReset = "SensorMalfunctionReset";

const int timer_solar_seconds = 5; //Enter Solar tank Overflow Timer value in seconds

unsigned long lastOffMessage_millis;


/*
 * --Save sensor malfunction in EEPROM--
 * --Send on message repeatedly--
 * --Don't care-> Send on if on or off if off: Currently sending only OFF msg--
 * --Delay to be given for off message frequency.--
 * --Unique LED pattern for WiFi, MQTT etc.--
 * --//const char TOPIC_GroundReset = "GroundReset"; //To be checked Motor ON/OFF Condition- Toggle, Also check the callback--
 * ??Add more delays or yields??
 * ??Change sleep time for when motor is ON on a timer??
 * ??When tank reset detach all tickers??
 * --Should we add while(!EEPROM.commit())? delay()? -> instead read and write to a variable on status--
 * To check - MQTT persistence
 * Trigger solar tank timer when solar tank 0 to 1. (Take care of solar sensor malfunction)
 * Solar time elapsed but sensor still zero = error(Solar sensor malfunction).
 * Take feedback from broker about Sensor Malfunction after tank reset.
 * Motor control program TBD - Reset motor timer for every ON message.
 * State machine to be analysed and implemented to detect illegal state changes (Eg. Sensor[main mid, main overflow, solar] [000] to [100] not possible).
 * Indentation to be taken care by Ashwin
 * String issue to be addressed for Memory overflow & Board RESET
 * *********DONOT DELETE ANY CODE IN THE PROGRAM Without thorough CHECK*********
 */

bool blink_flag;   //Blink Flag interrupt
bool motor_state;   //Current state of the motor
bool solartimer_flag;      //True indicates that the motor is on a pure timer
bool sensor_malfunction;
bool s1 = 1, s2 = 1, s3 = 1, s1prev = 1, s2prev = 1, s3prev = 1;  //Sensor values
bool sensorStatusFlag;  //Flag for sending status of all sensors for the first time
bool onTimerFlag;

WiFiClient wclient;
PubSubClient client(wclient);

Ticker timer_to_reset;
Ticker BlinkLED;

void setupWiFi() {
 
  delay(10);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED){
    //Blink Fast for WiFi Status(Not Connected) indication
    digitalWrite(LED_BUILTIN, LOW);
    delay(250);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(250); 
    digitalWrite(LED_BUILTIN, LOW);
    delay(250);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(250); 
  }  
}

void blinkfun() {
  blink_flag = 1;
}

bool EEPROM_write(int value_to_be_written) {
  
  if(EEPROM.read(0) == value_to_be_written)
    return true;
  else {
    for(int i = 0; i < 5; i++) {
      EEPROM.write(0, value_to_be_written);
      if(EEPROM.commit()){
        EEPROM.end();
        return true;
      }
      EEPROM.end();
      delay(10);
    }
  }
  return false;
}

void setup() {
  
  blink_flag = 0;
  motor_state = 0;
  solartimer_flag = 0;
  sensor_malfunction = 0;
  sensorStatusFlag = 1;
  lastOffMessage_millis = 0;
  onTimerFlag = 0;
  
  pinMode(SENSOR1, INPUT);
  pinMode(SENSOR2, INPUT);
  pinMode(SENSOR3, INPUT);
  pinMode(EEPROM_INIT_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);// Initial state Of LED Active Low->specifying explicitly

  setupWiFi();
  EEPROM.begin(10);
 
  client.setServer(host_name, 1883);
  client.setCallback(callback);
  connectMQTT();

  if(digitalRead(EEPROM_INIT_PIN)) 
    EEPROM_write(0);
  
  if(EEPROM.read(0)) {
    client.publish(TOPIC_SensorMalfunction, (uint8_t*)ON, 2, true);
    sensor_malfunction = 1;
  }
  else {
    client.publish(TOPIC_SensorMalfunction, (uint8_t*)OFF, 3, true);
  }
 
  for(int i=0; i<=10; i++){
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);  
  }
 
  BlinkLED.attach(5, blinkfun);
 
}

void connectMQTT() {

  while (!client.connected()) {
    String clientID = "BCterrace-";
    clientID += String(random(0xffff), HEX);    //Unique client ID each time

    if(client.connect(clientID.c_str())){   //Subscribe to required topics
//       client.subscribe(TOPIC_GroundReset);
      client.subscribe(TOPIC_SysKill);
      client.subscribe(TOPIC_PingTank);
      client.subscribe(TOPIC_SensorMalfunctionReset);
    }
    else {
       //Blink Fast for WiFi Status(Not Connected) indication
      digitalWrite(LED_BUILTIN, LOW);
      delay(700);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(300); 
      digitalWrite(LED_BUILTIN, LOW);
      delay(700);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(300); 
    }
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
  }
 
  if(!strcmp(msgTopic, TOPIC_SensorMalfunctionReset))
    if(!strcmp(message, ON)) {
      sensor_malfunction = 0;
      EEPROM_write(0);
      client.publish(TOPIC_SensorMalfunction, (uint8_t*)OFF, 3, true);
    }  

  if(!strcmp(msgTopic, TOPIC_SysKill))
    if(!strcmp(message, TANK) || !strcmp(message, ALL))
      ESP.deepSleep(0);   //Disable system if asked to
 
/****Story start****
 * Once upon a time in Ground, Board got Reset, when motor was ON.
 * Hence a Message was sent to Tank to indicate Reset of Ground,
 * which was analysed by Tank to sort the problem faced during don't care condition.
 * Where ON message was not sent continuously (doing so would lead to other problems)
 * and Tank wouldn't send any messages (ON or OFF). This created a problem i.e., 
 * Ground would be in OFF state where as Tank would say motor is ON. To solve this
 * Turn OFF motor when Ground RESET is received.
 ****Story end****/
  if(!strcmp(msgTopic, TOPIC_GroundReset))
    if(!strcmp(message, ON))
      {
        motor_state = 0;
        client.publish(TOPIC_MotorChange, OFF);
        lastOffMessage_millis = millis();
      }
      
 
}

void resetVar() {
  solartimer_flag = 1;
}

void loop() {
  // put your main code here, to run repeatedly:
  if(blink_flag){
    digitalWrite(LED_BUILTIN, LOW);  //Active low
    delay(500);
    digitalWrite(LED_BUILTIN, HIGH);
    blink_flag = 0;
  }

  if(WiFi.status() != WL_CONNECTED)
    setupWiFi();

  if(!client.connected())   //Make sure MQTT is connected
    connectMQTT();
 
  if(solartimer_flag ) {
    motor_state = 0;
    solartimer_flag = 0;
    onTimerFlag = 0;
    client.publish(TOPIC_MotorChange, OFF);
  }

  s1prev = s1;
  s2prev = s2;
  s3prev = s3;

  s1 = digitalRead(SENSOR1);
  s2 = digitalRead(SENSOR2);
  s3 = digitalRead(SENSOR3);

  if(!sensor_malfunction) {     //If sensor malfunction, do nothing
   
    if(!s2 && (!s3 || !s1)) {
     
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
  
      EEPROM_write(1);
     
      client.publish(TOPIC_SensorMalfunction, (uint8_t*)ON, 2, true);
      motor_state = 0;
      client.publish(TOPIC_MotorChange, OFF);   //Safety
      
      sensor_malfunction = 1;
    }
    
    else if (s2 && !s3 && s1) {
      //Use timer to turn on
      client.publish(TOPIC_MotorChange, ON_WITH_TIMER);
      onTimerFlag = 1;
      if(!motor_state) {
        motor_state = 1;
        timer_to_reset.once(timer_solar_seconds, resetVar);
      }
    }
  
    else if (s1 && !s2 && s3) {
      //Don't care
      if ((!motor_state) && (millis() - lastOffMessage_millis >= OFF_MESSAGE_FREQ_MILLISEC)){
        client.publish(TOPIC_MotorChange, OFF);
        lastOffMessage_millis = millis();
      }
    } 
  
    else if(!onTimerFlag){  //if solar timer is On don't send off message
      if(!motor_state){
        if(millis() - lastOffMessage_millis >= OFF_MESSAGE_FREQ_MILLISEC){
          client.publish(TOPIC_MotorChange, OFF);
          lastOffMessage_millis = millis();
        }
      }
      else {
        client.publish(TOPIC_MotorChange, OFF);
        motor_state = 0;         
        lastOffMessage_millis = millis();
      }
    }
  
    //Report changes to sensor values
    if (sensorStatusFlag){   //
      client.publish(TOPIC_MainTankMid, (uint8_t*)(s1 ? ON : OFF), (s1 ? 2 : 3), true);
      client.publish(TOPIC_MainTankMid, (uint8_t*)(s2 ? ON : OFF), (s2 ? 2 : 3), true);
      client.publish(TOPIC_MainTankMid, (uint8_t*)(s3 ? ON : OFF), (s3 ? 2 : 3), true);
      sensorStatusFlag = 0;
    }
   
    else {
    s1 ^ s1prev ? client.publish(TOPIC_MainTankMid, (uint8_t*)(s1 ? ON : OFF), (s1 ? 2 : 3), true) : 0;
    s2 ^ s2prev ? client.publish(TOPIC_MainTankMid, (uint8_t*)(s2 ? ON : OFF), (s2 ? 2 : 3), true) : 0;
    s3 ^ s3prev ? client.publish(TOPIC_MainTankMid, (uint8_t*)(s3 ? ON : OFF), (s3 ? 2 : 3), true) : 0;
    }   
  }
  
  client.loop();
  delay(SECONDS(0.5));
}
