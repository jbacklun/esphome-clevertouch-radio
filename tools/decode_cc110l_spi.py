#!/usr/bin/env python3
"""Decode a Touch E3's MCU <-> CC110L radio SPI traffic from a PulseView .sr capture.

    python decode_cc110l_spi.py capture.sr [--hide-poll] [--packets]

Default channel map (FX2 analyzer, PulseView D-numbers; change with --cs etc.):
  D2 SCLK, D5 CSn, D0 SI (MOSI), D3 SO (MISO), D4 GDO0.
SPI is bit-banged at ~8 kHz, mode 0; the last clock of a byte can be only
0.7 us high, so capture at >= 4 MHz.

Packets (CC110L variable length, first byte = length): the last two bytes
are a CRC-16/MODBUS (little-endian) over everything after the length byte.
--packets prints only the radio packets, with the CRC checked.
"""
import argparse
import re
import zipfile

import numpy as np

REG = ['IOCFG2', 'IOCFG1', 'IOCFG0', 'FIFOTHR', 'SYNC1', 'SYNC0', 'PKTLEN', 'PKTCTRL1', 'PKTCTRL0', 'ADDR',
       'CHANNR', 'FSCTRL1', 'FSCTRL0', 'FREQ2', 'FREQ1', 'FREQ0', 'MDMCFG4', 'MDMCFG3', 'MDMCFG2', 'MDMCFG1',
       'MDMCFG0', 'DEVIATN', 'MCSM2', 'MCSM1', 'MCSM0', 'FOCCFG', 'BSCFG', 'AGCCTRL2', 'AGCCTRL1', 'AGCCTRL0',
       'WOREVT1', 'WOREVT0', 'WORCTRL', 'FREND1', 'FREND0', 'FSCAL3', 'FSCAL2', 'FSCAL1', 'FSCAL0', 'RCCTRL1',
       'RCCTRL0', 'FSTEST', 'PTEST', 'AGCTEST', 'TEST2', 'TEST1', 'TEST0', '0x2F']
STROBE = {0x30: 'SRES', 0x31: 'SFSTXON', 0x32: 'SXOFF', 0x33: 'SCAL', 0x34: 'SRX', 0x35: 'STX', 0x36: 'SIDLE',
          0x37: 'SAFC', 0x38: 'SWOR', 0x39: 'SPWD', 0x3A: 'SFRX', 0x3B: 'SFTX', 0x3C: 'SWORRST', 0x3D: 'SNOP'}
STATUS = {0x30: 'PARTNUM', 0x31: 'VERSION', 0x32: 'FREQEST', 0x33: 'LQI', 0x34: 'RSSI', 0x35: 'MARCSTATE',
          0x36: 'WORTIME1', 0x37: 'WORTIME0', 0x38: 'PKTSTATUS', 0x39: 'VCO_VC_DAC', 0x3A: 'TXBYTES',
          0x3B: 'RXBYTES', 0x3C: 'RCCTRL1_STATUS', 0x3D: 'RCCTRL0_STATUS'}
STATE = ['IDLE', 'RX', 'TX', 'FSTXON', 'CALIB', 'SETTLE', 'RXOVF', 'TXUNF']


def crc16_modbus(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def load_sr(path):
    """Return (idx, val, rate): the sample index and new value of every level change.
    Streams the chunks, so long captures don't need the whole file in memory."""
    z = zipfile.ZipFile(path)
    meta = z.read('metadata').decode()
    num, unit = re.search(r'samplerate=([\d.]+) (\w?)Hz', meta).groups()
    rate = float(num) * {'M': 1e6, 'k': 1e3, '': 1}[unit]
    names = sorted((n for n in z.namelist() if n.startswith('logic-1-')), key=lambda n: int(n.split('-')[-1]))
    idx, val, base, last = [np.zeros(1, np.int64)], [], 0, None
    for n in names:
        c = np.frombuffer(z.read(n), np.uint8)
        if last is None:
            val.append(c[:1])
        else:
            c = np.concatenate([[last], c])
            base -= 1
        ch = np.flatnonzero(c[1:] != c[:-1]) + 1
        idx.append(ch + base)
        val.append(c[ch])
        base += len(c)
        last = c[-1]
    return np.concatenate(idx), np.concatenate(val), rate


def transactions(idx, val, a):
    bit = lambda ch: (val >> ch) & 1
    cs, clk, si, so = bit(a.cs), bit(a.clk), bit(a.si), bit(a.so)
    edge = lambda b, frm: idx[1:][(b[:-1] == frm) & (b[1:] != frm)]
    falls, rises_cs, rises = edge(cs, 1), edge(cs, 0), edge(clk, 0)
    at_sample = lambda b, t: b[np.searchsorted(idx, t, 'right') - 1]
    for f in falls:
        r = rises_cs[np.searchsorted(rises_cs, f, 'right'):][:1]
        end = r[0] if len(r) else idx[-1] + 1
        e = rises[np.searchsorted(rises, f, 'right'):np.searchsorted(rises, end)]
        n = len(e) // 8
        to = lambda bits: [int(''.join(map(str, bits[k * 8:k * 8 + 8])), 2) for k in range(n)]
        yield f, to(at_sample(si, e + 1)), to(at_sample(so, e + 1)), len(e) % 8


def describe(mosi, miso):
    h, st = mosi[0], miso[0]
    a, rd, burst = h & 0x3F, bool(h & 0x80), bool(h & 0x40)
    stat = f'[{STATE[(st >> 4) & 7]} f{st & 15}{" !RDY" if st & 0x80 else ""}]'
    data = miso[1:] if rd else mosi[1:]
    hx = ' '.join(f'{b:02x}' for b in data)
    if a == 0x3F:
        return stat, ('RX FIFO read ' if rd else 'TX FIFO write ') + hx
    if a == 0x3E:
        return stat, ('PATABLE read ' if rd else 'PATABLE write ') + hx
    if a >= 0x30 and len(mosi) == 1:
        return stat, STROBE.get(a, hex(a))
    if a >= 0x30 and burst:
        return stat, f'{STATUS.get(a, hex(a))} = {hx}'
    regs = ', '.join(f'{REG[min(a + i, 47)]}={v:02x}' for i, v in enumerate(data))
    return stat, ('read ' if rd else 'write ') + (regs or REG[a] + ' (no data)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sr')
    ap.add_argument('--cs', type=int, default=5)
    ap.add_argument('--clk', type=int, default=2)
    ap.add_argument('--si', type=int, default=0)
    ap.add_argument('--so', type=int, default=3)
    ap.add_argument('--hide-poll', action='store_true', help='skip SNOP status polls')
    ap.add_argument('--packets', action='store_true', help='only radio packets, CRC checked')
    a = ap.parse_args()
    idx, val, rate = load_sr(a.sr)
    rx = []  # RX FIFO reads are split over several CS frames; join them by length byte
    for t, mosi, miso, extra in transactions(idx, val, a):
        ms = t / rate * 1e3
        if not mosi:
            continue
        addr = mosi[0] & 0x3F
        if a.packets:
            if addr == 0x3F and not mosi[0] & 0x80:
                pkt = mosi[1:]
                ok = crc16_modbus(pkt[1:-2]) == pkt[-2] | pkt[-1] << 8
                print(f'{ms:10.1f} ms  E3 -> {" ".join(f"{b:02x}" for b in pkt)}  crc {"ok" if ok else "BAD"}')
            elif addr == 0x3F:
                rx += miso[1:]
                if len(rx) > rx[0]:
                    pkt, rx = rx[:rx[0] + 1], []
                    ok = crc16_modbus(pkt[1:-2]) == pkt[-2] | pkt[-1] << 8
                    print(f'{ms:10.1f} ms  E3 <- {" ".join(f"{b:02x}" for b in pkt)}  crc {"ok" if ok else "BAD"}')
            elif addr == 0x3A:  # SFRX: anything half-read is dropped
                rx = []
            continue
        if a.hide_poll and len(mosi) == 1 and addr == 0x3D:
            continue
        stat, what = describe(mosi, miso)
        print(f'{ms:10.3f} ms  {stat} {what}' + (f'  (+{extra} stray clocks)' if extra else ''))


if __name__ == '__main__':
    main()
