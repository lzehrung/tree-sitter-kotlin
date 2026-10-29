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
        let mut parser = kotlin_parser();
        let mut lexed: Vec<(u32, u32)> = Vec::new();
        let mut buffer = [0u8; 4];
        for code_point in 0x80u32..=0x10FFFF {
            let Some(character) = char::from_u32(code_point) else {
                continue;
            };
            if is_lone_identifier(&mut parser, character.encode_utf8(&mut buffer)) {
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

    /// tree-sitter-cli 0.24 resolved `\p{L}` from a table that kept only the first and last code point of the
    /// large ranges and stopped at U+10915, so most CJK ideographs, Hangul syllables and every letter of the later
    /// scripts were not identifiers. Every code point of these blocks is a letter (Unicode category Lo or Lm), so each
    /// has to start an identifier, and to continue one.
    #[test]
    fn lexer_reads_every_letter_of_the_blocks_an_old_unicode_table_cut() {
        let blocks: [(u32, u32, &str); 15] = [
            (0x3400, 0x4DBF, "CJK Unified Ideographs Extension A"),
            (0x4E00, 0x9FFF, "CJK Unified Ideographs"),
            (0xA000, 0xA48C, "Yi Syllables"),
            (0xAC00, 0xD7A3, "Hangul Syllables"),
            (0x13000, 0x1342F, "Egyptian Hieroglyphs"),
            (0x17000, 0x187F7, "Tangut"),
            (0x18D00, 0x18D08, "Tangut Supplement"),
            (0x20000, 0x2A6DF, "CJK Unified Ideographs Extension B"),
            (0x2A700, 0x2B739, "CJK Unified Ideographs Extension C"),
            (0x2B740, 0x2B81D, "CJK Unified Ideographs Extension D"),
            (0x2B820, 0x2CEA1, "CJK Unified Ideographs Extension E"),
            (0x2CEB0, 0x2EBE0, "CJK Unified Ideographs Extension F"),
            (0x2EBF0, 0x2EE5D, "CJK Unified Ideographs Extension I"),
            (0x30000, 0x3134A, "CJK Unified Ideographs Extension G"),
            (0x31350, 0x323AF, "CJK Unified Ideographs Extension H"),
        ];
        let mut parser = kotlin_parser();
        for (first, last, block) in blocks {
            for code_point in first..=last {
                let letter = char::from_u32(code_point).expect("a scalar value");
                assert!(
                    is_lone_identifier(&mut parser, &letter.to_string()),
                    "U+{code_point:04X} ({block}) does not start an identifier"
                );
                assert!(
                    is_lone_identifier(&mut parser, &format!("a{letter}1")),
                    "U+{code_point:04X} ({block}) does not continue an identifier"
                );
            }
        }
    }

    /// Characters that are not letters stay out: CJK punctuation and radicals, a letter-number, an emoji and a currency
    /// sign are not identifiers, and a decimal digit of another script (category Nd) continues an identifier but does
    /// not start one.
    #[test]
    fn lexer_does_not_read_other_characters_as_letters() {
        let mut parser = kotlin_parser();
        for text in ["\u{3001}", "\u{3002}", "\u{2e80}", "\u{2f00}", "\u{2167}", "\u{1f600}", "\u{20ac}", "\u{ff01}"] {
            assert!(!is_lone_identifier(&mut parser, text), "{text:?} starts an identifier");
            assert!(!is_lone_identifier(&mut parser, &format!("a{text}")), "{text:?} continues an identifier");
        }
        for digit in ["\u{661}", "\u{ff11}", "\u{1d7ce}"] {
            assert!(!is_lone_identifier(&mut parser, digit), "{digit:?} starts an identifier");
            assert!(is_lone_identifier(&mut parser, &format!("a{digit}")), "{digit:?} does not continue an identifier");
        }
    }

    fn kotlin_parser() -> tree_sitter::Parser {
        let mut parser = tree_sitter::Parser::new();
        parser
            .set_language(&super::LANGUAGE.into())
            .expect("Error loading Kotlin parser");
        parser
    }

    /// True when `text` parses without error to a single `identifier` that covers all of it.
    fn is_lone_identifier(parser: &mut tree_sitter::Parser, text: &str) -> bool {
        let tree = parser.parse(text, None).expect("a tree");
        let root = tree.root_node();
        !root.has_error() && root.child_count() == 1 && {
            let child = root.child(0).expect("a child");
            child.kind() == "identifier" && child.start_byte() == 0 && child.end_byte() == text.len()
        }
    }

    fn find<'tree>(node: tree_sitter::Node<'tree>, kind: &str) -> Option<tree_sitter::Node<'tree>> {
        if node.kind() == kind {
            return Some(node);
        }
        let mut cursor = node.walk();
        let found = node.children(&mut cursor).find_map(|child| find(child, kind));
        found
    }

    /// The named children of the first multi-dollar string in `source` as (kind, text), with neighbouring
    /// `string_content` nodes joined: how a long stretch of text is cut into tokens is the scanner's business, what it
    /// covers is not. The children must be contiguous, so no text is left out.
    fn string_parts(source: &str) -> Vec<(String, String)> {
        let tree = kotlin_parser().parse(source, None).expect("a tree");
        assert!(!tree.root_node().has_error(), "parse error in {source:?}");
        let literal = find(tree.root_node(), "multi_dollar_string_literal").expect("a multi-dollar string");
        let mut parts: Vec<(String, String)> = Vec::new();
        let mut previous_end = None;
        let mut cursor = literal.walk();
        for child in literal.children(&mut cursor) {
            if let Some(end) = previous_end {
                assert_eq!(child.start_byte(), end, "a gap before {:?} in {source:?}", child.kind());
            }
            previous_end = Some(child.end_byte());
            let text = source[child.byte_range()].to_string();
            match parts.last_mut() {
                Some((kind, joined)) if kind == "string_content" && child.kind() == "string_content" => {
                    joined.push_str(&text)
                }
                _ => parts.push((child.kind().to_string(), text)),
            }
        }
        parts
    }

    /// The text of every node of `kind` in `source`, in order.
    fn texts_of_kind(source: &str, kind: &str) -> Vec<String> {
        let tree = kotlin_parser().parse(source, None).expect("a tree");
        assert!(!tree.root_node().has_error(), "parse error in {source:?}");
        let mut texts = Vec::new();
        let mut cursor = tree.walk();
        'walk: loop {
            let node = cursor.node();
            if node.kind() == kind {
                texts.push(source[node.byte_range()].to_string());
            }
            if cursor.goto_first_child() {
                continue;
            }
            while !cursor.goto_next_sibling() {
                if !cursor.goto_parent() {
                    break 'walk;
                }
            }
        }
        texts
    }

    /// The corpus compares the shape of a tree, so a name that was read in two pieces would still pass there. This
    /// compares text: a name written with CJK ideographs, Hangul, a supplementary-plane letter or a digit of another
    /// script is one `identifier` in a declaration, a reference and a template, and one `label` before a loop.
    #[test]
    fn a_name_written_in_other_scripts_is_read_whole() {
        let source = "class 名前(val 値: Int)\nfun 한글(변수: Int) = 𠮷野家\nval x١ = 𝒜名前２\n名前@ for (i in 0..1) {}\n";
        assert_eq!(
            texts_of_kind(source, "identifier"),
            ["名前", "値", "Int", "한글", "변수", "Int", "𠮷野家", "x١", "𝒜名前２", "i"]
        );
        assert_eq!(texts_of_kind(source, "label"), ["名前@"]);

        let parts = string_parts("val a = $$\"x $$名前 $$𠮷野家 $${値} $名前 $$。 y\"\n");
        let expected = [
            ("string_content", "x "),
            ("interpolation", "$$名前"),
            ("string_content", " "),
            ("interpolation", "$$𠮷野家"),
            ("string_content", " "),
            ("interpolation", "$${値}"),
            ("string_content", " $名前 $$。 y"),
        ];
        let expected: Vec<(String, String)> = expected.iter().map(|(kind, text)| (kind.to_string(), text.to_string())).collect();
        assert_eq!(parts, expected);
    }

    /// With a prefix of n dollars only the last n dollars of a run start a template, and the dollars before them are
    /// text however long the run is. The same holds for the quotes before the last three of a triple-quoted string.
    #[test]
    fn a_long_run_before_a_template_or_the_end_is_text_up_to_its_last_characters() {
        let ten = "$".repeat(10);
        let eight = "$".repeat(8);
        let seven = "$".repeat(7);
        let quotes = "\"".repeat(7);
        let four_quotes = "\"".repeat(4);
        let content = "string_content";
        let interpolation = "interpolation";
        let cases: Vec<(String, Vec<(&str, String)>)> = vec![
            (
                format!("val x = $$\"{ten}name\"\n"),
                vec![(content, eight.clone()), (interpolation, "$$name".into())],
            ),
            (
                format!("val x = $$\"x {ten}{{y}} z\"\n"),
                vec![
                    (content, format!("x {eight}")),
                    (interpolation, "$${y}".into()),
                    (content, " z".into()),
                ],
            ),
            (
                format!("val x = $$\"{ten}`my name`\"\n"),
                vec![(content, eight.clone()), (interpolation, "$$`my name`".into())],
            ),
            (
                format!("val x = $$\"\\n{ten}z\"\n"),
                vec![
                    ("escape_sequence", "\\n".into()),
                    (content, eight.clone()),
                    (interpolation, "$$z".into()),
                ],
            ),
            (
                format!("val x = $$\"$$a{ten}b\"\n"),
                vec![
                    (interpolation, "$$a".into()),
                    (content, eight.clone()),
                    (interpolation, "$$b".into()),
                ],
            ),
            (
                format!("val x = $$\"{ten} $$\"\n"),
                vec![(content, format!("{ten} $$"))],
            ),
            (
                format!("val x = $$$\"{ten}name\"\n"),
                vec![(content, seven), (interpolation, "$$$name".into())],
            ),
            (
                format!("val x = $$\"\"\"{ten}name\"\"\"\n"),
                vec![(content, eight.clone()), (interpolation, "$$name".into())],
            ),
            (
                format!("val x = $$\"\"\"x{ten}name\"\"\"\n"),
                vec![(content, format!("x{eight}")), (interpolation, "$$name".into())],
            ),
            (
                format!("val x = $$\"\"\"{ten}`open\"\"\"\n"),
                vec![(content, format!("{ten}`open"))],
            ),
            (
                format!("val x = $$\"\"\"x{quotes}\n"),
                vec![(content, format!("x{four_quotes}"))],
            ),
            (
                format!("val x = $$\"\"\"{quotes}\n"),
                vec![(content, four_quotes)],
            ),
        ];
        for (source, expected) in cases {
            let expected: Vec<(String, String)> = expected
                .into_iter()
                .map(|(kind, text)| (kind.to_string(), text))
                .collect();
            assert_eq!(string_parts(&source), expected, "{source:?}");
        }
    }

    /// A run of dollars before a template took time quadratic in its length, because the scanner read the whole run
    /// again for each dollar it returned: 200,000 dollars took a minute. The linear scan needs milliseconds even in a
    /// debug build, so the bound is far from both.
    #[test]
    fn a_long_run_of_dollars_or_quotes_is_parsed_in_linear_time() {
        use std::time::{Duration, Instant};

        let mut parser = kotlin_parser();
        let forms: [(&str, fn(&str) -> String); 6] = [
            ("name", |run| format!("val x = $$\"{run}name\"\n")),
            ("brace", |run| format!("val x = $$\"{run}{{name}}\"\n")),
            ("quoted name", |run| format!("val x = $$\"{run}`name`\"\n")),
            ("text before", |run| format!("val x = $$\"text{run}name\"\n")),
            ("raw", |run| format!("val x = $$\"\"\"{run}name\"\"\"\n")),
            ("raw quotes", |run| format!("val x = $$\"\"\"{}\n", run.replace('$', "\""))),
        ];
        for (form, make) in forms {
            let mut times: Vec<Duration> = Vec::new();
            for count in [100_000usize, 200_000] {
                let source = make(&"$".repeat(count));
                let started = Instant::now();
                let tree = parser.parse(&source, None).expect("a tree");
                let elapsed = started.elapsed();
                assert!(!tree.root_node().has_error(), "{form}, {count}: parse error");
                assert!(elapsed < Duration::from_secs(2), "{form}, {count}: took {elapsed:?}");
                times.push(elapsed);
            }
            // Twice the input takes about twice the time and quadratic work four times as long. Below a few tens of
            // milliseconds the clock is too coarse to tell, and the bound above has already judged the time.
            if times[0] > Duration::from_millis(30) {
                assert!(times[1] < times[0] * 3, "{form}: {:?} for 100,000 and {:?} for 200,000", times[0], times[1]);
            }
        }
    }

    /// The scanner keeps the length of a run between the tokens that return it, and the parser takes tokens from the
    /// old tree when it can. After an edit in or beside a long run, a reparse has to give the tree a fresh parse gives.
    #[test]
    fn a_reparse_after_an_edit_around_a_long_run_equals_a_fresh_parse() {
        fn point(text: &str, byte: usize) -> tree_sitter::Point {
            let before = &text[..byte];
            let row = before.matches('\n').count();
            let column = before.len() - before.rfind('\n').map_or(0, |index| index + 1);
            tree_sitter::Point { row, column }
        }
        fn shape(tree: &tree_sitter::Tree) -> Vec<(String, usize, usize)> {
            let mut nodes = Vec::new();
            let mut cursor = tree.walk();
            loop {
                let node = cursor.node();
                nodes.push((node.kind().to_string(), node.start_byte(), node.end_byte()));
                if cursor.goto_first_child() {
                    continue;
                }
                while !cursor.goto_next_sibling() {
                    if !cursor.goto_parent() {
                        return nodes;
                    }
                }
            }
        }

        let mut parser = kotlin_parser();
        let dollars = "$".repeat(40);
        let quotes = "\"".repeat(40);
        // (text before the run, the run, text after it)
        let sources = [
            ("val a = 1\nval x = $$\"ab", dollars.as_str(), "name cd\"\nval b = 2\n"),
            ("val a = 1\nval x = $$\"", dollars.as_str(), "{n}\"\nval b = 2\n"),
            ("val a = 1\nval x = $$\"\"\"ab", dollars.as_str(), "name\"\"\"\nval b = 2\n"),
            ("val a = 1\nval x = $$\"\"\"", quotes.as_str(), "\nval b = 2\n"),
        ];
        let (mut compared, mut edits_made) = (0, 0);
        for (head, run, tail) in sources {
            let old_text = format!("{head}{run}{tail}");
            let run_start = head.len();
            let run_end = run_start + run.len();
            let middle = (run_start + run_end) / 2;
            // (start, old end, replacement) in the old text
            let edits: Vec<(usize, usize, &str)> = vec![
                (run_start, run_start, "$"),
                (middle, middle, "$"),
                (run_end, run_end, "$"),
                (run_start, run_start + 1, ""),
                (middle, middle + 1, ""),
                (run_end - 1, run_end, ""),
                (middle, middle + 1, "x"),
                (middle, middle, " "),
                (run_end, run_end + 1, " "),
                (run_end + 1, run_end + 2, ""),
                (run_start - 1, run_start, ""),
                (0, 0, "// c\n"),
            ];
            for (start, old_end, replacement) in edits {
                let new_text = format!("{}{}{}", &old_text[..start], replacement, &old_text[old_end..]);
                let mut old_tree = parser.parse(&old_text, None).expect("a tree");
                old_tree.edit(&tree_sitter::InputEdit {
                    start_byte: start,
                    old_end_byte: old_end,
                    new_end_byte: start + replacement.len(),
                    start_position: point(&old_text, start),
                    old_end_position: point(&old_text, old_end),
                    new_end_position: point(&new_text, start + replacement.len()),
                });
                let reparsed = parser.parse(&new_text, Some(&old_tree)).expect("a tree");
                let fresh = parser.parse(&new_text, None).expect("a tree");
                edits_made += 1;
                // Error recovery depends on what the parser reuses, so only a valid parse has to be the same.
                if !fresh.root_node().has_error() {
                    compared += 1;
                    assert_eq!(
                        shape(&reparsed),
                        shape(&fresh),
                        "edit {start}..{old_end} -> {replacement:?} in {old_text:?}"
                    );
                }
            }
        }
        assert!(compared * 2 > edits_made, "{compared} of {edits_made} edits left a valid parse to compare");
    }
}
