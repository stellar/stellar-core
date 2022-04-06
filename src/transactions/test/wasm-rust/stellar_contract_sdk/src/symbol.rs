use super::require;
use super::val::{Val, ValType, TAG_SYMBOL};

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Symbol(pub(crate) Val);

impl From<Symbol> for Val {
    #[inline(always)]
    fn from(s: Symbol) -> Self {
        s.0
    }
}

impl ValType for Symbol {
    #[inline(always)]
    unsafe fn unchecked_from_val(v: Val) -> Self {
        Symbol(v)
    }

    fn is_val_type(v: Val) -> bool {
        v.has_tag(TAG_SYMBOL)
    }
}

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
                _ => break,
            };
            accum |= v;
        }
        let v = unsafe { Val::from_body_and_tag(accum, TAG_SYMBOL) };
        Symbol(v)
    }
}

impl IntoIterator for Symbol {
    type Item = char;
    type IntoIter = SymbolIter;
    fn into_iter(self) -> Self::IntoIter {
        SymbolIter(self.0.get_body())
    }
}

pub struct SymbolIter(u64);

impl Iterator for SymbolIter {
    type Item = char;

    fn next(&mut self) -> Option<Self::Item> {
        while self.0 != 0 {
            let res = match ((self.0 >> (MAX_CHARS - 6)) & 63) as u8 {
                1 => b'_',
                n @ (2..=11) => (b'0' + n - 2),
                n @ (12..=37) => (b'A' + n - 12),
                n @ (38..=63) => (b'a' + n - 38),
                _ => b'\0',
            };
            self.0 <<= 6;
            if res != b'\0' {
                return Some(res as char);
            }
        }
        None
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
                _ => break,
            };
            accum |= v;
        }
        let v = unsafe { Val::from_body_and_tag(accum, super::val::TAG_SYMBOL) };
        Symbol(v)
    }
}
