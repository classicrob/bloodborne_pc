"""FromSoftware containers in the PS4 game data: DCX (zlib "DFLT") and BND4 binders.
Layouts as documented by SoulsFormats (DCX, BND4 / BinderFileHeader)."""
import struct
import zlib


def dcx_decompress(data: bytes) -> bytes:
    if data[:4] != b'DCX\0':
        return data
    if b'DFLT' not in data[:0x60]:
        raise ValueError('DCX: only DFLT (zlib) compression is handled')
    dca = data.index(b'DCA\0')
    (header_size,) = struct.unpack_from('>I', data, dca + 4)
    (size,) = struct.unpack_from('>I', data, data.index(b'DCS\0') + 4)
    out = zlib.decompress(data[dca + header_size:])
    if len(out) != size:
        raise ValueError(f'DCX: {len(out)} bytes decompressed, header says {size}')
    return out


def _format_bits(byte: int, bit_big_endian: bool) -> int:
    if bit_big_endian:
        return byte
    return int(f'{byte:08b}'[::-1], 2)


def bnd4_files(data: bytes):
    """Yields (name, id, bytes) of every file in a BND4 binder."""
    if data[:4] != b'BND4':
        raise ValueError('not a BND4 binder')
    big_endian = data[9] != 0
    bit_big_endian = data[10] == 0
    e = '>' if big_endian else '<'
    (count,) = struct.unpack_from(e + 'i', data, 0x0C)
    (file_header_size,) = struct.unpack_from(e + 'q', data, 0x20)
    unicode = data[0x30] != 0
    fmt = _format_bits(data[0x31], bit_big_endian)
    has_ids, names1, names2 = fmt & 0x02, fmt & 0x04, fmt & 0x08
    long_offsets, compression = fmt & 0x10, fmt & 0x20
    pos = 0x40
    for _ in range(count):
        start = pos
        pos += 8  # flags, 3 zero bytes, -1
        (size,) = struct.unpack_from(e + 'q', data, pos)
        pos += 8
        if compression:
            pos += 8
        if long_offsets:
            (offset,) = struct.unpack_from(e + 'q', data, pos)
            pos += 8
        else:
            (offset,) = struct.unpack_from(e + 'I', data, pos)
            pos += 4
        file_id = -1
        if has_ids:
            (file_id,) = struct.unpack_from(e + 'i', data, pos)
            pos += 4
        name = None
        if names1 or names2:
            (name_offset,) = struct.unpack_from(e + 'I', data, pos)
            pos += 4
            if unicode:
                end = name_offset
                while data[end:end + 2] != b'\0\0':
                    end += 2
                name = data[name_offset:end].decode('utf-16-be' if big_endian else 'utf-16-le')
            else:
                end = data.index(b'\0', name_offset)
                name = data[name_offset:end].decode('shift_jis', 'replace')
        if fmt == 0x04:  # Names1: id after the name offset
            (file_id,) = struct.unpack_from(e + 'i', data, pos)
            pos += 8
        if pos - start != file_header_size:
            pos = start + file_header_size
        yield name, file_id, data[offset:offset + size]
