#ifndef LORA_H
#define LORA_H

#include "_config.h"
#include <Arduino.h>
#include <HardwareSerial.h>
// #include <queue>
template<typename T, size_t Size>
class Queue {
    T data[Size];
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;

public:
    void push(const T& value) {
        if (count < Size) {
            data[tail] = value;
            tail = (tail + 1) % Size;
            ++count;
        }
    }

    T pop() {
        T value = T(); // Default value in case of underflow
        if (count > 0) {
            value = data[head];
            head = (head + 1) % Size;
            --count;
        }
        return value;
    }

    T front() const {
        return (count > 0) ? data[head] : T(); // Return the front element or a default value if empty
    }

    bool empty() const {
        return count == 0;
    }

    bool isFull() const {
        return count == Size;
    }

    size_t size() const {
        return count;
    }
};



struct Packet {
    byte *data;
    int length;
    int address;
};

class SOAR_Lora {
public:
    SOAR_Lora(String address, String network_id, String frequency);
    void begin();
    // void sendCommand(String command);
    String sendATCommand(const byte* command, int length, unsigned long timeout=1000);
    void loraSendStr(const char* toSend, unsigned long timeout=1000);
    void handleQueue();
    bool available();
    bool read(int *address, int *length, byte **data, int *rssi, int *snr);
    void beginPacket();
    void sendSingleStr(const char* str, int address=6);
    void sendChar(const char *str);
    void sendFloat(float value);
    void sendInt(int value);
    void sendLong(uint32_t value);
    bool bytesToInt(byte* data, int start, int* value);
    void endPacket(int address=6);
    bool checkChecksum(byte* data, int length);
    bool matchBytes(byte* data, const char* match, int start=0);
    bool bytesToFloat(byte* data, int start, float* value);

private:
    HardwareSerial* loraSerial;
    Queue<Packet, 10> messageQueue;
    const unsigned long MIN_QUEUE_TIME = 1000; 
    unsigned long lastSentTime;
    String address;
    String network_id;
    String frequency;
    byte packetBuffer[64]; // Adjust size as needed
    size_t bufferIndex = 0;
    void loraSend(const byte* toSend, int length, unsigned long timeout=1000);
};

#endif // LORA_H
