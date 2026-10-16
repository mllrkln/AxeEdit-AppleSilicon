#include "Extras.h"
#include <complex>
#include <set>

//==============================================================================
namespace Graphs
{
    double biquadDb (int type, double f0, double q, double gainDb, double atHz, double fs)
    {
        if (type <= 0 || type > 7)
            return 0.0;

        f0 = jlimit (1.0, fs * 0.49, f0);
        q  = jmax (0.01, q);
        const double w0 = MathConstants<double>::twoPi * f0 / fs;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * q);
        const double A = std::pow (10.0, gainDb / 40.0);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (type)
        {
            case 1: b0 = (1 - cw) * 0.5; b1 = 1 - cw;    b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
            case 2: b0 = alpha;          b1 = 0;         b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
            case 3: b0 = (1 + cw) * 0.5; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
            case 4:
            {
                const double s = sw * std::sqrt (A) / q;       // firmware lowshelfEQ
                b0 = A * ((A + 1) - (A - 1) * cw + s);  b1 = 2 * A * ((A - 1) - (A + 1) * cw);  b2 = A * ((A + 1) - (A - 1) * cw - s);
                a0 = (A + 1) + (A - 1) * cw + s;        a1 = -2 * ((A - 1) + (A + 1) * cw);     a2 = (A + 1) + (A - 1) * cw - s;
                break;
            }
            case 5:
            {
                const double s = sw * std::sqrt (A) / q;       // firmware highshelfEQ
                b0 = A * ((A + 1) + (A - 1) * cw + s);  b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
                a0 = (A + 1) - (A - 1) * cw + s;        a1 = 2 * ((A - 1) - (A + 1) * cw);      a2 = (A + 1) - (A - 1) * cw - s;
                break;
            }
            case 6: b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
            case 7: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
            default: break;
        }

        const double w = MathConstants<double>::twoPi * atHz / fs;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2 * w);
        const auto num = b0 + b1 * z1 + b2 * z2;
        const auto den = a0 + a1 * z1 + a2 * z2;
        const double mag = std::abs (num) / jmax (1e-12, std::abs (den));
        return 20.0 * std::log10 (jmax (1e-9, mag));
    }

    static double paramValue (const Document& d, int effectId, const String& type, const String& name, double fallback)
    {
        const int id = d.config.paramId (type, name);
        auto* info = d.profile.find (type, id);
        if (id < 0 || info == nullptr)
            return fallback;
        int raw = d.preset.getParam (effectId, id);
        if (raw < 0) raw = info->def;
        return info->type == ParamInfo::INT ? (double) raw : info->value (raw);
    }

    static double frequencyAt (int i)       { return 20.0 * std::pow (10.0, 3.0 * i / 63.0); }   // firmware: 20 * 10^(3i/63)

    bool compute (const Document& d, int effectId, const String& monitor, const String& pageName, Curve& c)
    {
        const auto type = d.config.typeOf (effectId);
        c = {};

        if (monitor == "FilterGraph" || monitor == "PEQGraph")
        {
            c.minimum = -15.0f; c.maximum = 15.0f;      // _FilterGraph1..4 / _PEQGraph
            c.labels = { "20", "200", "2K", "20K" };
            c.gridLines = 2;

            struct Band { int type; double f, q, g; };
            Array<Band> bands;
            if (type == "Filter")
            {
                bands.add ({ (int) paramValue (d, effectId, type, "FILTER_TYPE", 0), paramValue (d, effectId, type, "FILTER_FREQ", 1000),
                             paramValue (d, effectId, type, "FILTER_Q", 0.707), paramValue (d, effectId, type, "FILTER_GAIN", 0) });
            }
            else if (type == "ParametricEQ")
            {
                // paraEQ_set typeMap: LF {shelving, peaking, blocking} -> {low shelf, peak, high-pass},
                //                     HF {shelving, peaking, blocking} -> {high shelf, peak, low-pass}.
                static const int lf[] { 4, 6, 3 }, hf[] { 5, 6, 1 };
                const int lt = jlimit (0, 2, (int) paramValue (d, effectId, type, "PEQ_LFTYPE", 0));
                const int ht = jlimit (0, 2, (int) paramValue (d, effectId, type, "PEQ_HFTYPE", 0));
                for (int b = 1; b <= 5; ++b)
                    bands.add ({ b == 1 ? lf[lt] : b == 5 ? hf[ht] : 6,
                                 paramValue (d, effectId, type, "PEQ_FREQ" + String (b), 1000),
                                 paramValue (d, effectId, type, "PEQ_Q" + String (b), 0.707),
                                 paramValue (d, effectId, type, "PEQ_GAIN" + String (b), 0) });
            }
            else
                return false;

            for (int i = 0; i < 64; ++i)
            {
                double db = 0;
                for (auto& b : bands)
                    db += biquadDb (b.type, b.f, b.q, b.g, frequencyAt (i));
                c.points.add ((float) db);
            }
            return true;
        }

        if (monitor == "ControllersGraph" && type == "Controllers")
        {
            // calcGraph(ADSR*): attack 0->1, decay 1->level, hold the level for the sustain time, release to 0.
            const String n = pageName.contains ("2") ? "2" : "1";
            const double a = paramValue (d, effectId, type, "CONTROLLERS_ADSR" + n + "ATTACK", 10);
            const double dc = paramValue (d, effectId, type, "CONTROLLERS_ADSR" + n + "DECAY", 10);
            const double s = paramValue (d, effectId, type, "CONTROLLERS_ADSR" + n + "SUSTAIN", 10);
            const double lv = paramValue (d, effectId, type, "CONTROLLERS_ADSR" + n + "LEVEL", 50) / 100.0;
            const double r = paramValue (d, effectId, type, "CONTROLLERS_ADSR" + n + "RELEASE", 10);
            const double total = jmax (1e-6, a + dc + s + r), step = total * 0.01;

            c.minimum = 0.0f; c.maximum = 1.0f; c.gridLines = 1;
            for (int i = 0; i < 100; ++i)
            {
                const double t = step * 0.5 + i * step;
                double v;
                if (t <= a)                     v = t / jmax (1e-6, a);
                else if (t <= a + dc)           v = 1.0 + (lv - 1.0) * (t - a) / jmax (1e-6, dc);
                else if (t <= a + dc + s)       v = lv;
                else if (t <= total)            v = lv * (1.0 - (t - a - dc - s) / jmax (1e-6, r));
                else                            v = 0.0;
                c.points.add ((float) v);
            }
            return true;
        }
        return false;
    }

    void paint (Graphics& g, Rectangle<float> plot, const Curve& c)
    {
        // plot: the GRAPH rectangle (180 x 120). The skin's monitor image already carries the frame,
        // the dotted grid and the axis labels.
        if (c.points.size() < 2) return;
        plot = plot.reduced (2.0f, 2.0f);
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (plot.expanded (1.0f).toNearestInt());

        Path p;
        for (int i = 0; i < c.points.size(); ++i)
        {
            const float norm = jlimit (0.0f, 1.0f, (c.points[i] - c.minimum) / (c.maximum - c.minimum));
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) (c.points.size() - 1);
            const float y = plot.getBottom() - norm * plot.getHeight();
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (Colour (0xff9be05a));
        g.strokePath (p, PathStrokeType (1.6f, PathStrokeType::curved, PathStrokeType::rounded));
    }
}

//==============================================================================
double cpuUnits (const Document& d)
{
    // BLOCK::cpu as set by each *_init in the firmware model (reverb_set lowers it to 5 for the
    // plate types, reverb type 15..17).
    static const std::map<String, double> cost {
        { "Amp", 24 }, { "Cab", 9 }, { "Chorus", 7 }, { "Compressor", 4 }, { "Crossover", 2 }, { "Delay", 6 },
        { "Drive", 8 }, { "Enhancer", 1 }, { "FeedbackSend", 1 }, { "FeedbackReturn", 1 }, { "Filter", 1 },
        { "Flanger", 4 }, { "Formant", 2 }, { "GateExpander", 3 }, { "GraphicEQ", 2 }, { "MegaTap", 5 },
        { "MultibandComp", 8 }, { "MultiDelay", 8 }, { "ParametricEQ", 2 }, { "Phaser", 6 }, { "Pitch", 8 },
        { "QuadChorus", 9 }, { "Resonator", 3 }, { "Reverb", 10 }, { "RingMod", 2 }, { "Rotary", 3 },
        { "Synth", 8 }, { "Vocoder", 13 }, { "VolPan", 1 }, { "PanTrem", 4 }, { "Wah", 1 }, { "Mixer", 1 },
        { "Output", 1 }, { "EffectsLoop", 1 }, { "NoiseGate", 1 } };

    double sum = 0;
    std::set<int> seen;
    for (auto& cell : d.preset.grid)
    {
        if (cell.id == 0 || Preset::isShunt (cell.id) || ! seen.insert (cell.id).second)
            continue;
        const auto type = d.config.typeOf (cell.id);
        auto it = cost.find (type);
        double c = it != cost.end() ? it->second : 1.0;
        if (type == "Reverb")
        {
            const int t = d.preset.getParam (cell.id, d.config.paramId (type, "REVERB_TYPE"));
            if (t >= 15 && t <= 17) c = 5;
        }
        sum += c;
    }
    return sum;
}

//==============================================================================
AudioMonitor::AudioMonitor() {}
AudioMonitor::~AudioMonitor()           { manager.removeAudioCallback (this); manager.closeAudioDevice(); }

void AudioMonitor::restore (const String& stateXml)
{
    auto xml = parseXML (stateXml);
    if (xml == nullptr || ! xml->getBoolAttribute ("enabled"))
        return;
    if (auto* dev = xml->getChildByName ("DEVICESETUP"))
    {
        auto copy = std::make_unique<XmlElement> (*dev);
        if (manager.initialise (2, 2, copy.get(), true).isEmpty())
        {
            manager.addAudioCallback (this);
            running = true;
        }
    }
}

void AudioMonitor::enable (bool shouldRun, String& error)
{
    if (shouldRun == running) return;
    if (shouldRun)
    {
        error = manager.initialise (2, 2, nullptr, true);
        if (error.isEmpty())
        {
            manager.addAudioCallback (this);
            running = true;
        }
    }
    else
    {
        manager.removeAudioCallback (this);
        manager.closeAudioDevice();
        running = false;
        for (auto& side : peaks) for (auto& p : side) p = 0.0f;
    }
}

String AudioMonitor::state() const
{
    XmlElement root ("AUDIO");
    root.setAttribute ("enabled", running);
    if (running)
        if (auto dev = manager.createStateXml())
            root.addChildElement (dev.release());
    return root.toString();
}

void AudioMonitor::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                     int numSamples, const AudioIODeviceCallbackContext&)
{
    const float gi = inputGain.load(), go = outputGain.load();
    float inPeak[2] {}, outPeak[2] {};

    for (int ch = 0; ch < numOut; ++ch)
    {
        if (out[ch] == nullptr) continue;
        const int src = jmin (ch, numIn - 1);
        const float* s = src >= 0 ? in[src] : nullptr;
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = s != nullptr ? s[i] * gi : 0.0f;
            const float y = x * go;
            out[ch][i] = y;
            if (ch < 2)
            {
                inPeak[ch]  = jmax (inPeak[ch], std::abs (x));
                outPeak[ch] = jmax (outPeak[ch], std::abs (y));
            }
        }
    }
    if (numOut < 2 && numIn > 0)
        for (int ch = 0; ch < jmin (2, numIn); ++ch)
            if (in[ch] != nullptr)
                for (int i = 0; i < numSamples; ++i)
                    inPeak[ch] = jmax (inPeak[ch], std::abs (in[ch][i] * gi));
    if (numIn == 1) inPeak[1] = inPeak[0];

    for (int ch = 0; ch < 2; ++ch)
    {
        // hold the highest peak until the GUI has read it (it decays the display itself)
        peaks[0][ch] = jmax (peaks[0][ch].load() * 0.9f, inPeak[ch]);
        peaks[1][ch] = jmax (peaks[1][ch].load() * 0.9f, outPeak[ch]);
        if (inPeak[ch] > 0.99f)  clip[0] = true;    // AxeFxFilter::processBlock: > 0.99 latches the clip LED
        if (outPeak[ch] > 0.99f) clip[1] = true;
    }
}

//==============================================================================
static const char* const modSources[] { "none", "LFO 1A", "LFO 1B", "LFO 2A", "LFO 2B", "ADSR 1", "ADSR 2", "Sequencer",
                                        "Envelope", "Pitch", "Extern 1", "Extern 2", "Extern 3", "Extern 4",
                                        "Extern 5", "Extern 6", "Extern 7", "Extern 8" };

// slider index -> modifier value index, display range (ModifierDialog::setRange) and unit
struct ModSlider { int index; const char* name; double lo, hi; const char* unit; };
static const ModSlider modSliders[] {
    { Preset::ModStart,   "Start",   0, 100,  "%" }, { Preset::ModMid,    "Mid",     0, 100,  "%" },
    { Preset::ModEnd,     "End",     0, 100,  "%" }, { Preset::ModSlope,  "Slope",   0, 100,  "%" },
    { Preset::ModScale,   "Scale",   0, 100,  "%" }, { Preset::ModOffset, "Offset",  0, 100,  "%" },
    { Preset::ModDamping, "Damping", 0, 1000, "ms" }, { Preset::ModOffVal, "Off Val", 5, 95,  "%" } };

ModifierEditor::ModifierEditor (Document& d, int effect, int param, const String& l, std::function<void (int, int, int)> a)
    : doc (d), effectId (effect), modParam (param), label (l), apply (std::move (a))
{
    for (int i = 0; i < (int) std::size (modSources); ++i)
        source.addItem (modSources[i], i + 1);
    for (auto* s : { "Off", "Slow", "Medium", "Fast" })   // firmware engageStr: OFF SLOW MED FAST
        autoEngage.addItem (s, autoEngage.getNumItems() + 1);
    pcReset.addItem ("Off", 1);
    pcReset.addItem ("On", 2);

    source.onChange     = [this] { if (! updating) set (Preset::ModSource, source.getSelectedId() - 1); };
    autoEngage.onChange = [this] { if (! updating) set (Preset::ModAutoEngage, autoEngage.getSelectedId() - 1); };
    pcReset.onChange    = [this] { if (! updating) set (Preset::ModPCReset, pcReset.getSelectedId() - 1); };

    for (auto& ms : modSliders)
    {
        auto* s = sliders.add (new Slider (Slider::LinearHorizontal, Slider::TextBoxRight));
        s->setRange (0, 254, 1);
        const double lo = ms.lo, hi = ms.hi;
        const String unit = ms.unit;
        s->textFromValueFunction = [lo, hi, unit] (double v) { return String (lo + (hi - lo) * v / 254.0, unit == "ms" ? 0 : 1) + " " + unit; };
        s->valueFromTextFunction = [lo, hi] (const String& t) { return jlimit (0.0, 254.0, (t.getDoubleValue() - lo) * 254.0 / (hi - lo)); };
        s->setTextBoxStyle (Slider::TextBoxRight, false, 70, 20);
        const int index = ms.index;
        s->onValueChange = [this, s, index] { if (! updating) set (index, (int) s->getValue()); };
        s->updateText();
        addAndMakeVisible (s);
        auto* lab = sliderLabels.add (new Label ({}, ms.name));
        lab->setJustificationType (Justification::centredRight);
        addAndMakeVisible (lab);
    }

    for (auto* c : { &source, &autoEngage, &pcReset })
        addAndMakeVisible (c);
    info.setJustificationType (Justification::topLeft);
    info.setFont (FontOptions (13.0f));
    addAndMakeVisible (info);

    chunkId = doc.preset.findModifier (effectId, modParam);
    if (chunkId < 0)
        for (int id = 1; id <= Preset::numModifierSlots; ++id)
            if (auto* c = doc.preset.chunk (id))
                if (c->values.size() >= Preset::modifierValues && c->values[Preset::ModEffect] == effectId
                     && c->values[Preset::ModParamId] == modParam)
                    chunkId = id;
    refresh();
    setSize (560, 360);
    startTimer (400);     // follows undo/redo and edits from the unit
}

int ModifierEditor::raw (int index) const
{
    if (chunkId > 0)
        if (const int v = doc.preset.modifierValue (chunkId, index); v >= 0)
            return v;
    return Preset::modifierDefaults()[index];
}

void ModifierEditor::set (int index, int value)
{
    refresh();
    if (chunkId < 0 || doc.preset.chunk (chunkId) == nullptr)
    {
        if (index == Preset::ModSource && value == 0)
            return;     // nothing to disconnect
        chunkId = doc.preset.assignModifier (effectId, modParam);
        if (chunkId < 0)
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Modifier",
                "All 17 modifiers of this preset are in use (or the preset is full). Set another modifier's source to none first.");
            refresh();
            return;
        }
    }
    if (apply) apply (chunkId, index, value);
    refresh();
}

void ModifierEditor::refresh()
{
    // after undo, a preset change or an edit from the unit the slot may be gone or hold another modifier
    auto* c = chunkId > 0 ? doc.preset.chunk (chunkId) : nullptr;
    if (c == nullptr || c->values.size() < Preset::modifierValues
         || c->values[Preset::ModEffect] != effectId || c->values[Preset::ModParamId] != modParam)
    {
        chunkId = doc.preset.findModifier (effectId, modParam);
        if (chunkId < 0)
            for (int id = 1; id <= Preset::numModifierSlots; ++id)
                if (auto* other = doc.preset.chunk (id))
                    if (other->values.size() >= Preset::modifierValues && other->values[Preset::ModEffect] == effectId
                         && other->values[Preset::ModParamId] == modParam)
                        chunkId = id;
    }
    const ScopedValueSetter<bool> s (updating, true);
    source.setSelectedId (jlimit (0, 17, raw (Preset::ModSource)) + 1, dontSendNotification);
    autoEngage.setSelectedId (jlimit (0, 3, raw (Preset::ModAutoEngage)) + 1, dontSendNotification);
    pcReset.setSelectedId (jlimit (0, 1, raw (Preset::ModPCReset)) + 1, dontSendNotification);
    const bool on = raw (Preset::ModSource) > 0;
    for (int i = 0; i < sliders.size(); ++i)
    {
        sliders[i]->setValue (raw (modSliders[i].index), dontSendNotification);
        sliders[i]->setEnabled (on);
    }
    autoEngage.setEnabled (on);
    pcReset.setEnabled (on);
    info.setText (on ? "Modifier " + String (chunkId) + " of this preset" : "Choose a source to attach a modifier.", dontSendNotification);
    repaint();
}

void ModifierEditor::resized()
{
    auto r = getLocalBounds().reduced (14);
    r.removeFromTop (26);
    auto row = [&] (int h) { auto x = r.removeFromTop (h); r.removeFromTop (6); return x; };
    auto top = row (24);
    top.removeFromLeft (120);
    source.setBounds (top.removeFromLeft (150));
    info.setBounds (top.withTrimmedLeft (12));

    auto left = r.removeFromLeft (310);
    for (int i = 0; i < sliders.size(); ++i)
    {
        auto line = left.removeFromTop (26);
        left.removeFromTop (3);
        sliderLabels[i]->setBounds (line.removeFromLeft (70));
        sliders[i]->setBounds (line);
    }
    r.removeFromLeft (16);
    graphArea = r.removeFromTop (170);
    r.removeFromTop (14);
    auto ae = r.removeFromTop (26);
    engageLabel = ae.removeFromLeft (90);
    autoEngage.setBounds (ae.withTrimmedLeft (6));
    r.removeFromTop (6);
    auto pc = r.removeFromTop (26);
    resetLabel = pc.removeFromLeft (90);
    pcReset.setBounds (pc.withTrimmedLeft (6));
}

void ModifierEditor::paint (Graphics& g)
{
    g.fillAll (Colour (0xff1e1e1e));
    g.setColour (Colours::white);
    g.setFont (FontOptions (16.0f, Font::bold));
    g.drawText ("Modifier: " + label, getLocalBounds().reduced (14).removeFromTop (22), Justification::centredLeft);
    g.setFont (FontOptions (13.0f));
    g.setColour (Colours::lightgrey);
    g.drawText ("Source", source.getBounds().withX (14).withWidth (source.getX() - 22), Justification::centredRight);
    g.drawText ("Auto Engage", engageLabel, Justification::centredRight);
    g.drawText ("PC Reset", resetLabel, Justification::centredRight);

    // modifier_set: 4th-order fit through start, the slope point, mid, the slope point and end;
    // output = curve * scale + offset, 50 points.
    auto area = graphArea.toFloat().withTrimmedBottom (16.0f);
    g.setColour (Colour (0xff101010));
    g.fillRect (area);
    g.setColour (Colour (0xff3a3a3a));
    g.drawRect (area);

    auto f = [this] (int i) { return raw (i) / 254.0; };
    const double y0 = f (Preset::ModStart) - 0.5, y2 = f (Preset::ModMid) - 0.5, y4 = f (Preset::ModEnd) - 0.5;
    const double slope = f (Preset::ModSlope);
    const double ys[5] { y0, (y2 - y0) * (1.0 - slope) + y0, y2, y2 + slope * (y4 - y2), y4 };
    const double xs[5] { 0.0, 0.25, 0.5, 0.75, 1.0 };
    const double scale = f (Preset::ModScale), offset = f (Preset::ModOffset);

    Path p;
    for (int i = 0; i < 50; ++i)
    {
        const double x = i / 49.0;
        double y = 0;     // Lagrange form of the exact fit through the five points
        for (int k = 0; k < 5; ++k)
        {
            double l = 1;
            for (int j = 0; j < 5; ++j)
                if (j != k) l *= (x - xs[j]) / (xs[k] - xs[j]);
            y += ys[k] * l;
        }
        y = y * scale + offset;
        const float px = area.getX() + 4 + (float) x * (area.getWidth() - 8);
        const float py = area.getBottom() - 4 - (float) jlimit (0.0, 1.0, y) * (area.getHeight() - 8);
        if (i == 0) p.startNewSubPath (px, py); else p.lineTo (px, py);
    }
    g.setColour (raw (Preset::ModSource) > 0 ? Colour (0xffffb020) : Colours::grey);
    g.strokePath (p, PathStrokeType (2.0f));
    g.setColour (Colours::grey);
    g.setFont (FontOptions (11.0f));
    g.drawText ("controller in  ->", graphArea.withTop (graphArea.getBottom() - 14), Justification::centred);
}

void ModifierEditor::show (Component& parent, Document& d, int effectId, int modifierParam, const String& label,
                           std::function<void (int, int, int)> apply)
{
    DialogWindow::LaunchOptions o;
    o.content.setOwned (new ModifierEditor (d, effectId, modifierParam, label, std::move (apply)));
    o.dialogTitle = "Modifier";
    o.componentToCentreAround = &parent;
    o.dialogBackgroundColour = Colour (0xff1e1e1e);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

//==============================================================================
namespace Csv
{
    static String quoted (const String& s)  { return "\"" + s.removeCharacters ("\"") + "\""; }

    String exportPreset (const Document& d, const Preset& p)
    {
        StringArray f;
        f.add (String (p.version));
        f.add ("10.0");
        f.add (quoted (p.name));

        for (int i = 0; i < Preset::columns * Preset::rows; ++i)
        {
            // CSVManager walks the grid column by column, like the preset bytes.
            const auto& c = p.grid[(size_t) i];
            String name;
            if (c.id != 0 && ! Preset::isShunt (c.id))
                if (auto* item = d.config.poolItem (c.id))
                    name = item->name;
            f.add (String (c.id)); f.add (quoted (name)); f.add (String (c.mask));
        }

        for (int slot = 0; slot < Preset::numModifierSlots; ++slot)
            for (int v = 0; v < Preset::modifierValues; ++v)
            {
                const int raw = p.modifierValue (slot + 1, v);
                f.add (String (raw >= 0 ? raw : (int) Preset::modifierDefaults()[v]));
            }

        Array<Config::PoolItem> pool;
        for (auto& item : d.config.pool)
            if (item.id >= 100 && item.id < Preset::firstShunt)
                pool.add (item);
        std::sort (pool.begin(), pool.end(), [] (auto& a, auto& b) { return a.id < b.id; });

        for (auto& item : pool)
        {
            auto* chunk = p.chunk (item.id);
            const int count = chunk != nullptr ? chunk->values.size() : d.chunkSizeFor (item.id);
            if (chunk != nullptr)
            {
                f.add (String (item.id)); f.add (quoted (item.name));
                for (int i = 0; i < count; ++i)
                {
                    f.add (String (i)); f.add (quoted (d.config.paramName (item.type, i))); f.add (String ((int) chunk->values[i]));
                }
            }
            else
            {
                f.add ("0"); f.add (quoted ({}));
                for (int i = 0; i < count; ++i) { f.add ("0"); f.add (quoted ({})); f.add ("0"); }
            }
        }
        f.add (quoted ("EOF"));
        return f.joinIntoString (",");
    }

    static StringArray tokens (const String& text)
    {
        StringArray out;
        String cur;
        bool inQuotes = false;
        for (auto ch : text)
        {
            if (ch == '"')                     inQuotes = ! inQuotes;
            else if (ch == ',' && ! inQuotes)  { out.add (cur.trim()); cur.clear(); }
            else if (ch != '\r' && ch != '\n') cur << ch;
        }
        if (cur.isNotEmpty()) out.add (cur.trim());
        return out;
    }

    bool importPreset (const Document& d, const String& text, Preset& out, String& error)
    {
        const auto t = tokens (text);
        int i = 0;
        auto next = [&] () -> String { return i < t.size() ? t[i++] : String(); };
        if (t.size() < 3 + 48 * 3 + Preset::numModifierSlots * Preset::modifierValues)
        {
            error = "This isn't an Axe-Fx Standard/Ultra preset CSV (too few values).";
            return false;
        }

        Preset p;
        p.version = (uint8) jlimit (0, 255, next().getIntValue());
        if (next() != "10.0")
        {
            error = "Unknown CSV version (expected 10.0).";
            return false;
        }
        p.name = next().substring (0, 32);
        zeromem (p.nameBytes, sizeof (p.nameBytes));

        for (int c = 0; c < Preset::columns * Preset::rows; ++c)
        {
            p.grid[(size_t) c].id = (uint8) jlimit (0, 255, next().getIntValue());
            next();
            p.grid[(size_t) c].mask = (uint8) jlimit (0, 255, next().getIntValue());
        }

        for (int slot = 0; slot < Preset::numModifierSlots; ++slot)
        {
            Preset::Chunk ch;
            ch.id = (uint8) (slot + 1);
            for (int v = 0; v < Preset::modifierValues; ++v)
                ch.values.add ((uint8) jlimit (0, 255, next().getIntValue()));
            if (ch.values[Preset::ModSource] > 0)
                p.chunks.add (ch);
        }

        // Blocks: id,"name" then index,"parameter",value with index counting up from 0. Unused pool
        // entries are written as 0,"" followed by 0,"",0 triples, i.e. nothing but zeros and empty
        // strings up to the next used block's id (>= 100) or "EOF".
        while (i < t.size())
        {
            const auto idText = next();
            if (idText == "EOF") break;
            const int id = idText.getIntValue();
            if (id == 0)
            {
                while (i < t.size() && t[i] != "EOF" && t[i].getIntValue() < 100)
                    ++i;
                continue;
            }
            next();     // block name
            Preset::Chunk ch;
            ch.id = (uint8) id;
            while (i + 2 < t.size() && t[i] == String (ch.values.size()))
            {
                ch.values.add ((uint8) jlimit (0, 255, t[i + 2].getIntValue()));
                i += 3;
            }
            if (id >= 100 && id < Preset::firstShunt && ! ch.values.isEmpty())
                p.chunks.add (ch);
        }

        if (p.bytesUsed() > Preset::size)
        {
            error = "The CSV holds more data than fits in one preset.";
            return false;
        }
        juce::ignoreUnused (d);
        out = p;
        return true;
    }
}

//==============================================================================
FirmwareUpdater::FirmwareUpdater (AxeMidi& m) : midi (m) {}

bool FirmwareUpdater::start (const File& file, String& error)
{
    MemoryBlock data;
    if (! file.loadFileAsData (data) || data.getSize() < 8)
    {
        error = "Couldn't read " + file.getFileName() + ".";
        return false;
    }

    messages.clear();
    auto* b = (const uint8*) data.getData();
    const int n = (int) data.getSize();
    for (int i = 0; i < n;)
    {
        if (b[i] != 0xf0) { error = "This file isn't a SysEx (.syx) file."; return false; }
        int end = i + 1;
        while (end < n && b[end] != 0xf7) ++end;
        if (end >= n) { error = "The file ends in the middle of a SysEx message."; return false; }
        const bool fractal = end - i > 5 && ((b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 0x7d) || (b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 0x74));
        if (! fractal) { error = "The file contains SysEx that isn't for a Fractal Audio unit."; return false; }
        messages.add (MemoryBlock (b + i, (size_t) (end - i + 1)));
        i = end + 1;
    }

    totalBytes = n;
    sentBytes = 0;
    next = 0;
    newMajor = newMinor = -1;
    stage = Sending;
    midi.setExclusive (true);
    status = "Sending firmware...";
    startTimer (250);
    sendNext();
    return true;
}

void FirmwareUpdater::sendNext()
{
    if (stage != Sending) return;
    if (next >= messages.size())
    {
        midi.setExclusive (false);
        stage = Installing;
        stageStart = Time::getMillisecondCounter();
        status = "Firmware Send Finished...\nDO NOT reboot the Axe-Fx or use the front panel\nwhile it is installing the new firmware!\n\nPlease wait";
        return;
    }

    const auto& m = messages.getReference (next);
    const bool ok = midi.sendPaced (m, [this, size = (int64) m.getSize()] (bool sent)
    {
        if (stage != Sending) return;
        if (! sent) { finish (false, "Firmware Update Canceled!"); return; }
        sentBytes += size;
        ++next;
        // a short gap between messages, as the original's timer-driven sender left
        Timer::callAfterDelay (40, [this] { sendNext(); });
    });
    if (! ok)
        finish (false, "Couldn't send to the MIDI output. Check that the Axe-Fx is still connected.");
}

double FirmwareUpdater::progress() const
{
    if (stage == Sending)   return totalBytes > 0 ? (double) sentBytes / (double) totalBytes : 0.0;
    return stage == Idle ? 0.0 : 1.0;
}

void FirmwareUpdater::cancel()
{
    if (stage == Sending)
        midi.cancelTransfer();
    finish (false, "Firmware Update Canceled!");
}

void FirmwareUpdater::versionReceived (int major, int minor)
{
    if (stage != Polling) return;
    newMajor = major;
    newMinor = minor;
    if (major == oldMajor && minor == oldMinor && oldMajor >= 0)
        finish (false, "Firmware Update Unknown!\n(Hardware is still at same version.)\nPlease check your MIDI settings and try again.");
    else
        finish (true, "Firmware Update Success!\nThe Axe-Fx reports firmware " + String (major) + "." + String (minor).paddedLeft ('0', 2) + ".");
}

void FirmwareUpdater::timerCallback()
{
    const auto elapsed = Time::getMillisecondCounter() - stageStart;
    if (stage == Installing && elapsed > 10000)      // AxeFxUpdateFirmware::run waits 10 s, then polls
    {
        stage = Polling;
        stageStart = Time::getMillisecondCounter();
        status = "Waiting for the Axe-Fx to restart...";
    }
    else if (stage == Polling)
    {
        if (elapsed > 100000)
            finish (false, "Firmware Update Unknown!\n(Did not get a response from the hardware.)\nPlease check your MIDI settings and try again.");
        else if ((elapsed / 250) % 12 == 0)
            midi.queryVersion();
    }
}

void FirmwareUpdater::finish (bool ok, const String& message)
{
    if (stage == Idle) return;
    stage = Idle;
    midi.setExclusive (false);
    stopTimer();
    status = message;
    if (onFinished)
        onFinished (ok, message);
}
