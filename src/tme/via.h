
#include <stdint.h>

// VIA input line selectors used by the Mac Plus model.
#define VIA_CA1 0
#define VIA_CA2 1
#define VIA_CB1 2
#define VIA_CB2 3

// VIA port selectors used by helper callbacks.
#define VIA_PORTA 0
#define VIA_PORTB 1

// Read and write the emulated 6522 register window.
void viaWrite(unsigned int addr, unsigned int val);
unsigned int viaRead(unsigned int addr);
// Reset VIA state and clear queued keyboard activity.
void viaInit(void);
// Inject an edge-triggered change on one of the VIA control inputs.
void viaControlWrite(int no, int val);
// Advance timers, shift-register state, and pending IRQ bookkeeping.
void viaStep(int clockcycles);
// Set or clear bits on the current VIA input latch.
void viaSet(int no, int mask);
void viaClear(int no, int mask);
