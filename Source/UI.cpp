#include "UI.h"

static void drawAt (Graphics& g, const Image& img, int x, int y)
{
    if (img.isValid())
        g.drawImageAt (img, x, y);
}

static Point<int> posOf (const XmlElement* e, const String& prefix = "position")
{
    return e != nullptr ? Point<int> (e->getStringAttribute (prefix + "X").getIntValue(),
                                      e->getStringAttribute (prefix + "Y").getIntValue())
                        : Point<int>();
}

//==============================================================================
Document& UnitBase::doc()   { return main.doc; }
Skin& UnitBase::skin()      { return main.doc.skin; }
AxeMidi& UnitBase::midi()   { return main.midi; }

void UnitBase::drawText (Graphics& g, const String& font, const String& text, Rectangle<int> area,
                         double size, Colour c, int just)
{
    skin().font (font).draw (g, text, area.toFloat(), (float) size, c, Justification (just == 0 ? 36 : just));
}

SkinButton UnitBase::makeButton (XmlElement* w, const String& text)
{
    SkinButton b;
    b.el = w;
    b.name = w->getStringAttribute ("name");
    b.tip = w->getStringAttribute ("toolTip");
    b.text = text;
    auto first = [&] (std::initializer_list<const char*> names)
    {
        for (auto* n : names)
            if (auto img = skin().image (w->getStringAttribute (n)); img.isValid())
                return img;
        return Image();
    };
    b.up   = first ({ "upImage", "imageUp", "imageOff", "icon" });
    b.over = first ({ "overImage", "imageOver", "iconOver", "upImage", "imageUp", "imageOff", "icon" });
    b.down = first ({ "downImage", "imageDown", "iconDown", "imageOn", "upImage" });
    b.on   = first ({ "upImageOn", "imageOnUp", "imageOn", "downImage", "iconDown" });
    const auto p = posOf (w);
    b.bounds = { p.x, p.y, b.up.getWidth(), b.up.getHeight() };
    return b;
}

void UnitBase::paintButton (Graphics& g, const SkinButton& b)
{
    const bool isDown = pressed == &b, isOver = hover == &b;
    drawAt (g, b.toggled ? b.on : isDown ? b.down : isOver ? b.over : b.up, b.bounds.getX(), b.bounds.getY());

    if (b.text.isNotEmpty() && b.el != nullptr)
    {
        const auto col = Skin::colour (b.el, isDown ? "fontColorDown" : isOver ? "fontColorOver" : "fontColorUp", Colours::lightgrey);
        drawText (g, b.el->getStringAttribute ("font", "_default.stf"), b.text, b.bounds.reduced (6, 0),
                  b.el->getDoubleAttribute ("fontSize", 16), col, 36);
    }
}

SkinButton* UnitBase::buttonAt (Array<SkinButton>& list, Point<int> p)
{
    for (auto& b : list)
        if (b.bounds.contains (p))
            return &b;
    return nullptr;
}

void UnitBase::mouseMove (const MouseEvent& e)
{
    auto* b = buttonAt (buttons, e.getPosition());
    if (b != hover)
    {
        hover = b;
        setTooltip (b != nullptr ? b->tip : String());
        repaint();
    }
}

void UnitBase::mouseExit (const MouseEvent&)    { hover = nullptr; repaint(); }

//==============================================================================
ControlUnit::ControlUnit (MainComponent& m) : UnitBase (m)
{
    cu = skin().child ("ControlUnitSkin");
    const std::map<String, String> texts { { "sourceButton", "Source" }, { "recallButton", "Recall" }, { "saveAsButton", "Save As" } };

    for (auto* n : { "logoButton", "powerButton", "tempoButton", "tunerButton", "settingsButton", "AMButton", "helpButton",
                     "presetUpButton", "presetDownButton", "sourceButton", "recallButton", "saveAsButton", "saveButton",
                     "snapshotButton" })
        if (auto* w = skin().widget (cu, n))
        {
            auto it = texts.find (n);
            buttons.add (makeButton (w, it != texts.end() ? it->second : String()));
        }
    startTimer (40);
}

static float meterX (float db);

void ControlUnit::timerCallback()
{
    const auto now = Time::getMillisecondCounter();

    // Tempo LED: follows the unit's TEMPO messages while they arrive, otherwise blinks at the
    // preset's tempo (ControlLayoutManager timer 1 runs at 60000 / bpm).
    const int bpm = tempoBpm();
    if (now - hardwareBeat > 3000 && bpm > 0 && now - localBeat >= (uint32) (60000 / bpm))
        localBeat = now;

    auto& a = main.audio;
    if (a.isRunning())
        for (int side = 0; side < 2; ++side)
        {
            for (int ch = 0; ch < 2; ++ch)
                shown[side][ch] = jmax (shown[side][ch] * 0.82f, a.level (side == 1, ch));
            if (a.takeClip (side == 1))
                clipUntil[side] = now + 1000;
        }
    else
        for (auto& side : shown) side[0] = side[1] = 0.0f;

    // Repaint only when a LED, the meters or the tempo/CPU readouts actually change.
    const bool tempoLed = now - hardwareBeat < 100 || (now - hardwareBeat > 3000 && bpm > 0 && now - localBeat < 100);
    int64 state = (now - inFlash < 120 ? 1 : 0) | (now - outFlash < 120 ? 2 : 0) | (tempoLed ? 4 : 0)
                    | (now < clipUntil[0] ? 8 : 0) | (now < clipUntil[1] ? 16 : 0) | (main.doc.dirty ? 32 : 0);
    for (int side = 0; side < 2; ++side)
        for (int ch = 0; ch < 2; ++ch)
            state = state * 131 + roundToInt (meterX (Decibels::gainToDecibels (shown[side][ch], -60.0f)));
    state = state * 1009 + bpm;
    state = state * 1009 + roundToInt (cpuUnits (main.doc));
    if (state != lastState)
    {
        lastState = state;
        repaint();
    }
}

int ControlUnit::tempoBpm() const
{
    auto& d = main.doc;
    const int id = d.config.paramId ("Controllers", "CONTROLLERS_TEMPO");
    const int raw = d.preset.getParam (141, id);
    return raw >= 0 ? jlimit (30, 250, raw + 30) : -1;   // profile: INT 30..250
}

void ControlUnit::tapTempo()
{
    // ControlLayoutManager::learnTempo: every tap sends the tap-tempo CC (value 1); two taps less than
    // 2 s apart also set the preset's tempo from the interval.
    const auto now = Time::getMillisecondCounter();
    if (midi().isOpen())
        midi().sendController (main.tempoCC, 1);

    if (lastTap != 0 && now - lastTap < 2000 && now > lastTap)
    {
        const int bpm = jlimit (30, 250, roundToInt (60000.0 / (double) (now - lastTap)));
        auto& d = main.doc;
        const int id = d.config.paramId ("Controllers", "CONTROLLERS_TEMPO");
        if (d.preset.getParam (141, id) >= 0)
        {
            main.pushUndo();
            d.preset.setParam (141, id, bpm - 30);
            main.paramChanged (141, id, bpm - 30);
            d.touched();
        }
        main.setStatus ("Tempo " + String (bpm) + " BPM");
    }
    lastTap = now;
}

void ControlUnit::editTempo()
{
    auto& d = main.doc;
    const int id = d.config.paramId ("Controllers", "CONTROLLERS_TEMPO");
    if (d.preset.getParam (141, id) < 0) { main.setStatus ("This preset has no Controllers data to hold a tempo."); return; }

    auto* w = new AlertWindow ("Tempo", "Beats per minute (30-250).", MessageBoxIconType::NoIcon);
    w->addTextEditor ("bpm", String (tempoBpm()));
    w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w, id] (int r)
    {
        const auto text = w->getTextEditorContents ("bpm").trim();
        if (r != 1 || text.isEmpty() || ! text.containsOnly ("0123456789")) return;
        const int bpm = jlimit (30, 250, text.getIntValue());
        main.pushUndo();
        main.doc.preset.setParam (141, id, bpm - 30);
        main.paramChanged (141, id, bpm - 30);
        main.doc.touched();
    }), true);
}

// The meters' scale image reads -36, -24, -12, -6, 0 dB, CLIP; these are its tick positions on the
// 116-pixel meter.
static float meterX (float db)
{
    static const float dbs[] { -36.0f, -24.0f, -12.0f, -6.0f, 0.0f };
    static const float xs[]  {   0.0f,  34.0f,  74.0f, 84.0f, 100.0f };
    if (db <= dbs[0]) return 0.0f;
    for (int i = 1; i < 5; ++i)
        if (db <= dbs[i])
            return xs[i - 1] + (xs[i] - xs[i - 1]) * (db - dbs[i - 1]) / (dbs[i] - dbs[i - 1]);
    return jmin (116.0f, 100.0f + db * 5.0f);
}

static float meterDb (float x)
{
    for (float db = -36.0f; db <= 0.0f; db += 0.25f)
        if (meterX (db) >= x) return db;
    return 0.0f;
}

Rectangle<int> ControlUnit::pinBounds (int which)
{
    if (auto* w = skin().widget (cu, which == 0 ? "inputMeter" : "outputMeter"))
    {
        const auto p = posOf (w);
        const auto pin = skin().image (w->getStringAttribute ("imagePinUp"));
        const float gain = (which == 0 ? main.audio.inputGain : main.audio.outputGain).load();
        const float x = meterX (Decibels::gainToDecibels (gain, -36.0f));
        return { p.x + roundToInt (x) - pin.getWidth() / 2, p.y + 21 - pin.getHeight() / 2 + 4, pin.getWidth(), pin.getHeight() };
    }
    return {};
}

void ControlUnit::drawMeter (Graphics& g, const String& name, int which)
{
    auto* w = skin().widget (cu, name);
    if (w == nullptr) return;
    const auto p = posOf (w);
    drawAt (g, skin().image (w->getStringAttribute ("imageOff")), p.x, p.y);
    const auto on = skin().image (w->getStringAttribute ("imageOn"));
    if (on.isValid())
        for (int ch = 0; ch < 2; ++ch)
        {
            const float db = Decibels::gainToDecibels (shown[which][ch], -60.0f);
            const int width = shown[which][ch] > 0.99f ? on.getWidth() : roundToInt (meterX (db));
            const int h = on.getHeight() / 2, y = ch * h;
            if (width > 0)
                g.drawImage (on, p.x, p.y + y, width, ch == 0 ? h : on.getHeight() - h, 0, y, width, ch == 0 ? h : on.getHeight() - h);
        }
    drawAt (g, skin().image (w->getStringAttribute ("imageScale")), p.x + w->getIntAttribute ("imageScaleOffsetX"),
            p.y + w->getIntAttribute ("imageScaleOffsetY"));
    if (main.audio.isRunning())
    {
        const auto r = pinBounds (which);
        drawAt (g, skin().image (w->getStringAttribute (draggingPin == which ? "imagePinDown" : "imagePinUp")), r.getX(), r.getY());
    }
}

void ControlUnit::flashLed (bool incoming)
{
    (incoming ? inFlash : outFlash) = Time::getMillisecondCounter();
}

void ControlUnit::drawLed (Graphics& g, const String& name, bool on)
{
    if (auto* w = skin().widget (cu, name))
        drawAt (g, skin().image (w->getStringAttribute (on ? "imageOn" : "imageOff")), posOf (w).x, posOf (w).y);
}

void ControlUnit::paint (Graphics& g)
{
    g.fillAll (Colours::black);
    if (cu == nullptr) return;
    drawAt (g, skin().image (cu->getStringAttribute ("background")), 0, 0);
    auto& d = doc();

    drawMeter (g, "inputMeter", 0);
    drawMeter (g, "outputMeter", 1);

    // CPU: ControlLayoutManager::getCpuUsage shows sum / 350 on the meter and sum / 350 * 300 as text.
    const double cpu = cpuUnits (d) / 350.0;
    if (auto* w = skin().widget (cu, "cpuMeter"))
    {
        const auto p = posOf (w);
        drawAt (g, skin().image (w->getStringAttribute ("imageOff")), p.x, p.y);
        const auto on = skin().image (w->getStringAttribute ("imageOn"));
        const int width = roundToInt (on.getWidth() * jlimit (0.0, 1.0, cpu));
        if (width > 0)
            g.drawImage (on, p.x, p.y, width, on.getHeight(), 0, 0, width, on.getHeight());
    }
    const auto now = Time::getMillisecondCounter();
    drawLed (g, "inputClipLed", now < clipUntil[0]);
    drawLed (g, "outputClipLed", now < clipUntil[1]);
    drawLed (g, "midiInLed", now - inFlash < 120);
    drawLed (g, "midiOutLed", now - outFlash < 120);
    drawLed (g, "editLed", d.dirty);
    drawLed (g, "tempoLed", now - hardwareBeat < 100 || (now - hardwareBeat > 3000 && tempoBpm() > 0 && now - localBeat < 100));

    if (auto* w = skin().widget (cu, "presetDisplay"))
    {
        const auto p = posOf (w);
        const auto bg = skin().image (w->getStringAttribute ("background"));
        drawAt (g, bg, p.x, p.y);
        const auto col = Skin::colour (w, "fontColor", Colours::orange);
        const double fs = w->getDoubleAttribute ("fontSize", 27);
        Rectangle<int> r (p.x + w->getIntAttribute ("textOffsetX"), p.y + w->getIntAttribute ("textOffsetY"),
                          460, bg.isValid() ? bg.getHeight() : 36);

        String text = tunerOn ? ("TUNER  " + tunerText)
                              : ((d.presetIndex >= 0 ? String (d.presetIndex).paddedLeft ('0', 3) : String ("---"))
                                   + "  " + d.preset.name);
        drawText (g, w->getStringAttribute ("font"), text, r, fs, col, 33);

        if (! tunerOn)
            for (int amp = 0; amp < 2; ++amp)
            {
                const int id = 106 + amp, typeParam = d.config.paramId ("Amp", "DISTORT_TYPE");
                const int v = d.preset.getParam (id, typeParam);
                auto* info = d.profile.find ("Amp", typeParam);
                if (v < 0 || info == nullptr || ! d.preset.isPlaced (id)) continue;
                const String a = "amp" + String (amp + 1);
                drawText (g, w->getStringAttribute (a + "Font"), "AMP " + String (amp + 1) + ": " + info->valueOnly (v),
                          { p.x + w->getIntAttribute (a + "positionX"), p.y + w->getIntAttribute (a + "positionY") + 8, 180, 16 },
                          w->getDoubleAttribute (a + "FontSize", 14), Skin::colour (w, a + "FontColor", col), 33);
            }
    }

    auto field = [&] (const String& name, const String& text)
    {
        if (auto* w = skin().widget (cu, name))
        {
            const auto p = posOf (w);
            drawAt (g, skin().image (w->getStringAttribute ("background")), p.x, p.y);
            drawText (g, w->getStringAttribute ("font"), text, Skin::rect (w, "textRect").translated (p.x, p.y),
                      w->getDoubleAttribute ("fontSize", 14), Skin::colour (w, "fontColor"), w->getIntAttribute ("textJustify", 33));
        }
    };
    field ("cpuField", String (roundToInt (cpu * 300.0)));
    field ("tempoField", tempoBpm() > 0 ? String (tempoBpm()) : String ("--"));

    String source = main.sourceIsHardware ? ("Axe-Fx via " + midi().outputName)
                  : d.sourceFile.existsAsFile() ? d.sourceFile.getFileName() : String ("New preset (unsaved)");
    source = AxeModel::name (d.model) + "  |  " + source;
    if (main.firmwareMajor >= 0) source += "  |  firmware " + String (main.firmwareMajor) + "." + String (main.firmwareMinor).paddedLeft ('0', 2);
    if (main.status.isNotEmpty()) source += "  |  " + main.status;
    field ("sourceField", source);

    for (auto& b : buttons)
    {
        if (b.name == "powerButton") b.toggled = midi().isConnected();
        if (b.name == "tunerButton") b.toggled = tunerOn;
        paintButton (g, b);
    }
}

void ControlUnit::mouseDown (const MouseEvent& e)
{
    draggingPin = -1;
    if (main.audio.isRunning())
        for (int i = 0; i < 2; ++i)
            if (pinBounds (i).expanded (2).contains (e.getPosition()))
                draggingPin = i;
    pressed = draggingPin < 0 ? buttonAt (buttons, e.getPosition()) : nullptr;
    repaint();
}

void ControlUnit::mouseDrag (const MouseEvent& e)
{
    if (draggingPin < 0) return;
    if (auto* w = skin().widget (cu, draggingPin == 0 ? "inputMeter" : "outputMeter"))
    {
        const float db = meterDb ((float) (e.x - posOf (w).x));
        (draggingPin == 0 ? main.audio.inputGain : main.audio.outputGain) = Decibels::decibelsToGain (db, -36.0f);
        main.setStatus ((draggingPin == 0 ? "Input level " : "Output level ") + (db <= -36.0f ? String ("off") : String (db, 1) + " dB"));
    }
}

void ControlUnit::mouseUp (const MouseEvent& e)
{
    if (draggingPin >= 0) { draggingPin = -1; repaint(); return; }
    auto* b = pressed;
    pressed = nullptr;
    repaint();

    if (b == nullptr)
        if (auto* w = skin().widget (cu, "tempoField"))
            if (Rectangle<int> (posOf (w).x, posOf (w).y, w->getIntAttribute ("width", 66), w->getIntAttribute ("height", 24)).contains (e.getPosition()))
            {
                editTempo();
                return;
            }

    if (b != nullptr && b->bounds.contains (e.getPosition()))
        clicked (*b);
    else if (b == nullptr)
        if (auto* w = skin().widget (cu, "presetDisplay"))
        {
            const auto p = posOf (w) + Point<int> (w->getIntAttribute ("textOffsetX"), 0);
            if (main.sourceIsHardware && main.midi.isConnected() && Rectangle<int> (p.x - 10, p.y, 70, 34).contains (e.getPosition()))
                main.goToPreset();
            else if (Rectangle<int> (posOf (w).x + 30, posOf (w).y, 430, 34).contains (e.getPosition()))
                main.renamePreset();
        }
}

void ControlUnit::clicked (SkinButton& b)
{
    const auto n = b.name;
    if (n == "settingsButton")          main.showSettingsMenu();
    else if (n == "sourceButton")       main.showSourceMenu();
    else if (n == "recallButton")       main.showRecallMenu();
    else if (n == "saveAsButton")       main.showSaveAsMenu();
    else if (n == "saveButton")         main.save();
    else if (n == "snapshotButton")     main.snapshot();
    else if (n == "presetUpButton")     main.stepPreset (1);
    else if (n == "presetDownButton")   main.stepPreset (-1);
    else if (n == "helpButton" || n == "logoButton") main.showHelpMenu();
    else if (n == "powerButton")        main.showSettingsMenu();
    else if (n == "AMButton")           main.showManage();
    else if (n == "tempoButton")        tapTempo();
    else if (n == "tunerButton")
    {
        if (! midi().isOpen()) { main.setStatus ("Connect to the Axe-Fx to use the tuner."); return; }
        tunerOn = ! tunerOn;
        tunerText = "--";
        midi().setTuner (tunerOn);
        repaint();
    }
}

//==============================================================================
GridUnit::GridUnit (MainComponent& m) : UnitBase (m)
{
    auto* gu = skin().child ("GridUnitSkin");
    gridEl = gu != nullptr ? gu->getChildByName ("Grid") : nullptr;
    if (gridEl == nullptr) return;

    gridArea = { gridEl->getIntAttribute ("positionX"), gridEl->getIntAttribute ("positionY"),
                 gridEl->getIntAttribute ("width"), gridEl->getIntAttribute ("height") };
    bw = gridEl->getIntAttribute ("blockWidth", 45);    bh = gridEl->getIntAttribute ("blockHeight", 37);
    sx = gridEl->getIntAttribute ("blockSpacingX", 23); sy = gridEl->getIntAttribute ("blockSpacingY", 11);
    ox = gridEl->getIntAttribute ("blockOffsetX", 47);  oy = gridEl->getIntAttribute ("blockOffsetY", 7);

    if (auto* tools = gu->getChildByName ("Tools"))
    {
        int y = tools->getIntAttribute ("positionY");
        for (auto* t : tools->getChildWithTagNameIterator ("Tool"))
        {
            auto b = makeButton (t);
            b.bounds.setPosition (tools->getIntAttribute ("positionX"), y);
            y += b.bounds.getHeight() + tools->getIntAttribute ("toolButtonSpacingY");
            buttons.add (b);
        }
    }

    const int midY = gridArea.getY() + oy + 2 * (bh + sy) - sy / 2;
    if (auto* in = gu->getChildByName ("GridInput"))
    {
        auto img = skin().image (in->getStringAttribute ("icon"));
        inputArea = { gridArea.getX() + in->getIntAttribute ("offsetX"), midY - img.getHeight() / 2, img.getWidth(), img.getHeight() };
    }
    if (auto* out = gu->getChildByName ("GridOutput"))
    {
        auto img = skin().image (out->getStringAttribute ("icon"));
        outputArea = { gridArea.getRight() + out->getIntAttribute ("offsetX"), midY - img.getHeight() / 2, img.getWidth(), img.getHeight() };
    }
}

Rectangle<int> GridUnit::cellBounds (int col, int row) const
{
    return { gridArea.getX() + ox + col * (bw + sx), gridArea.getY() + oy + row * (bh + sy), bw, bh };
}

Point<int> GridUnit::cellAt (Point<int> p) const
{
    for (int c = 0; c < Preset::columns; ++c)
        for (int r = 0; r < Preset::rows; ++r)
            if (cellBounds (c, r).expanded (4).contains (p))
                return { c, r };
    return { -1, -1 };
}

void GridUnit::drawBlock (Graphics& g, int col, int row)
{
    auto& d = doc();
    const auto cell = d.preset.cell (col, row);
    const auto r = cellBounds (col, row);
    const bool shunt = Preset::isShunt (cell.id);
    const auto type = cell.id == 0 ? String ("block") : shunt ? String ("shunt") : d.config.typeOf (cell.id);
    auto* icon = skin().named ("GridUnitSkin/BlockIcons", "BlockIcon", type);
    if (icon == nullptr) icon = skin().named ("GridUnitSkin/BlockIcons", "BlockIcon", "block");
    if (icon == nullptr) return;

    const bool selected = (cell.id != 0 && cell.id == d.selectedEffect) || selectedCell == Point<int> (col, row);
    const bool over = hoverCell == Point<int> (col, row);
    const bool bypassed = cell.id != 0 && ! shunt && d.isBypassed (cell.id);
    const String state = selected ? "Down" : over ? "Over" : bypassed ? "Bypassed" : "";

    g.setColour (Skin::colour (icon, "bgFill" + (bypassed && state.isEmpty() ? String ("Bypassed") : state), Colours::transparentBlack));
    g.fillRect (r);

    const auto img = skin().image (icon->getStringAttribute (bypassed ? "iconBypassed" : "icon"));
    if (img.isValid())
        g.drawImageWithin (img, r.getX(), r.getY(), r.getWidth(), r.getHeight(), RectanglePlacement::centred);
    else if (cell.id != 0 && ! shunt)
        drawText (g, "_block.stf", type.substring (0, 6).toUpperCase(), r, 10, Colours::black, 36);

    const int bwid = icon->getIntAttribute ("borderWidth" + state, 1);
    g.setColour (Skin::colour (icon, "borderColor" + state, Colour (0xff222222)));
    g.drawRect (r, jmax (1, bwid));
}

void GridUnit::paint (Graphics& g)
{
    g.fillAll (Colours::black);
    auto* gu = skin().child ("GridUnitSkin");
    if (gu == nullptr || gridEl == nullptr) return;
    drawAt (g, skin().image (gu->getStringAttribute ("background")), 0, 0);
    drawAt (g, skin().image (gridEl->getStringAttribute ("background")), gridArea.getX(), gridArea.getY());

    auto& p = doc().preset;
    const float thick = (float) gridEl->getDoubleAttribute ("cableThickness", 2.5);
    g.setColour (Skin::colour (gridEl, "cableColor", Colours::lightgrey));

    for (int c = 0; c < Preset::columns; ++c)
        for (int r = 0; r < Preset::rows; ++r)
        {
            const auto cell = p.cell (c, r);
            if (cell.id == 0) continue;
            const auto b = cellBounds (c, r).toFloat();

            if (c == 0)
                g.drawLine ((float) inputArea.getRight() - 4, (float) inputArea.getCentreY(), b.getX(), b.getCentreY(), thick);
            else
                for (int from = 0; from < Preset::rows; ++from)
                    if ((cell.mask & (1 << from)) != 0 && p.cell (c - 1, from).id != 0)
                    {
                        const auto a = cellBounds (c - 1, from).toFloat();
                        g.drawLine (a.getRight(), a.getCentreY(), b.getX(), b.getCentreY(), thick);
                    }

            if (c == Preset::columns - 1)
                g.drawLine (b.getRight(), b.getCentreY(), (float) outputArea.getX() + 4, (float) outputArea.getCentreY(), thick);
        }

    if (dragging && dragFrom.x >= 0)
    {
        const auto a = cellBounds (dragFrom.x, dragFrom.y).toFloat();
        g.setColour (Colours::white);
        g.drawLine (a.getRight(), a.getCentreY(), (float) dragPos.x, (float) dragPos.y, thick);
    }

    for (int c = 0; c < Preset::columns; ++c)
        for (int r = 0; r < Preset::rows; ++r)
            drawBlock (g, c, r);

    if (auto* in = gu->getChildByName ("GridInput"))
        drawAt (g, skin().image (in->getStringAttribute (doc().selectedEffect == 139 ? "iconDown" : "icon")), inputArea.getX(), inputArea.getY());
    if (auto* out = gu->getChildByName ("GridOutput"))
        drawAt (g, skin().image (out->getStringAttribute (doc().selectedEffect == 140 ? "iconDown" : "icon")), outputArea.getX(), outputArea.getY());

    for (auto& b : buttons)
    {
        if (b.name == "Cable") b.toggled = cableMode;
        if (b.name == "Controllers") b.toggled = doc().selectedEffect == 141;
        paintButton (g, b);
    }
}

void GridUnit::mouseMove (const MouseEvent& e)
{
    UnitBase::mouseMove (e);
    const auto c = cellAt (e.getPosition());
    if (c != hoverCell)
    {
        hoverCell = c;
        const int id = c.x >= 0 ? doc().preset.cell (c.x, c.y).id : 0;
        if (hover == nullptr)
            if (auto* item = doc().config.poolItem (id))
                setTooltip (item->name);
        repaint();
    }
}

void GridUnit::mouseDown (const MouseEvent& e)
{
    pressed = buttonAt (buttons, e.getPosition());
    dragging = false;
    dragFrom = { -1, -1 };

    if (pressed == nullptr)
    {
        auto& d = doc();
        if (inputArea.contains (e.getPosition()))        { selectedCell = { -1, -1 }; d.selectedEffect = 139; d.sendChangeMessage(); return; }
        if (outputArea.contains (e.getPosition()))       { selectedCell = { -1, -1 }; d.selectedEffect = 140; d.sendChangeMessage(); return; }

        const auto c = cellAt (e.getPosition());
        if (c.x >= 0)
        {
            const int id = d.preset.cell (c.x, c.y).id;

            if (cableMode && selectedCell.x >= 0 && c.x == selectedCell.x + 1 && id != 0)
            {
                main.pushUndo();
                d.toggleCable (c.x, c.y, selectedCell.y);
                main.gridChanged();
                cableMode = false;
                return;
            }

            selectedCell = c;
            d.selectedEffect = Preset::isShunt (id) ? 0 : id;
            if (id != 0) dragFrom = c;
            d.sendChangeMessage();

            if (e.mods.isPopupMenu())
                showCellMenu (c.x, c.y);
        }
    }
    repaint();
}

void GridUnit::mouseDrag (const MouseEvent& e)
{
    if (dragFrom.x >= 0 && e.getDistanceFromDragStart() > 6)
    {
        dragging = true;
        dragPos = e.getPosition();
        repaint();
    }
}

void GridUnit::mouseUp (const MouseEvent& e)
{
    if (auto* b = pressed)
    {
        pressed = nullptr;
        if (b->bounds.contains (e.getPosition()))
            toolClicked (*b);
    }
    else if (dragging && dragFrom.x >= 0)
    {
        const auto c = cellAt (e.getPosition());
        if (c.x == dragFrom.x + 1 && doc().preset.cell (c.x, c.y).id != 0)
        {
            main.pushUndo();
            doc().toggleCable (c.x, c.y, dragFrom.y);
            main.gridChanged();
        }
    }
    dragging = false;
    repaint();
}

void GridUnit::mouseDoubleClick (const MouseEvent& e)
{
    const auto c = cellAt (e.getPosition());
    if (c.x >= 0 && buttonAt (buttons, e.getPosition()) == nullptr)
        showCellMenu (c.x, c.y);
}

void GridUnit::showCellMenu (int col, int row)
{
    auto& d = doc();
    const int current = d.preset.cell (col, row).id;
    PopupMenu m, blocks;

    for (auto& item : d.config.pool)
        blocks.addItem (1000 + item.id, item.name, ! d.preset.isPlaced (item.id) || item.id == current, item.id == current);

    m.addSubMenu (current == 0 ? "Insert Block" : "Replace With", blocks);
    m.addItem (1, "Shunt", true, Preset::isShunt (current));
    m.addSeparator();
    const int bp = current != 0 && ! Preset::isShunt (current) ? d.bypassParam (current) : -1;
    m.addItem (2, "Bypass", current != 0 && ! Preset::isShunt (current), d.isBypassed (current));
    m.addItem (3, "Delete", current != 0);

    m.showMenuAsync (PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (cellBounds (col, row))),
                     [this, col, row, current, bp] (int result)
    {
        auto& dd = doc();
        if (result == 0) return;
        main.pushUndo();

        if (result >= 1000 || result == 1)
        {
            const int id = result == 1 ? dd.preset.freeShuntId() : result - 1000;
            if (id == 0 || ! dd.placeEffect (col, row, id))
                main.message ("Preset full", "There isn't enough room left in the preset for this block.");
            main.gridChanged();
        }
        else if (result == 2)
        {
            const bool nowBypassed = ! dd.isBypassed (current);
            if (bp >= 0) { dd.preset.setParam (current, bp, nowBypassed ? 1 : 0); dd.touched(); }
            else         main.setStatus ("This block's bypass state isn't stored in the preset data; it was sent to the unit only.");
            if (midi().isConnected()) midi().setBypass (current, nowBypassed);
        }
        else if (result == 3)
        {
            dd.removeAt (col, row);
            main.gridChanged();
        }
    });
}

void GridUnit::deleteSelected()
{
    if (selectedCell.x < 0 || doc().preset.cell (selectedCell.x, selectedCell.y).id == 0) return;
    main.pushUndo();
    doc().removeAt (selectedCell.x, selectedCell.y);
    main.gridChanged();
}

void GridUnit::toolClicked (SkinButton& b)
{
    auto& d = doc();
    const bool hasCell = selectedCell.x >= 0;
    const int id = hasCell ? d.preset.cell (selectedCell.x, selectedCell.y).id : 0;

    if (b.name == "Controllers")    { selectedCell = { -1, -1 }; d.selectedEffect = 141; d.sendChangeMessage(); }
    else if (b.name == "Undo")      main.undo();
    else if (b.name == "Redo")      main.redo();
    else if (b.name == "Delete")    deleteSelected();
    else if (b.name == "Cable")
    {
        cableMode = ! cableMode && id != 0;
        main.setStatus (cableMode ? "Click a block in the next column to connect or disconnect it."
                                  : "Select a block first, or drag from a block to one in the next column.");
    }
    else if (b.name == "Shunt" && hasCell)
    {
        main.pushUndo();
        const int shunt = d.preset.freeShuntId();
        if (shunt != 0) d.placeEffect (selectedCell.x, selectedCell.y, shunt);
        main.gridChanged();
    }
    else if (b.name == "Bypass" && id != 0 && ! Preset::isShunt (id))
    {
        main.pushUndo();
        const int bp = d.bypassParam (id);
        const bool nowBypassed = ! d.isBypassed (id);
        if (bp >= 0) { d.preset.setParam (id, bp, nowBypassed ? 1 : 0); d.touched(); }
        else         main.setStatus ("This block's bypass state isn't stored in the preset data; it was sent to the unit only.");
        if (midi().isConnected()) midi().setBypass (id, nowBypassed);
    }
    else if (b.name == "Initialize" && d.selectedEffect != 0)
    {
        if (auto* c = d.preset.chunk (d.selectedEffect))
        {
            main.pushUndo();
            const auto type = d.config.typeOf (d.selectedEffect);
            for (int i = 0; i < c->values.size(); ++i)
                c->values.set (i, (uint8) d.defaultFor (type, i));
            d.touched();
            main.gridChanged();
        }
    }
    repaint();
}

//==============================================================================
EffectUnit::EffectUnit (MainComponent& m) : UnitBase (m)    { refresh(); }

XmlElement* EffectUnit::style (const String& name) const    { return main.doc.skin.widget (styleEl, name); }

void EffectUnit::refresh()
{
    auto& d = doc();
    const int id = d.selectedEffect;
    const auto t = id != 0 ? d.config.typeOf (id) : String();
    int vv = -1;

    if (auto* controls = d.config.layout (t))
        if (controls->getStringAttribute ("syncVariantsToType") != "false" && controls->hasAttribute ("typeParameter"))
            vv = d.preset.getParam (id, d.config.paramId (t, controls->getStringAttribute ("typeParameter")));

    const auto key = String (id) + "/" + t + "/" + String (vv) + "/" + String (d.model)
                       + "/" + String (d.preset.chunk (id) != nullptr ? d.preset.chunk (id)->values.size() : -1);
    if (key != structureKey)
    {
        if (id != effectId) page = 0;
        structureKey = key;
        effectId = id;
        type = t;
        variantValue = vv;
        rebuild();
    }
    repaint();
}

void EffectUnit::rebuild()
{
    auto& d = doc();
    widgets.clear();
    pageNames.clear();
    monitorName.clear();
    pageName.clear();
    active = -1;

    const String layoutName = type.isNotEmpty() && d.config.layout (type) != nullptr ? type : String ("Dummy");
    layoutEl = skin().named ("EffectLayouts", "EffectLayout", layoutName);
    if (layoutEl == nullptr) layoutEl = skin().named ("EffectLayouts", "EffectLayout", "Dummy");
    styleEl = layoutEl != nullptr ? skin().named ("WidgetStyles", "WidgetStyle", layoutEl->getStringAttribute ("widgetStyle", "GENERIC_STYLE")) : nullptr;
    pageLayoutEl = nullptr;

    auto* controls = d.config.layout (layoutName);
    if (controls == nullptr || layoutEl == nullptr || styleEl == nullptr) return;

    XmlElement* variant = nullptr;
    if (auto* variants = controls->getChildByName ("EffectVariants"))
    {
        for (auto* v : variants->getChildWithTagNameIterator ("EffectVariant"))
            if (v->getIntAttribute ("value") == variantValue)
                variant = v;
        if (variant == nullptr)
            variant = variants->getChildByName ("EffectVariant");
    }
    if (variant == nullptr) return;

    Array<XmlElement*> pages;
    for (auto* p : variant->getChildWithTagNameIterator ("Page"))
    {
        pages.add (p);
        pageNames.add (p->getStringAttribute ("name"));
    }
    page = jlimit (0, jmax (0, pages.size() - 1), page);

    // tabs
    if (pages.size() > 1)
        if (auto* tab = style ("tab"))
        {
            const auto img = skin().image (tab->getStringAttribute ("image"));
            for (int i = 0; i < pages.size(); ++i)
            {
                Widget w;
                w.kind = Widget::Tab;
                w.page = i;
                w.label = pageNames[i];
                w.pos = { layoutEl->getIntAttribute ("tabPositionX") + i * layoutEl->getIntAttribute ("tabSpacingX"),
                          layoutEl->getIntAttribute ("tabPositionY") + i * layoutEl->getIntAttribute ("tabSpacingY") };
                w.hit = { w.pos.x, w.pos.y, img.getWidth(), img.getHeight() };
                widgets.add (w);
            }
        }

    // block type selector
    if (controls->hasAttribute ("typeParameter") && effectId != 0)
    {
        Widget w;
        w.kind = Widget::Menu;
        w.size = "Large";
        w.label = "Type";
        w.paramName = controls->getStringAttribute ("typeParameter");
        w.paramId = d.config.paramId (type, w.paramName);
        w.info = d.profile.find (type, w.paramId);
        w.stored = d.preset.getParam (effectId, w.paramId) >= 0;
        w.pos = { layoutEl->getIntAttribute ("typePositionX"), layoutEl->getIntAttribute ("typePositionY") };
        if (auto* menu = style ("menu"))
        {
            const auto f = skin().image (menu->getStringAttribute ("fieldBackgroundLarge"));
            w.hit = { w.pos.x + menu->getIntAttribute ("fieldBackgroundOffsetX"), w.pos.y + menu->getIntAttribute ("fieldBackgroundOffsetY"),
                      f.getWidth() + 28, f.getHeight() };
        }
        widgets.add (w);
    }

    if (auto* p = pages[page])
    {
        monitorName = p->getStringAttribute ("monitor");
        pageName = p->getStringAttribute ("name");
        const auto which = p->getStringAttribute ("layout", "page1Layout");
        pageLayoutEl = skin().named ("PageLayouts", "PageLayout", layoutEl->getStringAttribute (which, layoutEl->getStringAttribute ("page1Layout")));
        if (pageLayoutEl == nullptr) return;

        addRows (p->getChildByName ("Parameters"), pageLayoutEl->getIntAttribute ("parametersX"), pageLayoutEl->getIntAttribute ("parametersY"),
                 pageLayoutEl->getIntAttribute ("parametersSpacingX", 67), pageLayoutEl->getIntAttribute ("parametersSpacingY", 106));
        addRows (p->getChildByName ("Mixer"), pageLayoutEl->getIntAttribute ("mixerX"), pageLayoutEl->getIntAttribute ("mixerY"),
                 pageLayoutEl->getIntAttribute ("mixerSpacingX", 67), pageLayoutEl->getIntAttribute ("mixerSpacingY", 105));
    }
}

void EffectUnit::addRows (XmlElement* parent, int baseX, int baseY, int spacingX, int spacingY)
{
    if (parent == nullptr) return;
    auto& d = doc();
    int rowIndex = 0;

    for (auto* row : parent->getChildWithTagNameIterator ("Row"))
    {
        double x = baseX;
        const int y = baseY + rowIndex++ * spacingY;

        for (auto* c : row->getChildWithTagNameIterator ("EditorControl"))
        {
            const auto t = c->getStringAttribute ("type", "knob");
            if (t == "spacer") { x += spacingX; continue; }

            Widget w;
            w.kind = t == "toggle" ? Widget::Toggle : t == "slider" ? Widget::Slider : t.startsWith ("dropdown") ? Widget::Menu : Widget::Knob;
            w.size = t == "dropdownsmall" ? "Small" : t == "dropdownlarge" ? "Large" : t == "dropdownhuge" ? "Huge" : "";
            w.label = c->getStringAttribute ("name");
            w.paramName = c->getStringAttribute ("parameterName");
            w.paramId = d.config.paramId (type, w.paramName);
            w.info = d.profile.find (type, w.paramId);

            if (c->hasAttribute ("toggleGroup"))
            {
                w.groupId = d.config.paramId (type, c->getStringAttribute ("toggleGroup"));
                w.bit = c->getIntAttribute ("toggleParameterBit");
            }
            if (c->hasAttribute ("secondaryParameterName"))
                w.fineId = d.config.paramId (type, c->getStringAttribute ("secondaryParameterName"));
            {
                const int mid = d.config.modifierId (type, w.paramName);
                w.modifierId = mid >= 200 && mid <= 254 ? mid : 0;     // 255 marks the bypass-mode switch
            }

            w.stored = d.preset.getParam (effectId, w.groupId >= 0 ? w.groupId : w.paramId) >= 0;

            auto* st = style (w.kind == Widget::Menu ? "menu" : t);
            if (st == nullptr) { x += spacingX; continue; }

            auto attr = [&] (const char* a, const char* b, const char* c2) { return c->getIntAttribute (a, c->getIntAttribute (b, c->getIntAttribute (c2))); };
            w.pos = { roundToInt (x) + st->getIntAttribute ("positionX") + attr ("offsetX", "OffsetX", "Offsetx"),
                      y + st->getIntAttribute ("positionY") + attr ("offsetY", "OffsetY", "Offsety") };

            if (w.kind == Widget::Menu)
            {
                const auto f = skin().image (st->getStringAttribute ("fieldBackground" + w.size));
                w.hit = { w.pos.x + st->getIntAttribute ("fieldBackgroundOffsetX"), w.pos.y + st->getIntAttribute ("fieldBackgroundOffsetY"),
                          f.getWidth() + 28, f.getHeight() };
                x += w.size == "Small" ? 0.0 : w.size == "Large" ? spacingX : w.size == "Huge" ? spacingX * 2.5 : spacingX * 0.5;
            }
            else
            {
                const auto bg = skin().image (st->getStringAttribute ("background"));
                w.hit = { w.pos.x, w.pos.y, jmax (30, bg.getWidth()), jmax (30, bg.getHeight()) };
            }
            {
                const int mid = d.config.modifierId (type, w.paramName);
                if (w.kind == Widget::Menu && mid >= 200 && mid <= 254)
                    w.hit.setLeft (w.hit.getX() - 26);      // the modifier button left of the field
            }

            widgets.add (w);
            x += spacingX;
        }
    }
}

int EffectUnit::value (const Widget& w) const
{
    auto& p = main.doc.preset;
    if (w.groupId >= 0)
        return (jmax (0, p.getParam (effectId, w.groupId)) >> w.bit) & 1;

    const int v = p.getParam (effectId, w.paramId);
    return v >= 0 ? v : (w.info != nullptr ? jlimit (0, 255, w.info->def) : 0);
}

void EffectUnit::setValue (const Widget& w, int raw)
{
    auto& d = doc();
    if (! w.stored) return;

    if (w.groupId >= 0)
    {
        const int flags = jmax (0, d.preset.getParam (effectId, w.groupId));
        const int updated = raw != 0 ? (flags | (1 << w.bit)) : (flags & ~(1 << w.bit));
        d.preset.setParam (effectId, w.groupId, updated);
        // The unit only understands the flags word: in the firmware, IDs >= 200 go to the
        // block's set handler, which has no case for the editor's switch IDs (e.g. 205).
        main.paramChanged (effectId, w.groupId, updated);
    }
    else
    {
        raw = jlimit (0, w.info != nullptr ? w.info->maxRaw() : 255, raw);
        if (raw == value (w)) return;
        d.preset.setParam (effectId, w.paramId, raw);
        hardwareText.erase ({ effectId, w.paramId });
        main.paramChanged (effectId, w.paramId, raw);
    }
    d.touched();
}

String EffectUnit::valueText (const Widget& w) const
{
    auto it = hardwareText.find ({ effectId, w.paramId });
    if (it != hardwareText.end() && it->second.isNotEmpty())
        return it->second;
    if (w.info == nullptr)
        return String (value (w));
    const int fine = w.fineId >= 0 ? jmax (0, main.doc.preset.getParam (effectId, w.fineId)) : 0;
    return w.info->valueOnly (value (w), fine);
}

int EffectUnit::widgetAt (Point<int> p) const
{
    for (int i = widgets.size(); --i >= 0;)
        if (widgets.getReference (i).hit.contains (p))
            return i;
    return -1;
}

void EffectUnit::paintWidget (Graphics& g, const Widget& w)
{
    auto& sk = skin();

    if (w.kind == Widget::Tab)
    {
        auto* st = style ("tab");
        const bool on = w.page == page;
        drawAt (g, sk.image (st->getStringAttribute (on ? "imageActive" : "image")), w.pos.x, w.pos.y);
        drawText (g, st->getStringAttribute ("labelFont"), w.label, w.hit.withTrimmedLeft (8).withTrimmedRight (4),
                  st->getDoubleAttribute ("labelFontSize", 16), Skin::colour (st, on ? "labelFontColorActive" : "labelFontColor"), 33);
        return;
    }

    auto* st = style (w.kind == Widget::Knob ? "knob" : w.kind == Widget::Toggle ? "toggle" : w.kind == Widget::Slider ? "slider" : "menu");
    if (st == nullptr) return;

    g.setOpacity (w.stored ? 1.0f : 0.35f);
    const int v = value (w);
    const double norm = w.info != nullptr ? jlimit (0.0, 1.0, v / (double) w.info->maxRaw()) : v / 255.0;
    const auto labelFont = st->getStringAttribute ("labelFont");
    const double labelSize = st->getDoubleAttribute ("labelFontSize", 16);
    const auto labelCol = Skin::colour (st, "labelFontColor", Colours::grey).withMultipliedAlpha (w.stored ? 1.0f : 0.4f);
    const auto fieldCol = Skin::colour (st, "fieldFontColor").withMultipliedAlpha (w.stored ? 1.0f : 0.4f);

    auto field = [&] (const String& suffix)
    {
        const Point<int> fp (w.pos.x + st->getIntAttribute ("fieldBackgroundOffsetX"), w.pos.y + st->getIntAttribute ("fieldBackgroundOffsetY"));
        drawAt (g, sk.image (st->getStringAttribute ("fieldBackground" + suffix)), fp.x, fp.y);
        drawText (g, st->getStringAttribute ("fieldFont"), valueText (w), Skin::rect (st, "fieldTextRect" + suffix).translated (fp.x, fp.y),
                  st->getDoubleAttribute ("fieldFontSize", 16), fieldCol, st->getIntAttribute ("fieldTextJustify", 36));
        return fp;
    };

    const bool modifiable = w.modifierId != 0 && w.stored;
    const bool modOn = modifiable && hasModifier (w);
    const String bgAttr = modifiable ? (modOn ? "backgroundModifierOn" : "backgroundModifier") : "background";

    if (w.kind == Widget::Knob)
    {
        drawAt (g, sk.image (st->getStringAttribute (bgAttr, st->getStringAttribute ("background"))), w.pos.x, w.pos.y);
        const auto top = sk.image (st->getStringAttribute (modifiable ? (modOn ? "knobModifierOn" : "knobModifier") : "knob",
                                                           st->getStringAttribute ("knob")));
        if (top.isValid())
        {
            const float angle = (float) ((norm - 0.5) * 1.5 * MathConstants<double>::pi);
            const float cx = (float) (w.pos.x + st->getIntAttribute ("knobCenterOffsetX")), cy = (float) (w.pos.y + st->getIntAttribute ("knobCenterOffsetY"));
            g.drawImageTransformed (top, AffineTransform::rotation (angle, top.getWidth() * 0.5f, top.getHeight() * 0.5f)
                                             .translated (cx - top.getWidth() * 0.5f, cy - top.getHeight() * 0.5f));
        }
        field ({});
        if (w.info != nullptr && w.info->unit.isNotEmpty())
            drawText (g, st->getStringAttribute ("unitFont"), w.info->unit,
                      { w.pos.x + st->getIntAttribute ("unitTextOffsetX"), w.pos.y + st->getIntAttribute ("unitTextOffsetY"), 30, 14 },
                      st->getDoubleAttribute ("unitFontSize", 14), labelCol, 33);
    }
    else if (w.kind == Widget::Toggle)
    {
        drawAt (g, sk.image (st->getStringAttribute ("background")), w.pos.x, w.pos.y);
        drawAt (g, sk.image (st->getStringAttribute (v != 0 ? "imageOn" : "imageOff")), w.pos.x, w.pos.y);
        if (modifiable)
            drawAt (g, sk.image (st->getStringAttribute (modOn ? "backgroundModifierOn" : "backgroundModifier")), w.pos.x, w.pos.y);
    }
    else if (w.kind == Widget::Slider)
    {
        drawAt (g, sk.image (st->getStringAttribute (bgAttr, st->getStringAttribute ("background"))), w.pos.x, w.pos.y);
        const auto kr = Skin::rect (st, "knobRect");
        const auto top = sk.image (st->getStringAttribute ("knob"));
        const auto bg = sk.image (st->getStringAttribute ("background"));
        drawAt (g, top, w.pos.x + (bg.getWidth() - top.getWidth()) / 2,
                w.pos.y + kr.getY() + roundToInt ((1.0 - norm) * kr.getHeight()) - top.getHeight() / 2 + 8);
        field ({});
    }
    else
    {
        const auto fp = field (w.size);
        const auto f = sk.image (st->getStringAttribute ("fieldBackground" + w.size));
        const int bx = fp.x + f.getWidth() + 2;
        drawAt (g, sk.image (st->getStringAttribute ("upButton")), bx, fp.y + st->getIntAttribute ("upButtonOffsetY") + 4);
        drawAt (g, sk.image (st->getStringAttribute ("downButton")), bx, fp.y + st->getIntAttribute ("downButtonOffsetY") + 2);
        if (modifiable)
            drawAt (g, sk.image (st->getStringAttribute (modOn ? "backgroundModifierOn" : "backgroundModifier")),
                    fp.x - 27 + st->getIntAttribute ("modifierButtonOffsetX"), fp.y + st->getIntAttribute ("modifierButtonOffsetY") - 4);
    }

    auto lr = Skin::rect (st, w.kind == Widget::Menu ? "labelTextRect" + w.size : String ("labelTextRect"));
    drawText (g, labelFont, w.label, lr.translated (w.pos.x, w.pos.y + (w.kind == Widget::Menu ? 9 : 0)), labelSize, labelCol,
              st->getIntAttribute ("labelTextJustify", 12));
    g.setOpacity (1.0f);
}

void EffectUnit::paint (Graphics& g)
{
    g.fillAll (Colours::black);
    if (layoutEl == nullptr) return;
    auto& d = doc();

    drawAt (g, skin().image (layoutEl->getStringAttribute ("background")), 0, 0);
    if (pageLayoutEl != nullptr)
    {
        drawAt (g, skin().image (pageLayoutEl->getStringAttribute ("background")),
                pageLayoutEl->getIntAttribute ("backgroundX"), pageLayoutEl->getIntAttribute ("backgroundY"));
        const auto monitor = skin().image (pageLayoutEl->getStringAttribute ("monitor"));
        const int mx = pageLayoutEl->getIntAttribute ("monitorX"), my = pageLayoutEl->getIntAttribute ("monitorY");
        drawAt (g, monitor, mx, my);

        Graphs::Curve curve;
        if (monitor.isValid() && monitorName.isNotEmpty() && Graphs::compute (d, effectId, monitorName, pageName, curve))
            Graphs::paint (g, Rectangle<float> ((float) mx + 3.0f, (float) my + 20.0f, 180.0f, 120.0f), curve);   // the image's screen
    }

    String title = "No Effect";
    if (auto* item = d.config.poolItem (effectId)) title = item->name;
    else if (effectId != 0)                        title = type;

    const auto tr = Skin::rect (layoutEl, "textRect");
    drawText (g, layoutEl->getStringAttribute ("textFont"), title, tr, layoutEl->getDoubleAttribute ("textSize", 20),
              Skin::colour (layoutEl, "textColor", Colours::lightgrey), layoutEl->getIntAttribute ("textJustify", 9));

    if (effectId != 0)
    {
        const auto icon = skin().image (layoutEl->getStringAttribute ("icon"));
        const Rectangle<int> ir (layoutEl->getIntAttribute ("iconPositionX"), tr.getY() + 26 + layoutEl->getIntAttribute ("iconPositionY"),
                                 icon.getWidth(), icon.getHeight());
        drawAt (g, icon, ir.getX(), ir.getY());
        g.setColour (Skin::colour (layoutEl, "iconBorderColor", Colours::lightgrey));
        g.drawRect (ir, layoutEl->getIntAttribute ("iconBorderWidth", 2));

        if (d.preset.chunk (effectId) == nullptr && effectId != 0)
            drawText (g, "_s.stf", "This block has no data in the current preset.", { 250, 110, 520, 24 }, 18, Colours::grey, 33);
    }

    for (auto& w : widgets)
        paintWidget (g, w);
}

bool EffectUnit::hasModifier (const Widget& w) const
{
    return w.modifierId != 0 && main.doc.preset.findModifier (effectId, w.modifierId) > 0;
}

// The modifier "button": the lit symbol in the centre of a modifiable knob, the small button left
// of a modifiable menu, or the lower part of a modifiable switch/fader.
bool EffectUnit::onModifierButton (const Widget& w, Point<int> p) const
{
    if (w.modifierId == 0 || ! w.stored) return false;
    if (w.kind == Widget::Knob)
        if (auto* st = style ("knob"))
            return p.getDistanceFrom ({ w.pos.x + st->getIntAttribute ("knobCenterOffsetX"), w.pos.y + st->getIntAttribute ("knobCenterOffsetY") }) <= 9;
    if (w.kind == Widget::Menu)
        return p.x < w.hit.getX() + 26;
    if (w.kind == Widget::Toggle || w.kind == Widget::Slider)
        return p.y > w.hit.getBottom() - 16;
    return false;
}

void EffectUnit::openModifier (const Widget& w)
{
    const int eff = effectId, mod = w.modifierId;
    ModifierEditor::show (*this, main.doc, eff, mod, (doc().config.poolItem (eff) != nullptr ? doc().config.poolItem (eff)->name + " - " : String()) + w.label,
                          [this, eff, mod] (int chunk, int index, int raw) { main.modifierChanged (eff, mod, chunk, index, raw); });
}

void EffectUnit::showMenu (const Widget& w)
{
    if (w.info == nullptr || ! w.stored) return;
    PopupMenu m;
    const int current = value (w), n = jmax (w.info->items.size(), w.info->numVals);
    for (int i = 0; i < n && i < 256; ++i)
    {
        if (i > 0 && i % 32 == 0) m.addColumnBreak();
        m.addItem (i + 1, w.info->valueOnly (i), true, i == current);
    }
    const Widget copy = w;
    m.showMenuAsync (PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (w.hit)).withItemThatMustBeVisible (current + 1),
                     [this, copy] (int r) { if (r > 0) { main.pushUndo(); setValue (copy, r - 1); refresh(); } });
}

void EffectUnit::mouseMove (const MouseEvent& e)
{
    const int i = widgetAt (e.getPosition());
    if (i >= 0 && widgets.getReference (i).kind != Widget::Tab)
    {
        auto& w = widgets.getReference (i);
        String tip = w.stored ? w.label + ": " + (w.info != nullptr ? w.info->format (value (w)) : valueText (w))
                              : w.label + " (not stored by this preset's firmware version)";
        if (w.modifierId != 0 && w.stored)
            tip << (hasModifier (w) ? "  (modifier attached" : "  (no modifier") << " - click the modifier symbol or right-click to edit it)";
        setTooltip (tip);
    }
    else
        setTooltip ({});
}

void EffectUnit::mouseDown (const MouseEvent& e)
{
    active = widgetAt (e.getPosition());
    if (active < 0) return;
    const Widget w = widgets.getReference (active);

    if (w.kind == Widget::Tab)
    {
        page = w.page;
        rebuild();
        repaint();
        return;
    }
    if (! w.stored) { active = -1; return; }

    if (w.modifierId != 0 && (e.mods.isPopupMenu() || onModifierButton (w, e.getPosition())))
    {
        active = -1;
        openModifier (w);
        return;
    }

    main.pushUndo();
    dragStartValue = value (w);

    if (w.kind == Widget::Toggle)
        setValue (w, dragStartValue != 0 ? 0 : 1);
    else if (w.kind == Widget::Menu)
    {
        const int fieldRight = w.hit.getRight() - 28;
        if (w.modifierId != 0 && e.x < w.hit.getX() + 26) { active = -1; return; }
        if (e.x >= fieldRight)  setValue (w, dragStartValue + (e.y < w.hit.getCentreY() ? 1 : -1));
        else                    showMenu (w);
        active = -1;
    }
    refresh();
}

void EffectUnit::mouseDrag (const MouseEvent& e)
{
    if (! isPositiveAndBelow (active, widgets.size())) return;
    const Widget w = widgets.getReference (active);
    if (w.kind != Widget::Knob && w.kind != Widget::Slider) return;

    const double range = w.info != nullptr ? w.info->maxRaw() : 255;
    const double travel = w.kind == Widget::Slider ? 100.0 : (e.mods.isShiftDown() ? 800.0 : 200.0);
    setValue (w, dragStartValue + roundToInt (-e.getDistanceFromDragStartY() * range / travel));
    repaint();
}

void EffectUnit::mouseUp (const MouseEvent&)    { active = -1; }

void EffectUnit::mouseDoubleClick (const MouseEvent& e)
{
    const int i = widgetAt (e.getPosition());
    if (i < 0) return;
    const Widget w = widgets.getReference (i);
    if ((w.kind == Widget::Knob || w.kind == Widget::Slider) && w.info != nullptr)
    {
        main.pushUndo();
        setValue (w, w.info->def);
        repaint();
    }
}

void EffectUnit::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& wheel)
{
    const int i = widgetAt (e.getPosition());
    if (i < 0) return;
    const Widget w = widgets.getReference (i);
    if (w.kind == Widget::Tab || w.kind == Widget::Toggle) return;

    wheelAccum += wheel.deltaY * 40.0;
    const int steps = (int) wheelAccum;
    if (steps != 0)
    {
        wheelAccum -= steps;
        setValue (w, value (w) + steps);
        refresh();
    }
}

//==============================================================================
MainComponent::MainComponent() : control (*this), grid (*this), effects (*this)
{
    setOpaque (true);
    addAndMakeVisible (control);
    addAndMakeVisible (grid);
    addAndMakeVisible (effects);
    doc.addChangeListener (this);
    doc.onModelReloaded = [this] { effects.invalidate(); };
    midi.listener = this;
    setWantsKeyboardFocus (true);
    setSize (1024, 640);

    String error;
    if (doc.loadError.isNotEmpty())
        message ("Axe-Edit data files", doc.loadError);
    else if (settings()->getIntValue ("model", AxeModel::Ultra) == AxeModel::Standard)
        doc.loadModelData (AxeModel::Standard, error);
    midi.model = doc.model;

    const File last (settings()->getValue ("lastFile"));
    auto factory = doc.dataDir().getChildFile ("Banks").getChildFile (AxeModel::folder (doc.model))
                      .findChildFiles (File::findFiles, false, "*.syx");
    factory.sort();
    if (last.existsAsFile())            openFile (last);
    else if (! factory.isEmpty())       openFile (factory.getFirst());
    else                                doc.newEmpty();

    librarian = std::make_unique<Librarian> (doc, midi);
    librarian->onEdit = [this] (LibContainer& c, int index) { editFromLibrarian (c, index); };

    tempoCC = jlimit (0, 127, settings()->getIntValue ("tempoCC", 14));
    audio.inputGain  = (float) settings()->getDoubleValue ("inputGain", 1.0);
    audio.outputGain = (float) settings()->getDoubleValue ("outputGain", 1.0);
    audio.restore (settings()->getValue ("audio"));      // only reopens audio the user turned on before
}

MainComponent::~MainComponent()
{
    settings()->setValue ("inputGain", (double) audio.inputGain.load());
    settings()->setValue ("outputGain", (double) audio.outputGain.load());
    settings()->setValue ("audio", audio.state());
    settings()->saveIfNeeded();
    firmware.onFinished = nullptr;
    if (firmware.isRunning())
        firmware.cancel();
    firmwareWindow.reset();
    // modifier / audio dialogs refer to this component and its document
    for (int i = TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
        if (auto* dw = dynamic_cast<DialogWindow*> (TopLevelWindow::getTopLevelWindow (i)))
            delete dw;
    manageWindow.reset();
    librarian.reset();
    doc.removeChangeListener (this);
    midi.listener = nullptr;
    midi.close();
}

PropertiesFile* MainComponent::settings()
{
    if (props == nullptr)
    {
        PropertiesFile::Options o;
        o.applicationName = "Axe-Edit-arm64";
        o.filenameSuffix = "settings";
        o.osxLibrarySubFolder = "Application Support";
        props = std::make_unique<PropertiesFile> (o);
    }
    return props.get();
}

void MainComponent::resized()
{
    control.setBounds (0, 0, 1024, 128);
    grid.setBounds (0, 128, 1024, 256);
    effects.setBounds (0, 384, 1024, 256);
}

void MainComponent::changeListenerCallback (ChangeBroadcaster*)
{
    if (librarian != nullptr)
        librarian->modelChanged();      // opening a file of the other model switches the librarian too
    effects.refresh();
    control.repaint();
    grid.repaint();
}

void MainComponent::setStatus (const String& s)     { status = s; control.repaint(); }

void MainComponent::message (const String& title, const String& text)
{
    AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, title, text);
}

bool MainComponent::keyPressed (const KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (mods.isCommandDown() && CharacterFunctions::toUpperCase ((juce_wchar) k.getKeyCode()) == 'O')                          { openFile(); return true; }
    if (mods.isCommandDown() && mods.isShiftDown() && CharacterFunctions::toUpperCase ((juce_wchar) k.getKeyCode()) == 'S')    { saveAs (doc.sourceIsBank); return true; }
    if (mods.isCommandDown() && CharacterFunctions::toUpperCase ((juce_wchar) k.getKeyCode()) == 'S')                          { save(); return true; }
    if (mods.isCommandDown() && mods.isShiftDown() && CharacterFunctions::toUpperCase ((juce_wchar) k.getKeyCode()) == 'Z')    { redo(); return true; }
    if (mods.isCommandDown() && CharacterFunctions::toUpperCase ((juce_wchar) k.getKeyCode()) == 'Z')                          { undo(); return true; }
    if (k == KeyPress::deleteKey || k == KeyPress::backspaceKey)                { grid.deleteSelected(); return true; }
    if (k == KeyPress::upKey)                                                   { stepPreset (1); return true; }
    if (k == KeyPress::downKey)                                                 { stepPreset (-1); return true; }
    return false;
}

void MainComponent::pushUndo()
{
    if (undoStack.isEmpty() || undoStack.getLast().bytesUsed() >= 0)
    {
        uint8 a[Preset::size], b[Preset::size];
        if (! undoStack.isEmpty() && undoStack.getLast().toBytes (a) && doc.preset.toBytes (b)
              && memcmp (a, b, sizeof (a)) == 0 && undoStack.getLast().name == doc.preset.name)
            return;
        undoStack.add (doc.preset);
        if (undoStack.size() > 100) undoStack.remove (0);
        redoStack.clear();
    }
}

void MainComponent::undo()
{
    if (undoStack.isEmpty()) { setStatus ("Nothing to undo."); return; }
    redoStack.add (doc.preset);
    doc.preset = undoStack.removeAndReturn (undoStack.size() - 1);
    doc.touched();
    gridChanged();
}

void MainComponent::redo()
{
    if (redoStack.isEmpty()) { setStatus ("Nothing to redo."); return; }
    undoStack.add (doc.preset);
    doc.preset = redoStack.removeAndReturn (redoStack.size() - 1);
    doc.touched();
    gridChanged();
}

void MainComponent::gridChanged()
{
    // Structural edits are pushed as a whole edit-buffer preset, which keeps the unit and editor in step.
    if (midi.isConnected())
        midi.sendPreset (doc.preset, true, 0);
}

void MainComponent::paramChanged (int effect, int param, int value)
{
    if (midi.isConnected())
        midi.setParam (effect, param, value);
}

void MainComponent::modifierChanged (int effect, int modifierParam, int chunkId, int index, int raw)
{
    // one undo step per burst of edits (a slider drag sends many)
    const auto now = Time::getMillisecondCounter();
    if (now - lastModifierUndo > 1500)
        pushUndo();
    lastModifierUndo = now;

    doc.preset.setModifierValue (chunkId, index, raw);
    if (midi.isConnected())
        midi.setModifier (effect, modifierParam, index, raw);
    doc.touched();
}

void MainComponent::showAudioSettings()
{
    // The original's Audio tab: pick an audio interface; its input is passed through to the output
    // with the meters' level pins as gain, and drives the input/output meters and clip LEDs.
    struct AudioPanel : public Component
    {
        AudioPanel (MainComponent& m) : owner (m), selector (m.audio.manager, 1, 2, 1, 2, false, false, true, false)
        {
            enable.setButtonText ("Pass audio through and show it on the meters");
            enable.setToggleState (m.audio.isRunning(), dontSendNotification);
            enable.onClick = [this]
            {
                String error;
                owner.audio.enable (enable.getToggleState(), error);
                if (error.isNotEmpty()) { owner.message ("Audio", error); enable.setToggleState (false, dontSendNotification); }
                selector.setVisible (owner.audio.isRunning());
                owner.settings()->setValue ("audio", owner.audio.state());
            };
            note.setText ("The chosen input is played on the chosen output (use the meters' level pins as gain). "
                          "Use headphones or a line output to avoid feedback. macOS asks for microphone access the first time.",
                          dontSendNotification);
            note.setFont (FontOptions (13.0f));
            addAndMakeVisible (enable);
            addAndMakeVisible (note);
            addChildComponent (selector);
            selector.setVisible (m.audio.isRunning());
            setSize (520, 420);
        }
        ~AudioPanel() override         { owner.settings()->setValue ("audio", owner.audio.state()); }
        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            enable.setBounds (r.removeFromTop (26));
            note.setBounds (r.removeFromTop (40));
            selector.setBounds (r);
        }
        MainComponent& owner;
        AudioDeviceSelectorComponent selector;
        ToggleButton enable;
        Label note;
    };

    DialogWindow::LaunchOptions o;
    o.content.setOwned (new AudioPanel (*this));
    o.dialogTitle = "Audio Settings";
    o.componentToCentreAround = this;
    o.dialogBackgroundColour = Colour (0xff2a2a2a);
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void MainComponent::showTempoSettings()
{
    auto* w = new AlertWindow ("Tap Tempo", "MIDI CC number the tempo button sends (the Axe-Fx's TAP TEMPO controller, default 14).",
                               MessageBoxIconType::NoIcon);
    w->addTextEditor ("cc", String (tempoCC));
    w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
    {
        const auto t = w->getTextEditorContents ("cc").trim();
        if (r == 1 && t.isNotEmpty() && t.containsOnly ("0123456789"))
        {
            tempoCC = jlimit (0, 127, t.getIntValue());
            settings()->setValue ("tempoCC", tempoCC);
        }
    }), true);
}

void MainComponent::updateFirmware()
{
    if (! midi.isConnected())
    {
        message ("Update Firmware", "No hardware detected!\n\nConnect to the Axe-Fx first (Settings > MIDI Input / Output) "
                                    "so the editor can check its firmware version before and after the update.");
        return;
    }
    if (midi.isTransferring() || (librarian != nullptr && librarian->isBusy()))
    {
        message ("Update Firmware", "Wait for the current transfer to finish first.");
        return;
    }

    chooser = std::make_unique<FileChooser> ("Select a Firmware File...", File::getSpecialLocation (File::userHomeDirectory).getChildFile ("Downloads"), "*.syx");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (! f.existsAsFile()) return;

        const String text = "Model : " + AxeModel::name (doc.model) + "\nFirmware : " + String (firmwareMajor) + "."
                            + String (firmwareMinor).paddedLeft ('0', 2) + "\nMIDI Output : " + midi.outputName
                            + "\n\nFile : " + f.getFileName() + " (" + File::descriptionOfSizeInBytes (f.getSize()) + ")\n\n"
                              "Only use a firmware file Fractal Audio released for this exact model. Don't switch off the "
                              "Axe-Fx, unplug MIDI or quit Axe-Edit until the unit has finished installing it.";
        AlertWindow::showOkCancelBox (MessageBoxIconType::WarningIcon, "Ready to send firmware...", text, "Send", "Cancel", this,
            ModalCallbackFunction::create ([this, f] (int r)
        {
            if (r != 1) return;
            firmware.oldMajor = firmwareMajor;
            firmware.oldMinor = firmwareMinor;
            String error;
            if (! firmware.start (f, error)) { message ("Update Firmware", error); return; }

            struct Progress : public Component, private Timer
            {
                Progress (FirmwareUpdater& u) : updater (u)
                {
                    cancel.setButtonText ("Abort");
                    cancel.onClick = [this] { updater.cancel(); };
                    addAndMakeVisible (cancel);
                    addAndMakeVisible (bar);
                    setSize (460, 200);
                    startTimer (200);
                }
                void timerCallback() override   { value = updater.progress(); cancel.setEnabled (updater.isRunning()); repaint(); }
                void paint (Graphics& g) override
                {
                    g.fillAll (Colour (0xff2a2a2a));
                    g.setColour (Colours::white);
                    g.setFont (FontOptions (14.0f));
                    g.drawFittedText (updater.statusText(), getLocalBounds().reduced (14).withTrimmedBottom (70), Justification::topLeft, 6);
                }
                void resized() override
                {
                    auto r = getLocalBounds().reduced (14);
                    cancel.setBounds (r.removeFromBottom (26).removeFromRight (100));
                    r.removeFromBottom (8);
                    bar.setBounds (r.removeFromBottom (20));
                }
                FirmwareUpdater& updater;
                double value = 0;
                ProgressBar bar { value };
                TextButton cancel;
            };

            firmwareWindow = std::make_unique<DialogWindow> ("Updating Firmware...", Colour (0xff2a2a2a), false, false);
            firmwareWindow->setContentOwned (new Progress (firmware), true);
            firmwareWindow->setUsingNativeTitleBar (true);
            firmwareWindow->centreAroundComponent (this, firmwareWindow->getWidth(), firmwareWindow->getHeight());
            firmwareWindow->setVisible (true);
            firmware.onFinished = [this] (bool ok, const String& text)
            {
                message (ok ? "Firmware Update" : "Firmware Update", text);
                Timer::callAfterDelay (1500, [this] { firmwareWindow.reset(); });
            };
        }));
    });
}

void MainComponent::exportCsv()
{
    auto dir = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Axe-Edit");
    dir.createDirectory();
    chooser = std::make_unique<FileChooser> ("Export preset as CSV",
                                             dir.getChildFile (File::createLegalFileName (doc.preset.name.isEmpty() ? String ("Preset") : doc.preset.name) + ".csv"), "*.csv");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                          [this] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        f = f.withFileExtension ("csv");
        if (f.replaceWithText (Csv::exportPreset (doc, doc.preset), false, false, nullptr))  setStatus ("Exported " + f.getFileName());
        else                                                                                message ("Error whilst saving", "Couldn't write to the specified file!");
    });
}

void MainComponent::importCsv()
{
    chooser = std::make_unique<FileChooser> ("Import a preset CSV", File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Axe-Edit"), "*.csv");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this] (const FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        Preset p;
        String error;
        if (! Csv::importPreset (doc, f.loadFileAsString(), p, error)) { message ("Import CSV", error); return; }
        pushUndo();
        doc.preset = p;
        doc.selectedEffect = 0;
        doc.touched();
        gridChanged();
        setStatus ("Imported " + f.getFileName() + " into the current preset (save it to keep it).");
    });
}

//==============================================================================
void MainComponent::openFile (const File& f)
{
    String error;
    if (! doc.open (f, error))
    {
        message ("Can't open file", error);
        return;
    }
    sourceIsHardware = false;
    midi.model = doc.model;
    undoStack.clear();
    redoStack.clear();
    settings()->setValue ("lastFile", f.getFullPathName());
    settings()->setValue ("model", doc.model);
    setStatus ({});
}

void MainComponent::openFile()
{
    chooser = std::make_unique<FileChooser> ("Open an Axe-Fx Standard/Ultra bank or preset",
                                             doc.sourceFile.existsAsFile() ? doc.sourceFile.getParentDirectory()
                                                                           : doc.dataDir().getChildFile ("Banks"), "*.syx");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this] (const FileChooser& fc) { if (fc.getResult().existsAsFile()) openFile (fc.getResult()); });
}

void MainComponent::save()
{
    // Never write into the application bundle's factory banks.
    const bool factory = doc.sourceFile.isAChildOf (doc.dataDir());
    if (! doc.sourceFile.existsAsFile() || factory || sourceIsHardware)
    {
        if (sourceIsHardware && midi.isConnected())
        {
            midi.sendPreset (doc.preset, true, 0);
            setStatus ("Preset sent to the Axe-Fx edit buffer. Use Save As to store it in a slot or file.");
            return;
        }
        saveAs (doc.sourceIsBank);
        return;
    }

    String error;
    if (doc.save (doc.sourceFile, doc.sourceIsBank, error))    setStatus ("Saved " + doc.sourceFile.getFileName());
    else                                                       message ("Save failed", error);
}

void MainComponent::saveAs (bool bank)
{
    auto dir = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Axe-Edit");
    dir.createDirectory();
    const auto suggested = bank ? (doc.sourceFile.existsAsFile() ? doc.sourceFile.getFileName() : String ("Bank.syx"))
                                : File::createLegalFileName (doc.preset.name.isEmpty() ? String ("Preset") : doc.preset.name) + ".syx";
    chooser = std::make_unique<FileChooser> (bank ? "Save bank as" : "Save preset as", dir.getChildFile (suggested), "*.syx");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles | FileBrowserComponent::warnAboutOverwriting,
                          [this, bank] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == File()) return;
        String error;
        if (doc.save (f.withFileExtension ("syx"), bank, error))  setStatus ("Saved " + f.getFileName());
        else                                                      message ("Save failed", error);
    });
}

void MainComponent::snapshot()
{
    auto dir = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("Axe-Edit/Snapshots");
    dir.createDirectory();
    const auto f = dir.getChildFile (File::createLegalFileName (doc.preset.name + " " + Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S")) + ".syx");
    const auto data = Sysex::encodePreset (doc.preset, doc.model, doc.manufacturer, 1, 0);
    setStatus (f.replaceWithData (data.getData(), data.getSize()) ? "Snapshot saved: " + f.getFileName() : String ("Couldn't write the snapshot."));
}

void MainComponent::renamePreset()
{
    auto* w = new AlertWindow ("Preset name", "Up to 32 characters.", MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", doc.preset.name);
    w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
    {
        if (r == 1)
        {
            pushUndo();
            doc.preset.name = w->getTextEditorContents ("name").substring (0, 32);
            doc.touched();
            gridChanged();
        }
    }), true);
}

void MainComponent::stepPreset (int delta)
{
    if (sourceIsHardware && midi.isConnected())
    {
        if (doc.presetIndex < 0)
            goToPreset();
        else
            recallHardwarePreset (jlimit (0, 383, doc.presetIndex + delta));
        return;
    }
    if (doc.bank.size() > 1)
    {
        doc.selectPreset ((doc.presetIndex + delta + doc.bank.size()) % doc.bank.size());
        undoStack.clear(); redoStack.clear();
        grid.selectedCell = { -1, -1 };
    }
}

void MainComponent::recallHardwarePreset (int number)
{
    doc.presetIndex = number;
    midi.changeProgram (number);
    Timer::callAfterDelay (250, [this] { if (midi.isConnected()) midi.requestPreset (true, 0); });
}

void MainComponent::useHardware()
{
    if (! sourceIsHardware)
        doc.presetIndex = -1;           // the file's index says nothing about the unit
    sourceIsHardware = true;
    midi.requestPreset (true, 0);
}

void MainComponent::goToPreset()
{
    auto* w = new AlertWindow ("Go to preset",
                               "The Axe-Fx can't report which preset it is on, so enter the number shown on the unit "
                               "(0-383) to recall it and keep the editor in step.", MessageBoxIconType::NoIcon);
    w->addTextEditor ("number", doc.presetIndex >= 0 ? String (doc.presetIndex) : String());
    w->addButton ("Go", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int r)
    {
        const auto text = w->getTextEditorContents ("number").trim();
        const int n = text.getIntValue();
        if (r == 1 && text.containsOnly ("0123456789") && text.isNotEmpty() && isPositiveAndBelow (n, 384))
            recallHardwarePreset (n);
    }), true);
}

void MainComponent::axeProgram (int number)
{
    if (! sourceIsHardware || ! isPositiveAndBelow (number, 384))
        return;
    doc.presetIndex = number;
    midi.requestPreset (true, 0);
}

void MainComponent::setModel (int model)
{
    String error;
    if (model == doc.model || model < 0 || model > AxeModel::Ultra) return;
    if (! doc.loadModelData (model, error)) { message ("Model data", error); return; }
    midi.model = model;
    settings()->setValue ("model", model);
    doc.newEmpty();
}

void MainComponent::connect (const String& in, const String& out)
{
    String error;
    midiInId = in; midiOutId = out;
    firmwareMajor = firmwareMinor = -1;
    if (in.isEmpty() || out.isEmpty()) { setStatus ("Choose both a MIDI input and a MIDI output."); return; }
    midi.model = doc.model;
    if (! midi.open (in, out, error)) { message ("MIDI", error); return; }
    settings()->setValue ("midiIn", in);
    settings()->setValue ("midiOut", out);
    setStatus ("Looking for an Axe-Fx on " + midi.outputName + "...");
    Timer::callAfterDelay (5000, [this] { if (midi.isOpen() && ! midi.isConnected()) setStatus ("No reply from an Axe-Fx. Check the cables, ports and SysEx settings."); });
}

//==============================================================================
void MainComponent::showSettingsMenu()
{
    PopupMenu m, ins, outs, models;
    const auto inDevs = MidiInput::getAvailableDevices();
    const auto outDevs = MidiOutput::getAvailableDevices();
    if (midiInId.isEmpty())  midiInId = settings()->getValue ("midiIn");
    if (midiOutId.isEmpty()) midiOutId = settings()->getValue ("midiOut");

    for (int i = 0; i < inDevs.size(); ++i)   ins.addItem (100 + i, inDevs[i].name, true, inDevs[i].identifier == midiInId);
    for (int i = 0; i < outDevs.size(); ++i)  outs.addItem (300 + i, outDevs[i].name, true, outDevs[i].identifier == midiOutId);
    if (inDevs.isEmpty())  ins.addItem (99, "(no MIDI inputs found)", false);
    if (outDevs.isEmpty()) outs.addItem (98, "(no MIDI outputs found)", false);
    models.addItem (10, "Axe-Fx Standard", true, doc.model == AxeModel::Standard);
    models.addItem (11, "Axe-Fx Ultra", true, doc.model == AxeModel::Ultra);

    m.addSectionHeader ("Axe-Fx connection");
    m.addSubMenu ("MIDI Input", ins);
    m.addSubMenu ("MIDI Output", outs);
    m.addItem (1, midi.isOpen() ? "Disconnect" : "Connect", midi.isOpen() || (midiInId.isNotEmpty() && midiOutId.isNotEmpty()));
    m.addSeparator();
    m.addSubMenu ("Model (offline editing)", models);
    m.addSeparator();
    m.addItem (2, "Read current preset from Axe-Fx", midi.isConnected());
    m.addItem (3, "Send this preset to Axe-Fx edit buffer", midi.isConnected());
    m.addSeparator();
    m.addItem (4, "Audio Settings...");
    m.addItem (5, "Tap Tempo CC (" + String (tempoCC) + ")...");
    m.addSeparator();
    m.addItem (6, "Update Firmware...", midi.isConnected() && ! firmware.isRunning());

    m.showMenuAsync ({}, [this, inDevs, outDevs] (int r)
    {
        if (r == 1)
        {
            if (midi.isOpen()) { midi.close(); sourceIsHardware = false; firmwareMajor = -1; setStatus ("Disconnected."); }
            else               connect (midiInId, midiOutId);
        }
        else if (r == 2)                useHardware();
        else if (r == 3)                { midi.sendPreset (doc.preset, true, 0); setStatus ("Preset sent to the edit buffer."); }
        else if (r == 4)                showAudioSettings();
        else if (r == 5)                showTempoSettings();
        else if (r == 6)                updateFirmware();
        else if (r == 10 || r == 11)    setModel (r - 10);
        else if (r >= 300)              { midiOutId = outDevs[r - 300].identifier; if (midiInId.isNotEmpty()) connect (midiInId, midiOutId); }
        else if (r >= 100)              { midiInId = inDevs[r - 100].identifier; if (midiOutId.isNotEmpty()) connect (midiInId, midiOutId); }
    });
}

void MainComponent::showSourceMenu()
{
    PopupMenu m, factory;
    Array<File> files;
    for (auto* folder : { "Standard", "Ultra" })
        for (auto& f : doc.dataDir().getChildFile ("Banks").getChildFile (folder).findChildFiles (File::findFiles, false, "*.syx"))
            files.add (f);
    files.sort();
    for (int i = 0; i < files.size(); ++i)
        factory.addItem (100 + i, files[i].getParentDirectory().getFileName() + " / " + files[i].getFileNameWithoutExtension(), true, files[i] == doc.sourceFile);

    m.addItem (1, "Open Bank or Preset File...");
    m.addSubMenu ("Factory Banks", factory);
    m.addItem (2, "Axe-Fx (current preset)", midi.isConnected(), sourceIsHardware);
    m.addSeparator();
    m.addItem (3, "New Empty Preset");
    m.addItem (4, "Import Preset from CSV...");

    m.showMenuAsync ({}, [this, files] (int r)
    {
        if (r == 4)             importCsv();
        else if (r == 1)        openFile();
        else if (r == 2)        useHardware();
        else if (r == 3)        { sourceIsHardware = false; doc.newEmpty(); undoStack.clear(); redoStack.clear(); }
        else if (r >= 100)      openFile (files[r - 100]);
    });
}

void MainComponent::showRecallMenu()
{
    PopupMenu m;
    if (doc.bank.size() > 1)
    {
        for (int start = 0; start < doc.bank.size(); start += 32)
        {
            PopupMenu sub;
            for (int i = start; i < jmin (start + 32, doc.bank.size()); ++i)
                sub.addItem (1 + i, String (i).paddedLeft ('0', 3) + "  " + (i == doc.presetIndex ? doc.preset.name : doc.bank[i]->name), true, i == doc.presetIndex);
            m.addSubMenu (String (start).paddedLeft ('0', 3) + " - " + String (jmin (start + 31, doc.bank.size() - 1)).paddedLeft ('0', 3), sub);
        }
    }
    else
        m.addItem (-1, sourceIsHardware ? "Use the preset up/down buttons to step through the unit's presets" : "Open a bank file to recall presets from it", false);

    m.showMenuAsync ({}, [this] (int r)
    {
        if (r > 0) { doc.selectPreset (r - 1); undoStack.clear(); redoStack.clear(); grid.selectedCell = { -1, -1 }; }
    });
}

void MainComponent::showSaveAsMenu()
{
    PopupMenu m;
    m.addItem (1, "Save Preset As File...");
    m.addItem (2, "Save Bank As File...", doc.bank.size() == Sysex::bankPresets);
    m.addItem (4, "Export Preset as CSV...");
    m.addSeparator();
    m.addItem (3, "Store to Axe-Fx Preset Slot...", midi.isConnected());

    m.showMenuAsync ({}, [this] (int r)
    {
        if (r == 1)         saveAs (false);
        else if (r == 2)    saveAs (true);
        else if (r == 4)    exportCsv();
        else if (r == 3)
        {
            auto* w = new AlertWindow ("Store to Axe-Fx", "Preset slot number (0-383). This overwrites the preset stored in that slot on the unit.",
                                       MessageBoxIconType::WarningIcon);
            w->addTextEditor ("slot", String (jmax (0, doc.presetIndex)));
            w->addButton ("Store", 1, KeyPress (KeyPress::returnKey));
            w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
            w->enterModalState (true, ModalCallbackFunction::create ([this, w] (int ok)
            {
                const int slot = w->getTextEditorContents ("slot").getIntValue();
                if (ok == 1 && isPositiveAndBelow (slot, 384))
                {
                    midi.sendPreset (doc.preset, false, slot);
                    if (librarian != nullptr)
                        librarian->presetStoredByEditor (slot, doc.preset);
                    setStatus ("Preset sent to slot " + String (slot) + ".");
                }
            }), true);
        }
    });
}

void MainComponent::showHelpMenu()
{
    PopupMenu m;
    m.addItem (1, "Axe-Edit Guide (PDF)");
    m.addItem (2, "Original Readme");
    m.addSeparator();
    m.addItem (3, "About This Rebuild");
    m.showMenuAsync ({}, [this] (int r)
    {
        if (r == 1) doc.dataDir().getChildFile ("Axe-Edit_Guide.pdf").startAsProcess();
        if (r == 2) doc.dataDir().getChildFile ("EditorReadme.txt").startAsProcess();
        if (r == 3) message ("Axe-Edit 1.0.191 - Apple Silicon rebuild",
                             "A native arm64 reconstruction of the Axe-Edit 1.0 build 191 editor for the Axe-Fx Standard and Ultra.\n\n"
                             "It uses the original skin, configs, parameter profiles and factory banks unchanged. "
                             "Preset/bank file handling is verified against the factory banks; the MIDI protocol follows the "
                             "original code but has not been tested against hardware.\n\n"
                             "See the README for what is not included.");
    });
}

//==============================================================================
void MainComponent::axeVersion (int model, int major, int minor)
{
    if (firmware.isRunning())
    {
        firmwareMajor = major; firmwareMinor = minor;
        firmware.versionReceived (major, minor);
        return;
    }
    firmwareMajor = major; firmwareMinor = minor;
    if (model <= AxeModel::Ultra && model != doc.model)
    {
        String error;
        if (doc.loadModelData (model, error))
            settings()->setValue ("model", model);
    }
    midi.model = doc.model;
    memcpy (doc.manufacturer, midi.manufacturer, 3);
    sourceIsHardware = true;
    doc.presetIndex = -1;
    setStatus ("Connected. Click the preset number to enter the one shown on the unit.");
    midi.requestPreset (true, 0);
}

void MainComponent::axeBank (int target, OwnedArray<Preset>& presets)
{
    if (librarian != nullptr)
        librarian->bankReceived (target, presets);
}

void MainComponent::showManage()
{
    if (manageWindow == nullptr)
        manageWindow = std::make_unique<ManageWindow> (*librarian, settings());
    manageWindow->setVisible (true);
    manageWindow->toFront (true);
}

void MainComponent::editFromLibrarian (LibContainer& c, int index)
{
    if (! isPositiveAndBelow (index, c.entries.size()))
        return;
    auto& e = c.entries.getReference (index);

    if (c.isHardware() && midi.isConnected())
    {
        if (e.state == LibEntry::Edited)
        {
            // Not on the unit yet: put the librarian's version in the edit buffer.
            sourceIsHardware = true;
            doc.bank.clear();
            doc.sourceFile = File();
            doc.sourceIsBank = false;
            doc.preset = e.preset;
            doc.presetIndex = c.slotNumber (index);
            doc.dirty = true;
            undoStack.clear(); redoStack.clear();
            doc.sendChangeMessage();
            midi.sendPreset (doc.preset, true, 0);
            setStatus ("Edited version from Axe-Manage sent to the edit buffer; store it to keep it.");
        }
        else
        {
            sourceIsHardware = true;
            recallHardwarePreset (c.slotNumber (index));
        }
    }
    else if (c.kind == LibContainer::BankFile && c.file.existsAsFile() && ! c.dirty)
    {
        sourceIsHardware = false;
        openFile (c.file);
        doc.selectPreset (index);
    }
    else if (c.kind == LibContainer::PresetFolder && e.file.existsAsFile())
    {
        sourceIsHardware = false;
        openFile (e.file);
    }
    else
    {
        sourceIsHardware = false;
        doc.bank.clear();
        doc.sourceFile = File();
        doc.sourceIsBank = false;
        doc.preset = e.preset;
        doc.presetIndex = c.slotNumber (index);
        doc.dirty = false;
        undoStack.clear(); redoStack.clear();
        doc.sendChangeMessage();
        setStatus (c.isHardware() ? "Loaded from the librarian (Axe-Fx not connected)." : "Loaded from " + c.name + ".");
    }

    if (auto* top = getTopLevelComponent())
        top->toFront (true);
}

void MainComponent::axePreset (const Preset& p, int target, int number)
{
    if (target == 0)                    // a stored preset the librarian asked for
    {
        if (librarian != nullptr)
            librarian->presetReceived (number, p);
        return;
    }
    if (! sourceIsHardware) return;
    const int index = doc.presetIndex;
    doc.bank.clear();
    doc.sourceFile = File();
    doc.sourceIsBank = false;
    doc.preset = p;
    // An edit-buffer dump (target 1) carries no real preset number: the unit echoes the request's.
    doc.presetIndex = target == 1 ? index : number;
    doc.selectedEffect = 0;
    doc.dirty = false;
    effects.hardwareText.clear();
    undoStack.clear(); redoStack.clear();
    doc.sendChangeMessage();
}

void MainComponent::axeParam (int effect, int param, int value, const String& text)
{
    if (text.isNotEmpty())
        effects.hardwareText[{ effect, param }] = text;
    if (! isMouseButtonDownAnywhere() && doc.preset.getParam (effect, param) >= 0 && doc.preset.getParam (effect, param) != value)
        doc.preset.setParam (effect, param, value);
    effects.repaint();
}

void MainComponent::axeTuner (int note, int string, int tune)
{
    static const char* const names[] = { "A", "Bb", "B", "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab" };
    const int cents = tune - 63;
    control.tunerText = String (names[note % 12]) + "   string " + String (string + 1) + "   "
                          + (cents == 0 ? String ("in tune") : String (cents > 0 ? "sharp " : "flat ") + String (std::abs (cents)));
    control.repaint();
}

void MainComponent::axeAck (int function, bool ok)
{
    if (librarian != nullptr)
        librarian->ackReceived (function, ok);
    if (function == Sysex::PatchRcv || function == Sysex::SetGrid)
        setStatus (ok ? "The Axe-Fx acknowledged the transfer." : "The Axe-Fx reported a checksum error.");
}

void MainComponent::axeActivity (bool incoming)    { control.flashLed (incoming); }
