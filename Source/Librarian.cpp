#include "Librarian.h"

//==============================================================================
void LibContainer::checkpoint()
{
    undoStack.add (entries);
    if (undoStack.size() > 50)
        undoStack.remove (0);
    redoStack.clear();
}

bool LibContainer::undo()
{
    if (undoStack.isEmpty())
        return false;
    redoStack.add (entries);
    entries = undoStack.removeAndReturn (undoStack.size() - 1);
    dirty = true;
    return true;
}

bool LibContainer::redo()
{
    if (redoStack.isEmpty())
        return false;
    undoStack.add (entries);
    entries = redoStack.removeAndReturn (redoStack.size() - 1);
    dirty = true;
    return true;
}

//==============================================================================
Librarian::Librarian (Document& d, AxeMidi& m) : doc (d), midi (m)
{
    for (int b = 0; b < numBanks; ++b)
    {
        auto* c = containers.add (new LibContainer());
        c->kind = LibContainer::HardwareBank;
        c->bank = b;
        c->name = bankName (b);
    }
    loadCache();
    startTimer (100);
}

Librarian::~Librarian()
{
    cancelAll();
    saveCache();
}

Preset Librarian::emptyPreset()
{
    Preset p;
    p.clear();
    p.name = "EMPTY";
    return p;
}

//==============================================================================
File& Librarian::storageRoot()
{
    static File root;
    return root;
}

File Librarian::cacheDir() const
{
    const auto base = storageRoot() != File() ? storageRoot().getChildFile ("Librarian")
                                              : File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("Axe-Edit-arm64/Librarian");
    return base.getChildFile (AxeModel::folder (doc.model));
}

File Librarian::backupsDir()
{
    return storageRoot() != File() ? storageRoot().getChildFile ("Backups")
                                   : File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Axe-Edit Backups");
}

static String stateName (LibEntry::State s)    { return s == LibEntry::Synced ? "synced" : s == LibEntry::Edited ? "edited" : "unknown"; }
static var element (const var& v, int index)     // var::operator[] dereferences null on non-arrays
{
    if (auto* a = v.getArray())
        if (isPositiveAndBelow (index, a->size()))
            return a->getReference (index);
    return {};
}

static LibEntry::State stateFrom (const String& s)
{
    return s == "synced" ? LibEntry::Synced : s == "edited" ? LibEntry::Edited : LibEntry::Unknown;
}

void Librarian::loadCache()
{
    loadedModel = doc.model;
    const auto dir = cacheDir();
    const auto state = JSON::parse (dir.getChildFile ("state.json"));

    for (int b = 0; b < numBanks; ++b)
    {
        auto* c = hardware (b);
        c->entries.clear();
        c->undoStack.clear();
        c->redoStack.clear();

        OwnedArray<Preset> presets;
        int m = 0; uint8 man[3], hb[3]; String error;
        const bool cached = Sysex::loadFile (dir.getChildFile (bankName (b) + ".syx"), presets, m, man, hb, error)
                              && presets.size() == Sysex::bankPresets;
        const auto bankStates = element (state["banks"], b);
        auto* states = bankStates.getArray();

        for (int i = 0; i < Sysex::bankPresets; ++i)
        {
            LibEntry e;
            if (cached)
            {
                e.preset = *presets[i];
                e.state = states != nullptr && i < states->size() ? stateFrom ((*states)[i].toString()) : LibEntry::Synced;
            }
            else
            {
                e.preset.clear();
                e.state = LibEntry::Unknown;
            }
            c->entries.add (e);
        }
    }

    for (int s = 0; s < numCabs; ++s)
    {
        auto& cab = cabs[(size_t) s];
        cab = {};
        dir.getChildFile ("Cab " + String (s + 1) + ".syx").loadFileAsData (cab.sysex);
        if (cab.sysex.getSize() != (size_t) cabMessageSize)
            cab.sysex.reset();
        const auto info = element (state["cabs"], s);
        cab.name = info["name"].toString();
        cab.state = cab.isEmpty() ? LibEntry::Unknown : stateFrom (info["state"].toString());
    }
    sendChangeMessage();
}

void Librarian::saveCache()
{
    if (loadedModel < 0)
        return;

    const auto base = storageRoot() != File() ? storageRoot().getChildFile ("Librarian")
                                              : File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("Axe-Edit-arm64/Librarian");
    const auto dir = base.getChildFile (AxeModel::folder (loadedModel));
    dir.createDirectory();

    auto* root = new DynamicObject();
    Array<var> banks, cabStates;

    for (int b = 0; b < numBanks; ++b)
    {
        auto* c = hardware (b);
        Array<var> states;
        bool anyKnown = false;
        for (auto& e : c->entries)
        {
            states.add (stateName (e.state));
            anyKnown = anyKnown || e.state != LibEntry::Unknown;
        }
        banks.add (states);

        const auto f = dir.getChildFile (bankName (b) + ".syx");
        if (anyKnown)
        {
            const auto data = bankMessage (b);
            f.replaceWithData (data.getData(), data.getSize());
        }
        else
            f.deleteFile();
    }

    for (int s = 0; s < numCabs; ++s)
    {
        auto& cab = cabs[(size_t) s];
        const auto f = dir.getChildFile ("Cab " + String (s + 1) + ".syx");
        if (cab.isEmpty())
            f.deleteFile();
        else
            f.replaceWithData (cab.sysex.getData(), cab.sysex.getSize());

        auto* o = new DynamicObject();
        o->setProperty ("name", cab.name);
        o->setProperty ("state", stateName (cab.state));
        cabStates.add (var (o));
    }

    root->setProperty ("banks", banks);
    root->setProperty ("cabs", cabStates);
    dir.getChildFile ("state.json").replaceWithText (JSON::toString (var (root)));
}

void Librarian::modelChanged()
{
    if (doc.model == loadedModel)
        return;
    cancelAll();
    saveCache();
    loadCache();
}

//==============================================================================
LibContainer* Librarian::openFile (const File& f, String& error)
{
    for (auto* c : containers)
        if (c->file == f && c->kind != LibContainer::PresetFolder)
            return c;

    OwnedArray<Preset> presets;
    int m = 0; uint8 man[3], hb[3];
    if (! Sysex::loadFile (f, presets, m, man, hb, error))
        return nullptr;
    if (m != doc.model)
    {
        error = f.getFileName() + " is an " + AxeModel::name (m) + " file; the librarian is set up for the "
                  + AxeModel::name (doc.model) + ".";
        return nullptr;
    }

    auto* c = containers.add (new LibContainer());
    c->kind = LibContainer::BankFile;
    c->file = f;
    c->name = f.getFileNameWithoutExtension();
    for (auto* p : presets)
        c->entries.add ({ *p, LibEntry::Synced, {} });
    sendChangeMessage();
    return c;
}

LibContainer* Librarian::openFolder (const File& folder)
{
    for (auto* c : containers)
        if (c->file == folder && c->kind == LibContainer::PresetFolder)
            return c;

    auto* c = containers.add (new LibContainer());
    c->kind = LibContainer::PresetFolder;
    c->file = folder;
    c->name = folder.getFileName();
    rescanFolder (*c);
    return c;
}

void Librarian::rescanFolder (LibContainer& c)
{
    c.entries.clear();
    auto files = c.file.findChildFiles (File::findFiles, false, "*.syx");
    files.sort();
    for (auto& f : files)
    {
        OwnedArray<Preset> presets;
        int m = 0; uint8 man[3], hb[3]; String error;
        if (f.getSize() == Sysex::presetMessageSize && Sysex::loadFile (f, presets, m, man, hb, error) && m == doc.model)
            c.entries.add ({ *presets[0], LibEntry::Synced, f });
    }
    sendChangeMessage();
}

LibContainer* Librarian::newBankFile()
{
    auto* c = containers.add (new LibContainer());
    c->kind = LibContainer::BankFile;
    c->name = "Untitled Bank";
    c->dirty = true;
    for (int i = 0; i < Sysex::bankPresets; ++i)
        c->entries.add ({ emptyPreset(), LibEntry::Synced, {} });
    sendChangeMessage();
    return c;
}

void Librarian::closeContainer (LibContainer* c)
{
    if (c != nullptr && ! c->isHardware())
        containers.removeObject (c);
    sendChangeMessage();
}

bool Librarian::save (LibContainer& c, const File& target, String& error)
{
    MemoryBlock data;
    if (c.entries.size() == Sysex::bankPresets)
    {
        OwnedArray<Preset> presets;
        for (auto& e : c.entries)
            presets.add (new Preset (e.preset));
        const uint8 hb[3] = { 2, 0, 0 };
        data = Sysex::encodeBank (presets, doc.model, midi.manufacturer, hb);
    }
    else
    {
        MemoryOutputStream out;
        for (auto& e : c.entries)
            out << Sysex::encodePreset (e.preset, doc.model, midi.manufacturer, 1, 0);
        data = out.getMemoryBlock();
    }

    if (! target.replaceWithData (data.getData(), data.getSize()))
    {
        error = "Couldn't write " + target.getFullPathName();
        return false;
    }

    if (c.kind == LibContainer::BankFile)
    {
        c.file = target;
        c.name = target.getFileNameWithoutExtension();
        c.dirty = false;
    }
    sendChangeMessage();
    return true;
}

bool Librarian::exportPresets (const LibContainer& c, const Array<int>& rows, const File& folder, String& error)
{
    for (int row : rows)
    {
        if (! isPositiveAndBelow (row, c.entries.size()))
            continue;
        auto& e = c.entries.getReference (row);
        auto name = File::createLegalFileName (e.preset.name.trim());
        if (name.isEmpty())
            name = "Preset";
        const auto f = folder.getChildFile (String (c.slotNumber (row)).paddedLeft ('0', 3) + " " + name + ".syx")
                         .getNonexistentSibling();
        const auto data = Sysex::encodePreset (e.preset, doc.model, midi.manufacturer, 1, 0);
        if (! f.replaceWithData (data.getData(), data.getSize()))
        {
            error = "Couldn't write " + f.getFullPathName();
            return false;
        }
    }
    for (auto* other : containers)
        if (other->kind == LibContainer::PresetFolder && other->file == folder)
            rescanFolder (*other);
    return true;
}

//==============================================================================
void Librarian::touched (LibContainer& c, const Array<int>& rows)
{
    for (int r : rows)
        if (isPositiveAndBelow (r, c.entries.size()))
        {
            auto& e = c.entries.getReference (r);
            e.file = File();
            e.state = c.isHardware() ? LibEntry::Edited : LibEntry::Synced;
        }
    c.dirty = true;
    if (c.isHardware())
        saveCache();
    sendChangeMessage();
}

static Array<int> rangeOf (int start, int end)
{
    Array<int> r;
    for (int i = start; i < end; ++i)
        r.add (i);
    return r;
}

void Librarian::pasteOver (LibContainer& c, int row, const Array<LibEntry>& src)
{
    if (! c.isEditable() || src.isEmpty())
        return;
    c.checkpoint();
    const int n = jmin (src.size(), c.entries.size() - row);
    for (int i = 0; i < n; ++i)
        c.entries.getReference (row + i).preset = src.getReference (i).preset;
    touched (c, rangeOf (row, row + n));
}

void Librarian::pasteInsert (LibContainer& c, int row, const Array<LibEntry>& src)
{
    if (! c.isEditable() || src.isEmpty())
        return;
    c.checkpoint();
    const int size = c.entries.size();
    for (int i = 0; i < src.size(); ++i)
        c.entries.insert (row + i, { src.getReference (i).preset, LibEntry::Synced, {} });
    if (c.entries.size() > size && (c.isHardware() || size == Sysex::bankPresets))
        c.entries.removeRange (size, c.entries.size() - size);     // banks hold exactly 128 presets
    touched (c, rangeOf (row, c.entries.size()));
}

void Librarian::moveRows (LibContainer& c, Array<int> rows, int insertBefore)
{
    if (! c.isEditable() || rows.isEmpty())
        return;
    rows.sort();
    c.checkpoint();

    Array<LibEntry> moving;
    for (int r : rows)
        moving.add (c.entries[r]);
    for (int i = rows.size(); --i >= 0;)
    {
        c.entries.remove (rows[i]);
        if (rows[i] < insertBefore)
            --insertBefore;
    }
    insertBefore = jlimit (0, c.entries.size(), insertBefore);
    for (int i = 0; i < moving.size(); ++i)
        c.entries.insert (insertBefore + i, moving.getReference (i));

    const int lo = jmin (rows.getFirst(), insertBefore), hi = jmax (rows.getLast() + 1, insertBefore + moving.size());
    touched (c, rangeOf (lo, jmin (hi, c.entries.size())));
}

void Librarian::swapRows (LibContainer& c, int a, int b)
{
    if (! c.isEditable() || a == b)
        return;
    c.checkpoint();
    c.entries.swap (a, b);
    touched (c, { a, b });
}

void Librarian::clearRows (LibContainer& c, const Array<int>& rows)
{
    if (! c.isEditable() || rows.isEmpty())
        return;
    c.checkpoint();
    for (int r : rows)
        if (isPositiveAndBelow (r, c.entries.size()))
            c.entries.getReference (r).preset = emptyPreset();
    touched (c, rows);
}

void Librarian::rename (LibContainer& c, int row, const String& newName)
{
    if (! c.isEditable() || ! isPositiveAndBelow (row, c.entries.size()))
        return;
    c.checkpoint();
    c.entries.getReference (row).preset.name = newName.substring (0, 32);
    touched (c, { row });
}

//==============================================================================
bool Librarian::loadCab (int slot, const File& f, String& error)
{
    MemoryBlock mb;
    f.loadFileAsData (mb);
    auto* d = (const uint8*) mb.getData();
    if (mb.getSize() != (size_t) cabMessageSize || d[0] != 0xf0 || d[5] != Sysex::CabIR || d[cabMessageSize - 1] != 0xf7)
    {
        error = f.getFileName() + " isn't an Axe-Fx Standard/Ultra user cab file (expected an 8,204-byte cab SysEx dump).";
        return false;
    }
    for (int i = 9; i < 9 + 0x2000; ++i)
        if (d[i] > 0x0f)
        {
            error = f.getFileName() + " has an unexpected data format.";
            return false;
        }

    auto& cab = cabs[(size_t) slot];
    cab.sysex = mb;
    cab.name = f.getFileNameWithoutExtension();
    cab.state = LibEntry::Edited;
    saveCache();
    sendChangeMessage();
    return true;
}

void Librarian::clearCab (int slot)
{
    cabs[(size_t) slot] = {};
    saveCache();
    sendChangeMessage();
}

MemoryBlock Librarian::cabMessage (int slot, int targetSlot) const
{
    // F0 <id> <model> 0A <slot: 1-10, 0 = the cab selected in the current preset> .. ..
    // 4096 values as nibble pairs from byte 9, XOR checksum, F7 (AxeFxMIDIProtocol::storeCab).
    MemoryBlock mb (cabs[(size_t) slot].sysex);
    auto* d = (uint8*) mb.getData();
    memcpy (d + 1, midi.manufacturer, 3);
    d[4] = (uint8) doc.model;
    d[6] = (uint8) targetSlot;
    uint8 ck = 0;
    for (int i = 0; i < 0x1000; ++i)
        ck ^= (uint8) ((d[9 + i * 2] & 0x0f) | (d[10 + i * 2] << 4));
    d[9 + 0x2000] = ck & 0x0f;
    d[10 + 0x2000] = ck >> 4;
    d[cabMessageSize - 1] = 0xf7;
    return mb;
}

MemoryBlock Librarian::bankMessage (int bank) const
{
    OwnedArray<Preset> presets;
    for (auto& e : containers[bank]->entries)
        presets.add (new Preset (e.preset));
    const uint8 hb[3] = { (uint8) (2 + bank), 0, 0 };
    return Sysex::encodeBank (presets, doc.model, midi.manufacturer, hb);
}

//==============================================================================
void Librarian::enqueue (const Job& j)
{
    if (! isBusy())
        lastError.clear();      // a new batch of transfers
    jobs.push_back (j);
    sendChangeMessage();
}

void Librarian::downloadBanks (int mask)
{
    for (int b = 0; b < numBanks; ++b)
        if (mask & (1 << b))
            enqueue ({ Job::DownloadBank, b });
}

void Librarian::uploadBanks (int mask)
{
    for (int b = 0; b < numBanks; ++b)
        if (mask & (1 << b))
            enqueue ({ Job::UploadBank, b });
}

void Librarian::uploadChanged (int bank)
{
    auto* c = hardware (bank);
    for (int i = 0; i < c->entries.size(); ++i)
        if (c->entries.getReference (i).state == LibEntry::Edited)
            enqueue ({ Job::UploadPreset, c->slotNumber (i) });
}

void Librarian::syncAndBackup (int mask)
{
    downloadBanks (mask);
    const auto stamp = Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S");
    enqueue ({ Job::WriteBackup, mask, backupsDir().getChildFile (AxeModel::name (doc.model) + " " + stamp) });
}

void Librarian::cancelAll()
{
    jobs.clear();
    if (running)
    {
        midi.cancelTransfer();
        running = false;
        activity = "Cancelled.";
        progress = 0;
    }
    keepAwake.reset();
    sendChangeMessage();
}

static String describe (const Librarian::Job& j)
{
    switch (j.kind)
    {
        case Librarian::Job::DownloadBank:    return "Downloading " + Librarian::bankName (j.index);
        case Librarian::Job::UploadBank:      return "Uploading " + Librarian::bankName (j.index);
        case Librarian::Job::DownloadPreset:  return "Downloading preset " + String (j.index).paddedLeft ('0', 3);
        case Librarian::Job::UploadPreset:    return "Storing preset " + String (j.index).paddedLeft ('0', 3);
        case Librarian::Job::UploadCab:       return "Uploading cab to USER " + String (j.index + 1);
        case Librarian::Job::AuditionCab:     return "Auditioning cab " + String (j.index + 1);
        case Librarian::Job::WriteBackup:     return "Writing backup";
    }
    return {};
}

void Librarian::startNext()
{
    if (keepAwake == nullptr)
        keepAwake = std::make_unique<KeepAwake> ("Axe-Fx MIDI transfer");
    current = jobs.front();
    jobs.pop_front();
    running = true;
    sendFinished = false;
    progress = 0;
    jobStarted = lastProgressTime = Time::getMillisecondCounter();
    activity = describe (current) + "...";
    sendChangeMessage();

    if (current.kind == Job::WriteBackup)
    {
        current.folder.createDirectory();
        for (int b = 0; b < numBanks; ++b)
            if (current.index & (1 << b))
            {
                const auto data = bankMessage (b);
                current.folder.getChildFile (bankName (b) + ".syx").replaceWithData (data.getData(), data.getSize());
            }
        finishJob (true);
        activity = "Backup saved to " + current.folder.getFullPathName();
        return;
    }

    if (! midi.isConnected())
    {
        finishJob (false, "The Axe-Fx isn't connected (choose its MIDI ports under Settings in the editor).");
        return;
    }

    auto sent = [this] (bool ok)
    {
        if (! running)
            return;
        sendFinished = true;
        lastProgressTime = Time::getMillisecondCounter();
        if (! ok)
            finishJob (false, "The transfer was interrupted.");
    };

    bool started = true;
    switch (current.kind)
    {
        case Job::DownloadBank:     midi.requestBank (current.index); break;
        case Job::DownloadPreset:   midi.requestPreset (false, current.index); break;
        case Job::UploadBank:       started = midi.sendPaced (bankMessage (current.index), sent); break;
        case Job::UploadPreset:
        {
            auto& e = hardware (current.index / Sysex::bankPresets)->entries.getReference (current.index % Sysex::bankPresets);
            started = midi.sendPaced (Sysex::encodePreset (e.preset, doc.model, midi.manufacturer, 0, current.index), sent);
            break;
        }
        case Job::UploadCab:        started = midi.sendPaced (cabMessage (current.index, current.index + 1), sent); break;
        case Job::AuditionCab:      started = midi.sendPaced (cabMessage (current.index, 0), sent); break;
        case Job::WriteBackup:      break;
    }

    if (! started)
        finishJob (false, "Couldn't start the MIDI transfer.");
}

void Librarian::finishJob (bool ok, const String& error)
{
    running = false;
    progress = ok ? 1.0 : 0.0;
    if (! ok)
    {
        jobs.clear();
        lastError = activity = error;
    }
    else if (jobs.empty())
        activity = "Done.";
    if (jobs.empty())
        keepAwake.reset();
    saveCache();
    sendChangeMessage();
}

void Librarian::timerCallback()
{
    if (! running)
    {
        if (! jobs.empty())
            startNext();
        return;
    }

    const auto now = Time::getMillisecondCounter();

    switch (current.kind)
    {
        case Job::DownloadBank:
        {
            // JUCE delivers the 262 KB reply only once it is complete, so progress is estimated from
            // MIDI wire speed (3,125 bytes/s: about 85 s per bank over 5-pin DIN).
            const double expected = Sysex::bankMessageSize / 3125.0 * 1000.0;
            progress = jmin (0.97, (now - jobStarted) / expected);
            if (now - jobStarted > 200000)
                finishJob (false, "The Axe-Fx didn't send " + bankName (current.index) + " (check that its MIDI Out is connected to this Mac).");
            break;
        }

        case Job::DownloadPreset:
            if (now - jobStarted > 6000)
                finishJob (false, "The Axe-Fx didn't answer the request for preset " + String (current.index) + ".");
            break;

        case Job::UploadBank:
        case Job::UploadPreset:
        case Job::UploadCab:
        case Job::AuditionCab:
        {
            progress = sendFinished ? 1.0 : midi.sendProgress();
            // Give the unit time to write its flash before the next message.
            const uint32 settle = current.kind == Job::UploadBank ? 3000 : current.kind == Job::UploadPreset ? 800 : 1500;
            if (sendFinished && now - lastProgressTime > settle)
            {
                if (current.kind == Job::UploadBank)
                    for (auto& e : hardware (current.index)->entries)
                        e.state = LibEntry::Synced;
                else if (current.kind == Job::UploadPreset)
                    hardware (current.index / Sysex::bankPresets)->entries.getReference (current.index % Sysex::bankPresets).state = LibEntry::Synced;
                else if (current.kind == Job::UploadCab)
                    cabs[(size_t) current.index].state = LibEntry::Synced;
                finishJob (true);
            }
            break;
        }

        case Job::WriteBackup:
            break;
    }

    if (running)
        sendChangeMessage();
}

//==============================================================================
void Librarian::bankReceived (int target, OwnedArray<Preset>& presets)
{
    const int b = target - 2;
    if (! isPositiveAndBelow (b, (int) numBanks) || presets.size() != Sysex::bankPresets)
        return;

    auto* c = hardware (b);
    c->checkpoint();
    c->entries.clear();
    for (auto* p : presets)
        c->entries.add ({ *p, LibEntry::Synced, {} });

    if (running && current.kind == Job::DownloadBank && current.index == b)
        finishJob (true);
    else
    {
        saveCache();
        sendChangeMessage();
    }
}

void Librarian::presetReceived (int number, const Preset& p)
{
    if (! isPositiveAndBelow (number, numBanks * (int) Sysex::bankPresets))
        return;
    auto& e = hardware (number / Sysex::bankPresets)->entries.getReference (number % Sysex::bankPresets);
    e.preset = p;
    e.state = LibEntry::Synced;

    if (running && current.kind == Job::DownloadPreset && current.index == number)
        finishJob (true);
    else
    {
        saveCache();
        sendChangeMessage();
    }
}

void Librarian::ackReceived (int function, bool ok)
{
    if (! ok && running && (function == Sysex::PatchRcv || function == Sysex::CabIR))
        finishJob (false, "The Axe-Fx reported a checksum error during the transfer.");
}

void Librarian::presetStoredByEditor (int number, const Preset& p)
{
    if (! isPositiveAndBelow (number, numBanks * (int) Sysex::bankPresets))
        return;
    auto& e = hardware (number / Sysex::bankPresets)->entries.getReference (number % Sysex::bankPresets);
    e.preset = p;
    e.state = LibEntry::Synced;
    saveCache();
    sendChangeMessage();
}
