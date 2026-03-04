#!/usr/bin/env python3
"""
Mac Plus emulator UDP input client.
Sends mouse and keyboard events to the ESP32 emulator over WiFi.

Usage: python3 mac_input.py [host] [port]
  host  defaults to macplus.local
  port  defaults to 4444

Move mouse over the window to control the Mac cursor.
Click to send mouse clicks. Keys are mapped to Mac scancodes.
"""
import sys
import struct
import socket
import subprocess
import tkinter as tk

HOST = sys.argv[1] if len(sys.argv) > 1 else "macplus.local"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4444

MAC_W = 640
MAC_H = 480
MAX_STEP = 30  # max mouse ticks per update (1 tick = 1 pixel now)

# Mac M0110A scancodes
KEYMAP = {
    'a': 0x01, 'b': 0x0B, 'c': 0x08, 'd': 0x02, 'e': 0x0E,
    'f': 0x03, 'g': 0x05, 'h': 0x04, 'i': 0x22, 'j': 0x26,
    'k': 0x28, 'l': 0x25, 'm': 0x2E, 'n': 0x2D, 'o': 0x1F,
    'p': 0x23, 'q': 0x0C, 'r': 0x0F, 's': 0x00, 't': 0x11,
    'u': 0x20, 'v': 0x09, 'w': 0x0D, 'x': 0x07, 'y': 0x10,
    'z': 0x06,
    '1': 0x12, '2': 0x13, '3': 0x14, '4': 0x15, '5': 0x17,
    '6': 0x16, '7': 0x1A, '8': 0x1C, '9': 0x19, '0': 0x1D,
    'minus': 0x1B, 'equal': 0x18, 'bracketleft': 0x21,
    'bracketright': 0x1E, 'backslash': 0x2A, 'semicolon': 0x29,
    'apostrophe': 0x27, 'comma': 0x2B, 'period': 0x2F,
    'slash': 0x2C, 'grave': 0x32,
    'Return': 0x24, 'Tab': 0x30, 'space': 0x31,
    'BackSpace': 0x33, 'Delete': 0x33,
    'Shift_L': 0x38, 'Shift_R': 0x38,
    'Control_L': 0x36, 'Control_R': 0x36,
    'Alt_L': 0x3A, 'Alt_R': 0x3A,
    'Meta_L': 0x37, 'Meta_R': 0x37,
    'Super_L': 0x37, 'Super_R': 0x37,
    'Caps_Lock': 0x39,
    'Up': 0x4D, 'Down': 0x48, 'Left': 0x46, 'Right': 0x42,
}

# --- UDP transport ---

try:
    ADDR = socket.gethostbyname(HOST)
    print(f"Resolved {HOST} -> {ADDR}")
except socket.gaierror:
    ADDR = HOST

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
_use_helper = False
try:
    sock.sendto(b'\x00', (ADDR, PORT))
    print("Using direct UDP socket")
except OSError:
    sock.close()
    sock = None
    _use_helper = True
    print("Direct UDP blocked, using /usr/bin/python3 helper")
    _helper = subprocess.Popen(
        ['/usr/bin/python3', '-u', '-c',
         'import socket,sys\n'
         f'S=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)\n'
         f'A=("{ADDR}",{PORT})\n'
         'while True:\n'
         ' f=sys.stdin.buffer.read(8)\n'
         ' if not f:break\n'
         ' S.sendto(f[1:1+f[0]],A)\n'],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )


def _send(data):
    if _use_helper:
        try:
            frame = bytes([len(data)]) + data
            _helper.stdin.write(frame.ljust(8, b'\x00'))
            _helper.stdin.flush()
        except Exception:
            pass
    else:
        try:
            sock.sendto(data, (ADDR, PORT))
        except OSError:
            pass


def send_mouse(dx, dy, button):
    dx = max(-127, min(127, int(dx)))
    dy = max(-127, min(127, int(dy)))
    _send(struct.pack('<4b', ord('M'), dx, dy, button))


def send_key(scancode, up):
    cmd = ord('U') if up else ord('K')
    _send(struct.pack('<bB', cmd, scancode))


# --- GUI ---

class App:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title(f"Mac Input -> {HOST}:{PORT}")
        self.root.geometry("640x480")

        self.canvas = tk.Canvas(self.root, bg="#222222", highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.canvas.bind("<Configure>", lambda e: self.update_dot())

        # Cursor position in Mac pixel coords
        self.cur_x = 0.0
        self.cur_y = 0.0
        self.target_x = 0.0
        self.target_y = 0.0
        self.button_state = 0

        self.dot = self.canvas.create_oval(0, 0, 10, 10, fill="#00ff00", outline="")
        self.crossh = self.canvas.create_line(0, 0, 0, 0, fill="#004400", width=1)
        self.crossv = self.canvas.create_line(0, 0, 0, 0, fill="#004400", width=1)

        self.canvas.bind("<Motion>", self.on_motion)
        self.canvas.bind("<ButtonPress-1>", self.on_click)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.root.bind("<KeyPress>", self.on_key_press)
        self.root.bind("<KeyRelease>", self.on_key_release)

        self.root.after(100, self.home_cursor)

    def home_cursor(self):
        """Drive cursor to (0,0) so we know absolute position."""
        for _ in range(30):
            send_mouse(-127, -127, 0)
        self.cur_x = 0
        self.cur_y = 0
        self.target_x = 0
        self.target_y = 0
        print("Cursor homed to (0,0)")
        self.root.after(4, self.update)

    def canvas_to_mac(self, cx, cy):
        """Window position -> Mac pixel coords (normalized)."""
        cw = max(1, self.canvas.winfo_width())
        ch = max(1, self.canvas.winfo_height())
        mx = cx / cw * MAC_W
        my = cy / ch * MAC_H
        return max(0, min(MAC_W - 1, mx)), max(0, min(MAC_H - 1, my))

    def mac_to_canvas(self, mx, my):
        """Mac pixel coords -> window position."""
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        return mx / MAC_W * cw, my / MAC_H * ch

    def update_dot(self):
        cx, cy = self.mac_to_canvas(self.cur_x, self.cur_y)
        r = 5
        self.canvas.coords(self.dot, cx - r, cy - r, cx + r, cy + r)
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        self.canvas.coords(self.crossh, 0, cy, cw, cy)
        self.canvas.coords(self.crossv, cx, 0, cx, ch)

    def on_motion(self, event):
        self.target_x, self.target_y = self.canvas_to_mac(event.x, event.y)

    def update(self):
        dx = self.target_x - self.cur_x
        dy = self.target_y - self.cur_y

        if abs(dx) >= 0.5 or abs(dy) >= 0.5:
            sdx = max(-MAX_STEP, min(MAX_STEP, int(round(dx))))
            sdy = max(-MAX_STEP, min(MAX_STEP, int(round(dy))))
            if sdx != 0 or sdy != 0:
                send_mouse(sdx, sdy, self.button_state)
                self.cur_x = max(0, min(MAC_W - 1, self.cur_x + sdx))
                self.cur_y = max(0, min(MAC_H - 1, self.cur_y + sdy))

        self.update_dot()
        self.root.after(16, self.update)

    def on_click(self, event):
        self.button_state = 1
        send_mouse(0, 0, 1)

    def on_release(self, event):
        self.button_state = 0
        send_mouse(0, 0, 0)

    def on_key_press(self, event):
        sc = self._lookup(event)
        if sc is not None:
            send_key(sc, False)

    def on_key_release(self, event):
        sc = self._lookup(event)
        if sc is not None:
            send_key(sc, True)

    def _lookup(self, event):
        ks = event.keysym
        if ks in KEYMAP:
            return KEYMAP[ks]
        if ks.lower() in KEYMAP:
            return KEYMAP[ks.lower()]
        return None

    def run(self):
        print(f"Sending to {HOST}:{PORT}, screen {MAC_W}x{MAC_H}")
        self.root.mainloop()


if __name__ == "__main__":
    App().run()
