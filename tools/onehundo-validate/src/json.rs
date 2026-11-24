use std::collections::BTreeMap;
use std::fmt;

#[derive(Debug, Clone, PartialEq)]
pub enum JsonValue {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    String(String),
    Array(Vec<JsonValue>),
    Object(BTreeMap<String, JsonValue>),
}

impl JsonValue {
    pub fn as_object(&self) -> Option<&BTreeMap<String, JsonValue>> {
        match self {
            JsonValue::Object(map) => Some(map),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[JsonValue]> {
        match self {
            JsonValue::Array(arr) => Some(arr),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            JsonValue::String(s) => Some(s.as_str()),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            JsonValue::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_i64(&self) -> Option<i64> {
        match self {
            JsonValue::Int(n) => Some(*n),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match self {
            JsonValue::Int(n) => Some(*n as f64),
            JsonValue::Float(f) => Some(*f),
            _ => None,
        }
    }
}

#[derive(Debug, Clone)]
pub struct JsonError {
    pub pos: usize,
    pub message: String,
}

impl fmt::Display for JsonError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "JSON parse error at byte {}: {}", self.pos, self.message)
    }
}

impl std::error::Error for JsonError {}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Parser<'a> {
    fn new(input: &'a str) -> Self {
        Self {
            bytes: input.as_bytes(),
            pos: 0,
        }
    }

    fn err(&self, msg: impl Into<String>) -> JsonError {
        JsonError {
            pos: self.pos,
            message: msg.into(),
        }
    }

    fn skip_ws(&mut self) {
        while self.pos < self.bytes.len() {
            match self.bytes[self.pos] {
                b' ' | b'\t' | b'\r' | b'\n' => self.pos += 1,
                _ => break,
            }
        }
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn next_byte(&mut self) -> Result<u8, JsonError> {
        if self.pos >= self.bytes.len() {
            return Err(self.err("unexpected end of input"));
        }
        let b = self.bytes[self.pos];
        self.pos += 1;
        Ok(b)
    }

    fn parse_value(&mut self) -> Result<JsonValue, JsonError> {
        self.skip_ws();
        match self.peek() {
            Some(b'n') => self.parse_null(),
            Some(b't') | Some(b'f') => self.parse_bool(),
            Some(b'"') => self.parse_string().map(JsonValue::String),
            Some(b'[') => self.parse_array(),
            Some(b'{') => self.parse_object(),
            Some(b'-') | Some(b'0'..=b'9') => self.parse_number(),
            Some(c) => Err(self.err(format!("unexpected byte '{}'", c as char))),
            None => Err(self.err("unexpected end of input")),
        }
    }

    fn parse_null(&mut self) -> Result<JsonValue, JsonError> {
        if self.bytes[self.pos..].starts_with(b"null") {
            self.pos += 4;
            Ok(JsonValue::Null)
        } else {
            Err(self.err("expected 'null'"))
        }
    }

    fn parse_bool(&mut self) -> Result<JsonValue, JsonError> {
        if self.bytes[self.pos..].starts_with(b"true") {
            self.pos += 4;
            Ok(JsonValue::Bool(true))
        } else if self.bytes[self.pos..].starts_with(b"false") {
            self.pos += 5;
            Ok(JsonValue::Bool(false))
        } else {
            Err(self.err("expected boolean"))
        }
    }

    fn parse_hex4(&mut self) -> Result<u16, JsonError> {
        let mut val: u16 = 0;
        for _ in 0..4 {
            let b = self.next_byte()?;
            let d = match b {
                b'0'..=b'9' => (b - b'0') as u16,
                b'a'..=b'f' => (b - b'a' + 10) as u16,
                b'A'..=b'F' => (b - b'A' + 10) as u16,
                _ => return Err(self.err("invalid hex digit in \\u escape")),
            };
            val = (val << 4) | d;
        }
        Ok(val)
    }

    fn parse_string(&mut self) -> Result<String, JsonError> {
        if self.next_byte()? != b'"' {
            return Err(self.err("expected '\"'"));
        }
        let mut out = String::new();
        loop {
            let b = self.next_byte()?;
            match b {
                b'"' => return Ok(out),
                b'\\' => {
                    let esc = self.next_byte()?;
                    match esc {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\x08'),
                        b'f' => out.push('\x0c'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let u1 = self.parse_hex4()?;
                            if (0xD800..=0xDBFF).contains(&u1) {
                                if self.next_byte()? != b'\\' || self.next_byte()? != b'u' {
                                    return Err(self.err("unpaired UTF-16 high surrogate"));
                                }
                                let u2 = self.parse_hex4()?;
                                if !(0xDC00..=0xDFFF).contains(&u2) {
                                    return Err(self.err("invalid UTF-16 low surrogate"));
                                }
                                let cp = 0x10000 + (((u1 as u32 - 0xD800) << 10) | (u2 as u32 - 0xDC00));
                                let ch = char::from_u32(cp)
                                    .ok_or_else(|| self.err("invalid Unicode codepoint"))?;
                                out.push(ch);
                            } else {
                                let ch = char::from_u32(u1 as u32)
                                    .ok_or_else(|| self.err("invalid Unicode codepoint"))?;
                                out.push(ch);
                            }
                        }
                        _ => return Err(self.err("invalid escape sequence")),
                    }
                }
                0x00..=0x1F => return Err(self.err("unescaped control character in string")),
                _ => {
                    if b < 0x80 {
                        out.push(b as char);
                    } else {
                        // Decode multi-byte UTF-8 sequence
                        let start = self.pos - 1;
                        let width = if b & 0xE0 == 0xC0 {
                            2
                        } else if b & 0xF0 == 0xE0 {
                            3
                        } else if b & 0xF8 == 0xF0 {
                            4
                        } else {
                            return Err(self.err("invalid UTF-8 lead byte"));
                        };
                        let end = start + width;
                        if end > self.bytes.len() {
                            return Err(self.err("truncated UTF-8 sequence"));
                        }
                        let s = std::str::from_utf8(&self.bytes[start..end])
                            .map_err(|_| self.err("invalid UTF-8 in string"))?;
                        out.push_str(s);
                        self.pos = end;
                    }
                }
            }
        }
    }

    fn parse_number(&mut self) -> Result<JsonValue, JsonError> {
        let start = self.pos;
        if self.peek() == Some(b'-') {
            self.pos += 1;
        }
        match self.peek() {
            Some(b'0') => self.pos += 1,
            Some(b'1'..=b'9') => {
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.pos += 1;
                }
            }
            _ => return Err(self.err("invalid number")),
        }
        let mut is_float = false;
        if self.peek() == Some(b'.') {
            is_float = true;
            self.pos += 1;
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(self.err("expected digit after decimal point"));
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            is_float = true;
            self.pos += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.pos += 1;
            }
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(self.err("expected exponent digits"));
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.pos += 1;
            }
        }
        let slice = std::str::from_utf8(&self.bytes[start..self.pos])
            .map_err(|_| self.err("invalid utf8 in number"))?;
        if is_float {
            let f: f64 = slice.parse().map_err(|_| self.err("invalid float"))?;
            Ok(JsonValue::Float(f))
        } else if let Ok(i) = slice.parse::<i64>() {
            Ok(JsonValue::Int(i))
        } else {
            let f: f64 = slice.parse().map_err(|_| self.err("invalid integer/float"))?;
            Ok(JsonValue::Float(f))
        }
    }

    fn parse_array(&mut self) -> Result<JsonValue, JsonError> {
        self.next_byte()?; // '['
        self.skip_ws();
        let mut items = Vec::new();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Ok(JsonValue::Array(items));
        }
        loop {
            items.push(self.parse_value()?);
            self.skip_ws();
            match self.next_byte()? {
                b',' => continue,
                b']' => return Ok(JsonValue::Array(items)),
                _ => return Err(self.err("expected ',' or ']' in array")),
            }
        }
    }

    fn parse_object(&mut self) -> Result<JsonValue, JsonError> {
        self.next_byte()?; // '{'
        self.skip_ws();
        let mut map = BTreeMap::new();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Ok(JsonValue::Object(map));
        }
        loop {
            self.skip_ws();
            if self.peek() != Some(b'"') {
                return Err(self.err("expected string key in object"));
            }
            let key = self.parse_string()?;
            self.skip_ws();
            if self.next_byte()? != b':' {
                return Err(self.err("expected ':' after object key"));
            }
            let val = self.parse_value()?;
            map.insert(key, val);
            self.skip_ws();
            match self.next_byte()? {
                b',' => continue,
                b'}' => return Ok(JsonValue::Object(map)),
                _ => return Err(self.err("expected ',' or '}' in object")),
            }
        }
    }
}

pub fn parse_json(input: &str) -> Result<JsonValue, JsonError> {
    let mut p = Parser::new(input);
    let val = p.parse_value()?;
    p.skip_ws();
    if p.pos != p.bytes.len() {
        return Err(p.err("trailing data after JSON value"));
    }
    Ok(val)
}
