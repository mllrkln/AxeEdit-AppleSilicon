#include "AxeMidi.h"

// AXE_MIDI_TRACE=1 prints short SysEx in/out and the size of every incoming message to stderr.
static bool traceMidi()
{
    static const bool on = SystemStats::getEnvironmentVariable ("AXE_MIDI_TRACE", {}).isNotEmpty();
    return on;
}

AxeMidi::AxeMidi() {}
AxeMidi::~AxeMidi() { close(); }

bool AxeMidi::open (const String& inputId, const String& outputId, String& error)
{
    close();

    for (auto& d : MidiOutput::getAvailableDevices())
        if (d.identifier == outputId)
        {
            output = MidiOutput::openDevice (d.identifier);
            outputName = d.name;
            this->outputId = d.identifier;
        }

    for (auto& d : MidiInput::getAvailableDevices())
        if (d.identifier == inputId)
        {
            input = MidiInput::openDevice (d.identifier, this);
            inputName = d.name;
        }

    if (output == nullptr || input == nullptr)
    {
        error = output == nullptr ? "Couldn't open the MIDI output." : "Couldn't open the MIDI input.";
        close();
        return false;
    }

    input->start();
    startTimer (30);
    versionAttempts = 0;
    queryVersion();
    return true;
}

void AxeMidi::close()
{
    stopTimer();
    if (input != nullptr)
        input->stop();
    input.reset();
    output.reset();
    connected = false;
    const ScopedLock sl (lock);
    pending.clear();
}

void AxeMidi::sendRaw (const void* data, size_t size)
{
    // Nothing may be interleaved into a long dump that is still on its way out.
    if (traceMidi() && size < 12)
        std::cerr << "TRACE out " << Time::getMillisecondCounter() << " " << String::toHexString (data, (int) size) << (output == nullptr || sender.busy() || exclusive ? " BLOCKED" : "") << std::endl;
    if (output == nullptr || sender.busy() || exclusive)
        return;
    output->sendMessageNow (MidiMessage (data, (int) size));
    if (listener != nullptr)
        listener->axeActivity (false);
}

// Builds F0 <manufacturer> <model> <bytes...> F7.
void AxeMidi::send (std::initializer_list<int> bytes)
{
    MemoryOutputStream out;
    out.writeByte ((char) 0xf0);
    out.write (manufacturer, 3);
    out.writeByte ((char) model);
    for (int b : bytes)
        out.writeByte ((char) (b & 0x7f));
    out.writeByte ((char) 0xf7);
    sendRaw (out.getData(), out.getDataSize());
}

void AxeMidi::queryVersion()
{
    ++versionAttempts;
    send ({ Sysex::QueryVersion, 0, 0 });
}

void AxeMidi::requestPreset (bool editBuffer, int number)
{
    send ({ Sysex::PatchDump, editBuffer ? 1 : 0, number & 0x0f, (number >> 4) & 0x7f });
}

void AxeMidi::sendPreset (const Preset& p, bool editBuffer, int number)
{
    const auto data = Sysex::encodePreset (p, model, manufacturer, editBuffer ? 1 : 0, editBuffer ? 0 : number);
    sendRaw (data.getData(), data.getSize());
}

void AxeMidi::sendParam (int effect, int param, int value, bool set)
{
    send ({ Sysex::ParamSet, effect & 0x0f, (effect >> 4) & 0x0f, param & 0x0f, (param >> 4) & 0x0f,
            value & 0x0f, (value >> 4) & 0x0f, set ? 1 : 0 });
}

void AxeMidi::setParam (int effect, int param, int value)
{
    const ScopedLock sl (lock);
    pending[{ effect, param }] = jlimit (0, 255, value);
}

void AxeMidi::queryParam (int effect, int param)       { sendParam (effect, param, 0, false); }

void AxeMidi::sendGrid (const Preset& p)
{
    MemoryOutputStream out;
    out.writeByte ((char) 0xf0);
    out.write (manufacturer, 3);
    out.writeByte ((char) model);
    out.writeByte ((char) Sysex::SetGrid);

    uint8 ck = 0;
    for (auto& c : p.grid)
        for (uint8 v : { c.id, c.mask })
        {
            out.writeByte ((char) (v & 0x0f));
            out.writeByte ((char) (v >> 4));
            ck ^= v;
        }

    out.writeByte ((char) (ck & 0x0f));
    out.writeByte ((char) (ck >> 4));
    out.writeByte ((char) 0xf7);
    sendRaw (out.getData(), out.getDataSize());
}

bool AxeMidi::sendPaced (const MemoryBlock& data, std::function<void (bool)> done)
{
    if (output == nullptr || sender.busy())
        return false;

    int index = -1;
    auto devices = MidiOutput::getAvailableDevices();
    for (int i = 0; i < devices.size(); ++i)
        if (devices[i].identifier == outputId)
            index = i;

    std::vector<uint8_t> bytes ((const uint8_t*) data.getData(), (const uint8_t*) data.getData() + data.getSize());
    if (listener != nullptr)
        listener->axeActivity (false);

    return sender.start (index, std::move (bytes), [done] (bool ok)
    {
        MessageManager::callAsync ([done, ok] { if (done) done (ok); });
    });
}

void AxeMidi::changeProgram (int number, int channel)
{
    if (output == nullptr || sender.busy() || exclusive)
        return;
    output->sendMessageNow (MidiMessage::controllerEvent (channel, 0, number / 128));
    output->sendMessageNow (MidiMessage::programChange (channel, number % 128));
}

void AxeMidi::setTuner (bool on, int channel, int cc)
{
    if (output != nullptr && ! sender.busy() && ! exclusive)
        output->sendMessageNow (MidiMessage::controllerEvent (channel, cc, on ? 127 : 0));
}

void AxeMidi::sendController (int cc, int value, int channel)
{
    if (output != nullptr && ! sender.busy() && ! exclusive)
        output->sendMessageNow (MidiMessage::controllerEvent (channel, cc & 0x7f, value & 0x7f));
}

// AxeFxMIDIProtocol::setModifier: F0 <id> <model> 07 eff(2) param(2) index(2) value(2) 01 F7, nibbles low first.
void AxeMidi::setModifier (int effect, int modifierParam, int index, int value)
{
    send ({ Sysex::ModifierSet, effect & 0x0f, (effect >> 4) & 0x0f, modifierParam & 0x0f, (modifierParam >> 4) & 0x0f,
            index & 0x0f, (index >> 4) & 0x0f, value & 0x0f, (value >> 4) & 0x0f, 1 });
}

void AxeMidi::timerCallback()
{
    std::map<std::pair<int, int>, int> toSend;
    {
        const ScopedLock sl (lock);
        toSend.swap (pending);
    }
    if (sender.busy() || exclusive)
    {
        const ScopedLock sl (lock);     // keep edits until the dump has gone out
        for (auto& p : toSend)
            pending.insert (p);
        return;
    }
    for (auto& p : toSend)
        sendParam (p.first.first, p.first.second, p.second, true);

    // The unit may use either the original (00 00 7D) or the later (00 01 74) manufacturer id.
    static int ticks = 0;
    if (! connected && output != nullptr && ++ticks % 40 == 0 && versionAttempts < 8)
    {
        const bool alt = (versionAttempts % 2) == 1;
        manufacturer[1] = alt ? 0x01 : 0x00;
        manufacturer[2] = alt ? 0x74 : 0x7d;
        queryVersion();
    }
}

void AxeMidi::handleIncomingMidiMessage (MidiInput*, const MidiMessage& m)
{
    // The SysEx protocol has no "current preset" query, so preset changes are followed through
    // the bank select / program change messages the unit (or a foot controller on a merge) sends.
    if (m.isController() && m.getControllerNumber() == 0)
    {
        bankSelect = m.getControllerValue();
        return;
    }

    if (m.isProgramChange())
    {
        const int number = bankSelect * 128 + m.getProgramChangeNumber();
        MessageManager::callAsync ([this, number] { if (listener != nullptr && input != nullptr) listener->axeProgram (number); });
        return;
    }

    if (! m.isSysEx())
        return;
    if (traceMidi())
        std::cerr << "TRACE in " << Time::getMillisecondCounter() << " " << String::toHexString (m.getRawData(), jmin (12, m.getRawDataSize()))
                  << " (" << m.getRawDataSize() << " bytes)" << std::endl;

    MemoryBlock data (m.getRawData(), (size_t) m.getRawDataSize());
    MessageManager::callAsync ([this, data] { handleSysex (data); });
}

void AxeMidi::handleSysex (const MemoryBlock& mb)
{
    auto* d = (const uint8*) mb.getData();
    const int len = (int) mb.getSize();
    if (listener == nullptr || input == nullptr || len < 7 || d[0] != 0xf0)
        return;

    const bool fractal = (d[1] == 0 && d[2] == 0 && d[3] == 0x7d) || (d[1] == 0 && d[2] == 1 && d[3] == 0x74);
    if (! fractal)
        return;

    listener->axeActivity (true);

    switch (d[5])
    {
        case Sysex::QueryVersion:
            if (len >= 9)
            {
                memcpy (manufacturer, d + 1, 3);
                connected = true;
                listener->axeVersion (d[4], d[6], d[7]);
            }
            break;

        case Sysex::PatchRcv:
        {
            if (len == Sysex::bankMessageSize)
            {
                OwnedArray<Preset> presets;
                int m = 0; uint8 man[3], hb[3]; String error;
                if (Sysex::decodeData (d, len, presets, m, man, hb, error))
                    listener->axeBank (d[6], presets);
                else
                    listener->axeLog ("Received a bank dump that failed its checksum.");
                break;
            }
            Preset p;
            int m = 0, target = 0, number = 0;
            if (Sysex::decodePreset (d, len, p, &m, &target, &number))
                listener->axePreset (p, target, number);
            else
                listener->axeLog ("Received a preset dump with a bad checksum or size (" + String (len) + " bytes).");
            break;
        }

        case Sysex::ParamSet:
            if (len >= 13)
            {
                String text;
                for (int i = 12; i < len - 1 && d[i] >= 0x20; ++i)
                    text += (juce_wchar) d[i];
                listener->axeParam (d[6] | (d[7] << 4), d[8] | (d[9] << 4), d[10] | (d[11] << 4), text.trim());
            }
            break;

        case Sysex::Tuner:
            if (len >= 10)
                listener->axeTuner (d[6], d[7], d[8]);
            break;

        case Sysex::Tempo:
            listener->axeTempoBeat();
            break;

        case Sysex::Checksum:
            if (len >= 9)
                listener->axeAck (d[6], d[7] == 1);    // 1 = "Checksum OK" in the original
            break;

        default:
            listener->axeOtherSysex (mb);
            break;
    }
}
