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
#define ACK "ACK"
#define TANK "TANK"


const char *ssid = "likith12345";
const char *password = "*druthi#";
const char *host_name = "192.168.0.106";
const char *TOPIC_MainTankMid = "Sensor/MainMid";
const char *TOPIC_MainTankOVF = "Sensor/MainOVF";
const char *TOPIC_SolarTankMid = "Sensor/SolarMid";
const char *TOPIC_MotorChange = "MotorStatusChange";
const char *TOPIC_SensorMalfunction = "SensorMalfunction";
const char *TOPIC_MotorTimeoutWarning = "MotorTimeoutWarning";
const char *TOPIC_SysKill = "SysKill";
const char *TOPIC_PingTank = "PingTank";
const char *TOPIC_TankResponse = "TankResponse";
const char *TOPIC_GroundResetAndAcknowledge = "GroundResetAndAcknowledge"; //To be checked Motor ON/OFF Condition- Toggle
const char *TOPIC_SensorMalfunctionReset = "SensorMalfunctionReset";

const int timer_solar_seconds = 5; //Enter Solar tank Overflow Timer value in seconds
// The value here should be atleast 30s more than actual solar timer otherwise motor might trigger sensor malfunction.

unsigned long lastOffMessage_millis;


/*
 * --Save sensor malfunction in EEPROM--
 * --Send on message repeatedly--
 * --Don't care-> Send on if on or off if off: Currently sending only OFF msg--
 * --Delay to be given for off message frequency.--
 * --Unique LED pattern for WiFi, MQTT etc.--
 * --//const char TOPIC_GroundResetAndAcknowledge = "GroundResetAndAcknowledge"; //To be checked Motor ON/OFF Condition- Toggle, Also check the callback--
 * --When motor is turned off by timer, tank should stop sending ON messages and even if it sends them motor should ignore it. Possible solution is to introduce a similar flag to sensor_malfunction in motor which will help ignore all tank messages until the variable is reset. This is to prevent overflow.--
 * --Should we add while(!EEPROM.commit())? delay()? -> instead read and write to a variable on status--
 * ??Add more delays or yields??
 * ??String issue to be addressed for Memory overflow & Board RESET??
 * ??When tank reset detach all tickers??
 * ??Take feedback from broker about Sensor Malfunction after tank reset.??
 *(2) --Solar time elapsed but sensor still zero = error(Solar sensor malfunction).--
 *(1) To check - MQTT persistence.
 *(1) Trigger solar tank timer when solar tank 0 to 1. (Take care of solar sensor malfunction)
 *(1) Related to above - Motor control program TBD - Reset motor timer for every ON message.
 *(1) Failsafe timer should be 1.3 times the actual sensor positions(time).
 *(3) State machine to be analysed and implemented to detect illegal state changes (Eg. Sensor[main mid, main overflow, solar] [000] to [100] not possible).
 *(3) Indentation to be taken care by Ashwin.
 * *********DONOT DELETE ANY CODE IN THE PROGRAM Without thorough CHECK*********
 */

bool blink_flag;   //Blink Flag interrupt
bool motor_state;   //Current state of the motor
bool solartimer_flag;      //True indicates that the motor is on a pure timer
bool sensor_malfunction;
bool s1 = 1, s2 = 1, s3 = 1, s1prev = 1, s2prev = 1, s3prev = 1;  //Sensor values
bool sensorStatusFlag;  //Flag for sending status of all sensors for the first time
bool onTimerFlag;
bool EEPROM_Write_Flag;
bool EEPROM_Value_To_Write;

WiFiClient wclient;
PubSubClient client(wclient);

Ticker timer_to_reset;
Ticker BlinkLED;

void setupWiFi() {

    digitalWrite(LED_BUILTIN, LOW);     //LED always ON in this function

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while(WiFi.status() != WL_CONNECTED)
        delay(200);

    digitalWrite(LED_BUILTIN, HIGH);
    delay(10);
}

void connectMQTT() {
    if(WiFi.status() != WL_CONNECTED)
        setupWiFi();

    while (!client.connected()) {
        String clientID = "BCterrace-";
        clientID += String(random(0xffff), HEX);    //Unique client ID each time

        if(client.connect(clientID.c_str())){   //Subscribe to required topics
            client.subscribe(TOPIC_GroundResetAndAcknowledge);
            client.subscribe(TOPIC_SysKill);
            client.subscribe(TOPIC_PingTank);
            client.subscribe(TOPIC_SensorMalfunctionReset);
            client.subscribe(TOPIC_SensorMalfunction);
        }
        else {
            //Blink Fast for MQTT Status(Not Connected) indication
            digitalWrite(LED_BUILTIN, LOW);
            delay(100);
            digitalWrite(LED_BUILTIN, HIGH);
            delay(100);
            digitalWrite(LED_BUILTIN, LOW);
            delay(100);
            digitalWrite(LED_BUILTIN, HIGH);
            delay(80);
        }
    }
}

bool check_and_publish(const char *Topic, const char *Message, bool Persistance) {/*Return type added because ternary op was expecting to return value*/

    if((WiFi.status() == WL_CONNECTED) && (client.connected()))
        Persistance ? client.publish(Topic, (uint8_t*)Message, strlen(Message), true) : client.publish(Topic, Message);
    return 0;//See block comment above -bool, '0' is a dummy Value.
}

void callback(char *msgTopic, byte *msgPayload, unsigned int msgLength) {

    static char message[MAX_MSG_LENGTH + 1];

    memcpy(message, (char *) msgPayload, msgLength);
    message[msgLength] = '\0';


    if(!sensor_malfunction) {
        if(!strcmp(msgTopic, TOPIC_PingTank))
            if(!strcmp(message, STATUS))
                check_and_publish(TOPIC_TankResponse, ON, 0);
    }


    if(!strcmp(msgTopic, TOPIC_SensorMalfunction)) {
        if(!strcmp(msgTopic, ON)) {
            EEPROM_Value_To_Write = 1;
            EEPROM_Write_Flag = !EEPROM_write(EEPROM_Value_To_Write);

            //check_and_publish(TOPIC_SensorMalfunction, ON, 1);
            motor_state = 0;
            check_and_publish(TOPIC_MotorChange, OFF, 0);   //Safety

            sensor_malfunction = 1;
        }

    }

    if(!strcmp(msgTopic, TOPIC_SensorMalfunctionReset))
        if(!strcmp(message, ON)) {
            sensor_malfunction = 0;
            EEPROM_Value_To_Write = 0;
            EEPROM_Write_Flag = !EEPROM_write(EEPROM_Value_To_Write);
            check_and_publish(TOPIC_SensorMalfunction, OFF, 1);
            check_and_publish(TOPIC_MotorTimeoutWarning, OFF, 1);
        }

    if(!strcmp(msgTopic, TOPIC_SysKill))
        if(!strcmp(message, TANK) || !strcmp(message, ALL))
            ESP.deepSleep(0);   //Disable system if asked to

/****Story start****
 * Once upon a time in Ground, Board got Reset, when motor was ON.
 * Hence a Message was sent to Tank to indicate Reset of Ground,
 * which was analysed by Tank to sort the problem faced during don't care condition (Also overflow sensor failure).
 * Where ON message was not sent continuously (doing so would lead to other problems)
 * and Tank wouldn't send any messages (ON or OFF). This created a problem i.e.,
 * Ground would be in OFF state where as Tank would say motor is ON. To solve this
 * Turn OFF motor when Ground RESET is received.
 ****Story end****/
    if(!strcmp(msgTopic, TOPIC_GroundResetAndAcknowledge))
        if(!strcmp(message, ON))
        {
            motor_state = 0;
            solartimer_flag = 0;
            onTimerFlag = 0;
            check_and_publish(TOPIC_MotorChange, OFF, 0);
            lastOffMessage_millis = millis();
            timer_to_reset.detach();      //If motor was ON due to solar timer, we need to stop the ticker because the motor is now OFF.
            check_and_publish(TOPIC_GroundResetAndAcknowledge, ACK, 1);
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

void resetVar() {
    solartimer_flag = 1;
}

void setup() {

    blink_flag = 0;
    motor_state = 0;
    solartimer_flag = 0;
    sensor_malfunction = 0;
    sensorStatusFlag = 1;
    lastOffMessage_millis = 0;
    onTimerFlag = 0;
    EEPROM_Write_Flag = 0;

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

    if(digitalRead(EEPROM_INIT_PIN)) {
        EEPROM_Value_To_Write = 0;
        EEPROM_Write_Flag = !EEPROM_write(EEPROM_Value_To_Write);
    }

    if(EEPROM.read(0)) {
        check_and_publish(TOPIC_SensorMalfunction, ON, 1);
        sensor_malfunction = 1;
    }
    else {
        check_and_publish(TOPIC_SensorMalfunction, OFF, 1);
    }

    for(int i=0; i<=10; i++){
        digitalWrite(LED_BUILTIN, LOW);
        delay(500);
        digitalWrite(LED_BUILTIN, HIGH);
        delay(500);
    }

    BlinkLED.attach(5, blinkfun);

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

    if(EEPROM_Write_Flag)
        EEPROM_Write_Flag = !EEPROM_write(EEPROM_Value_To_Write);

    if(solartimer_flag) {
        motor_state = 0;
        solartimer_flag = 0;
        onTimerFlag = 0;
        check_and_publish(TOPIC_MotorChange, OFF, 0);
        lastOffMessage_millis = millis();
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
                check_and_publish(TOPIC_MotorChange, ONs1s3, 0);
            else if(!s1)
                check_and_publish(TOPIC_MotorChange, ONs1, 0);
            else if(!s3)
                check_and_publish(TOPIC_MotorChange, ONs3, 0);

            motor_state = 1;
            //}
        }

        else if(s2 && !s1) {
            //Sensor malfunction

            EEPROM_Value_To_Write = 1;
            EEPROM_Write_Flag = !EEPROM_write(EEPROM_Value_To_Write);

            check_and_publish(TOPIC_SensorMalfunction, ON, 1);
            motor_state = 0;
            check_and_publish(TOPIC_MotorChange, OFF, 0);   //Safety

            sensor_malfunction = 1;
        }

        else if (s2 && !s3 && s1) {
            //Use timer to turn on
            check_and_publish(TOPIC_MotorChange, ON_WITH_TIMER, 0);
            onTimerFlag = 1;
            // if(!motor_state) { 
                motor_state = 1;
                timer_to_reset.detach(); 
                timer_to_reset.once(timer_solar_seconds, resetVar);
            // } 
        }

        else if (s1 && !s2 && s3) {
            //Don't care
            if ((!motor_state) && (millis() - lastOffMessage_millis >= OFF_MESSAGE_FREQ_MILLISEC)){
                check_and_publish(TOPIC_MotorChange, OFF, 0);
                lastOffMessage_millis = millis();
            }
        }

        else if(!onTimerFlag){  //if solar timer is On don't send off message
            if(!motor_state){
                if(millis() - lastOffMessage_millis >= OFF_MESSAGE_FREQ_MILLISEC){
                    check_and_publish(TOPIC_MotorChange, OFF, 0);
                    lastOffMessage_millis = millis();
                }
            }
            else {
                check_and_publish(TOPIC_MotorChange, OFF, 0);
                motor_state = 0;
                lastOffMessage_millis = millis();
            }
        }

        //Report changes to sensor values
        if (sensorStatusFlag){   //
            check_and_publish(TOPIC_MainTankMid, (s1 ? ON : OFF), 1);
            check_and_publish(TOPIC_MainTankMid, (s2 ? ON : OFF), 1);
            check_and_publish(TOPIC_MainTankMid, (s3 ? ON : OFF), 1);
            sensorStatusFlag = 0;
        }

        else {
            s1 ^ s1prev ? check_and_publish(TOPIC_MainTankMid, (s1 ? ON : OFF), 1) : 0;
            s2 ^ s2prev ? check_and_publish(TOPIC_MainTankMid, (s2 ? ON : OFF), 1) : 0;
            s3 ^ s3prev ? check_and_publish(TOPIC_MainTankMid, (s3 ? ON : OFF), 1) : 0;
        }
    }

    client.loop();
    delay(SECONDS(0.5));
}
