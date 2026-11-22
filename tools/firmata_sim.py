#!/usr/bin/env python3
"""tools/firmata_sim.py — QEMU の usb-serial につなぐ、Firmata のボードの模擬（Arduino Uno の形）

QEMU を -chardev socket,path=SOCK,server=on で起こし、このプログラムが SOCK につなぐ。
本物の Arduino（StandardFirmata を書き込んだもの）の代わりに、同じ通信文で答える。

ピン: 0〜13 デジタル、14〜19 アナログ A0〜A5。PWM は 3・5・6・9・10・11（8 ビット）、サーボは 2〜13。
試験のための配線（本物なら線でつなぐところ）:
  ピン 13（出力）→ ピン 2（入力）       デジタルの折り返し
  ピン 3 の PWM  → A1                   PWM の値を電圧として読む（0〜255 → 0〜1023）
  サーボ 9 の角度 → A2                   角度を読む（0〜180 度 → 0〜1023）
  A0 は 600 で一定（つまみの代わり）
使い方: python3 tools/firmata_sim.py build/firmata.sock   （QEMU が閉じたら終わる）
"""
import os
import select
import socket
import sys
import time

NPIN = 20
PWM_PINS = (3, 5, 6, 9, 10, 11)
log = sys.stderr if os.environ.get("FIRMATA_SIM_LOG") else None


def say(*a):
    if log:
        print("[firmata-sim]", *a, file=log, flush=True)


def connect(path):
    end = time.time() + 120
    while time.time() < end:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(path)
            return s
        except OSError:
            s.close()
            time.sleep(0.2)
    return None


class Board:
    def __init__(self, sock):
        self.s = sock
        self.mode = [None] * NPIN
        self.out_port = [0, 0, 0]
        self.pwm = [0] * NPIN
        self.servo = [0] * NPIN
        self.rep_analog = set()
        self.rep_digital = set()
        self.last_port = {}
        self.interval = 0.019
        self.buf = []
        self.sysex = None

    def send(self, b):
        try:
            self.s.sendall(bytes(b))
        except OSError:
            raise SystemExit(0)

    def analog_value(self, ch):
        if ch == 0:
            return 600
        if ch == 1:
            return self.pwm[3] * 1023 // 255
        if ch == 2:
            return min(180, self.servo[9]) * 1023 // 180
        return 0

    def port_value(self, port):
        v = 0
        for bit in range(8):
            pin = port * 8 + bit
            if pin >= NPIN:
                break
            if self.mode[pin] == 0:
                hi = (self.out_port[1] >> 5) & 1 if pin == 2 else 0     # ピン 13 → ピン 2
                v |= hi << bit
            elif self.mode[pin] == 1:
                v |= ((self.out_port[port] >> bit) & 1) << bit
        return v

    def report_ports(self, force=False):
        for port in sorted(self.rep_digital):
            v = self.port_value(port)
            if force or self.last_port.get(port) != v:
                self.last_port[port] = v
                self.send([0x90 | port, v & 0x7F, (v >> 7) & 1])

    def handle_sysex(self, m):
        if not m:
            return
        cmd = m[0]
        if cmd == 0x79:                                         # ファームウェア
            name = "StandardFirmata (Tamazusa sim)"
            out = [0xF0, 0x79, 2, 5]
            for ch in name:
                out += [ord(ch) & 0x7F, ord(ch) >> 7]
            self.send(out + [0xF7])
        elif cmd == 0x6B:                                       # できること
            out = [0xF0, 0x6C]
            for pin in range(NPIN):
                out += [0, 1, 1, 1]                             # 入力・出力
                if pin >= 14:
                    out += [2, 10]                              # アナログ 10 ビット
                if pin in PWM_PINS:
                    out += [3, 8]                               # PWM 8 ビット
                if 2 <= pin <= 13:
                    out += [4, 14]                              # サーボ
                out.append(0x7F)
            self.send(out + [0xF7])
        elif cmd == 0x69:                                       # アナログの対応表
            self.send([0xF0, 0x6A] + [(p - 14) if p >= 14 else 0x7F for p in range(NPIN)] + [0xF7])
        elif cmd == 0x7A and len(m) >= 3:                       # 報告の間隔
            self.interval = max(0.005, (m[1] | (m[2] << 7)) / 1000.0)
        elif cmd == 0x6F and len(m) >= 3:                       # 広いアナログの書き込み
            v = 0
            for i, b in enumerate(m[2:]):
                v |= b << (7 * i)
            self.analog_write(m[1], v)

    def analog_write(self, pin, v):
        if pin < NPIN:
            if self.mode[pin] == 3:
                self.pwm[pin] = v
            elif self.mode[pin] == 4:
                self.servo[pin] = v
            say("analog", pin, v)

    def feed(self, data):
        for c in data:
            if self.sysex is not None:
                if c == 0xF7:
                    self.handle_sysex(self.sysex)
                    self.sysex = None
                else:
                    self.sysex.append(c)
                continue
            if c & 0x80:
                if c == 0xF0:
                    self.sysex = []
                    self.buf = []
                elif c == 0xF9:
                    self.send([0xF9, 2, 5])
                    self.buf = []
                else:
                    self.buf = [c]
                continue
            if not self.buf:
                continue
            self.buf.append(c)
            cmd = self.buf[0]
            two = (cmd & 0xF0) in (0xC0, 0xD0)                      # 2 バイトの通信文
            if len(self.buf) < (2 if two else 3):
                continue
            hi = cmd & 0xF0
            if hi == 0x90:                                      # デジタルの書き込み（ポート）
                port = cmd & 15
                if port < 3:
                    self.out_port[port] = self.buf[1] | (self.buf[2] << 7)
                say("port", port, self.out_port[port] if port < 3 else 0)
            elif hi == 0xE0:
                self.analog_write(cmd & 15, self.buf[1] | (self.buf[2] << 7))
            elif hi == 0xC0:                                    # アナログの報告を始める・やめる
                (self.rep_analog.add if self.buf[1] else self.rep_analog.discard)(cmd & 15)
            elif hi == 0xD0:                                    # デジタルの報告
                port = cmd & 15
                if self.buf[1]:
                    self.rep_digital.add(port)
                    self.report_ports(force=True)
                else:
                    self.rep_digital.discard(port)
            elif cmd == 0xF4:                                   # ピンの向き
                pin, mode = self.buf[1], self.buf[2]
                if pin < NPIN:
                    self.mode[pin] = mode
                    say("mode", pin, mode)
            self.buf = []

    def run(self):
        next_report = time.time()
        while True:
            now = time.time()
            r, _, _ = select.select([self.s], [], [], max(0, next_report - now))
            if r:
                try:
                    data = self.s.recv(4096)
                except OSError:
                    return
                if not data:
                    return
                self.feed(data)
            if time.time() >= next_report:
                next_report = time.time() + self.interval
                for ch in sorted(self.rep_analog):
                    v = self.analog_value(ch)
                    self.send([0xE0 | ch, v & 0x7F, (v >> 7) & 0x7F])
                self.report_ports()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    s = connect(sys.argv[1])
    if s is None:
        return 1
    say("connected")
    Board(s).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
