/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */
#include "mouse.h"
#include "input_host.h"
#include "scc.h"
#include "via.h"

typedef struct {
	// Pending mouse movement plus the current quadrature phase state.
	int dx, dy;
	int btn;
	int rpx, rpy;
} Mouse;

static const int quad[4]={0x0, 0x1, 0x3, 0x2};

#define MOUSE_QXA (1<<0)
#define MOUSE_QXB (1<<1)
#define MOUSE_QYA (1<<2)
#define MOUSE_QYB (1<<3)
#define MOUSE_BTN (1<<4)
#define MAXCDX 640

// Mouse state is owned by the emulation loop. Host input is drained from
// input_host at slice boundaries.
Mouse mouse;
static int lastMouseLines = -1;

// Reset accumulated motion, quadrature phase, and button state.
void mouseReset(void) {
	mouse.dx = 0;
	mouse.dy = 0;
	mouse.btn = 0;
	mouse.rpx = 0;
	mouse.rpy = 0;
	lastMouseLines = -1;
}

static void mousePresentLines(int lines) {
	if (lines & MOUSE_BTN) {
		viaClear(VIA_PORTB, (1 << 3));
	} else {
		viaSet(VIA_PORTB, (1 << 3));
	}
	if (lines & MOUSE_QXB) {
		viaClear(VIA_PORTB, (1 << 4));
	} else {
		viaSet(VIA_PORTB, (1 << 4));
	}
	if (lines & MOUSE_QYB) {
		viaClear(VIA_PORTB, (1 << 5));
	} else {
		viaSet(VIA_PORTB, (1 << 5));
	}
	sccSetDcd(SCC_CHANA, lines & MOUSE_QXA);
	sccSetDcd(SCC_CHANB, lines & MOUSE_QYA);
}

// Advance the mouse model by one emulation tick and present it to VIA/SCC.
// Steps:
// 1. consume at most one unit of pending X/Y motion,
// 2. update the quadrature phase counters,
// 3. update the VIA/SCC lines seen by the Mac when they changed.
void mouseTick(void) {
	int lines=0;
	int hostDx = 0;
	int hostDy = 0;
	int hostBtn = 0;

	inputHostDrainMouse(&hostDx, &hostDy, &hostBtn);
	mouse.dx += hostDx * 2;
	mouse.dy += hostDy * 2;
	if (mouse.dx > MAXCDX) mouse.dx = MAXCDX;
	if (mouse.dy > MAXCDX) mouse.dy = MAXCDX;
	if (mouse.dx < -MAXCDX) mouse.dx = -MAXCDX;
	if (mouse.dy < -MAXCDX) mouse.dy = -MAXCDX;
	mouse.btn = hostBtn ? 1 : 0;

	if (mouse.dx>0) {
		mouse.dx--;
		mouse.rpx--;
	}
	if (mouse.dx<0) {
		mouse.dx++;
		mouse.rpx++;
	}
	if (mouse.dy>0) {
		mouse.dy--;
		mouse.rpy++;
	}
	if (mouse.dy<0) {
		mouse.dy++;
		mouse.rpy--;
	}
	lines=quad[mouse.rpx&3];
	lines|=quad[mouse.rpy&3]<<2;
	lines|=mouse.btn<<4;

	if (lines != lastMouseLines) {
		mousePresentLines(lines);
		lastMouseLines = lines;
	}
}
