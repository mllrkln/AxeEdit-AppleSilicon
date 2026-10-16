#pragma once
#include "Model.h"
#include "AxeMidi.h"
#include "ManageUI.h"
#include "Extras.h"

class MainComponent;

// A skinned image button painted and hit-tested by its owning unit.
struct SkinButton
{
    String name, text, tip;
    Rectangle<int> bounds;
    Image up, over, down, on;
    XmlElement* el = nullptr;
    bool toggled = false;
};

class UnitBase : public Component, public SettableTooltipClient
{
public:
    UnitBase (MainComponent& m) : main (m) {}
    MainComponent& main;
    Document& doc();
    Skin& skin();
    AxeMidi& midi();
    void drawText (Graphics&, const String& font, const String& text, Rectangle<int> area, double size,
                   Colour, int justification);
    SkinButton makeButton (XmlElement* widget, const String& text = {});
    void paintButton (Graphics&, const SkinButton&);
    SkinButton* buttonAt (Array<SkinButton>&, Point<int>);
    Array<SkinButton> buttons;
    SkinButton* hover = nullptr;
    SkinButton* pressed = nullptr;
    void mouseMove (const MouseEvent&) override;
    void mouseExit (const MouseEvent&) override;
};

//==============================================================================
class ControlUnit : public UnitBase, private Timer
{
public:
    ControlUnit (MainComponent&);
    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    void flashLed (bool incoming);
    void tempoBeat()                        { hardwareBeat = Time::getMillisecondCounter(); }
    int tempoBpm() const;                   // the preset's Controllers TEMPO (30..250), or -1
    String tunerText;
    bool tunerOn = false;
private:
    XmlElement* cu = nullptr;
    uint32 inFlash = 0, outFlash = 0, hardwareBeat = 0, localBeat = 0, lastTap = 0;
    uint32 clipUntil[2] {};
    float shown[2][2] {};                   // displayed meter levels (input/output x L/R), decaying
    int draggingPin = -1;                   // 0 input gain, 1 output gain
    int64 lastState = -1;                   // what the timer last drew: repaint only on change
    void timerCallback() override;
    void clicked (SkinButton&);
    void drawLed (Graphics&, const String& name, bool on);
    void drawMeter (Graphics&, const String& name, int which);
    Rectangle<int> pinBounds (int which);
    void tapTempo();
    void editTempo();
};

//==============================================================================
class GridUnit : public UnitBase
{
public:
    GridUnit (MainComponent&);
    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    void mouseMove (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    Point<int> selectedCell { -1, -1 };
    void deleteSelected();
private:
    XmlElement* gridEl = nullptr;
    Rectangle<int> gridArea, inputArea, outputArea;
    int bw = 45, bh = 37, sx = 23, sy = 11, ox = 47, oy = 7;
    Point<int> hoverCell { -1, -1 }, dragFrom { -1, -1 };
    Point<int> dragPos;
    bool dragging = false, cableMode = false;

    Rectangle<int> cellBounds (int col, int row) const;
    Point<int> cellAt (Point<int>) const;
    void showCellMenu (int col, int row);
    void toolClicked (SkinButton&);
    void drawBlock (Graphics&, int col, int row);
};

//==============================================================================
class EffectUnit : public UnitBase
{
public:
    EffectUnit (MainComponent&);
    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails&) override;
    void mouseMove (const MouseEvent&) override;
    void refresh();                         // rebuilds the widget list if the structure changed
    void nextPage()                         { ++page; rebuild(); repaint(); }
    void invalidate()                       { widgets.clear(); structureKey = {}; active = -1; }
    std::map<std::pair<int, int>, String> hardwareText;

private:
    struct Widget
    {
        enum Kind { Knob, Toggle, Slider, Menu, Tab };
        Kind kind = Knob;
        Point<int> pos;
        Rectangle<int> hit;
        String label, paramName, size;      // size: "", Small, Large, Huge
        int paramId = -1, fineId = -1, bit = -1, groupId = -1, page = 0;
        int modifierId = 0;                 // 200..254 when a modifier can drive this parameter
        const ParamInfo* info = nullptr;
        bool stored = false;
    };

    int effectId = 0, page = 0, variantValue = -1;
    String type, structureKey, monitorName, pageName;
    XmlElement* layoutEl = nullptr;
    XmlElement* styleEl = nullptr;
    XmlElement* pageLayoutEl = nullptr;
    Array<Widget> widgets;
    StringArray pageNames;
    int active = -1, dragStartValue = 0;
    double wheelAccum = 0;

    void rebuild();
    void addRows (XmlElement* rowsParent, int baseX, int baseY, int spacingX, int spacingY);
    int value (const Widget&) const;
    void setValue (const Widget&, int raw);
    String valueText (const Widget&) const;
    int widgetAt (Point<int>) const;
    void paintWidget (Graphics&, const Widget&);
    void showMenu (const Widget&);
    XmlElement* style (const String& name) const;
    bool hasModifier (const Widget&) const;
    bool onModifierButton (const Widget&, Point<int>) const;
    void openModifier (const Widget&);
};

//==============================================================================
class MainComponent : public Component, public ChangeListener, public AxeMidi::Listener
{
public:
    MainComponent();
    ~MainComponent() override;

    Document doc;
    AxeMidi midi;
    ControlUnit control;
    GridUnit grid;
    EffectUnit effects;
    String status;
    bool sourceIsHardware = false;
    int firmwareMajor = -1, firmwareMinor = -1;

    void resized() override;
    bool keyPressed (const KeyPress&) override;
    void changeListenerCallback (ChangeBroadcaster*) override;

    // actions
    void openFile();
    void openFile (const File&);
    void save();
    void saveAs (bool bank);
    void snapshot();
    void renamePreset();
    void showSettingsMenu();
    void showSourceMenu();
    void showRecallMenu();
    void showSaveAsMenu();
    void showHelpMenu();
    void stepPreset (int delta);
    void goToPreset();
    void useHardware();
    void recallHardwarePreset (int number);
    void setModel (int model);
    void connect (const String& in, const String& out);
    void pushUndo();
    void undo();
    void redo();
    void gridChanged();                     // push the routing to the unit when connected
    void paramChanged (int effect, int param, int value);
    void setStatus (const String&);
    void message (const String& title, const String& text);
    void modifierChanged (int effect, int modifierParam, int chunkId, int index, int raw);
    void showAudioSettings();
    void showTempoSettings();
    void updateFirmware();
    void exportCsv();
    void importCsv();
    int tempoCC = 14;                       // the original's default tap-tempo CC (gAXE_FX_TEMPO_CC_NUM)
    AudioMonitor audio;
    FirmwareUpdater firmware { midi };

    // AxeMidi::Listener
    void axeVersion (int model, int major, int minor) override;
    void axePreset (const Preset&, int target, int number) override;
    void axeParam (int effect, int param, int value, const String& text) override;
    void axeTuner (int note, int string, int tune) override;
    void axeProgram (int number) override;
    void axeAck (int function, bool ok) override;
    void axeActivity (bool incoming) override;
    void axeLog (const String& s) override  { setStatus (s); }
    void axeBank (int target, OwnedArray<Preset>&) override;
    void axeTempoBeat() override            { control.tempoBeat(); }

    std::unique_ptr<Librarian> librarian;
    std::unique_ptr<ManageWindow> manageWindow;
    void showManage();
    void editFromLibrarian (LibContainer&, int index);

private:
    std::unique_ptr<FileChooser> chooser;
    Array<Preset> undoStack, redoStack;
    TooltipWindow tooltips { this, 600 };
    String midiInId, midiOutId;
    PropertiesFile* settings();
    std::unique_ptr<PropertiesFile> props;
    uint32 lastModifierUndo = 0;
    std::unique_ptr<DialogWindow> firmwareWindow;
};
