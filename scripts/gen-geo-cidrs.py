#!/usr/bin/env python3
"""Generate an aggregated IP range file from APNIC delegation statistics.

Usage: gen-geo-cidrs.py delegated-apnic-latest CN[,HK,...] > cn-cidrs.txt

The output is one `start-end` range per line (dotted quad for IPv4, compressed
IPv6 otherwise) after merging overlapping and adjacent allocations. Only
`allocated` and `assigned` records are used. The header records the source and
the country codes so the file is reproducible.
"""
import sys
import ipaddress
from datetime import date


def parse(path, cc):
    v4, v6 = [], []
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            parts = line.rstrip('\n').split('|')
            if len(parts) < 7 or parts[0] != 'apnic':
                continue
            if parts[1] not in cc or parts[5] in ('summary', 'version'):
                continue
            status = parts[6] if len(parts) > 6 else ''
            if status not in ('allocated', 'assigned'):
                continue
            start, value = parts[3], parts[4]
            if parts[2] == 'ipv4':
                first = int(ipaddress.IPv4Address(start))
                count = int(value)
                v4.append([first, first + count - 1])
            elif parts[2] == 'ipv6':
                first = int(ipaddress.IPv6Address(start))
                last = first + (1 << (128 - int(value))) - 1
                v6.append([first, last])
    return v4, v6


def merge(ranges):
    ranges.sort()
    out = []
    for lo, hi in ranges:
        if out and lo <= out[-1][1] + 1:
            if hi > out[-1][1]:
                out[-1][1] = hi
        else:
            out.append([lo, hi])
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    cc = set(sys.argv[2].upper().split(','))
    v4, v6 = parse(sys.argv[1], cc)
    v4, v6 = merge(v4), merge(v6)
    print('# APNIC delegation statistics: https://ftp.apnic.net/apnic/stats/apnic/delegated-apnic-latest')
    print('# countries: %s  generated: %s  (reproduce with scripts/gen-geo-cidrs.py)' %
          (','.join(sorted(cc)), date.today().isoformat()))
    print('# ranges: ipv4=%d ipv6=%d' % (len(v4), len(v6)))
    for lo, hi in v4:
        print('%s-%s' % (ipaddress.IPv4Address(lo), ipaddress.IPv4Address(hi)))
    for lo, hi in v6:
        print('%s-%s' % (ipaddress.IPv6Address(lo), ipaddress.IPv6Address(hi)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
