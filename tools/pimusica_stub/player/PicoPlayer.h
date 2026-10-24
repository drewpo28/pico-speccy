#pragma once
#include <stdint.h>
#include <string>
namespace pp {
constexpr int RATE = 31250;
struct Meta { char format[40]; char title[64]; };
class Decoder {
public:
    virtual ~Decoder() {}
    virtual bool openMemory(const uint8_t* d, uint32_t n) = 0;
    virtual int render(int16_t* lr, int n) = 0;
    virtual int channels() const = 0;
    virtual void levels(uint8_t* out) = 0;
    virtual uint32_t posMs() const = 0;
    virtual uint32_t lenMs() const = 0;
    uint32_t trackLenMs() const { return lenMs(); }
    int play(int16_t* lr, int n) { return render(lr, n); }
    Meta meta {};
    const char* err = nullptr;
};
bool available();
Decoder* createDecoder(const std::string& ext);
}
