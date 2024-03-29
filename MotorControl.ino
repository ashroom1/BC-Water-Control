#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>
#include <Ticker.h>
#include <EEPROM.h>

#define FIRMWARE_VERSION "1.0.1"

#define SECONDS(s) s*1000
#define MINUTES(m) m*SECONDS(60)
#define MAX_MSG_LENGTH 25
#define SOLAR_ILLEGAL_WAITTIME_SECONDS 600      //Time to be elapsed before the next ON_WITH_TIMER message is expected.
#define PING_TANK_INTERVAL 6                    //Ping the tank this often
#define TANK_RESPONSE_WAITTIME (PING_TANK_INTERVAL-1) //Wait for this
#define WIFI_INFO_FREQUENCY_SECONDS 10  //Enter values in multiples of 5
#define MANUAL_OVERRIDE_FREQUENCY_SECONDS 10  //Enter values in multiples of 5

#define Motor D5
#define ManualOverride D6
#define ManualControl D7
#define Sump D1

#define ON "ON"
#define ONs1s3 "ONs1s3"
#define ONs1 "ONs1"
#define ONs3 "ONs3"
#define OFF "OFF"
#define STATUS "STATUS"
#define ON_WITH_TIMER "ONSolar"
#define ALL "ALL"
#define MOTOR "MOTOR"

const char *ssid = "SSID"
const char *password = "PASSWORD"
const char *host_name = "192.168.0.105";
const char *TOPIC_MotorStatusChange = "MotorStatusChange";
const char *TOPIC_PingGround = "PingGround";  //For broker to know if Motor board is working.
const char *TOPIC_GroundResponse = "GroundResponse";    //For broker to know if Motor board is working.
const char *TOPIC_SysKill = "SysKill";
const char *TOPIC_PingTank = "PingTank";        //Periodic message
const char *TOPIC_TankResponse = "TankResponse";
const char *TOPIC_ManualOverride = "ManualOverride";        //Periodic message
const char *TOPIC_MotorTimeoutWarning = "MotorTimeoutWarning";      //To be handled in broker.
const char *TOPIC_GroundResetAndAcknowledge = "GroundResetAndAcknowledge";    //To know if motor control board is resetting often.
const char *TOPIC_BoardResetCountReset = "BoardResetCountReset";
//const char *TOPIC_RequestBoardResetCount = "RequestBoardResetCount";
//const char *TOPIC_GroundResetCount = "GroundResetCount";
const char *TOPIC_SensorMalfunction = "SensorMalfunction";
const char *TOPIC_SensorMalfunctionReset = "SensorMalfunctionReset";
const char *TOPIC_CurrentMotorState = "CurrentMotorState";      //Periodic message
const char *TOPIC_WifiInfo = "WifiInfo";
const char *TOPIC_SystemErrorSensorMalfunction = "SystemError/SensorMalfunction";
const char *TOPIC_SensorSump = "Sensor/Sump";

bool blink_flag;   //Blink Flag interrupt
bool CurrentMotorState_message_flag;  //interrupt to send frequent on/off messages
bool tankresponsefun_flag;    //Tank ping response interrupt
bool pingNow_flag;  //Tank ping interrupt
bool sumpStatus_flag;   //Send sump sensor state if set
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
bool sump_state;
int no_response_count = 0;
unsigned short WifiInfo_flag;    //Non boolean flag (Considered true when value >= WIFI_INFO_FREQUENCY_SECONDS ÷ 5)
unsigned short ManualOverride_flag;    //Non boolean flag (Considered true when value >= MANUAL_OVERRIDE_FREQUENCY_SECONDS ÷ 5)

unsigned long lastTankResponse = 4294967294;
unsigned long lastSumpStateChange = 4294967294;
unsigned long pingTime;
unsigned int mqttDisconnectCounter;
unsigned int wifiDisconnectCounter;

Ticker ping_tank;
Ticker tank_response;   //Probably can be removed
Ticker timer_5sec;
Ticker water_timer;
Ticker make_solar_legal;
Ticker Ticker_manualEnableIgnore;
WiFiClient wclient;
PubSubClient client(wclient);
AsyncWebServer server(80);

const float timer_pure_seconds = 6*60; //Sensor states- MainOVF=1, MainMid=1 & Solar=0      //Ideal sensor position - 35% of solar tank, timer value to be set = 50% of total solar tank capacity.
                                                                                            //6mins with buffer, 6min = 0min(To fill 0% of Main Tank of 26min) + 6min(To fill ~50% of Solar Tank of 13min)
const float timer_s1s3 = 42*60;        //Sensor states- MainOVF=0, MainMid=0 & Solar=0      //42mins with buffer, 39min = 26min(To fill 100% of Main Tank of 26min) + 13min(To fill 100% of Solar Tank of 13min)
const float timer_s1 = 34*60;          //Sensor states- MainOVF=0, MainMid=0 & Solar=1      //34mins w/o buffer, 34min = 26min(To fill 100% of Main Tank of 26min) + 8min(To fill 60% of Solar Tank of 13min)
const float timer_s3 = 28*60;           //Sensor states- MainOVF=0, MainMid=1 & Solar=0     //28min with buffer, 27min = 14min(To fill ~50% of Main Tank of 26min) + 13min(To fill 100% of Solar Tank of 13min)
const float timer_manualEnableIgnore = 10; //Don't change. Delay for transition from manual to auto

/*
 *(1) --Manual Override-> "break;" in setupWiFi & connectMQTT to be addressed.--
 *(1) --Introduce flags for Wifi and MQTT connected. If Wifi not connected, don't attempt MQTT connection. If MQTT not connected don't attempt MQTT communication. Infinite while loops of MQTT and Wifi is a bad idea.--
 *(1) --Add error message when motor is Turned OFF via Timer--
 *(1) -?-?Maintain backlog messages to be sent if WiFi or MQTT does not connect.-?-?
 *(1) --Check manual override in ConnectMQTT and connectWifi and take action immediately on motor.--
 *(2) --Same as below - Delay("Or thought to be given") to be added during Manual Override ON to OFF(turned to Auto) Transition, To avoid toggle of Motor from Off(due to Manual Override) to On(due to auto).--
 *(2) ??Same as below - Add delay everywhere we turn off motor to avoid immediate turn on??
 *(2) To be solved after debugging - Frequent ON-OFF/OFF-ON messages from tank control leads to error message and motor is turned OFF. ON-OFF transition cannot happen within a minimum specified timer(Ticker). Warning and error to be sent if such multiple transitions happen.
 *(3) --Addressed above - Maybe send manualOverride message less frequently to avoid wasting time at both ends.--
 *(3) --Send "TOPIC_CurrentMotorState" and "TOPIC_ManualOverride" messages constantly (maybe once in 10s) in the loop.--
 *(3) --Board reset counter to be maintained in EEPROM and sent if requested through MQTT(new topic maybe required).--
 *(4) --Implement function to turn ON/OFF motor instead of repeating code.--
 *(4) --Implement - Ticker tank_response--
 *(4) ??const char *TOPIC_PingGround = "PingGround";  //To be checked Redundant PingTank can be used instead(STATUS)-> Can be used by broker to make sure motor is active.??
 *(4) --Not Required-Check Ticker overlap/clash, Ping Tank & Blink LED: Test in Ticker Only Program (Priority?)--
 *(4) --Remove String class - Can lead to board reset!!--
 *(4) --Not required-Ping tank more often when motor is ON--
 *
*/

void check_and_publish(const char *Topic, const char *Message, bool Persistance) {

    if((WiFi.status() == WL_CONNECTED) && (client.connected()))
        Persistance ? client.publish(Topic, (uint8_t*)Message, strlen(Message), true) : client.publish(Topic, Message);

}

//ISRs
void timer_fun_5sec() {
    blink_flag = 1;
    CurrentMotorState_message_flag = 1;
    ++WifiInfo_flag;
    ++ManualOverride_flag;
    sumpStatus_flag = 1;
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

uint8_t EEPROM_read_with_delay(int location_read) {
    uint8_t temp = EEPROM.read(location_read);
    delay(10);
    return temp;
}

bool EEPROM_write(int location, int value_to_be_written) {

    if(EEPROM_read_with_delay(location) == value_to_be_written)
        return true;
    else {
        for(int i = 0; i < 5; i++) {
            EEPROM.write(location, value_to_be_written);
            delay(100);
            if(EEPROM.commit()){
//                EEPROM.end(); // If this function is called EEPROM is disabled and can no longer be used until EEPROM.begin is called.
                return true;
            }
//            EEPROM.end();
//            delay(10);
        }
    }
    return false;
}

void increaseResetCount() {

    int temp_increaseResetCount01 = EEPROM_read_with_delay(1);
    int temp_increaseResetCount02 = EEPROM_read_with_delay(2);
    int temp_increaseResetCount03 = EEPROM_read_with_delay(3);

    if (temp_increaseResetCount01 == 0xff && temp_increaseResetCount02 == 0xff) {
        EEPROM_write(1, 0);
        EEPROM_write(2, 0);
        EEPROM_write(3, temp_increaseResetCount03 + 1);
    }
    else if (temp_increaseResetCount01 == 0xff) {
        EEPROM_write(1, 0);
        EEPROM_write(2, temp_increaseResetCount02 + 1);
    }
    else
        EEPROM_write(1, temp_increaseResetCount01 + 1);
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

void check_manual() {

    static unsigned long initial_time = 0;

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

        if(manual_state)
            turn_on_motor();
        else
            turn_off_motor();
    }
    else {
        if (motor_state && (millis() - initial_time) > SECONDS(5))  //Added "if" statement to avoid setting ticker multiple times while Motor=0 (Off)
        {
            turn_off_motor();            // [Bug(Ver1.0) Found during "MQTT/Wi-Fi Disconnect and Motor On" situations- Water Over flow (~~+10mins of excess)]

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

        if(WiFi.status() != WL_CONNECTED) {
            ++wifiDisconnectCounter;
            setupWiFi();
        }

        check_manual();

        String clientID = "BCground-";
        clientID += String(random(0xffff), HEX);    //Unique client ID each time

        if(client.connect(clientID.c_str())){   //Subscribe to required topics
            client.subscribe(TOPIC_TankResponse);
            client.subscribe(TOPIC_SysKill);
            client.subscribe(TOPIC_MotorStatusChange);
            client.subscribe(TOPIC_PingGround);
            client.subscribe(TOPIC_SensorMalfunction);
            client.subscribe(TOPIC_SensorMalfunctionReset);
            client.subscribe(TOPIC_BoardResetCountReset);
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
        if(!strcmp(message, MOTOR) || !strcmp(message, ALL)) {
            if(motor_state)
                turn_off_motor();

            ESP.deepSleep(0);
        }

    if(!strcmp(msgTopic, TOPIC_BoardResetCountReset))
        if(!strcmp(message, ALL) || !strcmp(message, MOTOR)) {
            EEPROM_write(1, 0);
            EEPROM_write(2, 0);
            EEPROM_write(3, 0);
        }

    if(!strcmp(msgTopic, TOPIC_MotorStatusChange) && tank_responsive && !manualEnable && !manualEnableIgnore && !sensor_malfunction && sump_state) {

        if(!strcmp(message, OFF)) {
            if(motor_state) {
                turn_off_motor();
                waterTimer_flag = 0;
                pureTimer_flag = 0;
                water_timer.detach(); //Turn off Fail-safe Timer
            }
        }

        if(!strcmp(message, ON_WITH_TIMER)) {
            if(!motor_state) {
                if (on_solar_illegal) {
                    sensor_malfunction = 1;
                    check_and_publish(TOPIC_SensorMalfunction, ON, 1);
                    check_and_publish(TOPIC_SystemErrorSensorMalfunction, "Sensor Malfunction 1", 1);
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
    ManualOverride_flag = 0;
    sumpStatus_flag = 0;
    sump_state = 0;
    mqttDisconnectCounter = 0;
    wifiDisconnectCounter = 0;

    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(Motor, OUTPUT);
    pinMode(ManualOverride, INPUT);
    pinMode(ManualControl, INPUT);
    pinMode(Sump, INPUT);

    digitalWrite(LED_BUILTIN, LOW);     // Initial state Of LED Active Low->specifying explicitly
    motor_state = 0;
    digitalWrite(Motor, LOW); //Set default Motor state to LOW
    delay(1000);

    sump_state = digitalRead(Sump);   //Aviods SensorMalfunction and set state to actual sensor value

    WiFi.hostname("NodeMCU Motor");
    WiFi.setOutputPower(20.5);    //Set to Max Wi-Fi Tx Power
    WiFi.setAutoReconnect(true);  //WiFi auto reconnect enabled - No need to call setupWifi() repeatedly but it is for safety
    setupWiFi();

    AsyncElegantOTA.begin(&server);    // Start ElegantOTA
    server.begin();

    EEPROM.begin(10);

    increaseResetCount();

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
        ++wifiDisconnectCounter;
        setupWiFi();
        wait_on_disconnect_to_turnoff = 0;
    }

//    Redundant: LED turned ON inside setupWiFi()
//    if(WiFi.status() != WL_CONNECTED) {        //Redundant "if" statement needed. Because we need to turn ON the LED only if Wifi is not connected.
//        digitalWrite(LED_BUILTIN, LOW);
//        delay(10);
//    }

    if(!client.connected() && (WiFi.status() == WL_CONNECTED)) {  //Make sure MQTT is connected
        ++mqttDisconnectCounter;
        connectMQTT();
        wait_on_disconnect_to_turnoff = 0;
    }

    if (CurrentMotorState_message_flag) {   //Repeated on/off message
        check_and_publish(TOPIC_CurrentMotorState, motor_state ? ON : OFF, 0);
        CurrentMotorState_message_flag = 0;
    }

    static char Local_WifiData[300];
    if (WifiInfo_flag >= WIFI_INFO_FREQUENCY_SECONDS / 5) {
        uint8_t macAddr[6];
        char *thislocalIP = (char *) &WiFi.localIP().v4();
        uint8_t *bssid = WiFi.BSSID();
        WiFi.macAddress(macAddr);

        uint32_t resetCount = 0;

        // Merging 3 bytes EEPROM data into 1 unsigned int
        resetCount |= (uint32_t) EEPROM_read_with_delay(3);
        resetCount *= 256; // Left shift 8 bits
        resetCount |= (uint32_t) EEPROM_read_with_delay(2);
        resetCount *= 256; // Left shift 8 bits
        resetCount |= (uint32_t) EEPROM_read_with_delay(1);

        sprintf(Local_WifiData, "Motor\nFirmware Version: %s\nRSSI: %d dBm\nWifi disconnect count: %u\nMQTT disconnect count: %u\nBoard reset count: %u\nIP: %d.%d.%d.%d\nFree heap size: %d\nRouter MAC: %02x:%02x:%02x:%02x:%02x:%02x\nESP MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", FIRMWARE_VERSION, WiFi.RSSI(), wifiDisconnectCounter, mqttDisconnectCounter, resetCount, *thislocalIP, *(thislocalIP + 1), *(thislocalIP + 2), *(thislocalIP + 3), ESP.getFreeHeap(), bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
        check_and_publish(TOPIC_WifiInfo, Local_WifiData, 0);
        WifiInfo_flag = 0;
    }

    if(tankresponsefun_flag) {

        signed long long elapsed = (signed long long) lastTankResponse - (signed long long) pingTime;
        //The below line can lead to interpreting tank response wrong.
        //if(abs(elapsed) < MINUTES(48 * 60 /* 2 days */)) {       Handle millis overflow

        if(abs(elapsed) <= SECONDS(TANK_RESPONSE_WAITTIME)) {
            no_response_count = 0;
            tank_responsive = 1;
        }
        else {
            if(no_response_count < 10)
                ++no_response_count;
        }
        //}
        tankresponsefun_flag = 0;
    }

    if(pingNow_flag) {
        check_and_publish(TOPIC_PingTank, STATUS, 0);
        pingTime = millis();
        tank_response.once(TANK_RESPONSE_WAITTIME, tankresponsefun);
        pingNow_flag = 0;
    }

    if(sumpStatus_flag) {
        bool sump_state_prev = sump_state;
        sump_state = digitalRead(Sump);
      
        if(!sump_state && sump_state_prev) {    //when sump sensor state changes from 1 to 0
          check_and_publish(TOPIC_GroundResetAndAcknowledge, ON, 1);    //Alert tank to read sensor states & resend MotorStatusChange to avoid Don't care condition 
        }
      
        if(sump_state != sump_state_prev) {
            unsigned long Local_current_millis = millis();
            if(abs((signed long long) Local_current_millis - (signed long long) lastSumpStateChange) < MINUTES(1)) {
                sensor_malfunction = 1;
                check_and_publish(TOPIC_SensorMalfunction, ON, 1);      //Will be cleared by SensorMalfunctionReset by the tank
                check_and_publish(TOPIC_SystemErrorSensorMalfunction, "Sensor Malfunction 5", 1);
            }
            lastSumpStateChange = Local_current_millis;
        }

        check_and_publish(TOPIC_SensorSump, sump_state ? ON : OFF, 0);
        sumpStatus_flag = 0;
    }

    bool manualEnablePrev = manualEnable;
    manualEnable = digitalRead(ManualOverride);

    if(waterTimer_flag && !manualEnable/*Just to make sure (reliability), not actually needed*/) {

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
            sensor_malfunction = 1;
            check_and_publish(TOPIC_MotorTimeoutWarning, ON, 1);    //Will be cleared by SensorMalfunctionReset by the tank
            check_and_publish(TOPIC_SensorMalfunction, ON, 1);      //Will be cleared by SensorMalfunctionReset by the tank
            check_and_publish(TOPIC_SystemErrorSensorMalfunction, "Sensor Malfunction 2", 1);
        }
    }

    if(manualEnable) {

        waterTimer_flag = 0;    //Reset timer variables and detach timer
        pureTimer_flag = 0;
        water_timer.detach();

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

        if(!sump_state && motor_state) {    //Turn off motor if sump gets empty
            turn_off_motor();
            waterTimer_flag = 0;
            pureTimer_flag = 0;
            water_timer.detach();   //Turn off Fail-safe Timer
        }

        if(no_response_count > 2) { //Wait for 3 response failures
            tank_responsive = 0;
            turn_off_motor();
            waterTimer_flag = 0;
            pureTimer_flag = 0;
            water_timer.detach();   //Turn off Fail-safe Timer
        }
    }

    if (ManualOverride_flag >= MANUAL_OVERRIDE_FREQUENCY_SECONDS / 5) {
        check_and_publish(TOPIC_ManualOverride, manualEnable ? ON : OFF, 0);
        ManualOverride_flag = 0;
    }

    client.loop();
    delay(SECONDS(0.05));
}
