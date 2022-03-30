use super::require;
use super::Symbol;

const MAX_CHARS: usize = 10;

#[cfg(test)]
mod test {
    use super::super::Symbol;

    #[test]
    fn test_from_static_str() {
        const HELLO: Symbol = Symbol::from_str("hello");
        let s: String = HELLO.collect();
        //dbg!(s);
    }
}

impl Symbol {
    pub const fn from_str(s: &'static str) -> Symbol {
        let mut n = 0;
        let mut accum: u64 = 0;
        let b: &[u8] = s.as_bytes();
        while n < b.len() {
            let ch = b[n] as char;
            if n >= MAX_CHARS {
                break;
            }
            n += 1;
            accum <<= 6;
            let v = match ch {
                '_' => 1,
                '0'..='9' => 2 + ((ch as u64) - ('0' as u64)),
                'A'..='Z' => 12 + ((ch as u64) - ('A' as u64)),
                'a'..='z' => 38 + ((ch as u64) - ('a' as u64)),
                _ => 0,
            };
            accum |= v;
        }
        accum <<= 6 * (MAX_CHARS - n);
        Symbol(accum)
    }
}

impl Iterator for Symbol {
    type Item = char;

    fn next(&mut self) -> Option<Self::Item> {
        let res = match ((self.0 >> (MAX_CHARS-6)) & 63) as u8 {
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
            require(n < MAX_CHARS);
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
        accum <<= 6 * (MAX_CHARS - n);
        Symbol(accum)
    }
}
