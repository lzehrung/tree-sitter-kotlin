# tree-sitter-kotlin

> **codegraph fork.** This is a fork of
> [tree-sitter-grammars/tree-sitter-kotlin](https://github.com/tree-sitter-grammars/tree-sitter-kotlin)
> that carries upstream fixes which are not merged or released yet, so that
> [codegraph](https://github.com/lzehrung/codegraph) can build against a Kotlin grammar that parses one-line class bodies and
> annotated declarations without ERROR nodes. Use the `codegraph` branch pinned by commit (`codegraph-forms` adds the fixes below
> on top of it); `master` tracks upstream. The
> Rust crate keeps the name and version `tree-sitter-kotlin-ng` 1.1.0, so it is a drop-in Cargo git dependency:
> `tree-sitter-kotlin-ng = { git = "https://github.com/lzehrung/tree-sitter-kotlin", rev = "<commit>" }`.
>
> Applied upstream pull requests (cherry-picked with their original authorship):
>
> - [#11](https://github.com/tree-sitter-grammars/tree-sitter-kotlin/pull/11): automatic semicolon before a closing brace on the same line
> - [#13](https://github.com/tree-sitter-grammars/tree-sitter-kotlin/pull/13): terminate class members before a declaration keyword on the same line
> - [#21](https://github.com/tree-sitter-grammars/tree-sitter-kotlin/pull/21): a declaration under annotations is a declaration, not an `annotated_expression`
> - [#14](https://github.com/tree-sitter-grammars/tree-sitter-kotlin/pull/14): stop the annotation scan at end of file (infinite loop)
> - [#17](https://github.com/tree-sitter-grammars/tree-sitter-kotlin/pull/17): `where` as an ordinary identifier at statement start
> - [#20](https://github.com/tree-sitter-grammars/tree-sitter-kotlin/pull/20): read whole identifiers when checking for keywords after a newline
> - [#24](https://github.com/tree-sitter-grammars/tree-sitter-kotlin/pull/24): a call after a local property is not an accessor
>
> Not applied: #16 (conflicts with #14 in the scanner), #18 (same fix as #14), and the larger grammar changes (#5, #7, #15, #22).
> #23 is not applied either; `codegraph-forms` parses multi-dollar strings with its own change.
>
> Fixes on `codegraph-forms` (written for this fork, each with a corpus test):
>
> - a primary constructor on the line after a class header, also with `private`, `internal actual` or an annotation before it
> - an annotated accessor after a property without a type
> - `dynamic`, `final` and `override` as identifiers, and `by` as a name at the start of a statement
> - multi-dollar string literals (`$$"..."`), as the new node `multi_dollar_string_literal`
> - a trailing comma in an index expression, and an empty collection literal `[]`
> - a NUL character inside a block comment
>
> `src/parser.c` is generated with `npx tree-sitter-cli@0.24.6 generate` (language ABI 14, the CLI that produced the upstream file),
> so regenerating an unmodified checkout gives no diff. `npx tree-sitter-cli@0.24.6 test` runs the corpus in `test/corpus`.

[![CI][ci]](https://github.com/tree-sitter-grammars/tree-sitter-kotlin/actions/workflows/ci.yml)
[![discord][discord]](https://discord.gg/w7nTvsVJhm)
[![matrix][matrix]](https://matrix.to/#/#tree-sitter-chat:matrix.org)
[![crates][crates]](https://crates.io/crates/tree-sitter-kotlin-ng)
[![npm][npm]](https://www.npmjs.com/package/@tree-sitter-grammars/tree-sitter-kotlin)
[![pypi][pypi]](https://pypi.org/project/tree-sitter-kotlin)

Kotlin grammar for [tree-sitter](https://github.com/tree-sitter/tree-sitter).

## References

- [Kotlin Grammar](https://kotlinlang.org/docs/reference/grammar.html)

[ci]: https://img.shields.io/github/actions/workflow/status/tree-sitter-grammars/tree-sitter-kotlin/ci.yml?logo=github&label=CI
[discord]: https://img.shields.io/discord/1063097320771698699?logo=discord&label=discord
[matrix]: https://img.shields.io/matrix/tree-sitter-chat%3Amatrix.org?logo=matrix&label=matrix
[npm]: https://img.shields.io/npm/v/@tree-sitter-grammars/tree-sitter-kotlin?logo=npm
[crates]: https://img.shields.io/crates/v/tree-sitter-kotlin-ng?logo=rust
[pypi]: https://img.shields.io/pypi/v/tree-sitter-kotlin?logo=pypi&logoColor=ffd242
