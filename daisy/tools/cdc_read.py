#!/usr/bin/env -S uv run
"""Read the lex224 pedal's USB log directly over libusb.

The Seed's USB port (VID:PID 0483:5740) shows up in one of two ways:
- USB MIDI (the pedal, `daisy/src/lex224_pedal.cpp`): the log travels as BARC
  LOG SysEx messages (`F0 7D 42 01 12 <ASCII> F7`, `daisy/src/editor_protocol.h`),
  one line per second plus a line on every program load. Other SysEx (the
  editor's STATE and PAGE reports) is skipped. The kernel's MIDI driver is
  detached while reading and reattached afterwards, so close the editor (or any
  other program holding the MIDI port) first.
- USB CDC (`daisy/bench`): plain text lines.

This script talks to the device through `pyusb` (the `usb` package, with a
libusb backend such as `libusb-1.0`), so no cdc_acm or MIDI driver binding is
needed. It polls `usb.core.find()` until the device appears, and again whenever
it disappears, so it keeps reading across resets and reflashes (the port
re-enumerates each time) for as long as it is asked to run.

Usage:
    uv run daisy/tools/cdc_read.py [SECONDS]

SECONDS is how long to capture (default 30). Each output line is prefixed
with the seconds since the device was last (re)connected.
Requires `pyusb` (in the repo's uv environment: `uv sync` at the repo root)
and a working libusb backend.
"""
import sys
import time

import usb.core
import usb.util

# Bytes per USB-MIDI event packet by code index number (libDaisy usb_midi.cpp).
CIN_SIZE = (3, 3, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1)
LOG_HEADER = bytes([0xF0, 0x7D, 0x42, 0x01, 0x12])


def in_endpoint(intf):
    return next(e for e in intf if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)


def emit(t0: float, text: str) -> None:
    print(f'{time.time() - t0:7.2f}  {text}', flush=True)


def read_cdc(d, data_if, end: float) -> None:
    ep = in_endpoint(data_if)
    usb.util.claim_interface(d, data_if.bInterfaceNumber)
    d.ctrl_transfer(0x21, 0x22, 0x03, 0, None)  # SET_CONTROL_LINE_STATE: DTR | RTS
    t0 = time.time()
    buf = b''
    while time.time() < end:
        try:
            buf += bytes(ep.read(512, timeout=500))
        except usb.core.USBTimeoutError:
            continue
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            emit(t0, line.decode(errors='replace').rstrip())


def detach_drivers(d, cfg) -> list:
    """Detach the kernel driver from every interface that has one; returns their numbers."""
    detached = []
    for n in sorted({i.bInterfaceNumber for i in cfg}):
        try:
            if d.is_kernel_driver_active(n):
                d.detach_kernel_driver(n)
                detached.append(n)
        except NotImplementedError:  # backend without driver control (not Linux)
            pass
    return detached


def read_midi(d, cfg, ms_if, end: float) -> None:
    ep = in_endpoint(ms_if)
    # The audio driver owns both the control and the MIDI interface; detaching
    # one shuts its card down, so both are detached and reattached together.
    detached = detach_drivers(d, cfg)
    try:
        usb.util.claim_interface(d, ms_if.bInterfaceNumber)
        t0 = time.time()
        msg = None  # SysEx being assembled
        while time.time() < end:
            try:
                data = bytes(ep.read(512, timeout=500))
            except usb.core.USBTimeoutError:
                continue
            for i in range(0, len(data) - 3, 4):
                cin = data[i] & 0x0F
                if cin < 2:  # reserved codes, skipped like libDaisy does
                    continue
                for b in data[i + 1:i + 1 + CIN_SIZE[cin]]:
                    if b >= 0xF8:  # realtime
                        continue
                    if b == 0xF0:
                        msg = bytearray([b])
                    elif msg is not None and (b < 0x80 or b == 0xF7):
                        msg.append(b)
                        if b == 0xF7:
                            if msg.startswith(LOG_HEADER):
                                emit(t0, msg[len(LOG_HEADER):-1].decode('ascii', errors='replace'))
                            msg = None
                    else:  # other status byte: not SysEx
                        msg = None
    finally:
        try:
            usb.util.release_interface(d, ms_if.bInterfaceNumber)
        except usb.core.USBError:
            pass
        for n in detached:
            try:
                d.attach_kernel_driver(n)
            except usb.core.USBError:  # gone, or already claimed by the reattached control interface
                pass


def main() -> None:
    end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 30)
    while time.time() < end:
        d = usb.core.find(idVendor=0x0483, idProduct=0x5740)
        if d is None:
            time.sleep(0.2)
            continue
        try:
            cfg = d.get_active_configuration()
            data_if = next((i for i in cfg if i.bInterfaceClass == 0x0A), None)
            if data_if is not None:
                read_cdc(d, data_if, end)
                continue
            ms_if = next((i for i in cfg if i.bInterfaceClass == 0x01 and i.bInterfaceSubClass == 0x03), None)
            if ms_if is None:
                print('-- usb: no CDC data or MIDI streaming interface', flush=True)
                time.sleep(0.5)
                continue
            read_midi(d, cfg, ms_if, end)
        except usb.core.USBError as e:
            print(f'-- usb: {e}', flush=True)
            time.sleep(0.5)


if __name__ == '__main__':
    main()
