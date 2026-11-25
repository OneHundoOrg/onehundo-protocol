use std::fmt;

#[derive(Debug, Clone)]
pub struct DeflateError(pub String);

impl fmt::Display for DeflateError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "DEFLATE error: {}", self.0)
    }
}

impl std::error::Error for DeflateError {}

pub fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            let mask = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
        }
    }
    !crc
}

struct BitReader<'a> {
    data: &'a [u8],
    byte_pos: usize,
    bit_buf: u64,
    bit_cnt: u8,
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self {
            data,
            byte_pos: 0,
            bit_buf: 0,
            bit_cnt: 0,
        }
    }

    fn ensure(&mut self, n: u8) -> Result<(), DeflateError> {
        while self.bit_cnt < n {
            if self.byte_pos >= self.data.len() {
                return Err(DeflateError("unexpected end of compressed stream".into()));
            }
            self.bit_buf |= (self.data[self.byte_pos] as u64) << self.bit_cnt;
            self.byte_pos += 1;
            self.bit_cnt += 8;
        }
        Ok(())
    }

    fn read_bits(&mut self, n: u8) -> Result<u32, DeflateError> {
        if n == 0 {
            return Ok(0);
        }
        self.ensure(n)?;
        let val = (self.bit_buf & ((1u64 << n) - 1)) as u32;
        self.bit_buf >>= n;
        self.bit_cnt -= n;
        Ok(val)
    }

    fn align_byte(&mut self) {
        let drop = self.bit_cnt & 7;
        self.bit_buf >>= drop;
        self.bit_cnt -= drop;
    }

    fn read_u16_aligned(&mut self) -> Result<u16, DeflateError> {
        self.align_byte();
        let lo = self.read_bits(8)? as u16;
        let hi = self.read_bits(8)? as u16;
        Ok(lo | (hi << 8))
    }

    fn read_bytes_aligned(&mut self, len: usize, out: &mut Vec<u8>, max_out: usize) -> Result<(), DeflateError> {
        if out.len() + len > max_out {
            return Err(DeflateError("decompressed output exceeds maximum allowed size".into()));
        }
        self.align_byte();
        for _ in 0..len {
            let b = self.read_bits(8)? as u8;
            out.push(b);
        }
        Ok(())
    }
}

struct HuffmanTree {
    counts: [u16; 16],
    symbols: Vec<u16>,
}

impl HuffmanTree {
    fn from_lengths(lengths: &[u8]) -> Result<Self, DeflateError> {
        let mut counts = [0u16; 16];
        for &len in lengths {
            if len as usize >= counts.len() {
                return Err(DeflateError("invalid Huffman code length".into()));
            }
            counts[len as usize] += 1;
        }
        counts[0] = 0;

        let mut offsets = [0u16; 16];
        let mut total: u16 = 0;
        for bits in 1..16 {
            offsets[bits] = total;
            total = total.saturating_add(counts[bits]);
        }

        let mut symbols = vec![0u16; total as usize];
        for (sym, &len) in lengths.iter().enumerate() {
            if len != 0 {
                let idx = offsets[len as usize] as usize;
                offsets[len as usize] += 1;
                if idx >= symbols.len() {
                    return Err(DeflateError("Huffman table overflow".into()));
                }
                symbols[idx] = sym as u16;
            }
        }

        Ok(Self { counts, symbols })
    }

    fn decode(&self, reader: &mut BitReader<'_>) -> Result<u16, DeflateError> {
        let mut code: i32 = 0;
        let mut first: i32 = 0;
        let mut index: i32 = 0;

        for len in 1..16 {
            code |= reader.read_bits(1)? as i32;
            let count = self.counts[len] as i32;
            if code - count < first {
                let sym_idx = (index + (code - first)) as usize;
                return self
                    .symbols
                    .get(sym_idx)
                    .copied()
                    .ok_or_else(|| DeflateError("corrupt Huffman symbol index".into()));
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        Err(DeflateError("invalid Huffman code".into()))
    }
}

fn fixed_trees() -> Result<(HuffmanTree, HuffmanTree), DeflateError> {
    let mut lit_lengths = [0u8; 288];
    for item in lit_lengths.iter_mut().take(144) {
        *item = 8;
    }
    for item in lit_lengths.iter_mut().take(256).skip(144) {
        *item = 9;
    }
    for item in lit_lengths.iter_mut().take(280).skip(256) {
        *item = 7;
    }
    for item in lit_lengths.iter_mut().skip(280) {
        *item = 8;
    }
    let dist_lengths = [5u8; 32];
    Ok((
        HuffmanTree::from_lengths(&lit_lengths)?,
        HuffmanTree::from_lengths(&dist_lengths)?,
    ))
}

fn dynamic_trees(reader: &mut BitReader<'_>) -> Result<(HuffmanTree, HuffmanTree), DeflateError> {
    const ORDER: [usize; 19] = [
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
    ];
    let hlit = reader.read_bits(5)? as usize + 257;
    let hdist = reader.read_bits(5)? as usize + 1;
    let hclen = reader.read_bits(4)? as usize + 4;

    let mut cl_lengths = [0u8; 19];
    for i in 0..hclen {
        cl_lengths[ORDER[i]] = reader.read_bits(3)? as u8;
    }
    let cl_tree = HuffmanTree::from_lengths(&cl_lengths)?;

    let total = hlit + hdist;
    let mut lengths = Vec::with_capacity(total);
    while lengths.len() < total {
        let sym = cl_tree.decode(reader)?;
        match sym {
            0..=15 => lengths.push(sym as u8),
            16 => {
                let prev = *lengths
                    .last()
                    .ok_or_else(|| DeflateError("repeat code 16 with no previous length".into()))?;
                let repeat = reader.read_bits(2)? as usize + 3;
                lengths.resize(lengths.len() + repeat, prev);
            }
            17 => {
                let repeat = reader.read_bits(3)? as usize + 3;
                lengths.resize(lengths.len() + repeat, 0);
            }
            18 => {
                let repeat = reader.read_bits(7)? as usize + 11;
                lengths.resize(lengths.len() + repeat, 0);
            }
            _ => return Err(DeflateError("invalid code-length symbol".into())),
        }
    }
    if lengths.len() != total {
        return Err(DeflateError("code lengths overshoot HLIT + HDIST".into()));
    }
    let lit_tree = HuffmanTree::from_lengths(&lengths[..hlit])?;
    let dist_tree = HuffmanTree::from_lengths(&lengths[hlit..])?;
    Ok((lit_tree, dist_tree))
}

pub fn inflate(compressed: &[u8], max_output_bytes: usize) -> Result<Vec<u8>, DeflateError> {
    const LEN_BASE: [u16; 29] = [
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
        131, 163, 195, 227, 258,
    ];
    const LEN_EXTRA: [u8; 29] = [
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
    ];
    const DIST_BASE: [u16; 30] = [
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
        2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
    ];
    const DIST_EXTRA: [u8; 30] = [
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
        13, 13,
    ];

    let mut reader = BitReader::new(compressed);
    let mut out = Vec::new();

    loop {
        let bfinal = reader.read_bits(1)?;
        let btype = reader.read_bits(2)?;

        match btype {
            0 => {
                let len = reader.read_u16_aligned()?;
                let nlen = reader.read_u16_aligned()?;
                if len != !nlen {
                    return Err(DeflateError("stored block LEN/NLEN mismatch".into()));
                }
                reader.read_bytes_aligned(len as usize, &mut out, max_output_bytes)?;
            }
            1 | 2 => {
                let (lit_tree, dist_tree) = if btype == 1 {
                    fixed_trees()?
                } else {
                    dynamic_trees(&mut reader)?
                };
                loop {
                    let sym = lit_tree.decode(&mut reader)?;
                    if sym < 256 {
                        if out.len() + 1 > max_output_bytes {
                            return Err(DeflateError(
                                "decompressed output exceeds maximum allowed size".into(),
                            ));
                        }
                        out.push(sym as u8);
                    } else if sym == 256 {
                        break;
                    } else {
                        let lidx = (sym - 257) as usize;
                        if lidx >= LEN_BASE.len() {
                            return Err(DeflateError("invalid length symbol".into()));
                        }
                        let length = LEN_BASE[lidx] as usize
                            + reader.read_bits(LEN_EXTRA[lidx])? as usize;
                        let dsym = dist_tree.decode(&mut reader)? as usize;
                        if dsym >= DIST_BASE.len() {
                            return Err(DeflateError("invalid distance symbol".into()));
                        }
                        let dist = DIST_BASE[dsym] as usize
                            + reader.read_bits(DIST_EXTRA[dsym])? as usize;
                        if dist == 0 || dist > out.len() {
                            return Err(DeflateError("invalid back-reference distance".into()));
                        }
                        if out.len() + length > max_output_bytes {
                            return Err(DeflateError(
                                "decompressed output exceeds maximum allowed size".into(),
                            ));
                        }
                        for _ in 0..length {
                            let b = out[out.len() - dist];
                            out.push(b);
                        }
                    }
                }
            }
            _ => return Err(DeflateError("reserved DEFLATE block type 3".into())),
        }

        if bfinal == 1 {
            break;
        }
    }

    Ok(out)
}
