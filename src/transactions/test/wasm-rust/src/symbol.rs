use super::require;
use super::Symbol;

impl Iterator for Symbol {
    type Item = char;

    fn next(&mut self) -> Option<Self::Item> {
        let res = match ((self.0 >> 42) & 63) as u8 {
            1 => b'_',
            n @ (2..=11) => (b'0' + n - 2),
            n @ (12..=37) => (b'A' + n - 12),
            n @ (38..=63) => (b'a' + n - 38),
            _ => b'\0',
        };
        self.0 <<= 6;
        if res == 0 {
            None
        } else {
            Some(res as char)
        }
    }
}

impl FromIterator<char> for Symbol {
    fn from_iter<T: IntoIterator<Item = char>>(iter: T) -> Self {
        let mut n = 0;
        let mut accum: u64 = 0;
        for i in iter {
            require(n < 8);
            n += 1;
            accum <<= 6;
            let v = match i {
                '_' => 1,
                '0'..='9' => 2 + ((i as u64) - ('0' as u64)),
                'A'..='Z' => 12 + ((i as u64) - ('A' as u64)),
                'a'..='z' => 38 + ((i as u64) - ('a' as u64)),
                _ => 0,
            };
            accum |= v;
        }
        accum <<= 6 * (8 - n);
        Symbol(accum)
    }
}
