//! This crate provides Kotlin language support for the [tree-sitter][] parsing library.
//!
//! Typically, you will use the [LANGUAGE][] constant to add this language to a
//! tree-sitter [Parser][], and then use the parser to parse some code:
//!
//! ```
//! let code = r#"
//! // write some kotlin code
//! fun main(args : Array<String>) {
//!   println("Hello, World!")
//! }
//! "#;
//! let mut parser = tree_sitter::Parser::new();
//! let language = tree_sitter_kotlin_ng::LANGUAGE;
//! parser
//!     .set_language(&language.into())
//!     .expect("Error loading Kotlin parser");
//! let tree = parser.parse(code, None).unwrap();
//! assert!(!tree.root_node().has_error());
//! ```
//!
//! [Parser]: https://docs.rs/tree-sitter/*/tree_sitter/struct.Parser.html
//! [tree-sitter]: https://tree-sitter.github.io/

use tree_sitter_language::LanguageFn;

extern "C" {
    fn tree_sitter_kotlin() -> *const ();
}

/// The tree-sitter [`LanguageFn`][LanguageFn] for this grammar.
///
/// [LanguageFn]: https://docs.rs/tree-sitter-language/*/tree_sitter_language/struct.LanguageFn.html
pub const LANGUAGE: LanguageFn = unsafe { LanguageFn::from_raw(tree_sitter_kotlin) };

/// The content of the [`node-types.json`][] file for this grammar.
///
/// [`node-types.json`]: https://tree-sitter.github.io/tree-sitter/using-parsers#static-node-types
pub const NODE_TYPES: &str = include_str!("../../src/node-types.json");

#[cfg(test)]
mod tests {
    #[test]
    fn test_can_load_grammar() {
        let mut parser = tree_sitter::Parser::new();
        parser
            .set_language(&super::LANGUAGE.into())
            .expect("Error loading Kotlin parser");
    }

    /// The ranges in the `LETTER_RANGES` table of `src/scanner.c`.
    fn scanner_letter_ranges() -> Vec<(u32, u32)> {
        let scanner = include_str!("../../src/scanner.c");
        let begin = scanner.find("// BEGIN LETTER RANGES").expect("table start marker");
        let end = scanner.find("// END LETTER RANGES").expect("table end marker");
        let mut ranges = Vec::new();
        let mut rest = &scanner[begin..end];
        while let Some(open) = rest.find("{0x") {
            let after = &rest[open + 1..];
            let close = after.find('}').expect("closing brace");
            let (low, high) = after[..close].split_once(',').expect("a pair");
            let hex = |text: &str| u32::from_str_radix(text.trim().trim_start_matches("0x"), 16).expect("hex number");
            ranges.push((hex(low), hex(high)));
            rest = &after[close..];
        }
        ranges
    }

    /// The scanner decides from its own table whether a name starts after the dollars of a template in a
    /// multi-dollar string, and the grammar's `identifier` token then has to read the name. A letter the table
    /// lacks hides a template, and a letter in the table that the lexer does not read leaves a parse error after the
    /// template start. So the table must be exactly the characters above ASCII that the generated lexer accepts as
    /// the first character of an `identifier`. This reads them out of the lexer one code point at a time.
    #[test]
    fn scanner_letter_table_matches_the_lexer() {
        let mut parser = tree_sitter::Parser::new();
        parser
            .set_language(&super::LANGUAGE.into())
            .expect("Error loading Kotlin parser");

        let mut lexed: Vec<(u32, u32)> = Vec::new();
        let mut buffer = [0u8; 4];
        for code_point in 0x80u32..=0x10FFFF {
            let Some(character) = char::from_u32(code_point) else {
                continue;
            };
            let text: &str = character.encode_utf8(&mut buffer);
            let tree = parser.parse(text, None).expect("a tree");
            let root = tree.root_node();
            let is_identifier = !root.has_error() && root.child_count() == 1 && {
                let child = root.child(0).expect("a child");
                child.kind() == "identifier" && child.start_byte() == 0 && child.end_byte() == text.len()
            };
            if is_identifier {
                match lexed.last_mut() {
                    Some(last) if last.1 + 1 == code_point => last.1 = code_point,
                    _ => lexed.push((code_point, code_point)),
                }
            }
        }

        let table = scanner_letter_ranges();
        if table != lexed {
            let rows: Vec<String> = lexed
                .chunks(5)
                .map(|chunk| {
                    let cells: Vec<String> = chunk.iter().map(|(low, high)| format!("{{0x{low:x}, 0x{high:x}}}")).collect();
                    format!("    {},", cells.join(", "))
                })
                .collect();
            panic!(
                "LETTER_RANGES in src/scanner.c does not match the lexer ({} ranges in the table, {} in the lexer). Replace its rows with:\n{}",
                table.len(),
                lexed.len(),
                rows.join("\n")
            );
        }
    }
}
