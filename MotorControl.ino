#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>

#define Seconds(s) s*1000
#define Minutes(m) m*Seconds(60)
#define MAX_MSG_LENGTH 25

#define Motor D5
#define ManualOverride D6
#define ManualControl D7

#define ON "ON"
#define ONs1s3 "ONs1s3"
#define ONs1 "ONs1"
#define ONs3 "ONs3"
#define OFF "OFF"
#define STATUS "STATUS"
#define ON_WITH_TIMER "ONSolar"
#define ALL "ALL"
#define MOTOR "MOTOR"

const char *ssid = "likith12345";
const char *password = "*druthi#";
const char *host_name = "192.168.0.100";
const char *TOPIC_MotorChange = "MotorStatusChange";
const char *TOPIC_PingGround = "PingGround";  //For broker to know if Motor board is working.
const char *TOPIC_GroundResponse = "GroundResponse";    //For broker to know if Motor board is working.
const char *TOPIC_SysKill = "SysKill";
const char *TOPIC_PingTank = "PingTank";        //Periodic message
const char *TOPIC_TankResponse = "TankResponse";
const char *TOPIC_ManualOverride = "ManualOverride";        //Periodic message
//const char *TOPIC_MotorReset = "MotorReset";      //Not used
const char *TOPIC_CurrentMotorState = "CurrentMotorState";      //Periodic message
const char *TOPIC_MotorTimeoutWarning = "MotorTimeoutWarning";      //To be handled in broker.
const char *TOPIC_GroundReset = "GroundReset";    //To know if motor control board is resetting often.

bool blink_flag;   //Blink Flag interrupt
bool tankresponsefun_flag;    //Tank ping response interrupt
bool pingNow_flag;  //Tank ping interrupt
bool waterTimer_flag;   //Water Timer interrupt
bool pureTimer_flag;    //True when solar timer enabled
bool motor_state;
bool manualEnable;
bool manual_state;
bool tank_responsive;
int no_response_count = 0;

unsigned long lastTankResponse = 4294967294;
unsigned long pingTime;
Ticker ping_tank;
Ticker tank_response;   //Probably can be removed
Ticker BlinkLED;
Ticker water_timer;
WiFiClient wclient;
PubSubClient client(wclient);

const float timer_pure_seconds = 5;
const float timer_s1s3 = 10;
const float timer_s1 = 15;
const float timer_s3 = 20;

/*
 *(1) Manual Overide-> "break;" in setupWiFi & connectMQTT to be addressed.
 *(1) Maintain backlog messages to be sent if WiFi or MQTT does not connect.
 *(1) Check manual override in ConnectMQTT and connectWifi and take action immediately on motor and also send "TOPIC_CurrentMotorState" and "TOPIC_ManualOverride" messages constantly in the loop.
 *(1) Introduce flags for Wifi and MQTT connected. If Wifi, don't attempt MQTT connection. If MQTT not connected don't attempt MQTT communication. Infinite while loops of MQTT and Wifi is a bad idea.
 *(1) Add error message when motor is Turned OFF via Timer
 *(1) Frequent ON-OFF/OFF-ON messages from tank control leads to error message and motor is turned OFF. ON-OFF transition cannot happen within a minimum specified timer(Ticker). Warning and error to be sent if such multiple transitions happen.
 *(2) Delay("Or thought to be given") to be added during Manual Override ON to OFF(turned to Auto) Transition, To avoid toggle of Motor from Off(due to Manual Override) to On(due to auto)
 *(2) Add delay everywhere we turn off motor to avoid immediate turn on
 *(3) Maybe send manualOverride message less frequently to avoid wasting time at both ends.
 *(3) Board reset counter to be maintained in EEPROM and sent if requested through MQTT(new topic maybe required).
 *(4) Ticker tank_response;   //Probably can be removed
 *(4) Check Ticker overlap/clash, Ping Tank & Blink LED: Test in Ticker Only Program (Priority?)
 *(4) Implement function to turn ON/OFF motor instead of repeating code.
 *(4) ??const char *TOPIC_PingGround = "PingGround";  //To be checked Redundant -> Can be used by broker to make sure motor is active.??
 *(4) Remove String class - Can lead to board reset!!
*/

void setupWiFi() {

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    bool manual = digitalRead(ManualOverride);
    for(int i = 0; (i < (manual ? 3 : 10)) && (WiFi.status() != WL_CONNECTED); i++)
        delay(200);

}

void tankresponsefun() {
    tankresponsefun_flag = 1;
}

void pingNow() {
    pingNow_flag = 1;
}

void waterTimer() {
    waterTimer_flag = 1;
}

void callback(char *msgTopic, byte *msgPayload, unsigned int msgLength) {

    static char message[MAX_MSG_LENGTH + 1];

    memcpy(message, (char *) msgPayload, msgLength);
    message[msgLength] = '\0';

    if(!strcmp(msgTopic, TOPIC_TankResponse))
        if(!strcmp(message, ON))
            lastTankResponse = millis();

    if(!strcmp(msgTopic, TOPIC_PingGround))
        if(!strcmp(message, ON))
            check_and_publish(TOPIC_GroundResponse, ON, 0);

    if(!strcmp(msgTopic, TOPIC_SysKill))
        if(!strcmp(message, MOTOR) | !strcmp(message, ALL)) {
            if(motor_state)
            {
                motor_state = 0;
                digitalWrite(Motor, LOW);
                check_and_publish(TOPIC_CurrentMotorState, OFF, 0);
            }
            ESP.deepSleep(0);
        }

    if(!strcmp(msgTopic, TOPIC_MotorChange) && tank_responsive) {

        if(!strcmp(message, OFF)) {
            if(motor_state) {
                motor_state = 0;
                digitalWrite(Motor, LOW);
                check_and_publish(TOPIC_CurrentMotorState, OFF, 0);
                water_timer.detach(); //Turn off Fail-safe Timer
            }
        }

        if(!strcmp(message, ON_WITH_TIMER)) {
            if(!motor_state) {
                motor_state = 1;
                pureTimer_flag = 1;
                digitalWrite(Motor, HIGH);
                check_and_publish(TOPIC_CurrentMotorState, ON, 0);
                water_timer.once(timer_pure_seconds, waterTimer);
            }
        }

        if(!strcmp(message, ONs1s3)) {
            if(!motor_state) {
                motor_state = 1;
                digitalWrite(Motor, HIGH);
                check_and_publish(TOPIC_CurrentMotorState, ON, 0);
                water_timer.once(timer_s1s3, waterTimer);
            }
        }

        if(!strcmp(message, ONs1)) {
            if(!motor_state) {
                motor_state = 1;
                digitalWrite(Motor, HIGH);
                check_and_publish(TOPIC_CurrentMotorState, ON, 0);
                water_timer.once(timer_s1, waterTimer);
            }
        }

        if(!strcmp(message, ONs3)) {
            if(!motor_state) {
                motor_state = 1;
                digitalWrite(Motor, HIGH);
                check_and_publish(TOPIC_CurrentMotorState, ON, 0);
                water_timer.once(timer_s3, waterTimer);
            }
        }
    }
}

void blinkfun() {
    blink_flag = 1;
}

void setup() {
    // put your setup code here, to run once:

    // Serial.begin(115200);
    motor_state = 0;
    tank_responsive = 1;
    blink_flag = 0;
    tankresponsefun_flag = 0;
    pingNow_flag = 0;
    waterTimer_flag = 0;
    pureTimer_flag = 0;


    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(Motor, OUTPUT);
    pinMode(ManualOverride, INPUT);
    pinMode(ManualControl, INPUT);
    digitalWrite(LED_BUILTIN, LOW);     // Initial state Of LED Active Low->specifying explicitly
    delay(1000);

    setupWiFi();

    client.setServer(host_name, 1883);
    client.setCallback(callback);

    if (WiFi.status() == WL_CONNECTED)
        connectMQTT();

    check_and_publish(TOPIC_GroundReset, ON, 0);

    for(int i = 0; i <= 10; i++){
        digitalWrite(LED_BUILTIN, LOW);
        delay(500);
        digitalWrite(LED_BUILTIN, HIGH);
        delay(500);
    }

    bool manual = digitalRead(ManualOverride);
    if(!manual){
        motor_state = 0;
        digitalWrite(Motor, LOW);
        check_and_publish(TOPIC_CurrentMotorState, OFF, 0);
    }
    ping_tank.attach(10, pingNow);
    BlinkLED.attach(5, blinkfun);
}

void check_and_publish(char *Topic, char *Message, bool Persistance) {

    if((WiFi.status() == WL_CONNECTED) && (client.connected()))
        Persistance ? client.publish(Topic, Message, strlen(Message), true) : client.publish(Topic, Message);

}

void connectMQTT() {

    bool manual = digitalRead(ManualOverride);
    for(int i = 0; (i < (manual ? 1 : 5)) && !client.connected(); i++) {

        String clientID = "BCground-";
        clientID += String(random(0xffff), HEX);    //Unique client ID each time

        if(client.connect(clientID.c_str())){   //Subscribe to required topics
            client.subscribe(TOPIC_TankResponse);
            client.subscribe(TOPIC_SysKill);
            client.subscribe(TOPIC_MotorChange);
            client.subscribe(TOPIC_PingGround);
        }
        else {
            digitalWrite(LED_BUILTIN, LOW);
            delay(100);
            digitalWrite(LED_BUILTIN, HIGH);
            delay(100);
            digitalWrite(LED_BUILTIN, LOW);
            delay(100);
            digitalWrite(LED_BUILTIN, HIGH);
            delay(100);
        }
    }
}


void loop() {
    // put your main code here, to run repeatedly:

    if(blink_flag) {
        digitalWrite(LED_BUILTIN, LOW);  //Active low
        delay(500);
        digitalWrite(LED_BUILTIN, HIGH);
        blink_flag = 0;
    }

    if(WiFi.status() != WL_CONNECTED) {
        setupWiFi();
    }

    if(WiFi.status() != WL_CONNECTED) {        //Redundant "if" statement needed. Because we need to turn ON the LED only if Wifi is not connected.
        digitalWrite(LED_BUILTIN, LOW);
        delay(10);
    }

    if(!client.connected() && (WiFi.status() == WL_CONNECTED))   //Make sure MQTT is connected
        connectMQTT();


    if(tankresponsefun_flag) {

        unsigned long elapsed = lastTankResponse - pingTime;
        if(abs(elapsed) < Minutes(48 * 60 /* 2 days */)) {       //Handle millis overflow
            if(elapsed <= Seconds(14)) {
                no_response_count = 0;
                tank_responsive = 1;
            }
            else {
                if(no_response_count < 10)
                    ++no_response_count;
            }
        }
        tankresponsefun_flag = 0;
    }

    if(pingNow_flag) {
        check_and_publish(TOPIC_PingTank, STATUS, 0);
        pingTime = millis();
        tank_response.once(14, tankresponsefun);
        pingNow_flag = 0;
    }

    if(waterTimer_flag){
        if(motor_state) {
            motor_state = 0;
            digitalWrite(Motor, LOW);
            check_and_publish(TOPIC_CurrentMotorState, OFF, 0);
        }
        waterTimer_flag = 0;

        if(pureTimer_flag)
            pureTimer_flag = 0;
        else
            check_and_publish(TOPIC_MotorTimeoutWarning, ON, 0);

    }

    bool manualEnablePrev = manualEnable;
    manualEnable = digitalRead(ManualOverride);

    if(manualEnable) {
        check_and_publish(TOPIC_ManualOverride, ON, 0);
        manual_state = digitalRead(ManualControl);

        if(manual_state) {
            motor_state = 1;
            digitalWrite(Motor, HIGH);
            check_and_publish(TOPIC_CurrentMotorState, ON, 0);
        }
        else {
            motor_state = 0;
            digitalWrite(Motor, LOW);
            check_and_publish(TOPIC_CurrentMotorState, OFF, 0);
        }
    }
    else {
        /*      Moved to top of loop

        if(WiFi.status() != WL_CONNECTED)
            setupWiFi();

        if(!client.connected() && (WiFi.status() == WL_CONNECTED))   //Make sure MQTT is connected
            connectMQTT();
        */
        
        if(manualEnablePrev && !manualEnable && motor_state)        //Turn off motor when Manual override is turned off while motor is ON.
        {
            motor_state = 0;
            digitalWrite(Motor, LOW);
            check_and_publish(TOPIC_CurrentMotorState, OFF, 0);
        }

        check_and_publish(TOPIC_ManualOverride, OFF, 0);

        if(no_response_count > 2) { //Wait for 3 response failures
            tank_responsive = 0;
            motor_state = 0;
            digitalWrite(Motor, LOW);
            water_timer.detach();   //Turn off Fail-safe Timer
            check_and_publish(TOPIC_CurrentMotorState, OFF, 0);
        }

        client.loop();
    }
    delay(Seconds(0.05));
}
