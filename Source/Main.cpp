#include "UI.h"
#include "SimTest.h"

class AxeEditApp : public JUCEApplication
{
public:
    const String getApplicationName() override      { return "Axe-Edit"; }
    const String getApplicationVersion() override   { return "1.0.191"; }
    bool moreThanOneInstanceAllowed() override      { return true; }

    void initialise (const String& commandLine) override
    {
        // Developer aid: "--snapshot out.png [--file x.syx] [--preset n] [--effect id] [--page n]" renders the editor to a PNG.
        const auto args = StringArray::fromTokens (commandLine, true);
        if (args.contains ("--selftest"))
        {
            setApplicationReturnValue (selfTest (args[args.indexOf ("--selftest") + 1].unquoted()));
            quit();
            return;
        }
        if (args.contains ("--axesim"))
        {
            if (! runSimulator (File (args[args.indexOf ("--axesim") + 1].unquoted())))
            {
                std::cerr << "Couldn't start the simulator (needs the Ultra factory banks folder)." << std::endl;
                setApplicationReturnValue (1);
                quit();
            }
            return;     // runs until killed
        }
        if (args.contains ("--simtest"))
        {
            runSimTest (File (args[args.indexOf ("--simtest") + 1].unquoted()), [this] (int failures)
            {
                setApplicationReturnValue (failures);
                quit();
            });
            return;
        }
        // "--snapshot-manage out.png [--file bank.syx]": renders the Axe-Manage window with a temporary cache.
        const int snapManage = args.indexOf ("--snapshot-manage");
        if (snapManage >= 0)
        {
            const auto temp = File::getSpecialLocation (File::tempDirectory).getChildFile ("axeedit-snap-" + String (Time::currentTimeMillis()));
            Librarian::storageRoot() = temp;
            {
                MainComponent c;
                ManageComponent m (*c.librarian, nullptr);
                m.setSize (1100, 640);
                const int fileArg = args.indexOf ("--file");
                if (fileArg >= 0)
                {
                    String error;
                    OwnedArray<Preset> presets; int model; uint8 man[3], hb[3];
                    if (Sysex::loadFile (File (args[fileArg + 1].unquoted()), presets, model, man, hb, error))
                        for (int i = 0; i < presets.size() && i < 128; ++i)
                        {
                            auto& e = c.librarian->containers[0]->entries.getReference (i);
                            e.preset = *presets[i];
                            e.state = i % 17 == 3 ? LibEntry::Edited : LibEntry::Synced;
                        }
                    c.librarian->openFile (File (args[fileArg + 1].unquoted()), error);
                    c.librarian->sendSynchronousChangeMessage();
                }
                const auto img = m.createComponentSnapshot (m.getLocalBounds(), true, 1.0f);
                File out (args[snapManage + 1].unquoted());
                out.deleteFile();
                if (auto stream = out.createOutputStream())
                    PNGImageFormat().writeImageToStream (img, *stream);
            }
            Librarian::storageRoot() = File();
            temp.deleteRecursively();
            quit();
            return;
        }
        // "--snapshot-modifier out.png --file x.syx --preset n --effect id --param 200..254": renders the modifier editor.
        const int snapMod = args.indexOf ("--snapshot-modifier");
        if (snapMod >= 0)
        {
            auto arg = [&] (const char* name) { const int i = args.indexOf (name); return i >= 0 ? args[i + 1].unquoted() : String(); };
            MainComponent c;
            if (arg ("--file").isNotEmpty())    c.openFile (File (arg ("--file")));
            if (arg ("--preset").isNotEmpty())  c.doc.selectPreset (arg ("--preset").getIntValue());
            ModifierEditor m (c.doc, arg ("--effect").getIntValue(), arg ("--param").getIntValue(), "Snapshot", nullptr);
            const auto img = m.createComponentSnapshot (m.getLocalBounds(), true, 1.0f);
            File out (args[snapMod + 1].unquoted());
            out.deleteFile();
            if (auto stream = out.createOutputStream())
                PNGImageFormat().writeImageToStream (img, *stream);
            quit();
            return;
        }
        const int snap = args.indexOf ("--snapshot");
        if (snap >= 0)
        {
            auto arg = [&] (const char* name) { const int i = args.indexOf (name); return i >= 0 ? args[i + 1].unquoted() : String(); };
            MainComponent c;
            if (arg ("--file").isNotEmpty())    c.openFile (File (arg ("--file")));
            if (arg ("--preset").isNotEmpty())  c.doc.selectPreset (arg ("--preset").getIntValue());
            if (arg ("--effect").isNotEmpty())  c.doc.selectedEffect = arg ("--effect").getIntValue();
            c.effects.refresh();
            for (int i = arg ("--page").getIntValue(); --i >= 0;) c.effects.nextPage();
            const auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
            File out (args[snap + 1].unquoted());
            out.deleteFile();
            if (auto stream = out.createOutputStream())
                PNGImageFormat().writeImageToStream (img, *stream);
            quit();
            return;
        }
        window = std::make_unique<Window>();
    }
    void shutdown() override                        { window.reset(); }
    void systemRequestedQuit() override             { quit(); }

private:
    // Round-trips every .syx under `dir` and exercises the edit operations. Returns the failure count.
    static int selfTest (const String& dir)
    {
        int failures = 0, files = 0, presets = 0;
        auto check = [&] (bool ok, const String& what) { if (! ok) { ++failures; std::cout << "FAIL: " << what << std::endl; } };

        for (auto& f : File (dir).findChildFiles (File::findFiles, true, "*.syx"))
        {
            Document d;
            String error;
            MemoryBlock original;
            f.loadFileAsData (original);
            if (! d.open (f, error)) { std::cout << "skip " << f.getFileName() << ": " << error << std::endl; continue; }
            ++files;

            const auto again = d.sourceIsBank ? Sysex::encodeBank (d.bank, d.model, d.manufacturer, d.bankHeader)
                                              : Sysex::encodePreset (d.preset, d.model, d.manufacturer, original[6], original[7] | (original[8] << 4));
            size_t diff = 0;
            while (diff < jmin (again.getSize(), original.getSize()) && again[diff] == original[diff]) ++diff;
            check (again == original, "byte-exact re-encode of " + f.getFileName() + " (first difference at byte " + String ((int) diff)
                                        + ", preset " + String (((int) diff - 9) / 2048) + " value " + String ((((int) diff - 9) % 2048) / 2) + ")");

            for (int i = 0; i < d.bank.size(); ++i)
            {
                ++presets;
                d.selectPreset (i);
                const auto single = Sysex::encodePreset (d.preset, d.model, d.manufacturer, 1, 0);
                Preset back;
                check (single.getSize() == (size_t) Sysex::presetMessageSize && Sysex::decodePreset ((const uint8*) single.getData(), (int) single.getSize(), back)
                         && back.name == d.preset.name && back.chunks.size() == d.preset.chunks.size(), "single preset round trip " + String (i));

                for (auto& c : d.preset.chunks)     // every stored block must be known to the config + profile
                    if (c.id > Preset::numModifierSlots)
                        check (d.config.typeOf (c.id).isNotEmpty() && d.profile.numParams (d.config.typeOf (c.id)) > 0,
                               "block id " + String ((int) c.id) + " has a type/profile (" + f.getFileName() + " #" + String (i) + ")");

                // CSV export -> import keeps the grid, every block's values and every active modifier
                Preset fromCsv;
                String csvError;
                const bool imported = Csv::importPreset (d, Csv::exportPreset (d, d.preset), fromCsv, csvError);
                bool same = imported && fromCsv.name == d.preset.name.substring (0, 32) && fromCsv.version == d.preset.version;
                for (int g = 0; same && g < Preset::columns * Preset::rows; ++g)
                    same = fromCsv.grid[(size_t) g].id == d.preset.grid[(size_t) g].id && fromCsv.grid[(size_t) g].mask == d.preset.grid[(size_t) g].mask;
                for (auto& c : d.preset.chunks)
                {
                    const bool activeModifier = c.id <= Preset::numModifierSlots && c.values.size() > 0 && c.values[0] > 0;
                    if (c.id > Preset::numModifierSlots || activeModifier)
                        same = same && fromCsv.chunk (c.id) != nullptr && fromCsv.chunk (c.id)->values == c.values;
                }
                check (same, "CSV round trip " + f.getFileName() + " #" + String (i) + (csvError.isNotEmpty() ? ": " + csvError : String()));
            }

            // modifiers: attach one to the first modifiable parameter of a placed block, edit it, re-parse
            d.selectPreset (0);
            {
                int eff = 0, mod = 0;
                for (auto& item : d.config.pool)
                    if (d.preset.isPlaced (item.id) && d.preset.chunk (item.id) != nullptr && eff == 0)
                        for (int p = 0; p < d.profile.numParams (item.type) && eff == 0; ++p)
                        {
                            const int m = d.config.modifierId (item.type, d.config.paramName (item.type, p));
                            if (m >= 200 && m <= 254 && d.preset.findModifier (item.id, m) < 0) { eff = item.id; mod = m; }
                        }
                check (eff != 0, "found a modifiable parameter in " + f.getFileName());
                if (eff != 0)
                {
                    const int chunkId = d.preset.assignModifier (eff, mod);
                    check (chunkId >= 1 && chunkId <= Preset::numModifierSlots, "modifier slot assigned");
                    d.preset.setModifierValue (chunkId, Preset::ModSource, 1);
                    d.preset.setModifierValue (chunkId, Preset::ModEnd, 200);
                    uint8 bytes[Preset::size];
                    Preset reparsed;
                    check (d.preset.toBytes (bytes) && reparsed.fromBytes (bytes) && reparsed.findModifier (eff, mod) == chunkId
                             && reparsed.modifierValue (chunkId, Preset::ModEnd) == 200 && reparsed.modifierValue (chunkId, Preset::ModParamId) == mod,
                           "modifier survives encode/decode");
                    d.preset.setModifierValue (chunkId, Preset::ModSource, 0);
                    check (d.preset.findModifier (eff, mod) < 0 && d.preset.assignModifier (eff, mod) == chunkId, "disconnected modifier slot is reused");
                }
            }

            // graphs and CPU estimate
            {
                using namespace Graphs;
                check (std::abs (biquadDb (1, 1000, 0.707, 0, 20)) < 0.1 && biquadDb (1, 1000, 0.707, 0, 1000) < -2.9
                         && biquadDb (1, 1000, 0.707, 0, 1000) > -3.1 && biquadDb (1, 1000, 0.707, 0, 10000) < -35, "low-pass response");
                check (std::abs (biquadDb (6, 1000, 1.0, 6, 1000) - 6.0) < 0.05 && std::abs (biquadDb (6, 1000, 1.0, 6, 20)) < 0.2, "peaking response");
                check (std::abs (biquadDb (4, 200, 0.707, -9, 20) + 9.0) < 0.3 && std::abs (biquadDb (5, 2000, 0.707, 9, 19000) - 9.0) < 0.5, "shelf responses");
                double units = 0;
                for (auto& c : d.preset.grid)
                    if (c.id == 106 || c.id == 107) units = 24;
                check (cpuUnits (d) >= units && cpuUnits (d) < 350, "CPU estimate in range");
                if (d.config.poolItem (104) != nullptr)
                {
                    Curve curve;
                    Preset saved = d.preset;
                    if (d.preset.chunk (104) == nullptr)
                    {
                        Preset::Chunk ch; ch.id = 104;
                        for (int p = 0; p < 24; ++p) ch.values.add ((uint8) jmax (0, d.defaultFor ("ParametricEQ", p)));
                        d.preset.chunks.add (ch);
                    }
                    for (int b = 1; b <= 5; ++b)
                        d.preset.setParam (104, d.config.paramId ("ParametricEQ", "PEQ_GAIN" + String (b)), 127);
                    bool flat = compute (d, 104, "PEQGraph", "Bands 1+5", curve) && curve.points.size() == 64;
                    for (auto v : curve.points) flat = flat && std::abs (v) < 0.3f;
                    check (flat, "flat PEQ draws a flat 64-point curve");
                    d.preset = saved;
                }
            }

            // edits: place a block, change a value, cable, remove - then make sure it still encodes
            d.selectPreset (0);
            const int before = d.preset.bytesUsed();
            int freeId = 0;
            for (auto& item : d.config.pool) if (! d.preset.isPlaced (item.id) && d.preset.chunk (item.id) == nullptr) { freeId = item.id; break; }
            int col = -1, row = -1;
            for (int c = 1; c < Preset::columns && col < 0; ++c)
                for (int r = 0; r < Preset::rows; ++r)
                    if (d.preset.cell (c, r).id == 0 && d.preset.cell (c - 1, r).id != 0) { col = c; row = r; break; }
            if (freeId != 0 && col >= 0)
            {
                check (d.placeEffect (col, row, freeId), "place block");
                check (d.preset.chunk (freeId) != nullptr && d.preset.cell (col, row).mask == (1 << row), "placed block has data and a cable");
                check (d.preset.setParam (freeId, 0, 77) && d.preset.getParam (freeId, 0) == 77, "set/get param");
                uint8 bytes[Preset::size];
                Preset reparsed;
                check (d.preset.toBytes (bytes) && reparsed.fromBytes (bytes) && reparsed.getParam (freeId, 0) == 77, "edited preset re-parses");
                d.removeAt (col, row);
                check (d.preset.chunk (freeId) == nullptr && d.preset.bytesUsed() == before, "remove restores size");
            }
        }
        std::cout << files << " files, " << presets << " presets, " << failures << " failures" << std::endl;
        return failures;
    }

    struct Window : public DocumentWindow
    {
        Window() : DocumentWindow ("Axe-Edit 1.0 (build 191, Apple Silicon rebuild)", Colours::black, closeButton | minimiseButton)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (false, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override          { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (AxeEditApp)
