// Mock Arduino.h for native unit testing
// Provides minimal type definitions needed by application code
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>

// Basic Arduino types
typedef uint8_t byte;
typedef bool boolean;

// String class stub (minimal implementation)
class String {
public:
    String() : data_("") {}
    String(const char* s) : data_(s ? s : "") {}
    String(const std::string& s) : data_(s) {}
    String(int val) : data_(std::to_string(val)) {}
    String(float val, int decimals = 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, val);
        data_ = buf;
    }
    
    const char* c_str() const { return data_.c_str(); }
    size_t length() const { return data_.length(); }
    bool isEmpty() const { return data_.empty(); }
    
    String& operator+=(const String& other) { data_ += other.data_; return *this; }
    String& operator+=(const char* s) { if(s) data_ += s; return *this; }
    String operator+(const String& other) const { return String(data_ + other.data_); }
    bool operator==(const String& other) const { return data_ == other.data_; }
    bool operator!=(const String& other) const { return data_ != other.data_; }
    
    int toInt() const { return std::stoi(data_); }
    float toFloat() const { return std::stof(data_); }
    
private:
    std::string data_;
};

// Serial stub
class SerialClass {
public:
    void begin(unsigned long) {}
    void print(const char*) {}
    void print(int) {}
    void print(float, int = 2) {}
    void println(const char* = "") {}
    void println(int) {}
    void println(float, int = 2) {}
    void printf(const char*, ...) {}
};
extern SerialClass Serial;

// Math functions
#ifndef PI
#define PI 3.14159265358979323846
#endif

#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295
#endif

#ifndef RAD_TO_DEG
#define RAD_TO_DEG 57.29577951308232
#endif

inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

template<typename T>
T constrain(T x, T a, T b) {
    return (x < a) ? a : ((x > b) ? b : x);
}

inline long random(long max) { return rand() % max; }
inline long random(long min, long max) { return min + rand() % (max - min); }

// Time functions (stub)
inline unsigned long millis() { return 0; }
inline unsigned long micros() { return 0; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}

// GPIO stubs
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0

inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }
inline int analogRead(int) { return 0; }
