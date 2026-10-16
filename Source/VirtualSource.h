// Test helper: a CoreMIDI virtual source that sends raw bytes with the classic packet API.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

class VirtualSource
{
public:
    explicit VirtualSource (const std::string& name);
    ~VirtualSource();
    bool isValid() const;
    int uniqueId() const;
    void send (const uint8_t* data, size_t size);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

