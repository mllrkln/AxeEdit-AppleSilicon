// Axe-Fx Standard/Ultra SysEx protocol ("protocol 1" in the original AxeFxMIDIProtocol class).
#pragma once
#include "Model.h"
#include "PacedSysex.h"

class AxeMidi : private MidiInputCallback, private Timer
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void axeVersion (int /*model*/, int /*major*/, int /*minor*/) {}
        virtual void axePreset (const Preset&, int /*target*/, int /*number*/) {}
        virtual void axeParam (int /*effect*/, int /*param*/, int /*value*/, const String& /*text*/) {}
        virtual void axeTuner (int /*note*/, int /*string*/, int /*tune*/) {}
        virtual void axeProgram (int /*number*/) {}      // bank select + program change seen on the input
        virtual void axeBank (int /*target*/, OwnedArray<Preset>& /*presets*/) {}   // target 2..4 = bank A..C
        virtual void axeAck (int /*function*/, bool /*ok*/) {}
        virtual void axeActivity (bool /*incoming*/) {}
        virtual void axeLog (const String&) {}
        virtual void axeOtherSysex (const MemoryBlock&) {}     // Fractal messages this class doesn't handle
        virtual void axeTempoBeat() {}                          // TEMPO (0x10): the unit sends one per beat
    };

    AxeMidi();
    ~AxeMidi() override;

    Listener* listener = nullptr;
    int model = AxeModel::Ultra;
    uint8 manufacturer[3] { 0x00, 0x00, 0x7d };

    bool open (const String& inputId, const String& outputId, String& error);
    void close();
    bool isOpen() const                 { return output != nullptr; }
    bool isConnected() const            { return connected; }
    String inputName, outputName;

    void queryVersion();
    void requestPreset (bool editBuffer, int number);
    void sendPreset (const Preset&, bool editBuffer, int number);
    void setParam (int effect, int param, int value);       // coalesced, like the original's refresh timer
    void queryParam (int effect, int param);
    void setBypass (int effect, bool bypassed)              { sendParam (effect, 0xff, bypassed ? 1 : 0, true); }
    void sendGrid (const Preset&);
    void changeProgram (int number, int channel = 1);
    void setTuner (bool on, int channel = 1, int cc = 15);
    void sendController (int cc, int value, int channel = 1);
    // MODIFIER_SET (07): effect, the knob's modifier slot (200..255), modifier value index (0..12), raw value.
    void setModifier (int effect, int modifierParam, int index, int value);
    void sendSysex (const MemoryBlock& m)                   { sendRaw (m.getData(), m.getSize()); }

    // Librarian transfers. A bank request is answered with one 262,156-byte message.
    void requestBank (int bank)                             { send ({ Sysex::PatchDump, 2 + bank, 0, 0 }); }
    bool sendPaced (const MemoryBlock&, std::function<void (bool ok)> done);   // done runs on the message thread
    bool isTransferring() const                             { return sender.busy() || exclusive; }
    // While a firmware file goes out nothing else may be sent, not even between its messages.
    void setExclusive (bool on)                             { exclusive = on; }
    double sendProgress() const                             { return sender.progress(); }
    void cancelTransfer()                                   { sender.cancel(); }

private:
    std::unique_ptr<MidiInput> input;
    std::unique_ptr<MidiOutput> output;
    bool connected = false;
    std::atomic<bool> exclusive { false };
    int versionAttempts = 0;
    int bankSelect = 0;
    String outputId;
    PacedSysexSender sender;
    std::map<std::pair<int, int>, int> pending;
    CriticalSection lock;

    void send (std::initializer_list<int> afterModel);
    void sendRaw (const void* data, size_t size);
    void sendParam (int effect, int param, int value, bool set);
    void handleIncomingMidiMessage (MidiInput*, const MidiMessage&) override;
    void handleSysex (const MemoryBlock&);
    void timerCallback() override;
};
