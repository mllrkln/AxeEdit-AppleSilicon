// Axe-Manage window: sources on the left (the unit's banks, user cabs, files and folders),
// the selected source's presets or cabs on the right.
#pragma once
#include "Librarian.h"

class ManageComponent;

class PresetTable : public TableListBox, public DragAndDropTarget
{
public:
    explicit PresetTable (ManageComponent& o);
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    void paintOverChildren (Graphics&) override;
    int dropRow = -1;
private:
    ManageComponent& owner;
    int rowAt (Point<int>) const;
};

// The source list accepts presets dragged from the table: they land in the same slot numbers.
class SourceList : public ListBox, public ListBoxModel, public DragAndDropTarget
{
public:
    SourceList (ManageComponent& o);
    int getNumRows() override;
    void paintListBoxItem (int row, Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void listBoxItemClicked (int row, const MouseEvent&) override;
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    void paintOverChildren (Graphics&) override;
private:
    ManageComponent& owner;
    int hoverRow = -1;
};

class ManageComponent : public Component,
                        public ChangeListener,
                        public TableListBoxModel,
                        public DragAndDropContainer,
                        public FileDragAndDropTarget
{
public:
    ManageComponent (Librarian&, PropertiesFile*);
    ~ManageComponent() override;

    void resized() override;
    void paint (Graphics&) override;
    bool keyPressed (const KeyPress&) override;
    void changeListenerCallback (ChangeBroadcaster*) override;

    // sources (SourceList forwards here)
    int numSources() const              { return lib.containers.size() + 1; }
    void paintSource (int row, Graphics&, int width, int height, bool selected);
    void sourceSelected (int row);
    void sourceClicked (int row, const MouseEvent&);

    // presets / cabs
    int getNumRows() override           { return visible.size(); }
    void paintRowBackground (Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellClicked (int row, int column, const MouseEvent&) override;
    void cellDoubleClicked (int row, int column, const MouseEvent&) override;
    void backgroundClicked (const MouseEvent&) override;
    void deleteKeyPressed (int) override;
    void returnKeyPressed (int) override;
    var getDragSourceDescription (const SparseSet<int>&) override;
    void sortOrderChanged (int newSortColumnId, bool isForwards) override;
    String getCellTooltip (int row, int column) override;

    bool isInterestedInFileDrag (const StringArray&) override     { return true; }
    void filesDropped (const StringArray&, int x, int y) override;

    void dropRows (int targetRow);       // internal drag finished over the table
    bool canDropOnSource (int sourceRow) const;
    void dropOnSource (int sourceRow);

private:
    friend class PresetTable;
    friend class SourceList;
    Librarian& lib;
    PropertiesFile* settings;
    LookAndFeel_V4 laf;

    SourceList sources { *this };
    PresetTable table { *this };
    TextEditor search;
    Label status;
    ProgressBar progressBar { lib.progress };
    OwnedArray<TextButton> tools;
    TextButton* tool (const String& name) const;
    std::map<String, Image> icons;
    Image icon (const String& name);

    int source = 0;                      // 0..2 banks, 3 = user cabs, 4.. = containers[source - 1]
    enum { cabsSource = 3 };
    bool showingCabs() const             { return source == cabsSource; }
    LibContainer* container() const;
    Array<int> visible;                  // table row -> entry index (search filter)
    Array<int> dragRows;
    LibContainer* dragFrom = nullptr;
    static Array<LibEntry>& clipboard();

    void rebuildColumns();
    void refreshRows();
    void updateButtons();
    Array<int> selectedEntries() const;
    int firstSelected() const;
    void selectEntries (int first, int count);

    String ampName (const Preset&, int effectId) const;
    String blocksUsed (const Preset&) const;

    // actions
    void openSomething();
    void openPath (const File&);
    void saveCurrent (bool saveAs);
    void exportSelected (bool asCsv = false);
    int sortColumn = 0;                  // 0 / number column = slot order
    bool sortForwards = true;
    bool isSorted() const                { return sortColumn > 1; }
    void showDownloadMenu();
    void showUploadMenu();
    void showBackupMenu();
    void showRowMenu (int row);
    void showCabMenu (int row);
    void renameSelected();
    void copySelected (bool cut);
    void paste (bool insert);
    void undoRedo (bool redo);
    void loadCabFile (int slot);
    void confirm (const String& title, const String& text, std::function<void()> action);
    int mask() const                     { return 1 << source; }
};

class ManageWindow : public DocumentWindow
{
public:
    ManageWindow (Librarian&, PropertiesFile*);
    ~ManageWindow() override;
    void closeButtonPressed() override   { setVisible (false); }
private:
    PropertiesFile* settings;
};
