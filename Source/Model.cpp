#include "Model.h"

//==============================================================================
bool StfFont::load (InputStream& compressed)
{
    GZIPDecompressorInputStream in (&compressed, false, GZIPDecompressorInputStream::zlibFormat);
    in.readString();                // typeface name
    in.readBool(); in.readBool();   // bold, italic
    ascent = in.readFloat();
    defaultChar = (juce_wchar) (uint16) in.readShort();
    const int num = in.readInt();
    if (num <= 0 || num > 70000)
        return false;

    for (int i = 0; i < num && ! in.isExhausted(); ++i)
    {
        const auto c = (juce_wchar) (uint16) in.readShort();
        Glyph g;
        g.width = in.readFloat();
        g.path.loadPathFromStream (in);
        glyphs[c] = std::move (g);
    }
    return ! glyphs.empty();
}

const StfFont::Glyph* StfFont::find (juce_wchar c) const
{
    auto it = glyphs.find (c);
    if (it == glyphs.end()) it = glyphs.find (defaultChar);
    if (it == glyphs.end()) it = glyphs.find (' ');
    return it == glyphs.end() ? nullptr : &it->second;
}

float StfFont::getWidth (const String& text, float height) const
{
    float w = 0;
    for (auto p = text.getCharPointer(); ! p.isEmpty();)
        if (auto* g = find (p.getAndAdvance()))
            w += g->width;
    return w * height;
}

void StfFont::draw (Graphics& g, const String& text, Rectangle<float> area, float height,
                    Colour colour, Justification just, bool shrinkToFit) const
{
    if (text.isEmpty() || colour.isTransparent())
        return;

    float w = getWidth (text, height), squash = 1.0f;
    if (shrinkToFit && area.getWidth() > 4 && w > area.getWidth())
    {
        squash = jmax (0.6f, area.getWidth() / w);
        w *= squash;
    }

    float x = area.getX();
    if (just.testFlags (Justification::horizontallyCentred)) x += (area.getWidth() - w) * 0.5f;
    else if (just.testFlags (Justification::right))          x += area.getWidth() - w;

    float top = area.getY();
    if (just.testFlags (Justification::verticallyCentred))   top += (area.getHeight() - height) * 0.5f;
    else if (just.testFlags (Justification::bottom))         top += area.getHeight() - height;

    const float baseline = top + ascent * height;
    g.setColour (colour);

    for (auto p = text.getCharPointer(); ! p.isEmpty();)
        if (auto* gl = find (p.getAndAdvance()))
        {
            g.fillPath (gl->path, AffineTransform::scale (height * squash, height).translated (x, baseline));
            x += gl->width * height * squash;
        }
}

//==============================================================================
bool Skin::load (const File& saxey, String& error)
{
    xml = XmlDocument::parse (saxey);
    if (xml == nullptr)                     { error = "Can't parse skin " + saxey.getFullPathName(); return false; }

    const auto zipFile = saxey.getSiblingFile (xml->getStringAttribute ("skin", "AE.paxege"));
    if (! zipFile.existsAsFile())           { error = "Missing skin archive " + zipFile.getFullPathName(); return false; }

    zip = std::make_unique<ZipFile> (zipFile);
    images.clear();
    fonts.clear();
    return zip->getNumEntries() > 0;
}

Image Skin::image (const String& name)
{
    if (name.trim().isEmpty() || zip == nullptr)
        return {};

    auto it = images.find (name);
    if (it != images.end())
        return it->second;

    Image img;
    const int index = zip->getIndexOfFileName (name.trim(), true);
    if (index >= 0)
        if (std::unique_ptr<InputStream> in { zip->createStreamForEntry (index) })
            img = ImageFileFormat::loadFrom (*in);

    images[name] = img;
    return img;
}

const StfFont& Skin::font (const String& name)
{
    auto& f = fonts[name];
    if (f == nullptr)
    {
        f = std::make_unique<StfFont>();
        const int index = zip != nullptr ? zip->getIndexOfFileName (name.trim(), true) : -1;
        bool ok = false;
        if (index >= 0)
            if (std::unique_ptr<InputStream> in { zip->createStreamForEntry (index) })
                ok = f->load (*in);

        if (! ok && name != "_default.stf")
            if (zip != nullptr && zip->getIndexOfFileName ("_default.stf", true) >= 0)
                if (std::unique_ptr<InputStream> in { zip->createStreamForEntry (zip->getIndexOfFileName ("_default.stf", true)) })
                    f->load (*in);
    }
    return *f;
}

XmlElement* Skin::child (const String& path) const
{
    XmlElement* e = xml.get();
    for (auto& part : StringArray::fromTokens (path, "/", ""))
        if (e != nullptr)
            e = e->getChildByName (part);
    return e;
}

XmlElement* Skin::widget (XmlElement* parent, const String& name) const
{
    if (parent != nullptr)
        for (auto* w : parent->getChildWithTagNameIterator ("Widget"))
            if (w->getStringAttribute ("name") == name)
                return w;
    return nullptr;
}

XmlElement* Skin::named (const String& listTag, const String& itemTag, const String& name) const
{
    if (auto* list = child (listTag))
        for (auto* e : list->getChildWithTagNameIterator (itemTag))
            if (e->getStringAttribute ("name") == name)
                return e;
    return nullptr;
}

Colour Skin::colour (const XmlElement* e, const String& attr, Colour fallback)
{
    if (e == nullptr || ! e->hasAttribute (attr))
        return fallback;
    return Colour ((uint32) e->getStringAttribute (attr).getHexValue64());
}

Rectangle<int> Skin::rect (const XmlElement* e, const String& attr)
{
    return e != nullptr ? Rectangle<int>::fromString (e->getStringAttribute (attr)) : Rectangle<int>();
}

//==============================================================================
double ParamInfo::value (int raw, int fine) const
{
    raw = jlimit (0, 255, raw);
    if (type == INT)
        return raw + minimum;

    const double range = (double) maxRaw();
    const double pos = jlimit (0.0, 1.0, raw / range);

    if (type == B16)        return minimum + (maximum - minimum) * jlimit (0.0, 1.0, (raw + fine / 256.0) / range);
    if (type == LOG && minimum > 0 && maximum > 0)
                            return minimum * std::pow (maximum / minimum, pos);
    return minimum + (maximum - minimum) * pos;
}

String ParamInfo::valueOnly (int raw, int fine) const
{
    raw = jlimit (0, 255, raw);

    if (type == INT)
    {
        if (isPositiveAndBelow (raw, items.size()) && items[raw].isNotEmpty())
            return items[raw];
        return String (raw + (int) minimum);
    }

    const double v = value (raw, fine);
    return precision > 0 ? String (v, precision) : String (roundToInt (v));
}

String ParamInfo::format (int raw, int fine) const
{
    auto s = valueOnly (raw, fine);
    return unit.isNotEmpty() && type != INT ? s + " " + unit : s;
}

bool Profile::load (const File& f)
{
    auto xml = XmlDocument::parse (f);
    if (xml == nullptr)
        return false;

    params.clear();
    model = xml->getIntAttribute ("model");
    major = xml->getIntAttribute ("majorVersion");
    minor = xml->getIntAttribute ("minorVersion");

    for (auto* e : xml->getChildWithTagNameIterator ("Parameter"))
    {
        ParamInfo p;
        const auto t = e->getStringAttribute ("paramType");
        p.type = t == "LOG" ? ParamInfo::LOG : t == "INT" ? ParamInfo::INT : t == "16B" ? ParamInfo::B16 : ParamInfo::LIN;
        p.id        = e->getIntAttribute ("paramID");
        p.def       = e->getIntAttribute ("default");
        p.minimum   = e->getDoubleAttribute ("minimum");
        p.maximum   = e->getDoubleAttribute ("maximum");
        p.precision = e->getIntAttribute ("precision");
        p.unit      = e->getStringAttribute ("unit");
        p.numVals   = e->getIntAttribute ("numVals", 255);
        for (int i = 0; e->hasAttribute ("item" + String (i)); ++i)
            p.items.add (e->getStringAttribute ("item" + String (i)));
        params[e->getStringAttribute ("effectType")][p.id] = p;
    }
    return ! params.empty();
}

const ParamInfo* Profile::find (const String& type, int id) const
{
    auto t = params.find (type);
    if (t == params.end()) return nullptr;
    auto p = t->second.find (id);
    return p == t->second.end() ? nullptr : &p->second;
}

int Profile::numParams (const String& type) const
{
    int n = 0;
    auto t = params.find (type);
    if (t != params.end())
        for (auto& p : t->second)
            if (p.first < 200)
                n = jmax (n, p.first + 1);
    return n;
}

//==============================================================================
bool Config::load (const File& f)
{
    xml = XmlDocument::parse (f);
    if (xml == nullptr)
        return false;

    pool.clear();
    names.clear();

    if (auto* p = xml->getChildByName ("EffectPool"))
        for (auto* e : p->getChildWithTagNameIterator ("EffectPoolInstance"))
            pool.add ({ e->getIntAttribute ("id"), e->getStringAttribute ("name"), e->getStringAttribute ("type") });

    if (auto* lists = xml->getChildByName ("EffectParameterLists"))
        for (auto* l : lists->getChildWithTagNameIterator ("EffectParameters"))
            for (auto* e : l->getChildWithTagNameIterator ("EffectParameter"))
                names[l->getStringAttribute ("name")][e->getStringAttribute ("name")]
                    = { e->getIntAttribute ("id"), e->getIntAttribute ("modifierID") };

    return ! pool.isEmpty();
}

const Config::PoolItem* Config::poolItem (int id) const
{
    for (auto& p : pool)
        if (p.id == id)
            return &p;
    return nullptr;
}

String Config::typeOf (int id) const
{
    if (auto* p = poolItem (id)) return p->type;
    if (id == 139) return "NoiseGate";
    if (id == 140) return "Output";
    if (id == 141) return "Controllers";
    return {};
}

int Config::paramId (const String& type, const String& name) const
{
    auto t = names.find (type);
    if (t == names.end()) return -1;
    auto p = t->second.find (name);
    return p == t->second.end() ? -1 : p->second.id;
}

String Config::paramName (const String& type, int paramId) const
{
    auto it = names.find (type);
    if (it != names.end())
        for (auto& n : it->second)
            if (n.second.id == paramId)
                return n.first;
    return {};
}

int Config::modifierId (const String& type, const String& name) const
{
    auto t = names.find (type);
    if (t == names.end()) return 0;
    auto p = t->second.find (name);
    return p == t->second.end() ? 0 : p->second.modifierId;
}

XmlElement* Config::layout (const String& type) const
{
    if (xml != nullptr)
        if (auto* l = xml->getChildByName ("EffectLayouts"))
            for (auto* e : l->getChildWithTagNameIterator ("EditorControls"))
                if (e->getStringAttribute ("name") == type)
                    return e;
    return nullptr;
}

//==============================================================================
void Preset::clear()
{
    name = {};
    zeromem (nameBytes, sizeof (nameBytes));
    trailing.clear();
    version = 0x35;
    reserved = 0;
    grid.fill ({});
    chunks.clear();
}

bool Preset::fromBytes (const uint8* d)
{
    clear();
    version = d[0];
    reserved = d[1];

    memcpy (nameBytes, d + 2, 32);
    name = decodeName (nameBytes);

    for (int i = 0; i < columns * rows; ++i)
        grid[(size_t) i] = { d[34 + i * 2], d[35 + i * 2] };

    for (int i = 130; i < size - 2 && d[i] != 0;)
    {
        const int n = d[i + 1];
        if (i + 2 + n > size)
            return false;

        Chunk c;
        c.id = d[i];
        c.values.addArray (d + i + 2, n);
        chunks.add (std::move (c));
        i += n + 2;
    }

    int end = size;
    while (end > bytesUsed() && d[end - 1] == 0)
        --end;
    if (end > bytesUsed())
        trailing.addArray (d + bytesUsed(), end - bytesUsed());
    return true;
}

String Preset::decodeName (const uint8* bytes)
{
    String s;
    for (int i = 0; i < 32 && bytes[i] != 0; ++i)
        s += (juce_wchar) (bytes[i] >= 32 && bytes[i] < 127 ? bytes[i] : '?');
    return s.trimEnd();
}

int Preset::bytesUsed() const
{
    int n = 130 + 1;    // header + grid + terminator
    for (auto& c : chunks)
        n += 2 + c.values.size();
    return n;
}

bool Preset::toBytes (uint8* d) const
{
    if (bytesUsed() > size)
        return false;

    zeromem (d, size);
    d[0] = version;
    d[1] = reserved;

    if (name == decodeName (nameBytes))
    {
        memcpy (d + 2, nameBytes, 32);
    }
    else
    {
        const auto n = name.substring (0, 32);
        for (int i = 0; i < n.length(); ++i)
            d[2 + i] = (uint8) (n[i] >= 32 && n[i] < 127 ? n[i] : ' ');
    }

    for (int i = 0; i < columns * rows; ++i)
    {
        d[34 + i * 2] = grid[(size_t) i].id;
        d[35 + i * 2] = grid[(size_t) i].mask;
    }

    int i = 130;
    for (auto& c : chunks)
    {
        d[i++] = c.id;
        d[i++] = (uint8) c.values.size();
        for (auto v : c.values)
            d[i++] = v;
    }

    ++i;    // terminator
    for (int t = 0; t < trailing.size() && i < size; ++t)
        d[i++] = trailing.getUnchecked (t);
    return true;
}

Preset::Chunk* Preset::chunk (int id)
{
    for (auto& c : chunks)
        if (c.id == id)
            return &c;
    return nullptr;
}

const Preset::Chunk* Preset::chunk (int id) const        { return const_cast<Preset*> (this)->chunk (id); }

int Preset::getParam (int effectId, int paramId) const
{
    if (auto* c = chunk (effectId))
        if (isPositiveAndBelow (paramId, c->values.size()))
            return c->values.getUnchecked (paramId);
    return -1;
}

bool Preset::setParam (int effectId, int paramId, int value)
{
    if (auto* c = chunk (effectId))
        if (isPositiveAndBelow (paramId, c->values.size()))
        {
            c->values.set (paramId, (uint8) jlimit (0, 255, value));
            return true;
        }
    return false;
}

bool Preset::isPlaced (int effectId) const
{
    for (auto& c : grid)
        if (c.id == effectId)
            return true;
    return false;
}

bool Preset::isEmpty() const
{
    for (auto& c : grid)
        if (c.id != 0)
            return false;
    return true;
}

int Preset::freeShuntId() const
{
    for (int id = firstShunt; id <= lastShunt; ++id)
        if (! isPlaced (id))
            return id;
    return 0;
}

void Preset::removeChunk (int id)
{
    for (int i = chunks.size(); --i >= 0;)
        if (chunks.getReference (i).id == id)
            chunks.remove (i);
}

// Defaults from the firmware's connectModifier() / modifier initvals (start 0 %, mid 50 %, end 100 %,
// slope 50 %, scale 100 %, offset 50 %); identical to the unused modifiers in the factory banks.
const uint8* Preset::modifierDefaults()
{
    static const uint8 d[modifierValues] { 0, 0, 127, 254, 127, 0, 0, 200, 254, 127, 0, 0, 0 };
    return d;
}

int Preset::findModifier (int effectId, int modifierParam) const
{
    for (int id = 1; id <= numModifierSlots; ++id)
        if (auto* c = chunk (id))
            if (c->values.size() >= modifierValues && c->values[ModSource] > 0
                 && c->values[ModEffect] == effectId && c->values[ModParamId] == modifierParam)
                return id;
    return -1;
}

int Preset::assignModifier (int effectId, int modifierParam)
{
    // Reuse a slot already pointing at this parameter (even one whose source was set to none).
    for (int id = 1; id <= numModifierSlots; ++id)
        if (auto* c = chunk (id))
            if (c->values.size() >= modifierValues && c->values[ModEffect] == effectId && c->values[ModParamId] == modifierParam)
                return id;

    for (int id = 1; id <= numModifierSlots; ++id)
    {
        auto* c = chunk (id);
        if (c != nullptr && c->values.size() >= modifierValues && c->values[ModSource] > 0)
            continue;
        if (c == nullptr && bytesUsed() + 2 + modifierValues > size)
            return -1;

        if (c == nullptr)
        {
            // Modifier chunks come first, in id order, as in the unit's own dumps.
            int at = 0;
            while (at < chunks.size() && chunks.getReference (at).id < id && chunks.getReference (at).id <= numModifierSlots)
                ++at;
            Chunk nc;
            nc.id = (uint8) id;
            chunks.insert (at, nc);
            c = &chunks.getReference (at);
        }

        c->values.clearQuick();
        c->values.addArray (modifierDefaults(), modifierValues);
        c->values.set (ModEffect, (uint8) effectId);
        c->values.set (ModParamId, (uint8) modifierParam);
        return id;
    }
    return -1;
}

int Preset::modifierValue (int chunkId, int index) const
{
    if (auto* c = chunk (chunkId))
        if (isPositiveAndBelow (index, c->values.size()))
            return c->values[index];
    return -1;
}

void Preset::setModifierValue (int chunkId, int index, int value)
{
    if (auto* c = chunk (chunkId))
        if (isPositiveAndBelow (index, c->values.size()))
            c->values.set (index, (uint8) jlimit (0, 255, value));
}

//==============================================================================
namespace Sysex
{
    static void appendNibbles (MemoryOutputStream& out, const uint8* values, int num, uint8& checksum)
    {
        for (int i = 0; i < num; ++i)
        {
            out.writeByte ((char) (values[i] & 0x0f));
            out.writeByte ((char) (values[i] >> 4));
            checksum ^= values[i];
        }
    }

    static void appendTail (MemoryOutputStream& out, uint8 checksum)
    {
        out.writeByte ((char) (checksum & 0x0f));
        out.writeByte ((char) (checksum >> 4));
        out.writeByte ((char) 0xf7);
    }

    MemoryBlock encodePreset (const Preset& p, int model, const uint8 m[3], int target, int number)
    {
        uint8 values[Preset::size];
        p.toBytes (values);

        MemoryOutputStream out;
        const uint8 header[] = { 0xf0, m[0], m[1], m[2], (uint8) model, PatchRcv, (uint8) target,
                                 (uint8) (number & 0x0f), (uint8) ((number >> 4) & 0x7f) };
        out.write (header, sizeof (header));
        uint8 ck = 0;
        appendNibbles (out, values, Preset::size, ck);
        appendTail (out, ck);
        return out.getMemoryBlock();
    }

    static bool unpack (const uint8* nibbles, int numValues, uint8* out)
    {
        uint8 ck = 0;
        for (int i = 0; i < numValues; ++i)
        {
            if ((nibbles[i * 2] | nibbles[i * 2 + 1]) & 0x80)
                return false;
            out[i] = (uint8) ((nibbles[i * 2] & 0x0f) | (nibbles[i * 2 + 1] << 4));
            ck ^= out[i];
        }
        const uint8 stored = (uint8) ((nibbles[numValues * 2] & 0x0f) | (nibbles[numValues * 2 + 1] << 4));
        return ck == stored;
    }

    bool decodePreset (const uint8* d, int length, Preset& out, int* model, int* target, int* number)
    {
        if (length < presetMessageSize || d[0] != 0xf0 || d[5] != PatchRcv)
            return false;

        uint8 values[Preset::size];
        if (! unpack (d + 9, Preset::size, values))
            return false;

        if (model != nullptr)  *model = d[4];
        if (target != nullptr) *target = d[6];
        if (number != nullptr) *number = d[7] | (d[8] << 4);
        return out.fromBytes (values);
    }

    MemoryBlock encodeBank (const OwnedArray<Preset>& presets, int model, const uint8 m[3], const uint8 hb[3])
    {
        MemoryOutputStream out;
        const uint8 header[] = { 0xf0, m[0], m[1], m[2], (uint8) model, PatchRcv, hb[0], hb[1], hb[2] };
        out.write (header, sizeof (header));

        uint8 ck = 0;
        for (int i = 0; i < bankPresets; ++i)
        {
            uint8 values[Preset::size] = {};
            if (auto* p = presets[i])
                p->toBytes (values);
            appendNibbles (out, values, Preset::size, ck);
        }
        appendTail (out, ck);
        return out.getMemoryBlock();
    }

    bool loadFile (const File& f, OwnedArray<Preset>& out, int& model, uint8 m[3], uint8 hb[3], String& error)
    {
        MemoryBlock mb;
        if (! f.loadFileAsData (mb) || mb.getSize() < (size_t) presetMessageSize)
        {
            error = "Not an Axe-Fx Standard/Ultra preset or bank file.";
            return false;
        }

        return decodeData ((const uint8*) mb.getData(), (int) mb.getSize(), out, model, m, hb, error);
    }

    bool decodeData (const uint8* d, int len, OwnedArray<Preset>& out, int& model, uint8 m[3], uint8 hb[3], String& error)
    {
        const int bankSize = bankMessageSize;

        if (len < presetMessageSize || d[0] != 0xf0 || d[5] != PatchRcv)
        {
            error = "Unrecognised SysEx header (expected an Axe-Fx Standard/Ultra preset dump).";
            return false;
        }
        if (d[4] > AxeModel::Ultra)
        {
            error = "This file is for a different Fractal model (id " + String ((int) d[4])
                      + "). Only Axe-Fx Standard and Ultra files are supported.";
            return false;
        }

        model = d[4];
        memcpy (m, d + 1, 3);
        out.clear();

        if (len == bankSize)
        {
            HeapBlock<uint8> values ((size_t) bankPresets * Preset::size);
            if (! unpack (d + 9, bankPresets * Preset::size, values))
            {
                error = "Bank checksum mismatch - the file looks corrupt.";
                return false;
            }
            memcpy (hb, d + 6, 3);
            for (int i = 0; i < bankPresets; ++i)
            {
                auto p = std::make_unique<Preset>();
                if (! p->fromBytes (values + i * Preset::size))
                    p->clear();
                out.add (p.release());
            }
            return true;
        }

        for (int pos = 0; pos + presetMessageSize <= len; pos += presetMessageSize)
        {
            auto p = std::make_unique<Preset>();
            if (! decodePreset (d + pos, len - pos, *p))
            {
                error = "Preset checksum mismatch - the file looks corrupt.";
                return false;
            }
            out.add (p.release());
        }
        return ! out.isEmpty();
    }
}

//==============================================================================
Document::Document()   { loadModelData (AxeModel::Ultra, loadError); }

File Document::dataDir() const
{
    auto app = File::getSpecialLocation (File::currentApplicationFile);
    auto d = app.getChildFile ("Contents/Resources/AxeData");
    return d.isDirectory() ? d : app.getSiblingFile ("AxeData");
}

bool Document::loadModelData (int newModel, String& error)
{
    const auto dir = dataDir();
    const auto folder = AxeModel::folder (newModel);
    if (onModelReloaded) onModelReloaded();

    if (skin.root() == nullptr && ! skin.load (dir.getChildFile ("Skins/Default/AE.saxey"), error))
        return false;
    if (! config.load (dir.getChildFile ("Configs/" + folder + "/default.axeml")))
        { error = "Can't load config for " + folder; return false; }
    if (! profile.load (dir.getChildFile ("Profiles/" + folder + "/default.profile")))
        { error = "Can't load profile for " + folder; return false; }

    model = newModel;
    factoryChunkSize.clear();

    for (auto& f : dir.getChildFile ("Banks/" + folder).findChildFiles (File::findFiles, false, "*.syx"))
    {
        OwnedArray<Preset> presets;
        int m; uint8 man[3], hb[3]; String e;
        if (Sysex::loadFile (f, presets, m, man, hb, e))
            for (auto* p : presets)
                for (auto& c : p->chunks)
                    factoryChunkSize[c.id] = jmax (factoryChunkSize[c.id], c.values.size());
    }
    return true;
}

bool Document::open (const File& f, String& error)
{
    OwnedArray<Preset> loaded;
    int m = model;
    uint8 man[3], hb[3] = { 2, 0, 0 };

    if (! Sysex::loadFile (f, loaded, m, man, hb, error))
        return false;
    if (m != model && ! loadModelData (m, error))
        return false;

    memcpy (manufacturer, man, 3);
    sourceIsBank = loaded.size() == Sysex::bankPresets;
    if (sourceIsBank)
        memcpy (bankHeader, hb, 3);

    bank.swapWith (loaded);
    sourceFile = f;
    presetIndex = 0;
    preset = *bank[0];
    selectedEffect = 0;
    dirty = false;
    sendChangeMessage();
    return true;
}

void Document::commitToBank()
{
    if (isPositiveAndBelow (presetIndex, bank.size()))
        *bank[presetIndex] = preset;
}

bool Document::save (const File& f, bool asBank, String& error)
{
    uint8 test[Preset::size];
    if (! preset.toBytes (test))
    {
        error = "The preset holds more data than fits in an Axe-Fx preset; remove a block first.";
        return false;
    }

    commitToBank();
    MemoryBlock data;

    if (asBank)
    {
        while (bank.size() < Sysex::bankPresets)
            bank.add (new Preset());
        data = Sysex::encodeBank (bank, model, manufacturer, bankHeader);
    }
    else
    {
        data = Sysex::encodePreset (preset, model, manufacturer, 1, 0);
    }

    if (! f.replaceWithData (data.getData(), data.getSize()))
    {
        error = "Couldn't write " + f.getFullPathName();
        return false;
    }

    if (asBank == sourceIsBank || bank.isEmpty())
    {
        sourceFile = f;
        sourceIsBank = asBank;
        dirty = false;
    }
    sendChangeMessage();
    return true;
}

void Document::selectPreset (int index)
{
    if (bank.isEmpty())
        return;
    commitToBank();
    presetIndex = jlimit (0, bank.size() - 1, index);
    preset = *bank[presetIndex];
    selectedEffect = 0;
    sendChangeMessage();
}

void Document::newEmpty()
{
    bank.clear();
    sourceFile = File();
    sourceIsBank = false;
    presetIndex = 0;
    preset.clear();
    preset.name = "New Preset";
    selectedEffect = 0;
    dirty = false;
    sendChangeMessage();
}

int Document::defaultFor (const String& type, int paramId) const
{
    if (auto* p = profile.find (type, paramId))
        return jlimit (0, 255, p->def);
    return 0;
}

int Document::chunkSizeFor (int effectId) const
{
    auto it = factoryChunkSize.find (effectId);
    if (it != factoryChunkSize.end() && it->second > 0)
        return it->second;

    const auto type = config.typeOf (effectId);
    for (auto& p : config.pool)             // another instance of the same block type
        if (p.type == type)
        {
            auto other = factoryChunkSize.find (p.id);
            if (other != factoryChunkSize.end() && other->second > 0)
                return other->second;
        }
    return jmin (127, profile.numParams (type));
}

bool Document::placeEffect (int col, int row, int effectId)
{
    const Preset backup (preset);
    const bool shunt = Preset::isShunt (effectId);

    if (! shunt)
        for (int c = 0; c < Preset::columns; ++c)       // a pool block exists once: move it
            for (int r = 0; r < Preset::rows; ++r)
                if (preset.cell (c, r).id == effectId)
                    preset.cell (c, r) = {};

    auto& target = preset.cell (col, row);
    const uint8 oldMask = target.mask;
    if (target.id != 0 && ! Preset::isShunt (target.id) && target.id != effectId)
        preset.removeChunk (target.id);

    target.id = (uint8) effectId;
    target.mask = oldMask;
    if (target.mask == 0 && col > 0 && preset.cell (col - 1, row).id != 0)
        target.mask = (uint8) (1 << row);

    if (! shunt && preset.chunk (effectId) == nullptr)
    {
        Preset::Chunk c;
        c.id = (uint8) effectId;
        const auto type = config.typeOf (effectId);
        for (int i = 0, n = chunkSizeFor (effectId); i < n; ++i)
            c.values.add ((uint8) defaultFor (type, i));

        int insertAt = preset.chunks.size();            // keep noise gate/output/controllers last
        for (int i = 0; i < preset.chunks.size(); ++i)
            if (preset.chunks.getReference (i).id >= 139 && preset.chunks.getReference (i).id <= 141)
                { insertAt = i; break; }
        preset.chunks.insert (insertAt, c);
    }

    if (preset.bytesUsed() > Preset::size)
    {
        preset = backup;
        return false;
    }

    selectedEffect = effectId;
    touched();
    return true;
}

void Document::removeAt (int col, int row)
{
    auto& c = preset.cell (col, row);
    if (c.id == 0)
        return;

    if (! Preset::isShunt (c.id))
        preset.removeChunk (c.id);
    if (selectedEffect == c.id)
        selectedEffect = 0;

    c = {};
    if (col + 1 < Preset::columns)
        for (int r = 0; r < Preset::rows; ++r)
            preset.cell (col + 1, r).mask &= (uint8) ~(1 << row);
    touched();
}

void Document::toggleCable (int col, int row, int fromRow)
{
    if (col <= 0 || preset.cell (col, row).id == 0 || preset.cell (col - 1, fromRow).id == 0)
        return;
    preset.cell (col, row).mask ^= (uint8) (1 << fromRow);
    touched();
}

int Document::bypassParam (int effectId) const
{
    const auto type = config.typeOf (effectId);
    static const char* const prefixes[] = { "CABINET", "COMP", "CROSSOVER", "DELAY", "FUZZ", "ENHANCER", "FEEDBACK", "FILTER",
                                            "FORMANT", "GATE", "GEQ", "LOOP", "MEGATAP", "MULTICOMP", "MULTITAP", "PEQ",
                                            "PHASER", "QUADCHO", "RESONATOR", "REVERB", "RINGMOD", "ROTARY", "SYNTH",
                                            "VOCODER", "VOLUME", "TREMOLO", "WAH", "NOISEGATE", "OUTPUT" };
    for (auto* p : prefixes)
    {
        const int id = config.paramId (type, String (p) + "_BYPASS");
        if (id >= 0)
            return id;
    }
    return -1;
}

bool Document::isBypassed (int effectId) const
{
    const int p = bypassParam (effectId);
    return p >= 0 && preset.getParam (effectId, p) > 0;
}
