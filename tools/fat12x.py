#!/usr/bin/env python3
"""Extract all files from a FAT12 floppy image into a directory (flat or tree)."""
import sys, os, struct

def read_bpb(d):
    bps, spc = struct.unpack_from('<HB', d, 11)
    rsvd, nfats, nroot, totsec16 = struct.unpack_from('<HBHH', d, 14)
    spf = struct.unpack_from('<H', d, 22)[0]
    return dict(bps=bps, spc=spc, rsvd=rsvd, nfats=nfats, nroot=nroot, spf=spf)

def fat12_entry(fat, n):
    off = n * 3 // 2
    v = fat[off] | (fat[off+1] << 8)
    return (v >> 4) if n & 1 else (v & 0xFFF)

def extract(img_path, out_dir):
    d = open(img_path, 'rb').read()
    b = read_bpb(d)
    bps = b['bps']
    fat_off = b['rsvd'] * bps
    fat = d[fat_off:fat_off + b['spf'] * bps]
    root_off = (b['rsvd'] + b['nfats'] * b['spf']) * bps
    data_off = root_off + b['nroot'] * 32
    cluster_size = b['spc'] * bps

    def read_chain(start, size=None):
        out = bytearray(); c = start
        while 2 <= c < 0xFF8:
            off = data_off + (c - 2) * cluster_size
            out += d[off:off + cluster_size]
            c = fat12_entry(fat, c)
        return bytes(out[:size]) if size is not None else bytes(out)

    def parse_dir(raw, path):
        for i in range(0, len(raw), 32):
            e = raw[i:i+32]
            if len(e) < 32 or e[0] == 0: break
            if e[0] == 0xE5 or e[11] == 0x0F: continue
            attr = e[11]
            name = e[0:8].decode('ascii', 'replace').rstrip()
            ext = e[8:11].decode('ascii', 'replace').rstrip()
            if e[0] == 0x05: name = '\xe5' + name[1:]
            fn = name + ('.' + ext if ext else '')
            clus = struct.unpack_from('<H', e, 26)[0]
            size = struct.unpack_from('<I', e, 28)[0]
            if attr & 0x08: continue  # volume label
            if attr & 0x10:
                if fn in ('.', '..'): continue
                sub = os.path.join(path, fn)
                os.makedirs(sub, exist_ok=True)
                parse_dir(read_chain(clus), sub)
            else:
                os.makedirs(path, exist_ok=True)
                with open(os.path.join(path, fn), 'wb') as f:
                    f.write(read_chain(clus, size))
                print(os.path.join(path, fn), size)

    parse_dir(d[root_off:data_off], out_dir)

if __name__ == '__main__':
    extract(sys.argv[1], sys.argv[2])
