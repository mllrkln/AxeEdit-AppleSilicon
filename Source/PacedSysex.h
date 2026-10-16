// Sends one long SysEx message (bank dumps, cab IRs) at MIDI wire speed through CoreMIDI's
// MIDISendSysex, so slow DIN interfaces aren't overrun. The original did the same job by
// splitting dumps into 256-byte pieces with a 90 ms gap (MIDIBufferSize / MIDIBufferDelay).
// Kept free of JUCE headers: CoreMIDI's MacTypes clash with juce names.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class PacedSysexSender
{
public:
    PacedSysexSender();
    ~PacedSysexSender();

    // destinationIndex: position in CoreMIDI's destination list, which is the same order as
    // juce::MidiOutput::getAvailableDevices(). `done` is called on a CoreMIDI thread.
    bool start (int destinationIndex, std::vector<uint8_t> data, std::function<void (bool completed)> done);
    bool busy() const;
    double progress() const;    // 0..1
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
