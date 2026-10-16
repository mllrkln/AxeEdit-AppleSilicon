// Editor features rebuilt from the original's embedded firmware model and dialogs:
// graph monitors, CPU estimate, audio pass-through meters, modifier editing, CSV and firmware update.
#pragma once
#include "Model.h"
#include "AxeMidi.h"

//==============================================================================
// The editor's graph monitors. Curves are computed the way the firmware's filter_set / paraEQ_set
// (biquad response at 64 log-spaced points, 20 Hz - 20 kHz) and calcGraph (ADSR, 100 points) do.
namespace Graphs
{
    struct Curve
    {
        Array<float> points;
        float minimum = 0, maximum = 1;
        StringArray labels;
        int gridLines = 0;
    };

    // monitor: the config page's "monitor" attribute (FilterGraph, PEQGraph, ControllersGraph).
    bool compute (const Document&, int effectId, const String& monitor, const String& pageName, Curve&);
    void paint (Graphics&, Rectangle<float> area, const Curve&);

    // |H(e^jw)| in dB of an RBJ biquad, as the firmware's calcBiquad + calcResponse.
    // type: 0 none, 1 low-pass, 2 band-pass, 3 high-pass, 4 low shelf, 5 high shelf, 6 peaking, 7 notch.
    double biquadDb (int type, double freq, double q, double gainDb, double atHz, double sampleRate = 48000.0);
}

//==============================================================================
// CPU estimate: the original added the firmware's per-block cost (BLOCK::cpu, set in each *_init)
// for every block in the grid and showed sum / 350 on the meter and sum / 350 * 300 in the field.
double cpuUnits (const Document&);

//==============================================================================
// Audio pass-through for the level meters: the original ran Mac audio through its AxeFxFilter
// (gain, meters, clip LEDs) when an audio device was chosen in its settings.
class AudioMonitor : public AudioIODeviceCallback
{
public:
    AudioMonitor();
    ~AudioMonitor() override;

    bool isRunning() const                  { return running; }
    void restore (const String& stateXml);  // reopens the devices chosen last time (if audio was enabled)
    void enable (bool shouldRun, String& error);
    String state() const;
    AudioDeviceManager manager;

    std::atomic<float> inputGain { 1.0f }, outputGain { 1.0f };
    float level (bool output, int channel) const     { return peaks[output ? 1 : 0][jlimit (0, 1, channel)].load(); }
    bool takeClip (bool output)                      { return clip[output ? 1 : 0].exchange (false); }

private:
    bool running = false;
    std::atomic<float> peaks[2][2] {};
    std::atomic<bool> clip[2] {};

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (AudioIODevice*) override {}
    void audioDeviceStopped() override {}
};

//==============================================================================
// Modifier editor (the original's ModifierDialog): source, curve points, scale/offset, damping,
// auto-engage, PC reset and the off value of the modifier attached to one block parameter.
class ModifierEditor : public Component, private Timer
{
public:
    // apply(chunkId, valueIndex, raw) is called for every change; the editor owns undo and MIDI.
    ModifierEditor (Document&, int effectId, int modifierParam, const String& parameterLabel,
                    std::function<void (int chunkId, int index, int raw)> apply);
    void paint (Graphics&) override;
    void resized() override;
    static void show (Component& parent, Document&, int effectId, int modifierParam, const String& label,
                      std::function<void (int, int, int)> apply);

private:
    Document& doc;
    int effectId, modParam, chunkId = -1;
    String label;
    std::function<void (int, int, int)> apply;
    ComboBox source, autoEngage, pcReset;
    OwnedArray<Slider> sliders;          // start, mid, end, slope, scale, offset, damping, off value
    OwnedArray<Label> sliderLabels;
    Label info;
    Rectangle<int> graphArea, engageLabel, resetLabel;
    bool updating = false;

    int raw (int index) const;
    void set (int index, int rawValue);
    void refresh();
    void timerCallback() override       { refresh(); }
};

//==============================================================================
// Preset <-> CSV in the original CSVManager layout ("protocol 1"):
//   version,10.0,"name", 48 x (id,"block",mask), 17 x 13 modifier values,
//   for every block of the effect pool: id,"name", then index,"parameter",value per parameter
//   (unused blocks are written as 0,"" with zeros), and finally "EOF".
namespace Csv
{
    String exportPreset (const Document&, const Preset&);
    bool importPreset (const Document&, const String& text, Preset& out, String& error);
}

//==============================================================================
// Firmware update (the original's AxeFxUpdateFirmware): sends a firmware .syx file message by
// message, waits for the unit to install it, then asks for the version again.
class FirmwareUpdater : private Timer
{
public:
    FirmwareUpdater (AxeMidi&);
    // Returns false (with a reason) when the file isn't a usable SysEx file.
    bool start (const File& syx, String& error);
    bool isRunning() const                  { return stage != Idle; }
    void cancel();
    double progress() const;
    String statusText() const               { return status; }
    std::function<void (bool success, const String& message)> onFinished;

    // From the main window's AxeMidi listener.
    void versionReceived (int major, int minor);
    int oldMajor = -1, oldMinor = -1;

private:
    enum Stage { Idle, Sending, Installing, Polling };
    AxeMidi& midi;
    Stage stage = Idle;
    Array<MemoryBlock> messages;
    int next = 0;
    int64 totalBytes = 0, sentBytes = 0;
    uint32 stageStart = 0;
    int newMajor = -1, newMinor = -1;
    String status;

    void sendNext();
    void finish (bool ok, const String&);
    void timerCallback() override;
};
