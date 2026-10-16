// Simulated Axe-Fx Ultra for testing the librarian without hardware.
//
// "--axesim <banks dir>" runs the simulator on virtual CoreMIDI ports named "Axe-Fx Simulator". It answers
// like the firmware code recovered from the original (midiSysexProc, midiDumpPatch, midiRcvPatch,
// midiRcvCabIR): version query, preset/bank dumps, and preset/bank/cab receive with an acknowledge.
// Test-only function 7E: 7E 01 <slot lo> <slot hi> <name...> renames a stored preset on the "unit";
// 7E 02 <user slot> asks for a received cab back; 7E 03 asks for transfer counters; 7E 04 / 05 return the
// last MODIFIER_SET / controller seen; 7E 06 sends three TEMPO beats; 7E 07 reports the test firmware
// (function 7F) received.
//
// "--simtest <banks dir>" starts the simulator as a separate process (as a real unit would be) and
// drives the librarian against it, checking every transfer by reading the data back.
#include "SimTest.h"
#include "Extras.h"
#include "VirtualSource.h"
#include <iostream>
#include <thread>

namespace
{
    std::vector<uint8> unpackValues (const uint8* nibbles, int count)
    {
        std::vector<uint8> v ((size_t) count);
        for (int i = 0; i < count; ++i)
            v[(size_t) i] = (uint8) ((nibbles[i * 2] & 0x0f) | (nibbles[i * 2 + 1] << 4));
        return v;
    }

    bool checksumOk (const uint8* nibbles, int count)
    {
        uint8 ck = 0;
        for (int i = 0; i < count; ++i)
            ck ^= (uint8) ((nibbles[i * 2] & 0x0f) | (nibbles[i * 2 + 1] << 4));
        return ck == (uint8) ((nibbles[count * 2] & 0x0f) | (nibbles[count * 2 + 1] << 4));
    }

    MemoryBlock message (int function, int b6, int number, const uint8* values, int count)
    {
        MemoryOutputStream out;
        const uint8 header[] = { 0xf0, 0, 0, 0x7d, 1, (uint8) function, (uint8) b6, (uint8) (number & 0x0f), (uint8) ((number >> 4) & 0x7f) };
        out.write (header, sizeof (header));
        uint8 ck = 0;
        for (int i = 0; i < count; ++i)
        {
            out.writeByte ((char) (values[i] & 0x0f));
            out.writeByte ((char) (values[i] >> 4));
            ck ^= values[i];
        }
        out.writeByte ((char) (ck & 0x0f));
        out.writeByte ((char) (ck >> 4));
        out.writeByte ((char) 0xf7);
        return out.getMemoryBlock();
    }

    const char* const simName = "Axe-Fx Simulator";

    class AxeSim : private MidiInputCallback
    {
    public:
        bool start (const File& banksDir)
        {
            for (int b = 0; b < 3; ++b)
            {
                MemoryBlock mb;
                const auto f = banksDir.getChildFile ("Ultra/Ultra_Bank" + String::charToString ((juce_wchar) ('A' + b)) + ".syx");
                if (! f.loadFileAsData (mb) || mb.getSize() != (size_t) Sysex::bankMessageSize)
                    return false;
                banks[b] = unpackValues ((const uint8*) mb.getData() + 9, 128 * 1024);
            }
            out = std::make_unique<VirtualSource> (simName);
            in = MidiInput::createNewDevice (simName, this);
            if (! out->isValid() || in == nullptr)
                return false;
            in->start();
            return true;
        }

    private:
        std::vector<uint8> banks[3];
        std::vector<uint8> cabs[11];
        int presetsStored = 0, banksStored = 0, cabsStored = 0, badChecksums = 0;
        int versionMinor = 0;
        uint8 lastModifier[4] {}, lastCC[3] {};
        int firmwareMessages = 0;
        uint8 firmwareXor = 0;
        std::unique_ptr<VirtualSource> out;
        std::unique_ptr<MidiInput> in;
        std::mutex lock;

        uint8* slot (int n)     { return banks[n / 128].data() + (n % 128) * 1024; }

        void reply (const MemoryBlock& m)
        {
            // Answer from another thread, as the unit's firmware would, not from inside the receive callback.
            std::thread ([this, m]
            {
                Thread::sleep (2);
                out->send ((const uint8_t*) m.getData(), m.getSize());
                if (SystemStats::getEnvironmentVariable ("AXE_MIDI_TRACE", {}).isNotEmpty())
                    std::cerr << "SIM sent " << Time::getMillisecondCounter() << " " << m.getSize() << " bytes" << std::endl;
            }).detach();
        }

        void ack (int function, bool ok)
        {
            const uint8 m[] = { 0xf0, 0, 0, 0x7d, 1, 0x0b, (uint8) function, (uint8) (ok ? 1 : 0), 0xf7 };
            reply (MemoryBlock (m, sizeof (m)));
        }

        void handleIncomingMidiMessage (MidiInput*, const MidiMessage& msg) override
        {
            if (msg.isController())
            {
                const std::lock_guard<std::mutex> l (lock);
                lastCC[0] = (uint8) msg.getControllerNumber();
                lastCC[1] = (uint8) msg.getControllerValue();
                ++lastCC[2];
                return;
            }
            if (! msg.isSysEx())
                return;
            auto* d = msg.getRawData();
            const int len = msg.getRawDataSize();
            if (len < 7 || d[1] != 0 || d[2] != 0 || d[3] != 0x7d)
                return;

            if (SystemStats::getEnvironmentVariable ("AXE_MIDI_TRACE", {}).isNotEmpty())
                std::cerr << "SIM in " << Time::getMillisecondCounter() << " fn " << (int) d[5] << " len " << len << std::endl;
            const std::lock_guard<std::mutex> l (lock);
            switch (d[5])
            {
                case 0x08:
                {
                    const uint8 m[] = { 0xf0, 0, 0, 0x7d, 1, 0x08, 11, (uint8) versionMinor, 0xf7 };
                    reply (MemoryBlock (m, sizeof (m)));
                    break;
                }
                case 0x03:
                {
                    const int target = d[6], number = d[7] | (d[8] << 4);
                    if (target == 0 && number < 384)        reply (message (4, 0, number, slot (number), 1024));
                    else if (target == 1)                   reply (message (4, 1, number, slot (0), 1024));
                    else if (target >= 2 && target <= 4)    reply (message (4, target, 0, banks[target - 2].data(), 128 * 1024));
                    break;
                }
                case 0x04:
                {
                    const int target = d[6], number = d[7] | (d[8] << 4);
                    const int count = target >= 2 ? 128 * 1024 : 1024;
                    if (len != 9 + count * 2 + 3 || ! checksumOk (d + 9, count))
                    {
                        ++badChecksums;
                        ack (4, false);
                        break;
                    }
                    const auto v = unpackValues (d + 9, count);
                    if (target == 0 && number < 384)        { memcpy (slot (number), v.data(), 1024); ++presetsStored; }
                    else if (target >= 2 && target <= 4)    { banks[target - 2] = v; ++banksStored; }
                    ack (4, true);
                    break;
                }
                case 0x0a:
                {
                    if (len != Librarian::cabMessageSize || ! checksumOk (d + 9, 4096) || d[6] < 1 || d[6] > 10)
                    {
                        ++badChecksums;
                        ack (0x0a, false);
                        break;
                    }
                    cabs[d[6]] = unpackValues (d + 9, 4096);
                    ++cabsStored;
                    ack (0x0a, true);
                    break;
                }
                case 0x07:      // MODIFIER_SET: effect, modifier slot, value index, value (nibbles) + 01
                    if (len == 16)
                    {
                        lastModifier[0] = (uint8) (d[6] | (d[7] << 4));
                        lastModifier[1] = (uint8) (d[8] | (d[9] << 4));
                        lastModifier[2] = (uint8) (d[10] | (d[11] << 4));
                        lastModifier[3] = (uint8) (d[12] | (d[13] << 4));
                    }
                    break;
                case 0x7f:      // test "firmware": the last message (d[6] == 1) installs version 11.01 three seconds later
                    ++firmwareMessages;
                    for (int i = 0; i < len; ++i)
                        firmwareXor ^= d[i];
                    if (d[6] == 1)
                        std::thread ([this] { Thread::sleep (3000); const std::lock_guard<std::mutex> l2 (lock); versionMinor = 1; }).detach();
                    break;
                case 0x7e:      // test hooks
                    if (d[6] == 1 && len >= 10)
                    {
                        const int n = d[7] | (d[8] << 7);
                        uint8 name[32];
                        memset (name, ' ', 32);
                        for (int i = 9; i < len - 1 && i - 9 < 32; ++i)
                            name[i - 9] = d[i];
                        memcpy (slot (n) + 2, name, 32);
                    }
                    else if (d[6] == 2 && len >= 9)
                    {
                        const auto& c = cabs[jlimit (0, 10, (int) d[7])];
                        reply (message (0x7e, 2, d[7], c.data(), (int) c.size()));
                    }
                    else if (d[6] == 3)
                    {
                        const uint8 counts[] = { (uint8) presetsStored, (uint8) banksStored, (uint8) cabsStored, (uint8) badChecksums };
                        reply (message (0x7e, 3, 0, counts, 4));
                    }
                    else if (d[6] == 4)
                        reply (message (0x7e, 4, 0, lastModifier, 4));
                    else if (d[6] == 5)
                        reply (message (0x7e, 5, 0, lastCC, 3));
                    else if (d[6] == 6)         // three TEMPO beats, 120 BPM
                        std::thread ([this]
                        {
                            const uint8 beat[] = { 0xf0, 0, 0, 0x7d, 1, 0x10, 0xf7 };
                            for (int i = 0; i < 3; ++i) { Thread::sleep (500); out->send (beat, sizeof (beat)); std::cerr << "TRACE sim beat" << std::endl; }
                        }).detach();
                    else if (d[6] == 7)
                    {
                        const uint8 fw[] = { (uint8) (firmwareMessages & 0xff), (uint8) (firmwareMessages >> 8), firmwareXor };
                        reply (message (0x7e, 7, 0, fw, 3));
                    }
                    break;
                default:
                    break;
            }
        }
    };

    struct Forwarder : AxeMidi::Listener
    {
        Librarian* lib = nullptr;
        FirmwareUpdater* firmware = nullptr;
        bool connected = false;
        int beats = 0;
        MemoryBlock hookReply;
        void axeVersion (int, int major, int minor) override
        {
            connected = true;
            if (firmware != nullptr && firmware->isRunning())
                firmware->versionReceived (major, minor);
        }
        void axeTempoBeat() override                                { ++beats; }
        void axeBank (int target, OwnedArray<Preset>& p) override   { lib->bankReceived (target, p); }
        void axePreset (const Preset& p, int target, int number) override
        {
            if (target == 0)
                lib->presetReceived (number, p);
        }
        void axeAck (int function, bool ok) override                { lib->ackReceived (function, ok); }
        void axeOtherSysex (const MemoryBlock& m) override          { hookReply = m; }
        void axeLog (const String& s) override                      { std::cout << "  log   " << s << std::endl; }
    };

    class SimTest : private Timer
    {
    public:
        SimTest (const File& dir, std::function<void (int)> d) : banksDir (dir), done (std::move (d))
        {
            temp = File::getSpecialLocation (File::tempDirectory).getChildFile ("axeedit-simtest-" + String (Time::currentTimeMillis()));
            temp.createDirectory();
            Librarian::storageRoot() = temp;
            startTimer (100);
        }

    private:
        File banksDir, temp;
        std::function<void (int)> done;
        ChildProcess simulator;
        Document doc;
        AxeMidi midi;
        Forwarder fwd;
        std::unique_ptr<Librarian> lib;
        int step = 0, failures = 0;
        uint32 stepStarted = 0;
        std::vector<uint8> expectedCab;
        std::vector<std::vector<uint8>> snapshot;
        std::unique_ptr<FirmwareUpdater> firmware;
        int firmwareResult = -1, firmwareCount = 0;
        uint8 firmwareXor = 0;

        void check (bool ok, const String& what)
        {
            std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
            if (! ok)
                ++failures;
        }

        std::vector<std::vector<uint8>> bankBytes (int bank)
        {
            std::vector<std::vector<uint8>> v;
            for (auto& e : lib->containers[bank]->entries)
            {
                std::vector<uint8> b (Preset::size);
                e.preset.toBytes (b.data());
                v.push_back (b);
            }
            return v;
        }

        std::vector<std::vector<uint8>> factoryBytes (char bank)
        {
            OwnedArray<Preset> p; int m; uint8 man[3], hb[3]; String error;
            Sysex::loadFile (banksDir.getChildFile (String ("Ultra/Ultra_Bank") + bank + ".syx"), p, m, man, hb, error);
            std::vector<std::vector<uint8>> v;
            for (auto* preset : p)
            {
                std::vector<uint8> b (Preset::size);
                preset->toBytes (b.data());
                v.push_back (b);
            }
            return v;
        }

        void hook (std::initializer_list<int> bytes)
        {
            MemoryOutputStream m;
            for (int b : { 0xf0, 0, 0, 0x7d, 1, 0x7e })
                m.writeByte ((char) b);
            for (int b : bytes)
                m.writeByte ((char) (b & 0x7f));
            m.writeByte ((char) 0xf7);
            fwd.hookReply.reset();
            midi.sendSysex (m.getMemoryBlock());
        }

        std::vector<uint8> hookValues (int count)
        {
            if (fwd.hookReply.getSize() != (size_t) (9 + count * 2 + 3))
                return {};
            return unpackValues ((const uint8*) fwd.hookReply.getData() + 9, count);
        }

        void next()                 { ++step; stepStarted = Time::getMillisecondCounter(); }

        void startEditorChecks()
        {
            std::cout << "Modifier, tap tempo and tempo beats" << std::endl;
            midi.setModifier (131, 200, Preset::ModEnd, 201);
            hook ({ 4 });
            step = 12;
            stepStarted = Time::getMillisecondCounter();
        }
        bool waited (uint32 ms)     { return Time::getMillisecondCounter() - stepStarted > ms; }

        void finish()
        {
            stopTimer();
            lib.reset();
            midi.close();
            simulator.kill();
            Librarian::storageRoot() = File();
            temp.deleteRecursively();
            std::cout << (failures == 0 ? "simtest passed" : "simtest FAILED (" + String (failures) + ")") << std::endl;
            done (failures);
        }

        static String findDevice (const Array<MidiDeviceInfo>& devices)
        {
            for (auto& d : devices)
                if (d.name == simName)
                    return d.identifier;
            return {};
        }

        void timerCallback() override
        {
            switch (step)
            {
                case 0:
                {
                    std::cout << "Starting the simulated Axe-Fx Ultra in its own process" << std::endl;
                    const auto exe = File::getSpecialLocation (File::currentExecutableFile).getFullPathName();
                    if (SystemStats::getEnvironmentVariable ("AXE_SIM_EXTERNAL", {}).isNotEmpty())
                        check (true, "using a simulator that is already running");
                    else
                        check (simulator.start (StringArray { exe, "--axesim", banksDir.getFullPathName() }, 0), "simulator process started");
                    next();
                    break;
                }
                case 1:
                {
                    const auto inId = findDevice (MidiInput::getAvailableDevices()), outId = findDevice (MidiOutput::getAvailableDevices());
                    if ((inId.isEmpty() || outId.isEmpty()) && ! waited (10000))
                        break;
                    lib = std::make_unique<Librarian> (doc, midi);
                    fwd.lib = lib.get();
                    midi.listener = &fwd;
                    String error;
                    check (midi.open (inId, outId, error), "opened the simulator's MIDI ports " + error);
                    next();
                    break;
                }
                case 2:
                    if (fwd.connected || waited (8000))
                    {
                        check (fwd.connected, "firmware version query answered");
                        if (! fwd.connected) { finish(); return; }
                        if (SystemStats::getEnvironmentVariable ("AXE_SIMTEST_QUICK", {}).isNotEmpty())
                        {
                            startEditorChecks();    // developer shortcut: only the editor's MIDI features
                            return;
                        }
                        std::cout << "Downloading banks A and C" << std::endl;
                        lib->downloadBanks (1 | 4);
                        next();
                    }
                    break;
                case 3:
                    if (! lib->isBusy() || waited (250000))
                    {
                        check (lib->lastError.isEmpty(), "downloads finished without error " + lib->lastError);
                        check (bankBytes (0) == factoryBytes ('A'), "bank A matches the unit byte for byte");
                        check (bankBytes (2) == factoryBytes ('C'), "bank C matches the unit byte for byte");
                        check (lib->containers[1]->entries.getReference (0).state == LibEntry::Unknown, "bank B still unknown");

                        auto& a = *lib->containers[0];
                        const auto name0 = a.entries.getReference (0).preset.name;
                        lib->rename (a, 5, "SIMTEST RENAMED");
                        lib->moveRows (a, { 0 }, 3);       // 0 -> 2
                        check (a.entries.getReference (2).preset.name == name0, "moving preset 0 below slot 2 lands it in slot 2");
                        int editedCount = 0;
                        for (auto& e : a.entries)
                            editedCount += e.state == LibEntry::Edited ? 1 : 0;
                        check (editedCount == 4, "rename + move mark 4 presets edited (got " + String (editedCount) + ")");
                        snapshot = bankBytes (0);
                        std::cout << "Uploading the edited presets" << std::endl;
                        lib->uploadChanged (0);
                        next();
                    }
                    break;
                case 4:
                    if (! lib->isBusy() || waited (250000))
                    {
                        check (lib->lastError.isEmpty(), "preset uploads finished without error " + lib->lastError);
                        bool allSynced = true;
                        for (auto& e : lib->containers[0]->entries)
                            allSynced = allSynced && e.state == LibEntry::Synced;
                        check (allSynced, "bank A all synced after the upload");

                        std::cout << "Reading bank A back from the unit" << std::endl;
                        lib->downloadBanks (1);
                        next();
                    }
                    break;
                case 5:
                    if (! lib->isBusy() || waited (250000))
                    {
                        check (bankBytes (0) == snapshot, "unit's bank A now holds exactly the librarian's edits");
                        check (lib->containers[0]->entries.getReference (5).preset.name == "SIMTEST RENAMED", "renamed preset is in slot 5");

                        // Change a preset on the "unit", then fetch just that one.
                        hook ({ 1, 300 & 0x7f, 300 >> 7, 'C', 'H', 'A', 'N', 'G', 'E', 'D', ' ', 'O', 'N', ' ', 'U', 'N', 'I', 'T' });
                        lib->enqueue ({ Librarian::Job::DownloadPreset, 300 });
                        next();
                    }
                    break;
                case 6:
                    if (! lib->isBusy() || waited (250000))
                    {
                        check (lib->lastError.isEmpty(), "single preset download finished without error " + lib->lastError);
                        check (lib->containers[2]->entries.getReference (300 - 256).preset.name == "CHANGED ON UNIT",
                               "single preset download (slot 300) picked up the change");

                        expectedCab.resize (4096);
                        Random r (42);
                        for (auto& v : expectedCab) v = (uint8) r.nextInt (256);
                        auto cab = message (0x0a, 7, 0, expectedCab.data(), 4096);
                        const auto f = temp.getChildFile ("test cab.syx");
                        f.replaceWithData (cab.getData(), cab.getSize());
                        String error;
                        check (lib->loadCab (2, f, error), "loaded a cab file " + error);
                        std::cout << "Uploading a cab to USER 3" << std::endl;
                        lib->enqueue ({ Librarian::Job::UploadCab, 2 });
                        next();
                    }
                    break;
                case 7:
                    if (! lib->isBusy() || waited (250000))
                    {
                        check (lib->lastError.isEmpty(), "cab upload finished without error " + lib->lastError);
                        check (lib->cabs[2].state == LibEntry::Synced, "cab marked synced");
                        hook ({ 2, 3 });
                        next();
                    }
                    break;
                case 8:
                    if (fwd.hookReply.getSize() > 0 || waited (5000))
                    {
                        check (hookValues (4096) == expectedCab, "cab arrived intact in USER 3 (slot byte rewritten, checksum valid)");

                        String error;
                        auto* file = lib->openFile (banksDir.getChildFile ("Ultra/Ultra_BankB.syx"), error);
                        check (file != nullptr, "opened Ultra_BankB.syx " + error);
                        if (file == nullptr) { finish(); return; }
                        auto& b = *lib->containers[1];
                        b.checkpoint();
                        for (int i = 0; i < 128; ++i)
                            b.entries.getReference (i).preset = file->entries.getReference (i).preset;
                        b.entries.getReference (127).preset.name = "LAST ONE";
                        lib->touched (b, {});
                        snapshot = bankBytes (1);
                        std::cout << "Uploading the whole of bank B (paced at MIDI speed, about 85 s)" << std::endl;
                        lib->uploadBanks (2);
                        next();
                    }
                    break;
                case 9:
                    if (! lib->isBusy() || waited (250000))
                    {
                        check (lib->lastError.isEmpty(), "bank upload finished without error " + lib->lastError);
                        std::cout << "Sync & backup of bank B (reads it back from the unit)" << std::endl;
                        lib->syncAndBackup (2);
                        next();
                    }
                    break;
                case 10:
                    if (! lib->isBusy() || waited (250000))
                    {
                        check (bankBytes (1) == snapshot, "unit's bank B matches what was uploaded");
                        check (lib->containers[1]->entries.getReference (127).preset.name == "LAST ONE", "edited last preset arrived");
                        auto backups = Librarian::backupsDir().findChildFiles (File::findFiles, true, "*.syx");
                        OwnedArray<Preset> p; int m; uint8 man[3], hb[3]; String error;
                        check (backups.size() == 1 && Sysex::loadFile (backups[0], p, m, man, hb, error) && hb[0] == 3 && p.size() == 128,
                               "backup written as a loadable Bank B file");
                        hook ({ 3 });
                        next();
                    }
                    break;
                case 11:
                    if (fwd.hookReply.getSize() > 0 || waited (5000))
                    {
                        const auto counts = hookValues (4);
                        check (counts.size() == 4 && counts[0] == 4 && counts[1] == 1 && counts[2] == 1,
                               "unit counted 4 preset stores, 1 bank, 1 cab");
                        check (counts.size() == 4 && counts[3] == 0, "no checksum errors on the unit side");

                        const auto before = bankBytes (0);
                        lib.reset();
                        lib = std::make_unique<Librarian> (doc, midi);
                        fwd.lib = lib.get();
                        check (bankBytes (0) == before, "a fresh librarian reloads the cached banks identically");
                        check (lib->cabs[2].name == "test cab" && lib->cabs[2].state == LibEntry::Synced, "cab slot reloads from the cache");
                        // The editor checks get a freshly started simulator: after the 262 kB bursts a virtual
                        // CoreMIDI source sends, it sometimes delivers nothing more to the app for seconds.
                        std::cout << "Restarting the simulator for the editor checks" << std::endl;
                        midi.close();
                        fwd.connected = false;
                        simulator.kill();
                        step = 21;
                        stepStarted = Time::getMillisecondCounter();
                    }
                    break;
                case 21:
                    if (waited (1500))
                    {
                        const auto exe = File::getSpecialLocation (File::currentExecutableFile).getFullPathName();
                        if (SystemStats::getEnvironmentVariable ("AXE_SIM_EXTERNAL", {}).isEmpty())
                            check (simulator.start (StringArray { exe, "--axesim", banksDir.getFullPathName() }, 0), "simulator restarted");
                        next();
                    }
                    break;
                case 22:
                {
                    const auto inId = findDevice (MidiInput::getAvailableDevices()), outId = findDevice (MidiOutput::getAvailableDevices());
                    if ((inId.isEmpty() || outId.isEmpty() || ! waited (1000)) && ! waited (10000))
                        break;
                    String error;
                    check (midi.open (inId, outId, error), "reopened the simulator's MIDI ports " + error);
                    next();
                    break;
                }
                case 23:
                    if (fwd.connected || waited (8000))
                    {
                        check (fwd.connected, "restarted simulator answered");
                        if (! fwd.connected) { finish(); return; }
                        startEditorChecks();
                    }
                    break;
                case 12:
                    if (fwd.hookReply.getSize() > 0 || waited (3000))
                    {
                        const auto v = hookValues (4);
                        check (v.size() == 4 && v[0] == 131 && v[1] == 200 && v[2] == Preset::ModEnd && v[3] == 201,
                               "unit decoded MODIFIER_SET as Filter 1 / slot 200 / End = 201");
                        midi.sendController (14, 1);
                        fwd.hookReply.reset();      // the check below must wait for hook 5's own reply
                        Timer::callAfterDelay (200, [this] { hook ({ 5 }); });
                        next();
                    }
                    break;
                case 13:
                    if (fwd.hookReply.getSize() > 0 || waited (3000))
                    {
                        const auto v = hookValues (3);
                        check (v.size() == 3 && v[0] == 14 && v[1] == 1 && v[2] >= 1, "unit received the tap-tempo CC 14 (value 1)");
                        fwd.beats = 0;
                        hook ({ 6 });
                        next();
                    }
                    break;
                case 14:
                    if (fwd.beats >= 3 || waited (4000))
                    {
                        check (fwd.beats == 3, "three TEMPO beats reached the listener (got " + String (fwd.beats) + ")");

                        std::cout << "Firmware update (synthetic 60 kB file)" << std::endl;
                        MemoryOutputStream file;
                        Random rng (42);
                        for (int m = 0; m < 60; ++m)
                        {
                            const uint8 head[] = { 0xf0, 0, 0, 0x7d, 1, 0x7f, (uint8) (m == 59 ? 1 : 0) };
                            file.write (head, sizeof (head));
                            for (int i = 0; i < 1000; ++i) file.writeByte ((char) rng.nextInt (128));
                            file.writeByte ((char) 0xf7);
                        }
                        firmwareXor = 0;
                        auto* b = (const uint8*) file.getData();
                        for (size_t i = 0; i < file.getDataSize(); ++i) firmwareXor ^= b[i];
                        firmwareCount = 60;
                        const auto f = temp.getChildFile ("firmware.syx");
                        f.replaceWithData (file.getData(), file.getDataSize());

                        firmware = std::make_unique<FirmwareUpdater> (midi);
                        fwd.firmware = firmware.get();
                        firmware->oldMajor = 11;
                        firmware->oldMinor = 0;
                        firmware->onFinished = [this] (bool ok, const String& text) { firmwareResult = ok ? 1 : 0; std::cout << "  info  " << text.replace ("\n", " ") << std::endl; };
                        String error;
                        check (firmware->start (f, error), "firmware file accepted " + error);
                        String bad;
                        const auto junk = temp.getChildFile ("junk.syx");
                        junk.replaceWithText ("not sysex");
                        FirmwareUpdater other (midi);
                        check (! other.start (junk, bad) && bad.isNotEmpty(), "a non-SysEx file is refused (" + bad + ")");
                        next();
                    }
                    break;
                case 15:
                    if (firmwareResult >= 0 || waited (60000))
                    {
                        check (firmwareResult == 1, "firmware update reported success after the unit came back with 11.01");
                        hook ({ 7 });
                        next();
                    }
                    break;
                case 16:
                    if (fwd.hookReply.getSize() > 0 || waited (3000))
                    {
                        const auto v = hookValues (3);
                        check (v.size() == 3 && (v[0] | (v[1] << 8)) == firmwareCount && v[2] == firmwareXor,
                               "unit received all 60 firmware messages intact");
                        fwd.firmware = nullptr;
                        firmware.reset();
                        finish();
                    }
                    break;
                default:
                    break;
            }
        }
    };
}

void runSimTest (const File& banksDir, std::function<void (int)> done)
{
    static std::unique_ptr<SimTest> test;
    test = std::make_unique<SimTest> (banksDir, [d = std::move (done)] (int failures)
    {
        MessageManager::callAsync ([d, failures] { test.reset(); d (failures); });
    });
}

bool runSimulator (const File& banksDir)
{
    static KeepAwake noAppNap ("Axe-Fx simulator");     // a windowless background process gets napped otherwise
    static AxeSim sim;
    return sim.start (banksDir);
}
