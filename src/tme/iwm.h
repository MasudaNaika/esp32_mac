// Handle one IWM register write from the Mac address space.
void iwmWrite(unsigned int addr, unsigned int val);
// Handle one IWM register read from the Mac address space.
unsigned int iwmRead(unsigned int addr);
// Reset the minimal IWM model.
void iwmInit(void);
// Update the VIA-driven head-select signal observed by the IWM model.
void iwmSetHeadSel(int s);
