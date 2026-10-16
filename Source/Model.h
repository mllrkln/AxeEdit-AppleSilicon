// Data model for the Axe-Edit 1.0.191 native rebuild.
// Formats here were recovered from the original binary (see README.md).
#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <map>
#include <array>

using namespace juce;

//==============================================================================
// ".stf" skin fonts: zlib-compressed JUCE 1.52 CustomTypeface streams (16-bit chars).
class StfFont
{
public:
    bool load (InputStream& compressed);
    float getWidth (const String& text, float height) const;
    void draw (Graphics&, const String& text, Rectangle<float> area, float height,
               Colour, Justification, bool shrinkToFit = true) const;
private:
    struct Glyph { Path path; float width = 0; };
    std::map<juce_wchar, Glyph> glyphs;
    float ascent = 0.8f;
    juce_wchar defaultChar = ' ';
    const Glyph* find (juce_wchar) const;
};

//==============================================================================
// A skin = XML description (.saxey) + zip of PNG images and fonts (.paxege).
class Skin
{
public:
    bool load (const File& saxey, String& error);
    Image image (const String& name);
    const StfFont& font (const String& name);
    XmlElement* root() const                         { return xml.get(); }
    XmlElement* child (const String& path) const;   // "GridUnitSkin/Grid"
    XmlElement* widget (XmlElement* parent, const String& name) const;
    XmlElement* named (const String& listTag, const String& itemTag, const String& name) const;
    static Colour colour (const XmlElement* e, const String& attr, Colour fallback = Colours::white);
    static Rectangle<int> rect (const XmlElement* e, const String& attr);
private:
    std::unique_ptr<XmlElement> xml;
    std::unique_ptr<ZipFile> zip;
    std::map<String, Image> images;
    std::map<String, std::unique_ptr<StfFont>> fonts;
};

//==============================================================================
struct ParamInfo
{
    enum Type { LIN, LOG, INT, B16 };
    Type type = LIN;
    int id = 0, def = 0, numVals = 255, precision = 0;
    double minimum = 0, maximum = 1;
    String unit;
    StringArray items;

    int maxRaw() const                      { return jmax (1, numVals - 1); }
    String format (int raw, int fine = 0) const;   // display text for a raw value
    double value (int raw, int fine = 0) const;    // numeric value in the parameter's unit
    String valueOnly (int raw, int fine = 0) const;
};

// Parameter ranges/defaults per effect type (Profiles/<model>/default.profile).
class Profile
{
public:
    bool load (const File&);
    const ParamInfo* find (const String& effectType, int paramId) const;
    int numParams (const String& effectType) const;   // highest stored param id + 1
    int model = 1, major = 0, minor = 0;
private:
    std::map<String, std::map<int, ParamInfo>> params;
};

// Effect pool, parameter names and editor page layouts (Configs/<model>/default.axeml).
class Config
{
public:
    struct PoolItem { int id; String name, type; };
    bool load (const File&);
    const PoolItem* poolItem (int effectId) const;
    String typeOf (int effectId) const;
    int paramId (const String& type, const String& paramName) const;     // -1 if unknown
    int modifierId (const String& type, const String& paramName) const;
    String paramName (const String& type, int paramId) const;           // empty if unknown
    XmlElement* layout (const String& type) const;                       // <EditorControls>
    Array<PoolItem> pool;
private:
    std::unique_ptr<XmlElement> xml;
    struct P { int id, modifierId; };
    std::map<String, std::map<String, P>> names;
};

//==============================================================================
// One Standard/Ultra preset: 1024 byte-sized values.
//   [0] format version (0x30..0x35)   [1] reserved   [2..33] name
//   [34..129] 12 columns x 4 rows of { effect id, input mask }
//   [130..] chunks { id, count, values[count] } terminated by id 0
//           ids 1..17 are modifiers, anything else is an effect block whose
//           value index is the profile's paramID.
class Preset
{
public:
    enum { size = 1024, columns = 12, rows = 4, firstShunt = 200, lastShunt = 247 };
    struct Cell  { uint8 id = 0, mask = 0; };
    struct Chunk { uint8 id = 0; Array<uint8> values; };

    Preset()                                { clear(); }
    void clear();
    bool fromBytes (const uint8* data1024);
    bool toBytes (uint8* out1024) const;     // false if the chunks no longer fit
    int bytesUsed() const;

    String name;
    uint8 version = 0x35, reserved = 0;
    uint8 nameBytes[32] {};                  // as read, so untouched presets re-encode byte for byte
    Array<uint8> trailing;                   // anything after the chunk terminator
    std::array<Cell, columns * rows> grid;
    Array<Chunk> chunks;

    Cell& cell (int col, int row)                       { return grid[(size_t) (col * rows + row)]; }
    const Cell& cell (int col, int row) const           { return grid[(size_t) (col * rows + row)]; }
    Chunk* chunk (int id);
    const Chunk* chunk (int id) const;
    int getParam (int effectId, int paramId) const;     // -1 when not stored
    bool setParam (int effectId, int paramId, int value);
    bool isPlaced (int effectId) const;
    bool isEmpty() const;
    static String decodeName (const uint8* bytes32);
    static bool isShunt (int id)                        { return id >= firstShunt && id <= lastShunt; }
    int freeShuntId() const;
    void removeChunk (int id);

    // Modifiers live in chunks 1..17, 13 raw values each (order of the firmware's modifier_set):
    //   0 source (0 = none)  1 start  2 mid  3 end  4 slope  5 damping  6 effect id  7 parameter (200..255)
    //   8 scale  9 offset  10 auto-engage  11 PC reset  12 off value.  Floats are 0..254.
    enum { numModifierSlots = 17, modifierValues = 13 };   // firmware slots 0..16 = chunk ids 1..17
    enum ModParam { ModSource, ModStart, ModMid, ModEnd, ModSlope, ModDamping, ModEffect, ModParamId,
                    ModScale, ModOffset, ModAutoEngage, ModPCReset, ModOffVal };
    int findModifier (int effectId, int modifierParam) const;   // chunk id or -1 (source must be set)
    int assignModifier (int effectId, int modifierParam);       // finds or creates one; -1 if none free
    int modifierValue (int chunkId, int index) const;
    void setModifierValue (int chunkId, int index, int value);
    static const uint8* modifierDefaults();
};

//==============================================================================
struct AxeModel
{
    enum { Standard = 0, Ultra = 1 };
    static String folder (int model)       { return model == Standard ? "Standard" : "Ultra"; }
    static String name (int model)         { return model == Standard ? "Axe-Fx Standard" : "Axe-Fx Ultra"; }
};

namespace Sysex
{
    enum Function { ParamDump = 1, ParamSet = 2, PatchDump = 3, PatchRcv = 4, PlaceEffect = 5,
                    ConnectEffect = 6, ModifierSet = 7, QueryVersion = 8, SetName = 9, CabIR = 10,
                    Checksum = 11, SetGrid = 12, Tuner = 13, QueryStates = 14, QueryName = 15,
                    Tempo = 16 };
    enum { presetMessageSize = 0x80c, bankPresets = 128, bankMessageSize = 9 + 128 * 1024 * 2 + 3 };

    // Complete F0..F7 preset message. target: 0 = store into preset slot `number`, 1 = edit buffer.
    MemoryBlock encodePreset (const Preset&, int model, const uint8 manufacturer[3], int target, int number);
    bool decodePreset (const uint8* msg, int length, Preset& out, int* model = nullptr, int* target = nullptr, int* number = nullptr);
    MemoryBlock encodeBank (const OwnedArray<Preset>&, int model, const uint8 manufacturer[3], const uint8 headerBytes[3]);
    // Parses a bank (128 presets) or one or more concatenated preset messages.
    bool decodeData (const uint8* data, int length, OwnedArray<Preset>& out, int& model, uint8 manufacturer[3], uint8 bankHeader[3], String& error);
    // Reads a bank (128 presets) or a single preset file.
    bool loadFile (const File&, OwnedArray<Preset>& out, int& model, uint8 manufacturer[3], uint8 bankHeader[3], String& error);
}

//==============================================================================
// Everything the UI needs, owned by the app.
class Document : public ChangeBroadcaster
{
public:
    Document();
    String loadError;
    std::function<void()> onModelReloaded;   // called synchronously: cached profile/config pointers are stale
    bool loadModelData (int model, String& error);
    File dataDir() const;

    Skin skin;
    Config config;
    Profile profile;
    int model = AxeModel::Ultra;
    std::map<int, int> factoryChunkSize;     // effect id -> stored value count seen in factory banks

    OwnedArray<Preset> bank;                 // 128 presets when a bank file is the source
    File sourceFile;
    bool sourceIsBank = false, dirty = false;
    uint8 manufacturer[3] { 0x00, 0x00, 0x7d };
    uint8 bankHeader[3]   { 0x02, 0x00, 0x00 };
    int presetIndex = 0;
    Preset preset;                           // the edit buffer
    int selectedEffect = 0;

    bool open (const File&, String& error);
    bool save (const File&, bool asBank, String& error);
    void selectPreset (int index);
    void commitToBank();
    void newEmpty();

    // Edit helpers (return false when the 1024-value preset is full).
    bool placeEffect (int col, int row, int effectId);
    void removeAt (int col, int row);
    void toggleCable (int col, int row, int fromRow);
    int defaultFor (const String& type, int paramId) const;
    int chunkSizeFor (int effectId) const;
    int bypassParam (int effectId) const;    // -1 if this block has no stored bypass flag
    bool isBypassed (int effectId) const;
    void touched()                           { dirty = true; sendChangeMessage(); }
};
