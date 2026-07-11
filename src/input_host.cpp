#include "input_host.h"

#include "freertos/FreeRTOS.h"

#define INPUT_HOST_KEY_COUNT 128
#define INPUT_HOST_KEY_QUEUE_SIZE 32
#define INPUT_HOST_MOUSE_MAX_DELTA 640

typedef struct {
	int dx;
	int dy;
	int buttonDown;
} HostMouseState;

static portMUX_TYPE s_inputMux = portMUX_INITIALIZER_UNLOCKED;
static HostMouseState s_mouse;
static bool s_keyDown[INPUT_HOST_KEY_COUNT];
static InputHostKeyEvent s_keyQueue[INPUT_HOST_KEY_QUEUE_SIZE];
static int s_keyHead;
static int s_keyTail;

static int clampMouseDelta(int value) {
	if (value > INPUT_HOST_MOUSE_MAX_DELTA) {
		return INPUT_HOST_MOUSE_MAX_DELTA;
	}
	if (value < -INPUT_HOST_MOUSE_MAX_DELTA) {
		return -INPUT_HOST_MOUSE_MAX_DELTA;
	}
	return value;
}

static bool queueIsFull(void) {
	return ((s_keyHead + 1) % INPUT_HOST_KEY_QUEUE_SIZE) == s_keyTail;
}

static void dropOldestPressForRelease(void) {
	int i = s_keyTail;
	while (i != s_keyHead) {
		if (!s_keyQueue[i].isRelease) {
			int next = (i + 1) % INPUT_HOST_KEY_QUEUE_SIZE;
			while (next != s_keyHead) {
				s_keyQueue[i] = s_keyQueue[next];
				i = next;
				next = (next + 1) % INPUT_HOST_KEY_QUEUE_SIZE;
			}
			s_keyHead = (s_keyHead + INPUT_HOST_KEY_QUEUE_SIZE - 1) %
				INPUT_HOST_KEY_QUEUE_SIZE;
			return;
		}
		i = (i + 1) % INPUT_HOST_KEY_QUEUE_SIZE;
	}
}

static void enqueueKeyEvent(uint8_t scancode, bool isRelease) {
	if (queueIsFull()) {
		if (isRelease) {
			dropOldestPressForRelease();
		} else {
			return;
		}
	}
	if (queueIsFull()) {
		return;
	}
	s_keyQueue[s_keyHead].scancode = scancode;
	s_keyQueue[s_keyHead].isRelease = isRelease ? 1 : 0;
	s_keyHead = (s_keyHead + 1) % INPUT_HOST_KEY_QUEUE_SIZE;
}

void inputHostMouseMove(int dx, int dy, int buttonDown) {
	portENTER_CRITICAL(&s_inputMux);
	s_mouse.dx = clampMouseDelta(s_mouse.dx + dx);
	s_mouse.dy = clampMouseDelta(s_mouse.dy + dy);
	s_mouse.buttonDown = buttonDown ? 1 : 0;
	portEXIT_CRITICAL(&s_inputMux);
}

void inputHostDrainMouse(int *dx, int *dy, int *buttonDown) {
	portENTER_CRITICAL(&s_inputMux);
	*dx = s_mouse.dx;
	*dy = s_mouse.dy;
	*buttonDown = s_mouse.buttonDown;
	s_mouse.dx = 0;
	s_mouse.dy = 0;
	portEXIT_CRITICAL(&s_inputMux);
}

void inputHostReleaseMouse(void) {
	portENTER_CRITICAL(&s_inputMux);
	s_mouse.dx = 0;
	s_mouse.dy = 0;
	s_mouse.buttonDown = 0;
	portEXIT_CRITICAL(&s_inputMux);
}

void inputHostKey(uint8_t scancode, bool down) {
	if (scancode >= INPUT_HOST_KEY_COUNT) {
		return;
	}
	portENTER_CRITICAL(&s_inputMux);
	if (down) {
		// Browser key repeat arrives as repeated keydown packets. Preserve those
		// as Mac keydown events while still tracking the physical held state.
		s_keyDown[scancode] = true;
		enqueueKeyEvent(scancode, false);
	} else if (s_keyDown[scancode]) {
		s_keyDown[scancode] = down;
		enqueueKeyEvent(scancode, !down);
	}
	portEXIT_CRITICAL(&s_inputMux);
}

bool inputHostPopKeyEvent(InputHostKeyEvent *event) {
	bool found = false;
	portENTER_CRITICAL(&s_inputMux);
	if (s_keyHead != s_keyTail) {
		*event = s_keyQueue[s_keyTail];
		s_keyTail = (s_keyTail + 1) % INPUT_HOST_KEY_QUEUE_SIZE;
		found = true;
	}
	portEXIT_CRITICAL(&s_inputMux);
	return found;
}

void inputHostReleaseAllKeys(void) {
	portENTER_CRITICAL(&s_inputMux);
	for (uint8_t scancode = 0; scancode < INPUT_HOST_KEY_COUNT; ++scancode) {
		if (s_keyDown[scancode]) {
			s_keyDown[scancode] = false;
			enqueueKeyEvent(scancode, true);
		}
	}
	portEXIT_CRITICAL(&s_inputMux);
}

void inputHostReset(void) {
	portENTER_CRITICAL(&s_inputMux);
	s_mouse.dx = 0;
	s_mouse.dy = 0;
	s_mouse.buttonDown = 0;
	for (int i = 0; i < INPUT_HOST_KEY_COUNT; ++i) {
		s_keyDown[i] = false;
	}
	s_keyHead = 0;
	s_keyTail = 0;
	portEXIT_CRITICAL(&s_inputMux);
}
