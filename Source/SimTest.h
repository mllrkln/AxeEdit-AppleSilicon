#pragma once
#include "Librarian.h"

// "--simtest <banks dir>": runs the librarian against a simulated Axe-Fx Ultra in a separate process.
void runSimTest (const File& banksDir, std::function<void (int failures)> done);

// "--axesim <banks dir>": runs only the simulated Axe-Fx Ultra on virtual MIDI ports until killed.
bool runSimulator (const File& banksDir);
