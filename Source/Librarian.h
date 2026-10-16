// Axe-Manage: librarian for the unit's preset banks, bank/preset files and user cabinets.
// Transfers follow the original AxeFxMIDIProtocol / AxeFxMidiProtocolHost (see README.md).
#pragma once
#include "Model.h"
#include "AxeMidi.h"
#include "KeepAwake.h"
#include <deque>

struct LibEntry
{
    enum State { Unknown, Synced, Edited };
    Preset preset;
    State state = Unknown;
    File file;                              // preset folders: the file this entry came from
};

// One list the librarian shows: a bank on the unit, a bank file, or a folder of preset files.
class LibContainer
{
public:
    enum Kind { HardwareBank, BankFile, PresetFolder };
    Kind kind = BankFile;
    String name;
    File file;
    int bank = -1;                          // hardware banks: 0..2 = A..C
    Array<LibEntry> entries;
    bool dirty = false;
    Array<Array<LibEntry>> undoStack, redoStack;

    bool isHardware() const                 { return kind == HardwareBank; }
    bool isEditable() const                 { return kind != PresetFolder; }
    int slotNumber (int index) const        { return isHardware() ? bank * Sysex::bankPresets + index : index; }
    void checkpoint();
    bool undo();
    bool redo();
};

struct CabSlot
{
    MemoryBlock sysex;                      // complete 8,204-byte cab message
    String name;
    LibEntry::State state = LibEntry::Unknown;
    bool isEmpty() const                    { return sysex.isEmpty(); }
};

class Librarian : public ChangeBroadcaster, private Timer
{
public:
    enum { numBanks = 3, numCabs = 10, cabMessageSize = 0x200c };

    Librarian (Document&, AxeMidi&);
    ~Librarian() override;

    Document& doc;
    AxeMidi& midi;
    OwnedArray<LibContainer> containers;    // [0..2] the unit's banks A..C, then opened files/folders
    std::array<CabSlot, numCabs> cabs;

    std::function<void (LibContainer&, int index)> onEdit;     // "Edit in Axe-Edit"

    // Files and folders
    LibContainer* openFile (const File&, String& error);
    LibContainer* openFolder (const File&);
    LibContainer* newBankFile();
    void closeContainer (LibContainer*);
    bool save (LibContainer&, const File& target, String& error);
    bool exportPresets (const LibContainer&, const Array<int>& rows, const File& folder, String& error);
    void rescanFolder (LibContainer&);
    static String bankName (int bank)       { return "Bank " + String::charToString ((juce_wchar) ('A' + bank)); }

    // Editing (all record an undo step and mark hardware entries as edited)
    void pasteOver (LibContainer&, int row, const Array<LibEntry>&);
    void pasteInsert (LibContainer&, int row, const Array<LibEntry>&);
    void moveRows (LibContainer&, Array<int> rows, int insertBefore);
    void swapRows (LibContainer&, int a, int b);
    void clearRows (LibContainer&, const Array<int>& rows);
    void rename (LibContainer&, int row, const String& newName);
    void touched (LibContainer&, const Array<int>& rows);
    static Preset emptyPreset();

    // User cabinets
    bool loadCab (int slot, const File&, String& error);
    void clearCab (int slot);

    // Transfers to/from the unit, run one at a time
    struct Job
    {
        enum Kind { DownloadBank, UploadBank, DownloadPreset, UploadPreset, UploadCab, AuditionCab, WriteBackup };
        Kind kind;
        int index = 0;
        File folder;
    };
    void enqueue (const Job&);
    void downloadBanks (int mask);
    void uploadBanks (int mask);
    void uploadChanged (int bank);
    void syncAndBackup (int mask);
    void cancelAll();
    bool isBusy() const                     { return running || ! jobs.empty(); }
    double progress = 0;                    // shown by the progress bar
    String activity;
    String lastError;

    // Forwarded by MainComponent from AxeMidi
    void bankReceived (int target, OwnedArray<Preset>&);
    void presetReceived (int number, const Preset&);
    void ackReceived (int function, bool ok);
    void presetStoredByEditor (int number, const Preset&);

    // Persistence: what we last knew about the unit lives in Application Support.
    File cacheDir() const;
    static File backupsDir();
    static File& storageRoot();             // tests point this at a temporary folder
    void loadCache();
    void saveCache();
    void modelChanged();

private:
    std::deque<Job> jobs;
    Job current { Job::DownloadBank };
    bool running = false;
    uint32 jobStarted = 0, lastProgressTime = 0;
    bool sendFinished = false;
    std::unique_ptr<KeepAwake> keepAwake;   // no App Nap while transfers run
    int loadedModel = -1;

    void timerCallback() override;
    void startNext();
    void finishJob (bool ok, const String& error = {});
    MemoryBlock bankMessage (int bank) const;
    MemoryBlock cabMessage (int slot, int targetSlot) const;
    LibContainer* hardware (int bank)       { return containers[bank]; }
};
