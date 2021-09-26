#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <String.h>

#define Seconds(s) s*1000
#define Minutes(m) m*Seconds(60)
#define MAX_MSG_LENGTH 25
#define SOLAR_ILLEGAL_WAITTIME_SECONDS 600      //Time to be elapsed before the next ON_WITH_TIMER message is expected.
#define PING_TANK_INTERVAL 6                    //Ping the tank this often
#define TANK_RESPONSE_WAITTIME (PING_TANK_INTERVAL-1) //Wait for this
#define WIFI_INFO_FREQUENCY_SECONDS 10  //Enter values in multiples of 5

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

const char *ssid = "likith";
const char *password = "*druthi#";
const char *host_name = "192.168.0.105";
const char *TOPIC_MotorChange = "MotorStatusChange";
const char *TOPIC_PingGround = "PingGround";  //For broker to know if Motor board is working.
const char *TOPIC_GroundResponse = "GroundResponse";    //For broker to know if Motor board is working.
const char *TOPIC_SysKill = "SysKill";
const char *TOPIC_PingTank = "PingTank";        //Periodic message
const char *TOPIC_TankResponse = "TankResponse";
const char *TOPIC_ManualOverride = "ManualOverride";        //Periodic message
const char *TOPIC_MotorTimeoutWarning = "MotorTimeoutWarning";      //To be handled in broker.
const char *TOPIC_GroundResetAndAcknowledge = "GroundResetAndAcknowledge";    //To know if motor control board is resetting often.
const char *TOPIC_RequestBoardResetCount = "RequestBoardResetCount";
const char *TOPIC_GroundResetCount = "GroundResetCount";
const char *TOPIC_SensorMalfunction = "SensorMalfunction";
const char *TOPIC_SensorMalfunctionReset = "SensorMalfunctionReset";
const char *TOPIC_CurrentMotorState = "CurrentMotorState";      //Periodic message
const char *TOPIC_WifiInfo = "WifiInfo";


bool blink_flag;   //Blink Flag interrupt
bool CurrentMotorState_message_flag;  //interrupt to send frequent on/off messages
bool tankresponsefun_flag;    //Tank ping response interrupt
bool pingNow_flag;  //Tank ping interrupt
bool waterTimer_flag;   //Water Timer interrupt
bool pureTimer_flag;    //True when solar timer enabled
bool motor_state;
bool manualEnable;
bool manualEnableIgnore;    //True during transition from manual to auto which avoids immediate motor toggle
bool manual_state;
bool tank_responsive;
bool on_solar_illegal;
bool sensor_malfunction;
bool wait_on_disconnect_to_turnoff;
int no_response_count = 0;
unsigned short WifiInfo_flag;    //Non boolean flag (Considered true when value >= WIFI_INFO_FREQUENCY_SECONDS ÷ 5)

unsigned long lastTankResponse = 4294967294;
unsigned long pingTime;
Ticker ping_tank;
Ticker tank_response;   //Probably can be removed
Ticker timer_5sec;
Ticker water_timer;
Ticker make_solar_legal;
Ticker Ticker_manualEnableIgnore;
WiFiClient wclient;
PubSubClient client(wclient);

const float timer_pure_seconds = 6*60; //Sensor states- MainOVF=1, MainMid=1 & Solar=0      //Ideal sensor position - 35% of solar tank, timer value to be set = 50% of total solar tank capacity.
const float timer_s1s3 = 42*60;        //Sensor states- MainOVF=0, MainMid=0 & Solar=0      //42mins with buffer, 39min = 26min(To fill 100% of Main Tank) + 13min(To fill 60% of Solar Tank of 13min)
const float timer_s1 = 34*60;          //Sensor states- MainOVF=0, MainMid=0 & Solar=1      //34min = 26min(To fill 100% of Main Tank) + 8min(To fill 60% of Solar Tank of 13min)
const float timer_s3 = 7*60;           //Sensor states- MainOVF=0, MainMid=1 & Solar=0
const float timer_manualEnableIgnore = 10; //Don't change. Delay for transition from manual to auto

/*
 *(1) --Manual Override-> "break;" in setupWiFi & connectMQTT to be addressed.--
 *(1) --Introduce flags for Wifi and MQTT connected. If Wifi not connected, don't attempt MQTT connection. If MQTT not connected don't attempt MQTT communication. Infinite while loops of MQTT and Wifi is a bad idea.--
 *(1) --Add error message when motor is Turned OFF via Timer--
 *(1) -?-?Maintain backlog messages to be sent if WiFi or MQTT does not connect.-?-?
 *(1) --Check manual override in ConnectMQTT and connectWifi and take action immediately on motor.--
 *(2) ??Same as below - Delay("Or thought to be given") to be added during Manual Override ON to OFF(turned to Auto) Transition, To avoid toggle of Motor from Off(due to Manual Override) to On(due to auto)??
 *(2) ??Same as below - Add delay everywhere we turn off motor to avoid immediate turn on??
 *(2) To be solved after debugging - Frequent ON-OFF/OFF-ON messages from tank control leads to error message and motor is turned OFF. ON-OFF transition cannot happen within a minimum specified timer(Ticker). Warning and error to be sent if such multiple transitions happen.
 *(3) ??Addressed above - Maybe send manualOverride message less frequently to avoid wasting time at both ends.??
 *(3) Send "TOPIC_CurrentMotorState" and "TOPIC_ManualOverride" messages constantly (maybe once in 10s) in the loop.
 *(3) Board reset counter to be maintained in EEPROM and sent if requested through MQTT(new topic maybe required).
 *(4) --Implement function to turn ON/OFF motor instead of repeating code.--
 *(4) ??Not required as of now - Ticker tank_response;   //Probably can be removed??
 *(4) ??const char *TOPIC_PingGround = "PingGround";  //To be checked Redundant -> Can be used by broker to make sure motor is active.??
 *(4) ??Check Ticker overlap/clash, Ping Tank & Blink LED: Test in Ticker Only Program (Priority?)??
 *(4) ??Remove String class - Can lead to board reset!!??
 *(4) Ping tank more often when motor is ON
 *
*/

void check_manual() {

    static unsigned long initial_time = 0;

    bool manualEnablePrev = manualEnable;           //Move up below blink
    manualEnable = digitalRead(ManualOverride);


    if (!wait_on_disconnect_to_turnoff) {
  wait_on_disconnect_to_turnoff = 1;
  initial_time = millis();
    }

    if(manualEnable) {

        waterTimer_flag = 0;    //Reset timer variables and detach timer
        pureTimer_flag = 0;
        water_timer.detach();

        manual_state = digitalRead(ManualControl);

        if(manual_state) {
            motor_state = 1;
            digitalWrite(Motor, HIGH);
        }
        else {
            motor_state = 0;
            digitalWrite(Motor, LOW);
        }
    }
    else {
        if (motor_state && (millis() - initial_time) > Seconds(5))  //Added "if" statement to avoid setting ticker multiple times while Motor=0 (Off)
        {
            motor_state = 0;            // [Bug(Ver1.0) Found during "MQTT/Wi-Fi Disconnect and Motor On" situations- Water Over flow (~~+10mins of excess)]
            digitalWrite(Motor, LOW);

            waterTimer_flag = 0;    //See Above, bug fix: For Safety Reasons- Reset timer variables and detach timer
            pureTimer_flag = 0;
            water_timer.detach();
                //Commented below Bug Fix Water Overflow during - MQTT/Wi-Fi disconnect
                //         if(manualEnablePrev && !manualEnable && motor_state)        //Turn off motor when Manual override is turned off while motor is ON.
                //         {
                //             motor_state = 0;         //Bug Fix Water Overflow during - MQTT/Wi-Fi disconnect//Bug Fix Water Overflow during - MQTT/Wi-Fi disconnect
                //             digitalWrite(Motor, LOW);
            manualEnableIgnore = 1;
            Ticker_manualEnableIgnore.once(timer_manualEnableIgnore, manualEnableIgnoreFun);
                //         }
        }
    }
}

void turn_on_motor() {

    digitalWrite(Motor, HIGH);
    if(!motor_state)        //Send only once when motor_state changes. Avoids message flooding
        check_and_publish(TOPIC_CurrentMotorState, ON, 0);
    motor_state = 1;

}

void turn_off_motor() {

    digitalWrite(Motor, LOW);
    if(motor_state)         //Send only once when motor_state changes. Avoids message flooding
        check_and_publish(TOPIC_CurrentMotorState, OFF, 0);
    motor_state = 0;

}

void setupWiFi() {

    digitalWrite(LED_BUILTIN, LOW);     //LED always ON

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while((WiFi.status() != WL_CONNECTED)) {
        delay(200);
        check_manual();
    }

    digitalWrite(LED_BUILTIN, HIGH);    //LED OFF
    delay(10);
}

void connectMQTT() {

    while(!client.connected()) {

        if(WiFi.status() != WL_CONNECTED)
            setupWiFi();

        check_manual();

        String clientID = "BCground-";
        clientID += String(random(0xffff), HEX);    //Unique client ID each time

        if(client.connect(clientID.c_str())){   //Subscribe to required topics
            client.subscribe(TOPIC_TankResponse);
            client.subscribe(TOPIC_SysKill);
            client.subscribe(TOPIC_MotorChange);
            client.subscribe(TOPIC_PingGround);
            client.subscribe(TOPIC_SensorMalfunction);
            client.subscribe(TOPIC_SensorMalfunctionReset);
        }
        else {
            digitalWrite(LED_BUILTIN, LOW);
            delay(100);
            digitalWrite(LED_BUILTIN, HIGH);
            delay(100);
            digitalWrite(LED_BUILTIN, LOW);
            delay(100);
            digitalWrite(LED_BUILTIN, HIGH);
            delay(80);      //To make LED pattern consistent. There might be delay in the rest of the loop.
        }
    }
}

void check_and_publish(const char *Topic, const char *Message, bool Persistance) {

    if((WiFi.status() == WL_CONNECTED) && (client.connected()))
        Persistance ? client.publish(Topic, (uint8_t*)Message, strlen(Message), true) : client.publish(Topic, Message);

}

void callback(char *msgTopic, byte *msgPayload, unsigned int msgLength) {

    static char message[MAX_MSG_LENGTH + 1];

    memcpy(message, (char *) msgPayload, msgLength);
    message[msgLength] = '\0';

    if(!strcmp(msgTopic, TOPIC_SensorMalfunction))
        if(!strcmp(message, ON))
            sensor_malfunction = 1;

    if(!strcmp(msgTopic, TOPIC_TankResponse))
        if(!strcmp(message, ON))
            lastTankResponse = millis();

    if(!strcmp(msgTopic, TOPIC_SensorMalfunctionReset))
        if(!strcmp(message, ON))
            sensor_malfunction = 0;

    if(!strcmp(msgTopic, TOPIC_PingGround))
        if(!strcmp(message, ON))
            check_and_publish(TOPIC_GroundResponse, ON, 0);

    if(!strcmp(msgTopic, TOPIC_SysKill))
        if(!strcmp(message, MOTOR) | !strcmp(message, ALL)) {
            if(motor_state)
                turn_off_motor();

            ESP.deepSleep(0);
        }

    if(!strcmp(msgTopic, TOPIC_MotorChange) && tank_responsive && !manualEnable && !manualEnableIgnore && !sensor_malfunction) {

        if(!strcmp(message, OFF)) {
            if(motor_state) {
                turn_off_motor();
                water_timer.detach(); //Turn off Fail-safe Timer
            }
        }

        if(!strcmp(message, ON_WITH_TIMER)) {
            if(!motor_state) {
                if (on_solar_illegal) {
                    sensor_malfunction = 1;
                    check_and_publish(TOPIC_SensorMalfunction, ON, 1);
                }
                else {
                    turn_on_motor();
                    pureTimer_flag = 1;
                    water_timer.once(timer_pure_seconds, waterTimer);
                }
            }
        }

        if(!strcmp(message, ONs1s3)) {
            if(!motor_state) {
                turn_on_motor();
                water_timer.once(timer_s1s3, waterTimer);
            }
        }

        if(!strcmp(message, ONs1)) {
            if(!motor_state) {
                turn_on_motor();
                water_timer.once(timer_s1, waterTimer);
            }
        }

        if(!strcmp(message, ONs3)) {
            if(!motor_state) {
                turn_on_motor();
                water_timer.once(timer_s3, waterTimer);
            }
        }
    }
}

//ISRs
void timer_fun_5sec() {
    blink_flag = 1;
    CurrentMotorState_message_flag = 1;
    ++WifiInfo_flag;
}

void waterTimer() {
    waterTimer_flag = 1;
}

void tankresponsefun() {
    tankresponsefun_flag = 1;
}

void pingNow() {
    pingNow_flag = 1;
}

void make_solar_legal_fun()
{
    on_solar_illegal = 0;
}

void manualEnableIgnoreFun()
{
    manualEnableIgnore = 0;
}

//Program starts here
void setup() {
    // put your setup code here, to run once:

    // Serial.begin(115200);

    //MQTT Connect Timeout Decreased from 15 seconds to 5 seconds
    // client.setSocketTimeout(5);

    motor_state = 0;
    tank_responsive = 1;
    blink_flag = 0;
    CurrentMotorState_message_flag = 0;
    tankresponsefun_flag = 0;
    pingNow_flag = 0;
    waterTimer_flag = 0;
    pureTimer_flag = 0;
    on_solar_illegal = 0;
    manualEnableIgnore = 0;
    sensor_malfunction = 0;
    wait_on_disconnect_to_turnoff = 0;
    WifiInfo_flag = 0;

    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(Motor, OUTPUT);
    pinMode(ManualOverride, INPUT);
    pinMode(ManualControl, INPUT);

    //Redundant To be checked if Off message is require at Start
    manualEnable = digitalRead(ManualOverride);
    if(!manualEnable)
        turn_off_motor();


    digitalWrite(LED_BUILTIN, LOW);     // Initial state Of LED Active Low->specifying explicitly
    motor_state = 0;
    digitalWrite(Motor, LOW); //Set default Motor state to LOW
    delay(1000);

    WiFi.hostname("NodeMCU Motor");
    WiFi.setAutoReconnect(true); //WiFi auto reconnect enabled - No need to call setupWifi() repeatedly but it is for safety
    setupWiFi();
    wait_on_disconnect_to_turnoff = 0;

    client.setServer(host_name, 1883);
    client.setCallback(callback);

    if (WiFi.status() == WL_CONNECTED) {
        connectMQTT();
  wait_on_disconnect_to_turnoff = 0;
    }

    check_and_publish(TOPIC_GroundResetAndAcknowledge, ON, 1);      //Be persistent

    for(int i = 0; i <= 10; i++){
        digitalWrite(LED_BUILTIN, LOW);
        delay(500);
        digitalWrite(LED_BUILTIN, HIGH);
        delay(500);
    }

    ping_tank.attach(PING_TANK_INTERVAL, pingNow);
    timer_5sec.attach(5, timer_fun_5sec);
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
  wait_on_disconnect_to_turnoff = 0;
    }

//    Redundant: LED turned ON inside setupWiFi()
//    if(WiFi.status() != WL_CONNECTED) {        //Redundant "if" statement needed. Because we need to turn ON the LED only if Wifi is not connected.
//        digitalWrite(LED_BUILTIN, LOW);
//        delay(10);
//    }

    if(!client.connected() && (WiFi.status() == WL_CONNECTED)) {  //Make sure MQTT is connected
        connectMQTT();
  wait_on_disconnect_to_turnoff = 0;
    }

    if (CurrentMotorState_message_flag) {   //Reapeated on/off message
      check_and_publish(TOPIC_CurrentMotorState, motor_state ? ON : OFF, 0);
      CurrentMotorState_message_flag = 0;
    }

    static char Local_WifiData[110];
    if (WifiInfo_flag >= WIFI_INFO_FREQUENCY_SECONDS / 5) {
      uint8_t macAddr[6];
      char *thislocalIP = (char *) &WiFi.localIP().v4();
      uint8_t *bssid = WiFi.BSSID();
      WiFi.macAddress(macAddr);
      sprintf(Local_WifiData, "Motor\nIP: %d.%d.%d.%d\nFree heap size: %d\nRouter MAC: %02x:%02x:%02x:%02x:%02x:%02x\nESP MAC: %02x:%02x:%02x:%02x:%02x:%02x\nRSSI: %d dBm\n", *thislocalIP, *(thislocalIP + 1), *(thislocalIP + 2), *(thislocalIP + 3), ESP.getFreeHeap(), bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5], WiFi.RSSI());
      check_and_publish(TOPIC_WifiInfo, Local_WifiData, 0);
      WifiInfo_flag = 0;
    }

    if(tankresponsefun_flag) {

        unsigned long elapsed = lastTankResponse - pingTime;
        if(abs(elapsed) < Minutes(48 * 60 /* 2 days */)) {       //Handle millis overflow
            if(elapsed <= Seconds(TANK_RESPONSE_WAITTIME)) {
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
        tank_response.once(TANK_RESPONSE_WAITTIME, tankresponsefun);
        pingNow_flag = 0;
    }

    bool manualEnablePrev = manualEnable;
    manualEnable = digitalRead(ManualOverride);

    if(waterTimer_flag && !manualEnable/*Just to make sure (reliability), not actually needed*/) {

        if(!pureTimer_flag)
            sensor_malfunction = 1;

        if(motor_state)
            turn_off_motor();

        waterTimer_flag = 0;

        if(pureTimer_flag) {
            on_solar_illegal = 1;
            make_solar_legal.attach(SOLAR_ILLEGAL_WAITTIME_SECONDS, make_solar_legal_fun);
            pureTimer_flag = 0;
        }
        else {
            //Fatal error
            check_and_publish(TOPIC_MotorTimeoutWarning, ON, 1);    //Will be cleared by SensorMalfunctionReset by the tank
            check_and_publish(TOPIC_SensorMalfunction, ON, 1);      //Will be cleared by SensorMalfunctionReset by the tank
        }
    }

    if(manualEnable) {

        waterTimer_flag = 0;    //Reset timer variables and detach timer
        pureTimer_flag = 0;
        water_timer.detach();


        check_and_publish(TOPIC_ManualOverride, ON, 0);
        manual_state = digitalRead(ManualControl);

        if(manual_state)
            turn_on_motor();
        else
            turn_off_motor();
    }
    else {

        if(manualEnablePrev && !manualEnable && motor_state) {       //Turn off motor when Manual override is turned off while motor is ON.
            turn_off_motor();
            manualEnableIgnore = 1;
            Ticker_manualEnableIgnore.once(timer_manualEnableIgnore, manualEnableIgnoreFun);
        }

        check_and_publish(TOPIC_ManualOverride, OFF, 0);

        if(no_response_count > 2) { //Wait for 3 response failures
            tank_responsive = 0;
            turn_off_motor();
            water_timer.detach();   //Turn off Fail-safe Timer
        }

        client.loop();
    }
    delay(Seconds(0.05));
}
