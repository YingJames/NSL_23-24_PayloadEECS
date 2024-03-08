
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <queue>
#include "buzzer_notify.h"
#include "MyVL53L0X.h"

// TODO: Get rid of whatever this library is doing
#include <HardwareSerial.h>
#include "soar_barometer.h"
#include "DCMotor.h"
#include "ota_update.h"
#include "SOAR_Lora.h"

#define DEBUG_BUZZ false
#define DEBUG_TRSHSET false
#define TEST_MOTOR false
#define TEST_MOTOR_BACK false
#define DEBUG_ALT false
#define FAKE_ALT_DATA true

#define stepPin A3
#define dirPin A2
#define motorInterfaceType 1 // TODO: Get rid of this
#define buzzerPin A0

// ALTIMETER VARIABLES
#define SEALEVELPRESSURE_HPA (1013.25)
float altimeter_latest;
int ALT_TRSH_CHECK = 21;       // Use -10 for parking lot test and maybe change it on location
int LOW_ALT_TRSH_CHECK = 19;   //=300 or 350 for actual launch
int UPPER_ALT_TRSH_CHECK = 20; // 500 for actual launch
bool forwardStatus = false;
bool backwardStatus = false;

OTA_Update otaUpdater("soar-deploy", "TP-Link_BCBD", "10673881");

SOAR_Lora lora("5", "5", "905000000"); // LoRa

// STEPPER MOTOR DELAYS
static const int microDelay = 900;
static const int betweenDelay = 250;

// DC motor
DCMotor motor(A2, 50, 50);

SOAR_BAROMETER barometer;

class KalmanFilter {
public:
    KalmanFilter(float process_noise, float measurement_noise, float estimated_error, float initial_value) {
        Q = process_noise;
        R = measurement_noise;
        P = estimated_error;
        value = initial_value;
        
        /* Arbitrary threshold for outlier detection, adjust based on your data */
        outlier_threshold = 20.0; 
    }
    
    float update(float measurement) {
        // Prediction update
        /* No actual prediction step because we assume a simple model */
        
        // Measurement update
        K = P / (P + R);
        value = value + K * (measurement - value);
        P = (1 - K) * P + Q;
        
        return value;
    }
    
    bool checkOutlier(float measurement) {
        return fabs(measurement - value) > outlier_threshold;
    }
    
private:
    float Q; // Process noise
    float R; // Measurement noise
    float P; // Estimation error
    float K; // Kalman gain
    float value; // Filtered measurement
    float Q_prev;
    float P_prev;
    float value_prev;

    float outlier_threshold; // Threshold for detecting outliers
};



class AltitudeTrigger
{
private:
  float _max_height = 0;
  float _h0;
  float _h1;
  float _h2;
  float _prev_altitude = -500;
  std::queue<float> altitudeQueue;
  float _average = 0;
  float _sum = 0;
  float _max_distance = 0;
  const float _MACH = 175; //0.5 * speed of sound (maximum velocity per second - 171.5)
  uint32_t _last_checkpoint = 0; //for time control
  KalmanFilter _kf;

public:
  int state=0;

  AltitudeTrigger(float H0, float H1, float H2) : _kf(1.0, 1.0, 1.0, 10.0)
  {
    _h0 = H0;
    _h1 = H1;
    _h2 = H2;
    
  }
  float GetMaxAltitude(){
    return _max_height;
  }
  bool CheckAltitude(float curr_altitude)
  {
    _kf.update(curr_altitude);
    if(_kf.checkOutlier(curr_altitude)) return false;
    switch (state)
    {
      case 0://if next value is greater than 100
        if (curr_altitude > _h0){
          state = 1;
         }
        break;
      case 1:
        if ((curr_altitude > _max_height) && (curr_altitude - _prev_altitude > 0))
        {
            _max_height = curr_altitude;
        }
        if ((_max_height-curr_altitude>10) && (curr_altitude - _prev_altitude < 0))
        {
          state = 2;
        }
        break;
      case 2:
        if (curr_altitude < _h2 && curr_altitude > _h1)
        {
          state = 3;
        }
        break;
      case 3:
        if (curr_altitude < _h1)
        {
          state = 4;
        }
        break;
      case 4:
        //retracting
        break;
    }
    _prev_altitude=curr_altitude;
    return true;
  }
  
  // bool isContinuous(float curr_altitude){
  //   curr_altitude = abs(curr_altitude);
  //   if(altitudeQueue.size() < 10){
  //     altitudeQueue.push(curr_altitude);
  //     _sum += curr_altitude;
  //     _last_checkpoint = millis();
  //     return true;
  //   }
  //   if(altitudeQueue.size() == 10){//if the altitude is greater than mach, multiply seconds*mach. If the difference is greater than the possible distance then function is not continuous
  //     _average=_sum / 10;
  //     _sum -= altitudeQueue.front();
  //     altitudeQueue.pop();
  //     _max_distance = (millis() - _last_checkpoint) * _MACH;
  //     switch(state){
  //       case 1:
  //         if(curr_altitude >= _average && curr_altitude <= _max_distance){
  //           altitudeQueue.push(curr_altitude);
  //           _sum += curr_altitude;
  //           _last_checkpoint = millis();
  //           return true; //altitude is in the expected range
  //       case 2:
  //     }
      
  //     }else if(state == 2 && (curr_altitude <= _average && curr_altitude <= _max_distance)){
  //       altitudeQueue.push(curr_altitude);
  //       _sum += curr_altitude;
  //     }
  //   }
  //   //get checkpoint
  //   return false;
  // }


};

AltitudeTrigger altTrigger(914,107,183);

float previous_altitude = -300;
float max_candidate = -300;
int alt_trigger_count = 0;
int low_alt_trigger_count = 0;
float immediate_previous = -6000;
int altitudeTrigger(float current_altitude)
{
#if DEBUG_ALT
  Serial.print("Dif:");
  Serial.println(current_altitude - previous_altitude);
  Serial.print("Prev:");
  Serial.println(previous_altitude);
#endif
  int res = 0;
  // Check if the altitude is decreasing and above ALT_TRSH_CHECK
  if ((current_altitude > ALT_TRSH_CHECK) && (current_altitude - previous_altitude < -2))
  {
    res = 0;
  }
  if (current_altitude > previous_altitude)
  {
    if (current_altitude - immediate_previous < 800 || immediate_previous == -60000)
    { // Default value for errors
      previous_altitude = current_altitude;
    }
  }

  if ((current_altitude - previous_altitude) < -2 && current_altitude <= UPPER_ALT_TRSH_CHECK && current_altitude > LOW_ALT_TRSH_CHECK)
  {
    res = 1; // move forward status
  }
  if ((current_altitude - previous_altitude) < -2 && current_altitude <= LOW_ALT_TRSH_CHECK)
  {
    res = 2;
  }

  if (current_altitude - immediate_previous > 800)
  { // If altitude shows sudden changes it must be a glitch
    res = 0;
  }
  // Update previous_altitude for the next function call
  immediate_previous = current_altitude;
#if DEBUG_ALT
  Serial.print("Returned value: ");
  Serial.println(res);
#endif
  return res;
}

BuzzerNotify buzzerNotify = BuzzerNotify(buzzerPin);

MyVL53L0X distanceSensor;

class Deployment
{
private:
  int _state = 0; // _standby = 0; _forward = 1; _wait = 2; _retract = 3; _complete = 4;  _paused = 5;
  const char *message[6] = {"STANDBY", "FORWARD", "WAITING", "RETRACTING", "COMPLETED", "PAUSED"};
  bool sensor_trigger = false;
  uint32_t _forward_checkpoint = 0;
  uint32_t _wait_checkpoint = 0;
  uint32_t _retract_checkpoint = 0;
  uint32_t _last_checkpoint = 0;
  uint32_t _forward_duration = 5000;   // 2.5 seconds 3500
  uint32_t _retract_duration = 11000;  // Around half of move duration
  uint32_t _wait_duration = 20000; // 10 seconds
  int last_state = 0;
  bool _warn = false;
  int fwd_sensor_checks = 0;
  int retract_sensor_checks = 0;

public:
  Deployment(){};
  void Deploy()
  {
    if (_state == 0)
    {
      _state = 1;
    }
    if (_state == 5)
    {
      _state = last_state;
      switch (last_state)
      {
      case 1:
        _forward_checkpoint = millis();
        break;
      case 2:
        _wait_checkpoint = millis();
        break;
      case 3:
        _retract_checkpoint = millis();
        break;
      }
    }
  };

  void Stop()
  {
    last_state = _state;
    _state = 5;
    motor.DC_STOP();
  };

  void ProcedureCheck()
  {
    uint16_t distance;
    int speed_fwd;
    switch (_state){
      case 0://standby
        break;
      case 1://forward
        if(_forward_checkpoint==0){
          GetStatus();
          for(int i=0; i<5; i++){
            buzzerNotify.Trigger();
            delay(100);
          }
          _forward_checkpoint=millis();
        }
        speed_fwd = 100;
        //Sensor and time logic comes first
        //Check if not FAKE_ALT_DATA
        #if !FAKE_ALT_DATA
        distance = distanceSensor.readDistance();
        Serial.println(distance);
        #else
        distance = GetFakeDistance();
        SendFakeDistanceData(distance);
        #endif
        sensor_trigger = distance>560 && distance != 65535;
        if(sensor_trigger){
          for (int i =0; i<3; i++){
            #if !FAKE_ALT_DATA
          distance += distanceSensor.readDistance();
          #else
          distance += GetFakeDistance();
          #endif
          }
          sensor_trigger = (distance/4) > 560;
        }
        else if(distance > 280){
          speed_fwd = 50;
        }

      if (sensor_trigger || (millis() - _forward_checkpoint) > _forward_duration)
      {
        if (sensor_trigger)
        {
          Serial.println("Stop triggered by sensor");
        }
        Serial.println("Stopped.");
        speed_fwd = 0;
        _state = 2;
      }
      #if !FAKE_ALT_DATA
      motor.DC_MOVE(speed_fwd);
      #else
      SendFakeMotor(speed_fwd);
      #endif
      break;
    case 2: // wait
      if (_wait_checkpoint == 0)
      {
        GetStatus();
        _wait_checkpoint = millis();
      }
      #if !FAKE_ALT_DATA
      motor.DC_STOP();
      #else
      SendFakeMotor(0);
      #endif
      if ((millis() - _wait_checkpoint) > _wait_duration)
      {
        _state = 3;
      }
      break;
    case 3: // retract
      if (_retract_checkpoint == 0)
      {
        GetStatus();
        for (int i = 0; i < 5; i++)
        {
          buzzerNotify.Trigger();
          delay(100);
        }
        _retract_checkpoint = millis();
      }
      // Sensor and time logic comes first
      #if !FAKE_ALT_DATA
      distance = distanceSensor.readDistance();
      Serial.println(distance);
      #else
      distance = GetFakeDistance();
      SendFakeDistanceData(distance);
      #endif
      sensor_trigger = distance < 90 && distance != 65535;
      if (sensor_trigger)
      {
        for (int i = 0; i < 3; i++)
        {
          #if !FAKE_ALT_DATA
          distance += distanceSensor.readDistance();
          #else
          distance += GetFakeDistance();
          #endif
        }
        sensor_trigger = (distance / 3) < 90;
      }
      else
        retract_sensor_checks = 0;
      if (sensor_trigger || (millis() - _retract_checkpoint) > _retract_duration)
      {
        if (sensor_trigger)
        {
          Serial.println("Stop triggered by sensor");
        }
        Serial.println("Stopped.");
        #if !FAKE_ALT_DATA
        motor.DC_STOP();
        #else
        SendFakeMotor(0);
        #endif
        _state = 4;
      }
      else
      {
        // Move back logic comes second
        #if !FAKE_ALT_DATA
        motor.DC_MOVE(-50);
        #else
        SendFakeMotor(-50);
        #endif
      }
      break;
    case 4: // complete
      #if !FAKE_ALT_DATA
      motor.DC_STOP();
      #else
      SendFakeMotor(0);
      #endif
      break;
    case 5: // paused
      #if !FAKE_ALT_DATA
      motor.DC_STOP();
      #else
      SendFakeMotor(0);
      #endif

      break;
    }
  };
  void Reset()
  {
    _state = 0;
    _forward_checkpoint = 0;
    _wait_checkpoint = 0;
    _retract_checkpoint = 0;
    _warn = false;
  };
  void Retract()
  {
    if (_state == 0 || _state == 2)
      _state = 3;
  }
  String GetStatus()
  {
    return message[_state];
  };
};
Deployment deployment;

BLEServer *pServer = NULL;
BLECharacteristic *pCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    deviceConnected = true;
  };

  void onDisconnect(BLEServer *pServer)
  {
    deviceConnected = false;
  }
};

class MyCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0)
    {
      String value_str = "";
      for (int i = 0; i < value.length(); i++)
        value_str += value[i];
      Serial.print("Received Value: ");
      Serial.println(value_str);
      if (value_str == "DEPLOY")
      {
        pCharacteristic->setValue("OK");
        pCharacteristic->notify();
        Serial.println("Deploy procedure\n");
        deployment.Deploy();
      }
      else if (value_str == "STOP")
      {
        pCharacteristic->setValue("OK");
        pCharacteristic->notify();
        Serial.println("Stoping deployment\n");
        deployment.Stop();
      }
      else if (value_str == "RESET")
      {
        pCharacteristic->setValue("OK");
        pCharacteristic->notify();
        Serial.println("Resetting deployment state\n");
        deployment.Reset();
      }
      else if (value_str == "RETRACT")
      {
        pCharacteristic->setValue("OK");
        pCharacteristic->notify();
        Serial.println("Rtracting deployment\n");
        deployment.Retract();
      }
      else if (value_str == "BEEP")
      {
        pCharacteristic->setValue("OK");
        pCharacteristic->notify();
        Serial.println("Rtracting deployment\n");
        buzzerNotify.Trigger();
      }
      else if (value_str == "STATUS")
      {
        String sts = deployment.GetStatus();
        String stat = "DEPLOY-STATUS:" + sts;
        std::string stat_std = stat.c_str(); // Convert Arduino String to std::string
        pCharacteristic->setValue(stat_std); // Set the value using std::string
      }
    }
  }
};

#if FAKE_ALT_DATA
float GetFakeAltitude()
{
  // Send a request for altitude
  byte request = 0x01; // Arbitrary request code
  Serial.write(request);

  uint32_t timeout_start = millis();
  while (millis() - timeout_start < 1000)
  {
    if (Serial.available())
    {
      byte responseCode = Serial.read();
      if (responseCode == 0x02) // Arbitrary response code
      {
        // Read the next 4 bytes as a float
        union {
          float altitude;
          byte bytes[4];
        } altitudeData;

        for (int i = 0; i < 4; i++)
        {
          if (Serial.available())
          {
            altitudeData.bytes[i] = Serial.read();
          }
          else
          {
            return 44330.00; // Timeout or incomplete data
          }
        }
        return altitudeData.altitude;
      }
    }
  }
  return 44330.00;
}

void SendAltitudeData(float altitude, float maxAltitude, int state, bool outlier)
{
  // Start with a unique identifier byte for this message type
  Serial.write(0x03);
  Serial.write(0x03);

  // Send altitude as 4 bytes
  union {
    float value;
    byte bytes[4];
  } altitudeData;
  altitudeData.value = altitude;
  Serial.write(altitudeData.bytes, sizeof(altitudeData.bytes));

  // Send max altitude as 4 bytes
  union {
    float value;
    byte bytes[4];
  } maxAltitudeData;
  maxAltitudeData.value = maxAltitude;
  Serial.write(maxAltitudeData.bytes, sizeof(maxAltitudeData.bytes));

  // Send state as 4 bytes (int)
  union {
    int value;
    byte bytes[4];
  } stateData;
  stateData.value = state;
  Serial.write(stateData.bytes, sizeof(stateData.bytes));
  //Send outlier a 1 byte (bool)
  
  Serial.write(outlier);
  
}

uint16_t GetFakeDistance(){
  // Send a request for distance
  byte request = 0x05;
  Serial.write(request);
  //Send confirmation byte
  Serial.write(request);

  uint32_t timeout_start = millis();
  while (millis() - timeout_start < 500)
  {
    if (Serial.available())
    {
      byte responseCode = Serial.read();
      if (responseCode == 0x06) // Arbitrary response code
      {
        // Read the next 2 bytes as a uint16_t
        union {
          uint16_t distance;
          byte bytes[2];
        } distanceData;

        for (int i = 0; i < 2; i++)
        {
          if (Serial.available())
          {
            distanceData.bytes[i] = Serial.read();
          }
          else
          {
            return 65535; // Timeout or incomplete data
          }
        }
        return distanceData.distance;
      }
    }
  }
  return 65535;
}
void SendFakeDistanceData(uint16_t distance){
  // Start with a unique identifier byte for this message type
  Serial.write(0x07);
  Serial.write(0x07);
  // Send distance as 2 bytes
  union {
    uint16_t value;
    byte bytes[2];
  } distanceData;
  distanceData.value = distance;
  Serial.write(distanceData.bytes, sizeof(distanceData.bytes));

}
void SendFakeMotor(int dir){
  Serial.println("Requesting fake motor");
  byte request = 0x04;
  Serial.write(request);
  Serial.write(request);
  // Send direction as 4 bytes (int)
  union {
    int value;
    byte bytes[4];
  } dirData;
  dirData.value = dir;
  Serial.write(dirData.bytes, sizeof(dirData.bytes));
}
#endif

void setup()
{
  // Set the maximum speed and acceleration

#if TEST_MOTOR
  deployment.TriggerProcedure();
#if TEST_MOTOR_BACK
  deployment.Retract();
#endif
#endif
  Serial.begin(115200);
  Serial.setRxBufferSize(1024);
  Wire.begin();
  // LORA SETUP
  lora.begin();

  buzzerNotify.Setup();
  // Stepper setup------------------
  pinMode(stepPin, OUTPUT);
  pinMode(dirPin, OUTPUT);
  // BMP setup---------------------
  buzzerNotify.Trigger();

  // Bluetooth setup---------------
  BLEDevice::init("SOAR_DeployModule");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  pCharacteristic = pService->createCharacteristic(
      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
      BLECharacteristic::PROPERTY_NOTIFY);

  pCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
      BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();
  // Start advertising
  pServer->getAdvertising()->start();
  Serial.println("Waiting a client connection to notify...");
  buzzerNotify.Trigger();
  barometer.Initialize();
  buzzerNotify.Trigger();
  motor.DC_SETUP();
  buzzerNotify.Trigger();

  distanceSensor.begin();
  otaUpdater.Setup();

  // Distance sensor setup
  delay(500);
  lora.sendCommand("AWAKE");
}

void loop()
{
  // Disconnecting
  if (!deviceConnected && oldDeviceConnected)
  {
    delay(500);                  // Give the Bluetooth stack the chance to get things ready
    pServer->startAdvertising(); // Restart advertising
    Serial.println("Advertising started");
    oldDeviceConnected = deviceConnected;
  }

  // // Connecting
  if (deviceConnected && !oldDeviceConnected)
  {
    // do stuff here on connecting
    oldDeviceConnected = deviceConnected;
  }

  // Automated Altitude Trigger Check
  static float altitude;
  static bool valid_value=true;
#if FAKE_ALT_DATA
  altitude = GetFakeAltitude();
    
#else
  altitude = barometer.get_last_altitude_reading();
  altimeter_latest = altitude;
#endif
  valid_value = altTrigger.CheckAltitude(altitude);
  int descending = altTrigger.state;
  
#if DEBUG_ALT
  Serial.println("Altitude: "+String(altitude)+" | Max: "+String(altTrigger.GetMaxAltitude()) + "| State: "+String(descending)+"| Outlier:" String(!valid_value));
#endif

#if FAKE_ALT_DATA
  SendAltitudeData(altitude, altTrigger.GetMaxAltitude(), descending, !valid_value);
#endif

  if (descending == 3 && forwardStatus == false)
  {
    Serial.println("Triggering deployment");
    forwardStatus = true;
    deployment.Deploy();
  }
  else if (descending == 4 && backwardStatus == false)
  {
      Serial.println("Low altitude detected");
      if (deployment.GetStatus() == "FORWARD")
      { // In case we haven't finished extended by the time we reach the lower altitude
        deployment.Stop();
        deployment.Reset();
      }
      backwardStatus = true;
      deployment.Retract();
  }

  // Deployment Procedure Constant Check
  deployment.ProcedureCheck();

  if (lora.available())
  {
    String incomingString = "";
    Serial.print("Request Received: ");
    String data_str = lora.read();
    if (data_str == "PING")
    {
      lora.queueCommand("PONG");
    }
    else if (data_str == "DEPLOY")
    {
      Serial.println("Deployment Triggered");
      deployment.Deploy();
      lora.queueCommand("DEPLOY:TRIGGERING");
    }
    else if (data_str == "STOP")
    {
      deployment.Stop();
      lora.queueCommand("DEPLOY:STOPING");
    }
    else if (data_str == "RESET")
    {
      deployment.Reset();
      lora.queueCommand("DEPLOY:RESETING");
    }
    else if (data_str == "STATUS")
    {
      String stat = "DEPLOY-STATUS:" + deployment.GetStatus();
      lora.queueCommand(stat);
    }
    else if (data_str == "RETRACT")
    {
      deployment.Retract();
      lora.queueCommand("DEPLOY:RETRACTING");
    }
    else if (data_str == "ALTITUDE")
    {
      char altimeter_latest_str[9];
      dtostrf(altimeter_latest, 4, 2, altimeter_latest_str);
      char altitude_str[100] = "ALTITUDE:";
      strcat(altitude_str, altimeter_latest_str);
      lora.queueCommand(altitude_str);
    }
    else if (data_str == "DISTANCE")
    {
      char distance_data[5];
      sprintf(distance_data, "%u", distanceSensor.readDistance());
      char distance_str[100] = "DISTANCE:";
      strcat(distance_str, distance_data);
      lora.queueCommand(distance_str);
    }
    else if (data_str.indexOf("THRESHOLD") >= 0)
    {
      try
      {
        for (int i = 0; i < data_str.length(); i++)
        {
          if (data_str[i] == ':')
          {
            String curstr = data_str.substring(i + 1);
            int new_trsh = curstr.toInt();
            ALT_TRSH_CHECK = new_trsh;
            break;
          }
        }
#if DEBUG_TRSHSET
        Serial.print("New Trsh: ");
        Serial.println(ALT_TRSH_CHECK);
#endif
        lora.queueCommand("THRESHOLD:SET");
      }
      catch (String error)
      {
        lora.queueCommand("THRESHOLD:ERROR");
      }
    }
    else if (data_str == "JOG:FWD")
    {
      lora.queueCommand("JOG:FWD+RCV");
      motor.DC_MOVE(50);
      delay(700);
      motor.DC_STOP();
    }
    else if (data_str == "JOG:REV")
    {
      lora.queueCommand("JOG:REV+RCV");
      motor.DC_MOVE(-50);
      delay(700);
      motor.DC_STOP();
    }
    else
    {
      lora.queueCommand(data_str + "+INVALID");
    }
  }
  // Vital Sign Indicator
  lora.handleQueue();
  buzzerNotify.Check();
  otaUpdater.Handle();
}
