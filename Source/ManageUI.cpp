#include "ManageUI.h"
#include "Extras.h"

namespace
{
    enum Columns { colNumber = 1, colName, colAmp1, colAmp2, colBlocks, colStatus };

    const Colour background (0xff2b2b2b), panel (0xff222222), rowAlt (0xff262626), textColour (0xffdcdcdc),
                 dimText (0xff8a8a8a), accent (0xffe8a33c), edited (0xffe8c53c), synced (0xff7cc46a);

    String abbreviation (const String& type)
    {
        static const std::map<String, String> names {
            { "Amp", "AMP" }, { "Cab", "CAB" }, { "Chorus", "CHO" }, { "Compressor", "CPR" }, { "Crossover", "XVR" },
            { "Delay", "DLY" }, { "Drive", "DRV" }, { "EffectsLoop", "FXL" }, { "Enhancer", "ENH" },
            { "FeedbackReturn", "RTN" }, { "FeedbackSend", "SND" }, { "Filter", "FIL" }, { "Flanger", "FLG" },
            { "Formant", "FRM" }, { "GateExpander", "GTE" }, { "GraphicEQ", "GEQ" }, { "MegaTap", "MGT" },
            { "Mixer", "MIX" }, { "MultibandComp", "MBC" }, { "MultiDelay", "MTD" }, { "PanTrem", "PAN" },
            { "ParametricEQ", "PEQ" }, { "Phaser", "PHR" }, { "Pitch", "PIT" }, { "QuadChorus", "QCH" },
            { "Resonator", "RES" }, { "Reverb", "REV" }, { "RingMod", "RNG" }, { "Rotary", "ROT" },
            { "Synth", "SYN" }, { "Vocoder", "VOC" }, { "VolPan", "VOL" }, { "Wah", "WAH" } };
        auto it = names.find (type);
        return it != names.end() ? it->second : type.substring (0, 3).toUpperCase();
    }
}

//==============================================================================
PresetTable::PresetTable (ManageComponent& o) : owner (o) {}

int PresetTable::rowAt (Point<int> p) const
{
    const int r = getRowContainingPosition (p.x, p.y);
    return r < 0 ? const_cast<PresetTable*> (this)->getNumRows() : r;
}

bool PresetTable::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description == "lib-rows" && owner.container() != nullptr && owner.container()->isEditable();
}

void PresetTable::itemDragMove (const SourceDetails& d)     { dropRow = rowAt (d.localPosition); repaint(); }
void PresetTable::itemDragExit (const SourceDetails&)       { dropRow = -1; repaint(); }

void PresetTable::itemDropped (const SourceDetails& d)
{
    const int row = rowAt (d.localPosition);
    dropRow = -1;
    repaint();
    owner.dropRows (row);
}

void PresetTable::paintOverChildren (Graphics& g)
{
    TableListBox::paintOverChildren (g);
    if (dropRow < 0)
        return;
    auto r = getRowPosition (jmin (dropRow, getNumRows() - 1), true);
    const int y = dropRow >= getNumRows() ? r.getBottom() : r.getY();
    g.setColour (accent);
    g.fillRect (0, y - 1, getWidth(), 2);
}

//==============================================================================
SourceList::SourceList (ManageComponent& o) : ListBox ("Sources", nullptr), owner (o)
{
    setModel (this);    // only once the ListBoxModel base exists
}

int SourceList::getNumRows()                                        { return owner.numSources(); }
void SourceList::paintListBoxItem (int r, Graphics& g, int w, int h, bool s) { owner.paintSource (r, g, w, h, s); }
void SourceList::selectedRowsChanged (int row)                      { owner.sourceSelected (row); }
void SourceList::listBoxItemClicked (int row, const MouseEvent& e)  { owner.sourceClicked (row, e); }

bool SourceList::isInterestedInDragSource (const SourceDetails& d)     { return d.description == "lib-rows"; }

void SourceList::itemDragMove (const SourceDetails& d)
{
    const int row = getRowContainingPosition (d.localPosition.x, d.localPosition.y);
    hoverRow = owner.canDropOnSource (row) ? row : -1;
    repaint();
}

void SourceList::itemDragExit (const SourceDetails&)   { hoverRow = -1; repaint(); }

void SourceList::itemDropped (const SourceDetails& d)
{
    const int row = getRowContainingPosition (d.localPosition.x, d.localPosition.y);
    hoverRow = -1;
    repaint();
    if (owner.canDropOnSource (row))
        owner.dropOnSource (row);
}

void SourceList::paintOverChildren (Graphics& g)
{
    ListBox::paintOverChildren (g);
    if (hoverRow >= 0)
    {
        g.setColour (accent);
        g.drawRect (getRowPosition (hoverRow, true), 2);
    }
}

//==============================================================================
ManageComponent::ManageComponent (Librarian& l, PropertiesFile* s) : lib (l), settings (s)
{
    laf.setColourScheme ({ background, panel, panel, Colour (0xff3a3a3a), textColour, Colour (0xff3d3d3d),
                           Colours::white, accent.withAlpha (0.35f), textColour });
    laf.setColour (ListBox::backgroundColourId, panel);
    laf.setColour (TableHeaderComponent::backgroundColourId, Colour (0xff333333));
    laf.setColour (TableHeaderComponent::textColourId, textColour);
    laf.setColour (TextEditor::backgroundColourId, Colour (0xff1b1b1b));
    laf.setColour (ProgressBar::foregroundColourId, accent);
    setLookAndFeel (&laf);

    for (auto* name : { "Open...", "New Bank", "Save", "Save As...", "Export...", "Download", "Upload",
                        "Sync & Backup", "Undo", "Redo", "Cancel" })
    {
        auto* b = tools.add (new TextButton (name));
        b->setName (name);
        addAndMakeVisible (b);
    }
    tool ("Open...")->onClick       = [this] { openSomething(); };
    tool ("New Bank")->onClick      = [this] { lib.newBankFile(); sources.selectRow (lib.containers.size()); };
    tool ("Save")->onClick          = [this] { saveCurrent (false); };
    tool ("Save As...")->onClick    = [this] { saveCurrent (true); };
    tool ("Export...")->onClick     = [this] { exportSelected(); };
    tool ("Download")->onClick      = [this] { showDownloadMenu(); };
    tool ("Upload")->onClick        = [this] { showUploadMenu(); };
    tool ("Sync & Backup")->onClick = [this] { showBackupMenu(); };
    tool ("Undo")->onClick          = [this] { undoRedo (false); };
    tool ("Redo")->onClick          = [this] { undoRedo (true); };
    tool ("Cancel")->onClick        = [this] { lib.cancelAll(); };

    tool ("Download")->setTooltip ("Read presets from the Axe-Fx");
    tool ("Upload")->setTooltip ("Write presets or cabs to the Axe-Fx");
    tool ("Sync & Backup")->setTooltip ("Download banks from the Axe-Fx and save them as bank files in Documents/Axe-Edit Backups");

    sources.setRowHeight (26);
    addAndMakeVisible (sources);

    table.setModel (this);
    table.setRowHeight (22);
    table.setMultipleSelectionEnabled (true);
    table.getHeader().setStretchToFitActive (true);
    addAndMakeVisible (table);

    search.setTextToShowWhenEmpty ("Search names", dimText);
    search.onTextChange = [this] { refreshRows(); };
    addAndMakeVisible (search);

    status.setColour (Label::textColourId, dimText);
    addAndMakeVisible (status);
    addChildComponent (progressBar);

    lib.addChangeListener (this);
    sources.selectRow (0);
    rebuildColumns();
    refreshRows();
    setWantsKeyboardFocus (true);
}

ManageComponent::~ManageComponent()
{
    lib.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

TextButton* ManageComponent::tool (const String& name) const
{
    for (auto* b : tools)
        if (b->getName() == name)
            return b;
    jassertfalse;
    return tools.getFirst();
}

Image ManageComponent::icon (const String& name)
{
    auto it = icons.find (name);
    if (it != icons.end())
        return it->second;

    Image img;
    ZipFile zip (lib.doc.dataDir().getChildFile ("Skins/Default/AM.paxege"));
    if (auto* e = zip.getEntry (name))
        if (std::unique_ptr<InputStream> in (zip.createStreamForEntry (*e)); in != nullptr)
            img = ImageFileFormat::loadFrom (*in);
    icons[name] = img;
    return img;
}

LibContainer* ManageComponent::container() const
{
    if (source < cabsSource)
        return lib.containers[source];
    if (source > cabsSource)
        return lib.containers[source - 1];
    return nullptr;
}

Array<LibEntry>& ManageComponent::clipboard()
{
    static Array<LibEntry> c;
    return c;
}

//==============================================================================
void ManageComponent::paint (Graphics& g)
{
    g.fillAll (background);
}

void ManageComponent::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto bar = r.removeFromTop (28);
    for (auto* b : tools)
    {
        const int w = jmax (58, (int) GlyphArrangement::getStringWidth (Font (FontOptions (14.0f)), b->getButtonText()) + 22);
        b->setBounds (bar.removeFromLeft (w));
        bar.removeFromLeft (4);
        if (b->getName() == "New Bank" || b->getName() == "Export..." || b->getName() == "Sync & Backup")
            bar.removeFromLeft (10);
    }
    r.removeFromTop (8);

    auto bottom = r.removeFromBottom (22);
    progressBar.setBounds (bottom.removeFromRight (260));
    status.setBounds (bottom);
    r.removeFromBottom (6);

    sources.setBounds (r.removeFromLeft (220));
    r.removeFromLeft (8);
    search.setBounds (r.removeFromTop (24).removeFromRight (260));
    r.removeFromTop (6);
    table.setBounds (r);
    table.updateContent();
}

//==============================================================================
void ManageComponent::paintSource (int row, Graphics& g, int width, int height, bool selected)
{
    if (selected)
        g.fillAll (accent.withAlpha (0.3f));

    String text, iconName;
    bool dirty = false;
    if (row == cabsSource)
    {
        text = "Axe-Fx  User Cabs";
        iconName = "axefx-cabs.png";
    }
    else if (auto* c = lib.containers[row < cabsSource ? row : row - 1])
    {
        if (c->isHardware())
        {
            text = "Axe-Fx  " + c->name;
            iconName = String ("axefx-bank-") + String::charToString ((juce_wchar) ('a' + c->bank)) + ".png";
        }
        else
        {
            text = c->name;
            iconName = c->kind == LibContainer::PresetFolder ? "folder.png" : "file_bank-3.png";
            dirty = c->dirty;
        }
    }

    const auto img = icon (iconName);
    if (img.isValid())
        g.drawImageWithin (img, 6, (height - 16) / 2, 16, 16, RectanglePlacement::centred);
    g.setColour (textColour);
    g.setFont (FontOptions (14.0f));
    g.drawText (text + (dirty ? " *" : ""), 28, 0, width - 32, height, Justification::centredLeft, true);

    if (row == cabsSource || row == cabsSource - 1)
    {
        g.setColour (Colour (0xff3a3a3a));
        g.drawHorizontalLine (height - 1, 0.0f, (float) width);
    }
}

void ManageComponent::sourceSelected (int lastRowSelected)
{
    if (lastRowSelected < 0)
        return;
    if (source != lastRowSelected)
    {
        source = lastRowSelected;
        table.deselectAllRows();
        rebuildColumns();
    }
    refreshRows();
}

void ManageComponent::sourceClicked (int row, const MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;
    auto* c = row > cabsSource ? lib.containers[row - 1] : nullptr;
    if (c == nullptr)
        return;

    PopupMenu m;
    if (c->kind == LibContainer::PresetFolder)
    {
        m.addItem ("Refresh", [this, c] { lib.rescanFolder (*c); });
        m.addItem ("Show in Finder", [c] { c->file.revealToUser(); });
    }
    else if (c->file.existsAsFile())
        m.addItem ("Show in Finder", [c] { c->file.revealToUser(); });
    m.addItem ("Close", [this, c]
    {
        auto close = [this, c] { lib.closeContainer (c); sources.selectRow (0); };
        if (c->dirty)
            confirm ("Close " + c->name, "This bank has unsaved changes. Close it anyway?", close);
        else
            close();
    });
    m.showMenuAsync ({});
}

//==============================================================================
void ManageComponent::rebuildColumns()
{
    auto& h = table.getHeader();
    h.removeAllColumns();
    const int flags = TableHeaderComponent::visible | TableHeaderComponent::resizable | TableHeaderComponent::sortable;
    sortColumn = 0;
    sortForwards = true;
    if (showingCabs())
    {
        h.addColumn ("Slot", colNumber, 70, 50, 90, flags);
        h.addColumn ("Name", colName, 380, 120, -1, flags);
        h.addColumn ("Status", colStatus, 120, 70, 160, flags);
    }
    else
    {
        h.addColumn ("#", colNumber, 50, 40, 70, flags);
        h.addColumn ("Name", colName, 220, 100, -1, flags);
        h.addColumn ("Amp 1", colAmp1, 120, 60, -1, flags);
        h.addColumn ("Amp 2", colAmp2, 120, 60, -1, flags);
        h.addColumn ("Blocks", colBlocks, 330, 100, -1, flags);
        h.addColumn ("Status", colStatus, 80, 60, 120, flags);
    }
}

void ManageComponent::refreshRows()
{
    visible.clear();
    if (showingCabs())
    {
        for (int i = 0; i < Librarian::numCabs; ++i)
            visible.add (i);
    }
    else if (auto* c = container())
    {
        const auto term = search.getText().trim();
        for (int i = 0; i < c->entries.size(); ++i)
            if (term.isEmpty() || c->entries.getReference (i).preset.name.containsIgnoreCase (term))
                visible.add (i);

        // Sorting only changes the view; slots keep their numbers (the original's "Sort By" menu).
        if (isSorted())
        {
            auto key = [this, c] (int i) -> String
            {
                auto& e = c->entries.getReference (i);
                switch (sortColumn)
                {
                    case colName:   return e.preset.name.trim().toLowerCase();
                    case colAmp1:   return ampName (e.preset, 106).toLowerCase();
                    case colAmp2:   return ampName (e.preset, 107).toLowerCase();
                    case colBlocks: return blocksUsed (e.preset).toLowerCase();
                    case colStatus: return String ((int) e.state);
                    default:        return String (i).paddedLeft ('0', 4);
                }
            };
            std::stable_sort (visible.begin(), visible.end(), [&] (int a, int b)
            {
                const int r = key (a).compareNatural (key (b));
                return sortForwards ? r < 0 : r > 0;
            });
        }
        else if (! sortForwards)
            std::reverse (visible.begin(), visible.end());
    }
    table.updateContent();
    table.repaint();
    sources.updateContent();
    sources.repaint();
    updateButtons();
}

void ManageComponent::changeListenerCallback (ChangeBroadcaster*)
{
    if (source >= numSources())
        sources.selectRow (0);
    refreshRows();
}

void ManageComponent::updateButtons()
{
    auto* c = container();
    const bool connected = lib.midi.isConnected(), busy = lib.isBusy();
    tool ("Save")->setEnabled (c != nullptr && c->kind == LibContainer::BankFile && c->dirty);
    tool ("Save As...")->setEnabled (c != nullptr && c->kind != LibContainer::PresetFolder);
    tool ("Export...")->setEnabled (c != nullptr && ! selectedEntries().isEmpty());
    tool ("Download")->setEnabled (connected && ! busy && c != nullptr && c->isHardware());
    tool ("Upload")->setEnabled (connected && ! busy && (showingCabs() || (c != nullptr && c->kind != LibContainer::PresetFolder)));
    tool ("Sync & Backup")->setEnabled (connected && ! busy);
    tool ("Undo")->setEnabled (c != nullptr && ! c->undoStack.isEmpty());
    tool ("Redo")->setEnabled (c != nullptr && ! c->redoStack.isEmpty());
    tool ("Cancel")->setEnabled (busy);

    progressBar.setVisible (busy);
    String text = lib.activity;
    if (text.isEmpty())
    {
        if (! connected)
            text = "Not connected: choose the Axe-Fx MIDI ports under Settings in the editor.";
        else if (c != nullptr && c->isHardware())
        {
            int unknown = 0, changed = 0;
            for (auto& e : c->entries)
            {
                unknown += e.state == LibEntry::Unknown ? 1 : 0;
                changed += e.state == LibEntry::Edited ? 1 : 0;
            }
            text = unknown == c->entries.size() ? "Not downloaded yet: use Download to read this bank from the Axe-Fx."
                 : changed > 0 ? String (changed) + " edited preset" + (changed == 1 ? "" : "s") + " not yet sent to the Axe-Fx."
                 : "In step with the Axe-Fx as of the last transfer.";
        }
    }
    status.setText (text, dontSendNotification);
}

//==============================================================================
Array<int> ManageComponent::selectedEntries() const
{
    Array<int> rows;
    const auto sel = table.getSelectedRows();
    for (int i = 0; i < sel.size(); ++i)
        if (isPositiveAndBelow (sel[i], visible.size()))
            rows.add (visible[sel[i]]);
    return rows;
}

int ManageComponent::firstSelected() const
{
    const auto rows = selectedEntries();
    return rows.isEmpty() ? -1 : rows.getFirst();
}

void ManageComponent::selectEntries (int first, int count)
{
    table.deselectAllRows();
    for (int i = 0; i < visible.size(); ++i)
        if (visible[i] >= first && visible[i] < first + count)
            table.selectRow (i, false, false);
}

String ManageComponent::ampName (const Preset& p, int effectId) const
{
    auto& d = lib.doc;
    const int typeParam = d.config.paramId ("Amp", "DISTORT_TYPE");
    const int v = p.getParam (effectId, typeParam);
    auto* info = d.profile.find ("Amp", typeParam);
    if (v < 0 || info == nullptr || ! p.isPlaced (effectId))
        return {};
    return info->valueOnly (v).trim();
}

String ManageComponent::blocksUsed (const Preset& p) const
{
    StringArray used;
    for (auto& cell : p.grid)
        if (cell.id != 0 && ! Preset::isShunt (cell.id))
        {
            const auto type = lib.doc.config.typeOf (cell.id);
            if (type.isNotEmpty() && type != "Dummy")
                used.addIfNotAlreadyThere (abbreviation (type));
        }
    return used.joinIntoString (" ");
}

void ManageComponent::paintRowBackground (Graphics& g, int row, int, int, bool selected)
{
    g.fillAll (selected ? accent.withAlpha (0.3f) : (row % 2 ? rowAlt : panel));
}

void ManageComponent::paintCell (Graphics& g, int row, int column, int width, int height, bool)
{
    if (! isPositiveAndBelow (row, visible.size()))
        return;
    const int index = visible[row];
    String text;
    Colour colour = textColour;
    LibEntry::State state = LibEntry::Unknown;

    if (showingCabs())
    {
        auto& cab = lib.cabs[(size_t) index];
        state = cab.state;
        if (column == colNumber)        text = "USER " + String (index + 1);
        else if (column == colName)     { text = cab.isEmpty() ? "(no cab loaded)" : cab.name; colour = cab.isEmpty() ? dimText : textColour; }
    }
    else if (auto* c = container())
    {
        auto& e = c->entries.getReference (index);
        state = c->isHardware() ? e.state : LibEntry::Synced;
        const bool unknown = c->isHardware() && e.state == LibEntry::Unknown;
        switch (column)
        {
            case colNumber: text = String (c->slotNumber (index)).paddedLeft ('0', 3); colour = dimText; break;
            case colName:   text = unknown ? "(not downloaded)" : e.preset.name; colour = unknown ? dimText : textColour; break;
            case colAmp1:   text = unknown ? String() : ampName (e.preset, 106); break;
            case colAmp2:   text = unknown ? String() : ampName (e.preset, 107); break;
            case colBlocks: text = unknown ? String() : blocksUsed (e.preset); colour = dimText; break;
            default: break;
        }
        if (! c->isHardware() && column == colStatus)
            return;
    }

    if (column == colStatus)
    {
        text = state == LibEntry::Synced ? "Synced" : state == LibEntry::Edited ? "Edited" : "Unknown";
        colour = state == LibEntry::Synced ? synced : state == LibEntry::Edited ? edited : dimText;
        if (showingCabs() && lib.cabs[(size_t) index].isEmpty())
            return;
    }

    g.setColour (colour);
    g.setFont (FontOptions (14.0f));
    g.drawText (text, 6, 0, width - 10, height, Justification::centredLeft, true);
}

String ManageComponent::getCellTooltip (int row, int column)
{
    if (column != colBlocks || showingCabs() || container() == nullptr || ! isPositiveAndBelow (row, visible.size()))
        return {};
    StringArray names;
    auto& p = container()->entries.getReference (visible[row]).preset;
    for (auto& cell : p.grid)
        if (cell.id != 0 && ! Preset::isShunt (cell.id))
            if (auto* item = lib.doc.config.poolItem (cell.id))
                names.addIfNotAlreadyThere (item->name);
    return names.joinIntoString (", ");
}

void ManageComponent::cellClicked (int row, int, const MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;
    if (! table.isRowSelected (row))
        table.selectRow (row);
    if (showingCabs())
        showCabMenu (row);
    else
        showRowMenu (row);
}

void ManageComponent::cellDoubleClicked (int row, int, const MouseEvent&)
{
    if (! isPositiveAndBelow (row, visible.size()))
        return;
    if (showingCabs())
        loadCabFile (visible[row]);
    else if (auto* c = container(); c != nullptr && lib.onEdit)
        lib.onEdit (*c, visible[row]);
}

void ManageComponent::backgroundClicked (const MouseEvent& e)
{
    table.deselectAllRows();
    if (e.mods.isPopupMenu() && ! showingCabs() && clipboard().size() > 0)
    {
        PopupMenu m;
        m.addItem ("Paste at End", container() != nullptr && container()->isEditable() && container()->entries.size() < Sysex::bankPresets, false,
                   [this] { lib.pasteInsert (*container(), container()->entries.size(), clipboard()); });
        m.showMenuAsync ({});
    }
}

void ManageComponent::deleteKeyPressed (int)
{
    if (showingCabs())
    {
        for (int i : selectedEntries())
            lib.clearCab (i);
    }
    else if (auto* c = container(); c != nullptr && c->isEditable())
        lib.clearRows (*c, selectedEntries());
}

void ManageComponent::returnKeyPressed (int)    { renameSelected(); }

var ManageComponent::getDragSourceDescription (const SparseSet<int>&)
{
    auto* c = container();
    if (c == nullptr || showingCabs())
        return {};
    dragRows = selectedEntries();
    dragFrom = c;
    return "lib-rows";
}

void ManageComponent::sortOrderChanged (int newSortColumnId, bool isForwards)
{
    sortColumn = newSortColumnId;
    sortForwards = isForwards;
    refreshRows();
}

void ManageComponent::dropRows (int targetRow)
{
    auto* to = container();
    if (to == nullptr || dragFrom == nullptr || dragRows.isEmpty() || ! lib.containers.contains (dragFrom))
        return;

    if (to == dragFrom && (isSorted() || ! sortForwards) && ! ModifierKeys::currentModifiers.isAltDown())
    {
        status.setText ("The list is sorted: click the # column to go back to slot order before moving presets.", dontSendNotification);
        return;
    }

    const int target = isPositiveAndBelow (targetRow, visible.size()) ? visible[targetRow] : to->entries.size();
    const auto mods = ModifierKeys::currentModifiers;

    if (to == dragFrom && ! mods.isAltDown())
    {
        if (mods.isShiftDown() && dragRows.size() == 1 && target < to->entries.size())
        {
            lib.swapRows (*to, dragRows[0], target);
            selectEntries (target, 1);
        }
        else
        {
            lib.moveRows (*to, dragRows, target);
            int before = 0;
            for (int r : dragRows)
                before += r < target ? 1 : 0;
            selectEntries (target - before, dragRows.size());
        }
    }
    else
    {
        Array<LibEntry> copy;
        for (int r : dragRows)
            copy.add (dragFrom->entries[r]);
        if (target >= to->entries.size() && to->entries.size() < Sysex::bankPresets)
            lib.pasteInsert (*to, to->entries.size(), copy);
        else
            lib.pasteOver (*to, jmin (target, to->entries.size() - 1), copy);
        selectEntries (target, copy.size());
    }
    dragRows.clear();
}

bool ManageComponent::canDropOnSource (int sourceRow) const
{
    if (sourceRow < 0 || sourceRow == cabsSource || sourceRow == source || dragFrom == nullptr)
        return false;
    auto* c = lib.containers[sourceRow < cabsSource ? sourceRow : sourceRow - 1];
    return c != nullptr && c->isEditable() && c->entries.size() == Sysex::bankPresets;
}

void ManageComponent::dropOnSource (int sourceRow)
{
    auto* to = lib.containers[sourceRow < cabsSource ? sourceRow : sourceRow - 1];
    if (to == nullptr || dragFrom == nullptr || dragRows.isEmpty() || ! lib.containers.contains (dragFrom))
        return;

    // Same slot numbers in the target bank (a preset folder's files go to their list positions).
    to->checkpoint();
    Array<int> changed;
    for (int r : dragRows)
    {
        const int slot = r % Sysex::bankPresets;
        to->entries.getReference (slot).preset = dragFrom->entries.getReference (r).preset;
        changed.add (slot);
    }
    lib.touched (*to, changed);
    status.setText (String (changed.size()) + " preset" + (changed.size() == 1 ? "" : "s") + " copied to " + to->name
                      + (to->isHardware() ? " (marked Edited; use Upload to send them)." : "."), dontSendNotification);
    dragRows.clear();
}

//==============================================================================
bool ManageComponent::keyPressed (const KeyPress& k)
{
    const bool cmd = k.getModifiers().isCommandDown();
    if (cmd && k.getKeyCode() == 'C')                               { copySelected (false); return true; }
    if (cmd && k.getKeyCode() == 'X')                               { copySelected (true); return true; }
    if (cmd && k.getKeyCode() == 'V')                               { paste (false); return true; }
    if (cmd && k.getKeyCode() == 'A')                               { table.selectRangeOfRows (0, table.getNumRows()); return true; }
    if (cmd && k.getKeyCode() == 'Z')                               { undoRedo (k.getModifiers().isShiftDown()); return true; }
    if (cmd && k.getKeyCode() == 'S')                               { saveCurrent (false); return true; }
    if (cmd && k.getKeyCode() == 'O')                               { openSomething(); return true; }
    return false;
}

void ManageComponent::copySelected (bool cut)
{
    auto* c = container();
    if (c == nullptr || showingCabs())
        return;
    clipboard().clear();
    const auto rows = selectedEntries();
    for (int r : rows)
        clipboard().add (c->entries[r]);
    if (cut && c->isEditable())
        lib.clearRows (*c, rows);
    status.setText (String (rows.size()) + " preset" + (rows.size() == 1 ? "" : "s") + (cut ? " cut." : " copied."), dontSendNotification);
}

void ManageComponent::paste (bool insert)
{
    auto* c = container();
    if (c == nullptr || ! c->isEditable() || clipboard().isEmpty())
        return;
    int row = firstSelected();
    if (row < 0)
        row = 0;
    if (insert)
        lib.pasteInsert (*c, row, clipboard());
    else
        lib.pasteOver (*c, row, clipboard());
    selectEntries (row, clipboard().size());
}

void ManageComponent::undoRedo (bool redo)
{
    if (auto* c = container())
    {
        if (redo ? c->redo() : c->undo())
        {
            if (c->isHardware())
                lib.saveCache();
            lib.sendChangeMessage();
        }
    }
}

void ManageComponent::renameSelected()
{
    const int row = firstSelected();
    if (row < 0)
        return;

    String current;
    if (showingCabs())
        current = lib.cabs[(size_t) row].name;
    else if (auto* c = container(); c != nullptr && c->isEditable())
        current = c->entries.getReference (row).preset.name;
    else
        return;

    auto* w = new AlertWindow ("Rename", showingCabs() ? "Cab name (kept in the librarian only)." : "Preset name, up to 32 characters.",
                               MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", current);
    w->addButton ("OK", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([this, w, row] (int r)
    {
        const auto name = w->getTextEditorContents ("name").trim();
        if (r != 1 || name.isEmpty())
            return;
        if (showingCabs())
        {
            lib.cabs[(size_t) row].name = name;
            lib.saveCache();
            lib.sendChangeMessage();
        }
        else if (auto* c = container())
            lib.rename (*c, row, name);
    }), true);
}

void ManageComponent::confirm (const String& title, const String& text, std::function<void()> action)
{
    AlertWindow::showOkCancelBox (MessageBoxIconType::WarningIcon, title, text, "Continue", "Cancel", this,
                                  ModalCallbackFunction::create ([action] (int r) { if (r == 1) action(); }));
}

//==============================================================================
void ManageComponent::openSomething()
{
    auto chooser = std::make_shared<FileChooser> ("Open a bank file, preset file or a folder of preset files",
                                                  File::getSpecialLocation (File::userDocumentsDirectory), "*.syx");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles
                            | FileBrowserComponent::canSelectDirectories | FileBrowserComponent::canSelectMultipleItems,
                          [this, chooser] (const FileChooser& fc)
    {
        for (auto& f : fc.getResults())
            openPath (f);
    });
}

void ManageComponent::openPath (const File& f)
{
    LibContainer* c = nullptr;
    String error;
    if (f.isDirectory())
        c = lib.openFolder (f);
    else
        c = lib.openFile (f, error);

    if (c == nullptr)
    {
        AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Can't open " + f.getFileName(), error);
        return;
    }
    const int index = lib.containers.indexOf (c);
    sources.selectRow (index < cabsSource ? index : index + 1);
}

void ManageComponent::filesDropped (const StringArray& files, int x, int y)
{
    if (showingCabs() && table.getBounds().contains (x, y))
    {
        const int row = table.getRowContainingPosition (x - table.getX(), y - table.getY());
        int slot = jmax (0, row);
        for (auto& path : files)
        {
            String error;
            if (slot < Librarian::numCabs && ! lib.loadCab (slot++, File (path), error))
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Can't load cab", error);
        }
        return;
    }
    for (auto& path : files)
        openPath (File (path));
}

void ManageComponent::saveCurrent (bool saveAs)
{
    auto* c = container();
    if (c == nullptr || c->kind == LibContainer::PresetFolder)
        return;

    auto doSave = [this, c] (const File& f)
    {
        String error;
        if (! lib.save (*c, f, error))
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Save failed", error);
        else
            status.setText ("Saved " + f.getFileName() + ".", dontSendNotification);
    };

    if (! saveAs && c->kind == LibContainer::BankFile && c->file != File())
    {
        doSave (c->file);
        return;
    }

    const auto suggested = c->isHardware() ? Librarian::backupsDir().getChildFile (AxeModel::name (lib.doc.model) + " " + c->name + ".syx")
                                           : (c->file != File() ? c->file : File::getSpecialLocation (File::userDocumentsDirectory)
                                                                              .getChildFile (c->name + ".syx"));
    suggested.getParentDirectory().createDirectory();
    auto chooser = std::make_shared<FileChooser> ("Save as bank file", suggested, "*.syx");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting,
                          [doSave, chooser] (const FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f != File())
            doSave (f.withFileExtension ("syx"));
    });
}

void ManageComponent::exportSelected (bool asCsv)
{
    auto* c = container();
    const auto rows = selectedEntries();
    if (c == nullptr || rows.isEmpty())
        return;

    auto chooser = std::make_shared<FileChooser> (asCsv ? "Choose a folder for the CSV files" : "Choose a folder for the preset files",
                                                  File::getSpecialLocation (File::userDocumentsDirectory));
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                          [this, c, rows, chooser, asCsv] (const FileChooser& fc)
    {
        const auto folder = fc.getResult();
        if (folder == File() || ! lib.containers.contains (c))
            return;
        String error;
        if (asCsv)
        {
            int written = 0;
            for (int r : rows)
            {
                auto& e = c->entries.getReference (r);
                if (e.state == LibEntry::Unknown) continue;
                const auto f = folder.getNonexistentChildFile (String (c->slotNumber (r)).paddedLeft ('0', 3) + " "
                                                                 + File::createLegalFileName (e.preset.name.trim()), ".csv", false);
                if (f.replaceWithText (Csv::exportPreset (lib.doc, e.preset), false, false, nullptr))
                    ++written;
            }
            status.setText ("Exported " + String (written) + " CSV file" + (written == 1 ? "" : "s") + " to " + folder.getFileName() + ".",
                            dontSendNotification);
            return;
        }
        if (lib.exportPresets (*c, rows, folder, error))
            status.setText ("Exported " + String (rows.size()) + " preset file" + (rows.size() == 1 ? "" : "s") + " to " + folder.getFileName() + ".",
                            dontSendNotification);
        else
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Export failed", error);
    });
}

//==============================================================================
void ManageComponent::showDownloadMenu()
{
    auto* c = container();
    if (c == nullptr || ! c->isHardware())
        return;

    PopupMenu m;
    const auto rows = selectedEntries();
    m.addItem ("Download " + c->name, [this] { lib.downloadBanks (mask()); });
    m.addItem ("Download All Banks", [this] { lib.downloadBanks (7); });
    m.addItem ("Download Selected Presets", ! rows.isEmpty(), false, [this, c, rows]
    {
        for (int r : rows)
            lib.enqueue ({ Librarian::Job::DownloadPreset, c->slotNumber (r) });
    });
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (tool ("Download")));
}

void ManageComponent::showUploadMenu()
{
    PopupMenu m;
    if (showingCabs())
    {
        const auto rows = selectedEntries();
        m.addItem ("Upload Changed Cabs", [this]
        {
            for (int s = 0; s < Librarian::numCabs; ++s)
                if (! lib.cabs[(size_t) s].isEmpty() && lib.cabs[(size_t) s].state != LibEntry::Synced)
                    lib.enqueue ({ Librarian::Job::UploadCab, s });
        });
        m.addItem ("Upload Selected Cabs", ! rows.isEmpty(), false, [this, rows]
        {
            for (int s : rows)
                if (! lib.cabs[(size_t) s].isEmpty())
                    lib.enqueue ({ Librarian::Job::UploadCab, s });
        });
        m.addItem ("Upload All Cabs", [this]
        {
            confirm ("Upload all cabs", "This overwrites every USER cab slot that has a cab loaded here.", [this]
            {
                for (int s = 0; s < Librarian::numCabs; ++s)
                    if (! lib.cabs[(size_t) s].isEmpty())
                        lib.enqueue ({ Librarian::Job::UploadCab, s });
            });
        });
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (tool ("Upload")));
        return;
    }

    auto* c = container();
    if (c == nullptr)
        return;
    const auto rows = selectedEntries();

    if (c->isHardware())
    {
        int unknown = 0;
        for (auto& e : c->entries)
            unknown += e.state == LibEntry::Unknown ? 1 : 0;

        m.addItem ("Upload Edited Presets", [this, c] { lib.uploadChanged (c->bank); });
        m.addItem ("Upload Selected Presets", ! rows.isEmpty(), false, [this, c, rows]
        {
            for (int r : rows)
                if (c->entries.getReference (r).state != LibEntry::Unknown)
                    lib.enqueue ({ Librarian::Job::UploadPreset, c->slotNumber (r) });
        });
        m.addItem ("Upload Whole " + c->name, [this, c, unknown]
        {
            confirm ("Upload " + c->name,
                     "This replaces all 128 presets in " + c->name + " on the Axe-Fx"
                       + (unknown > 0 ? ".\n\n" + String (unknown) + " presets in this bank were never downloaded and would be sent as empty presets. "
                                        "Download the bank first to keep them." : ".") + "\n\nIt takes about 1.5 minutes over 5-pin MIDI.",
                     [this, c] { lib.uploadBanks (1 << c->bank); });
        });
    }
    else if (c->entries.size() == Sysex::bankPresets)
    {
        for (int b = 0; b < Librarian::numBanks; ++b)
            m.addItem ("Send to Axe-Fx " + Librarian::bankName (b) + "...", [this, c, b]
            {
                confirm ("Send to " + Librarian::bankName (b),
                         "This replaces all 128 presets in " + Librarian::bankName (b) + " on the Axe-Fx with \"" + c->name
                           + "\". It takes about 1.5 minutes over 5-pin MIDI.", [this, c, b]
                {
                    auto* hw = lib.containers[b];
                    hw->checkpoint();
                    for (int i = 0; i < Sysex::bankPresets; ++i)
                        hw->entries.getReference (i).preset = c->entries.getReference (i).preset;
                    lib.touched (*hw, {});
                    for (auto& e : hw->entries)
                        e.state = LibEntry::Edited;
                    lib.uploadBanks (1 << b);
                });
            });
    }
    else
        m.addItem ("Copy presets into an Axe-Fx bank (drag them onto Bank A, B or C), then upload from there", false, false, std::function<void()> ([] {}));

    m.showMenuAsync (PopupMenu::Options().withTargetComponent (tool ("Upload")));
}

void ManageComponent::showBackupMenu()
{
    PopupMenu m;
    m.addItem ("Sync & Back Up All Banks", [this] { lib.syncAndBackup (7); });
    for (int b = 0; b < Librarian::numBanks; ++b)
        m.addItem ("Sync & Back Up " + Librarian::bankName (b), [this, b] { lib.syncAndBackup (1 << b); });
    m.addSeparator();
    m.addItem ("Show Backups Folder", []
    {
        Librarian::backupsDir().createDirectory();
        Librarian::backupsDir().revealToUser();
    });
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (tool ("Sync & Backup")));
}

void ManageComponent::showRowMenu (int row)
{
    auto* c = container();
    if (c == nullptr || ! isPositiveAndBelow (row, visible.size()))
        return;
    const int index = visible[row];
    const auto rows = selectedEntries();
    const bool editable = c->isEditable(), connected = lib.midi.isConnected() && ! lib.isBusy();
    const bool known = ! c->isHardware() || c->entries.getReference (index).state != LibEntry::Unknown;

    PopupMenu m;
    m.addItem ("Edit in Axe-Edit", rows.size() == 1 && known, false, [this, c, index] { if (lib.onEdit) lib.onEdit (*c, index); });
    m.addItem ("Audition (send to the edit buffer)", rows.size() == 1 && known && connected, false, [this, c, index]
    {
        lib.midi.sendPreset (c->entries.getReference (index).preset, true, 0);
        status.setText ("Sent \"" + c->entries.getReference (index).preset.name + "\" to the edit buffer.", dontSendNotification);
    });
    m.addItem ("Rename...", rows.size() == 1 && editable && known, false, [this] { renameSelected(); });
    m.addSeparator();
    m.addItem ("Copy", [this] { copySelected (false); });
    m.addItem ("Cut", editable, false, [this] { copySelected (true); });
    m.addItem ("Paste Over", editable && ! clipboard().isEmpty(), false, [this] { paste (false); });
    m.addItem ("Paste Insert", editable && ! clipboard().isEmpty(), false, [this] { paste (true); });
    m.addItem ("Clear", editable, false, [this, c, rows] { lib.clearRows (*c, rows); });
    if (c->isHardware())
    {
        m.addSeparator();
        m.addItem ("Download Selected", connected, false, [this, c, rows]
        {
            for (int r : rows)
                lib.enqueue ({ Librarian::Job::DownloadPreset, c->slotNumber (r) });
        });
        m.addItem ("Upload Selected", connected && known, false, [this, c, rows]
        {
            for (int r : rows)
                if (c->entries.getReference (r).state != LibEntry::Unknown)
                    lib.enqueue ({ Librarian::Job::UploadPreset, c->slotNumber (r) });
        });
    }
    m.addSeparator();
    m.addItem ("Export as Preset Files...", [this] { exportSelected(); });
    m.addItem ("Export as CSV...", [this] { exportSelected (true); });
    if (c->kind == LibContainer::PresetFolder && c->entries.getReference (index).file.existsAsFile())
        m.addItem ("Show in Finder", [c, index] { c->entries.getReference (index).file.revealToUser(); });
    m.showMenuAsync ({});
}

void ManageComponent::loadCabFile (int slot)
{
    auto chooser = std::make_shared<FileChooser> ("Load a user cab file for USER " + String (slot + 1),
                                                  File::getSpecialLocation (File::userDocumentsDirectory), "*.syx");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this, slot, chooser] (const FileChooser& fc)
    {
        const auto f = fc.getResult();
        String error;
        if (f != File() && ! lib.loadCab (slot, f, error))
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Can't load cab", error);
    });
}

void ManageComponent::showCabMenu (int row)
{
    if (! isPositiveAndBelow (row, visible.size()))
        return;
    const int slot = visible[row];
    const bool loaded = ! lib.cabs[(size_t) slot].isEmpty(), connected = lib.midi.isConnected() && ! lib.isBusy();

    PopupMenu m;
    m.addItem ("Load Cab File...", [this, slot] { loadCabFile (slot); });
    m.addItem ("Rename...", loaded, false, [this] { renameSelected(); });
    m.addItem ("Clear", loaded, false, [this, slot] { lib.clearCab (slot); });
    m.addSeparator();
    m.addItem ("Upload to USER " + String (slot + 1), loaded && connected, false, [this, slot]
    {
        lib.enqueue ({ Librarian::Job::UploadCab, slot });
    });
    m.addItem ("Audition (load into the current preset's cab)", loaded && connected, false, [this, slot]
    {
        lib.enqueue ({ Librarian::Job::AuditionCab, slot });
    });
    m.showMenuAsync ({});
}

//==============================================================================
ManageWindow::ManageWindow (Librarian& lib, PropertiesFile* s)
    : DocumentWindow ("Axe-Manage", Colour (0xff2b2b2b), DocumentWindow::allButtons), settings (s)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new ManageComponent (lib, s), false);
    setResizable (true, false);
    setResizeLimits (820, 420, 4000, 3000);
    const auto saved = settings != nullptr ? settings->getValue ("manageWindow") : String();
    if (saved.isNotEmpty())
        restoreWindowStateFromString (saved);
    else
        centreWithSize (1100, 680);
}

ManageWindow::~ManageWindow()
{
    if (settings != nullptr)
        settings->setValue ("manageWindow", getWindowStateAsString());
}
