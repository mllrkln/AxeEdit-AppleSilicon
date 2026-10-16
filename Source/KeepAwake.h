// Keeps macOS from App-Napping the process while it lives (long MIDI transfers).
// juce::ScopedLowPowerModeDisabler can't be used: it doesn't retain the activity token, so ending it crashes.
#pragma once

class KeepAwake
{
public:
    explicit KeepAwake (const char* reason);
    ~KeepAwake();
    KeepAwake (const KeepAwake&) = delete;
    KeepAwake& operator= (const KeepAwake&) = delete;
private:
    void* activity = nullptr;
};
