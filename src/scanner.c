#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

#include <string.h>
#include <wctype.h>

enum TokenType {
    SEMI,
    CLASS_MEMBER_SEMI,
    BLOCK_COMMENT,
    NOT_IS,
    IN,
    Q_DOT,
    MULTILINE_STRING_CONTENT,
    CONSTRUCTOR,
    GET,
    SET,
    DOLLAR,
    MD_STRING_START,
    MD_STRING_CONTENT,
    MD_INTERPOLATION,
    MD_INTERPOLATION_BRACE,
    MD_STRING_END,
    ERROR_SENTINEL,
};

#define MAX_WORD_SIZE 16
#define MAX_WORDS 16

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static bool scan_word(TSLexer *lexer, const char *const word) {
    for (uint8_t i = 0; word[i] != '\0'; i++) {
        if (lexer->lookahead != word[i]) {
            return false;
        }
        skip(lexer);
    }
    return true;
}

static bool scan_words(TSLexer *lexer, const char words[MAX_WORDS][MAX_WORD_SIZE], char scanned_word[16],
                       uint8_t *index) {
    if (!scanned_word[0]) {
        for (uint8_t i = 0; i < MAX_WORD_SIZE - 1; i++) {
            // Read the whole identifier (letters, digits, `_`), so that `in1`, `open_file` or
            // `set_value` are not mistaken for the keywords `in`, `open` or `set`.
            if (!(iswalpha(lexer->lookahead) || (i > 0 && (iswdigit(lexer->lookahead) || lexer->lookahead == '_')))) {
                if (i == 0) {
                    return false;
                }
                break;
            }
            scanned_word[i] = (char)lexer->lookahead;
            skip(lexer);
        }
    }

    for (uint8_t i = 0; i < MAX_WORDS; i++) {
        if (strncmp(scanned_word, words[i], MAX_WORD_SIZE) == 0) {
            if (index != NULL) {
                *index = i;
            }
            return true;
        }
    }

    return false;
}

// With the lookahead on the `(` after a `set` that may be a setter: true when it is a call on a
// new statement. A setter's parameter is a name, possibly annotated (`set(v) {`, `set(@A v) {`);
// `set("k") {` and `set("k", v)` are calls.
static bool set_paren_is_call(TSLexer *lexer) {
    skip(lexer);
    while (iswspace(lexer->lookahead)) {
        skip(lexer);
    }
    if (!(iswalpha(lexer->lookahead) || lexer->lookahead > 127 || lexer->lookahead == '_' ||
          lexer->lookahead == '`' || lexer->lookahead == '@')) {
        return true;
    }
    // skip until the closing parenthesis
    while (lexer->lookahead != ')' && !lexer->eof(lexer)) {
        skip(lexer);
    }
    skip(lexer);

    while (iswspace(lexer->lookahead)) {
        if (lexer->lookahead == '\n') {
            return true;
        }
        skip(lexer);
    }
    return false;
}

// True when a primary constructor may follow, as on the line after a class header: CONSTRUCTOR is valid and no
// accessor can follow. CONSTRUCTOR is also valid after the header of a property without a type (`val size`),
// because the grammar lets a receiver type be followed directly by a name, but there GET and SET are valid too.
static inline bool primary_constructor_may_follow(const bool *valid_symbols) {
    return valid_symbols[CONSTRUCTOR] && !valid_symbols[GET] && !valid_symbols[SET];
}

// A multi-dollar string (`$$"..."`, `$$"""..."""`) is a string with a template prefix of two or more dollars: only that
// many dollars in a row, followed by a name or `{`, start a template, and a run of fewer dollars is text. The scanner
// has to know the count while it reads the text, so it keeps one entry per open multi-dollar string: the dollar
// count, with bit 7 set for a triple-quoted string. Strings nest through templates (`$${ f($$"...") }`). The entries
// are the serialized state, so a parse that backtracks gets the entries of the token it backtracks to.
#define MD_MAX_DEPTH 32
#define MD_RAW 0x80
#define MD_MAX_DOLLARS 0x7f

typedef struct {
    uint8_t depth;
    uint8_t open[MD_MAX_DEPTH];
} Scanner;

void *tree_sitter_kotlin_external_scanner_create() { return ts_calloc(1, sizeof(Scanner)); }

void tree_sitter_kotlin_external_scanner_destroy(void *payload) { ts_free(payload); }

unsigned tree_sitter_kotlin_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    if (scanner->depth > 0) {
        memcpy(buffer, scanner->open, scanner->depth);
    }
    return scanner->depth;
}

void tree_sitter_kotlin_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->depth = 0;
    if (length > 0 && length <= MD_MAX_DEPTH) {
        memcpy(scanner->open, buffer, length);
        scanner->depth = (uint8_t)length;
    }
}

// The first character of a name after the dollars of a template. Only ASCII letters and `_` count: `iswalpha` gives
// different answers for other characters on different platforms, and a name that starts with another letter is read as
// text, which is the conservative reading.
static inline bool is_identifier_start(int32_t c) {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// At a `$` where a value can start: the opening of a multi-dollar string, `$$"` or `$$"""` with any number of dollars
// from two. The token is the prefix and the opening quotes.
static bool scan_multi_dollar_start(Scanner *scanner, TSLexer *lexer) {
    unsigned dollars = 0;
    while (lexer->lookahead == '$') {
        advance(lexer);
        dollars++;
    }
    if (dollars < 2 || dollars > MD_MAX_DOLLARS || lexer->lookahead != '"' || scanner->depth == MD_MAX_DEPTH) {
        return false;
    }
    advance(lexer);
    lexer->mark_end(lexer);
    bool raw = false;
    if (lexer->lookahead == '"') {
        advance(lexer);
        if (lexer->lookahead == '"') {
            advance(lexer);
            lexer->mark_end(lexer);
            raw = true;
        }
    }
    scanner->open[scanner->depth++] = (uint8_t)(dollars | (raw ? MD_RAW : 0));
    lexer->result_symbol = MD_STRING_START;
    return true;
}

// Inside a multi-dollar string, one token per call: a template start, the end of the string, or text. Text stops
// before whatever the grammar reads itself (a template, an escape sequence, the end), and the next call returns that.
// The lexer can mark the end of a token only where it stands, so a run of `$` or `"` is measured by reading it, and
// when its leading characters are text and the rest is not, they are returned one per call.
static bool scan_multi_dollar_part(Scanner *scanner, TSLexer *lexer) {
    const uint8_t open = scanner->open[scanner->depth - 1];
    const unsigned dollars = open & MD_MAX_DOLLARS;
    const bool raw = (open & MD_RAW) != 0;
    bool has_text = false;

    for (;;) {
        if (lexer->eof(lexer)) {
            // An unterminated string: the text up to the end of the input.
            lexer->mark_end(lexer);
            lexer->result_symbol = MD_STRING_CONTENT;
            return has_text;
        }

        const int32_t c = lexer->lookahead;

        if (c == '$') {
            // The text ends before a run of dollars, unless the run is text too.
            lexer->mark_end(lexer);
            unsigned run = 0;
            if (!has_text) {
                advance(lexer);
                run = 1;
                lexer->mark_end(lexer);
            }
            while (lexer->lookahead == '$') {
                advance(lexer);
                run++;
            }
            if (run >= dollars && (lexer->lookahead == '{' || is_identifier_start(lexer->lookahead))) {
                if (has_text || run > dollars) {
                    lexer->result_symbol = MD_STRING_CONTENT;
                    return true;
                }
                const bool brace = lexer->lookahead == '{';
                if (brace) {
                    advance(lexer);
                }
                lexer->mark_end(lexer);
                lexer->result_symbol = brace ? MD_INTERPOLATION_BRACE : MD_INTERPOLATION;
                return true;
            }
            has_text = true;
            continue;
        }

        if (c == '"') {
            lexer->mark_end(lexer);
            if (!raw) {
                if (has_text) {
                    lexer->result_symbol = MD_STRING_CONTENT;
                    return true;
                }
                advance(lexer);
                lexer->mark_end(lexer);
                scanner->depth--;
                lexer->result_symbol = MD_STRING_END;
                return true;
            }
            // A triple-quoted string ends at the last three quotes of a run, the quotes before them are text.
            unsigned run = 0;
            if (!has_text) {
                advance(lexer);
                run = 1;
                lexer->mark_end(lexer);
            }
            while (lexer->lookahead == '"') {
                advance(lexer);
                run++;
            }
            if (run >= 3) {
                if (has_text || run > 3) {
                    lexer->result_symbol = MD_STRING_CONTENT;
                    return true;
                }
                lexer->mark_end(lexer);
                scanner->depth--;
                lexer->result_symbol = MD_STRING_END;
                return true;
            }
            has_text = true;
            continue;
        }

        if (!raw && (c == '\\' || c == '\n' || c == '\r')) {
            // The grammar reads an escape sequence, and a line break cannot be part of a single-line string.
            if (!has_text) {
                return false;
            }
            lexer->mark_end(lexer);
            lexer->result_symbol = MD_STRING_CONTENT;
            return true;
        }

        advance(lexer);
        has_text = true;
    }
}

bool tree_sitter_kotlin_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    // During error recovery every symbol is valid; the multi-dollar tokens are left to a normal parse, because
    // reading them there would push and pop entries for tokens that may be thrown away.
    if (!valid_symbols[ERROR_SENTINEL] && ((Scanner *)payload)->depth > 0 &&
        (valid_symbols[MD_STRING_CONTENT] || valid_symbols[MD_STRING_END])) {
        return scan_multi_dollar_part((Scanner *)payload, lexer);
    }

    if (valid_symbols[MULTILINE_STRING_CONTENT]) {
        bool did_advance = false;
        lexer->result_symbol = MULTILINE_STRING_CONTENT;
        while (!lexer->eof(lexer)) {
            switch (lexer->lookahead) {
                case '$':
                    lexer->mark_end(lexer);
                    advance(lexer);
                    if (iswalpha(lexer->lookahead) || lexer->lookahead == '{') {
                        return did_advance;
                    }
                    did_advance = true;
                    break;
                case '"':
                    lexer->mark_end(lexer);
                    // 3 or 4 quotes means we're done
                    advance(lexer);
                    if (lexer->lookahead == '"') {
                        advance(lexer);
                        if (lexer->lookahead == '"') {
                            advance(lexer);
                            if (lexer->lookahead == '"') {
                                advance(lexer);
                            }
                            return did_advance;
                        }
                    }
                    did_advance = true;
                    break;
                default:
                    advance(lexer);
                    did_advance = true;
                    break;
            }
        }
    }

    if (valid_symbols[SEMI] || valid_symbols[CLASS_MEMBER_SEMI]) {
        lexer->result_symbol = valid_symbols[SEMI] ? SEMI : CLASS_MEMBER_SEMI;
        lexer->mark_end(lexer);
        bool saw_newline = false;
        for (;;) {
            if (lexer->eof(lexer)) {
                return true;
            }

            if (lexer->lookahead == ';') {
                advance(lexer);
                lexer->mark_end(lexer);
                return true;
            }

            if (!iswspace(lexer->lookahead)) {
                break;
            }

            if (lexer->lookahead == '\n') {
                skip(lexer);
                saw_newline = true;
                break;
            }

            if (lexer->lookahead == '\r') {
                skip(lexer);

                if (lexer->lookahead == '\n') {
                    skip(lexer);
                }

                saw_newline = true;
                break;
            }

            skip(lexer);
        }

        // Skip whitespace and comments
        while (iswspace(lexer->lookahead)) {
            skip(lexer);
        }
        if (lexer->lookahead == '/') {
            goto comment;
        }

        if (!saw_newline) {
            switch (lexer->lookahead) {
                case '!':
                    skip(lexer);
                    goto continue_not_is_from_semi;
                case '?':
                    if (valid_symbols[Q_DOT]) {
                        goto q_dot_from_semi;
                    }
                    return false;
                // Inside a class body, a declaration-starting hard keyword right
                // after a complete member terminates it even without a newline:
                // the spec grammar makes `semis` optional after declarations, so
                // `class C { fun a() {} fun b() {} }` is valid Kotlin. All of these
                // are hard keywords and can never continue the preceding member
                // (e.g. as an infix function name). Restricted to CLASS_MEMBER_SEMI:
                // at statement level a soft keyword like `enum` in `enum class`
                // parses as a bare identifier first, and a speculative SEMI before
                // `class` would split the modifier from its declaration. Class
                // members cannot be bare expressions, so that hazard does not exist
                // for CLASS_MEMBER_SEMI.
                case 'c':
                case 'f':
                case 'o':
                case 't':
                case 'v':
                case 'i':
                    if (valid_symbols[CLASS_MEMBER_SEMI] && !valid_symbols[SEMI]) {
                        char decl_word[16] = {0};
                        if (scan_words(lexer,
                                       (const char[16][16]){"class", "fun", "interface", "object",
                                                            "typealias", "val", "var"},
                                       decl_word, NULL)) {
                            // Word boundary: reject identifiers that merely start
                            // with a keyword (`val2`, `fun_x`).
                            return !iswalnum(lexer->lookahead) && lexer->lookahead != '_';
                        }
                        return false;
                    }
                    if (lexer->lookahead == 'i') {
                        return scan_word(lexer, "import");
                    }
                    return false;
                case ';':
                    advance(lexer);
                    lexer->mark_end(lexer);
                    return true;
                // A closing brace terminates the current statement/member even without a
                // newline (`class A { fun m() {} }`). mark_end already sat at the start, so
                // this yields a zero-width semicolon before `}`.
                case '}':
                    return true;
                // A value can follow on the same line where a statement may also end (`return $$"..."`).
                case '$':
                    if (valid_symbols[MD_STRING_START] && !valid_symbols[ERROR_SENTINEL]) {
                        return scan_multi_dollar_start((Scanner *)payload, lexer);
                    }
                    return false;
                default:
                    return false;
            }
        }

        char scanned_word[16] = {0};
    _switch:
        switch (lexer->lookahead) {
            case ',':
            case '.':
            case ':':
            case '*':
            case '%':
            case '>':
            case '<':
            case '=':
            case '{':
            case '[':
            case '|':
            case '&':
            case '/':
                return false;
            // Insert a semicolon before `--` and `++`, but not before binary `+` or `-`.
            // Insert before +/-{float}
            case '+':
                skip(lexer);
                if (lexer->lookahead == '+') {
                    return true;
                }
                return iswdigit(lexer->lookahead);
            case '-':
                skip(lexer);
                if (lexer->lookahead == '-') {
                    return true;
                }
                return iswdigit(lexer->lookahead);
            // Don't insert a semicolon before `!=`, but do insert one before a unary `!`.
            case '!':
                skip(lexer);
                if (lexer->lookahead == 'i' && valid_symbols[NOT_IS]) {
                    skip(lexer);
                    if (lexer->lookahead == 's') {
                        skip(lexer);
                        if (!iswalnum(lexer->lookahead)) {
                            return true;
                        }
                    }
                }
                return lexer->lookahead != '=';
            case '?':
                if (valid_symbols[Q_DOT]) {
                    goto q_dot_from_semi;
                }
                return true;
            case 'e':
            case 'i':
            case 'g':
            case 's':
            case 'p':
            case 'a':
            case 'f':
            case 'o':
            case 'l':
            case 'v':
            case 'n':
            case 'c':
            case 'b':
            case 'w':
                // `actual` and `expect` can precede a primary constructor (`internal actual constructor(`), so they
                // count as modifiers there. Elsewhere they are ordinary names (`actual as T`, `expect in xs`) and
                // must not hide the word that follows them.
                while (scan_words(lexer,
                                  (const char[16][16]){"public", "private", "protected", "internal", "abstract",
                                                       "final", "open", "override", "lateinit", "vararg", "noinline",
                                                       "crossinline", "external", "suspend", "inline"},
                                  scanned_word, NULL) ||
                       (primary_constructor_may_follow(valid_symbols) &&
                        scan_words(lexer, (const char[16][16]){"actual", "expect"}, scanned_word, NULL))) {
                    memset(scanned_word, 0, MAX_WORD_SIZE);
                    while (iswspace(lexer->lookahead)) {
                        skip(lexer);
                    }
                }

                uint8_t index = -1;
                bool res = scan_words(
                    lexer,
                    (const char[16][16]){"else", "in", "instanceof", "get", "set", "constructor", "by", "as", "where"},
                    scanned_word, &index);

                // If `CLASS_MEMBER_SEMI` is valid, we found a secondary constructor and so we want to insert a semi, OR
                // we found a variable named constructor whose field is being accessed.
                // Exception: when `constructor` may still continue a class header (as after `class Options` on the
                // line above), it is that class's primary constructor. The Kotlin grammar allows a newline before a
                // primary constructor, so no semi is inserted even inside a class body.
                if (index == 5) {
                    while (iswspace(lexer->lookahead)) {
                        skip(lexer);
                    }
                    if (primary_constructor_may_follow(valid_symbols) && lexer->lookahead == '(') {
                        return false;
                    }
                    if (valid_symbols[CLASS_MEMBER_SEMI] || lexer->lookahead == '.' || lexer->lookahead == '=') {
                        return true;
                    }
                }
                // Ordinarily, we should not insert a semicolon if there is an `else` on the next line,
                // except for when it's a 'when entry', which has a `->` after the `else`.
                else if (index == 0) {
                    while (iswspace(lexer->lookahead)) {
                        skip(lexer);
                    }
                    if (lexer->lookahead == '-') {
                        skip(lexer);
                        if (lexer->lookahead == '>') {
                            return true;
                        }
                    }
                }
                // If `get` was found and the keyword is not valid, return a semi since it's being used as an identifier
                else if (index == 3 && (!valid_symbols[GET] || lexer->lookahead == '[')) {
                    return true;
                }
                // A getter is `get` alone on its line or `get()`: anything else (`get("/a") {`, `get {`,
                // `get<T>()`, `get_x`) is a call on a new statement.
                else if (index == 3) {
                    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                        skip(lexer);
                    }
                    if (lexer->lookahead == '(') {
                        skip(lexer);
                        while (iswspace(lexer->lookahead)) {
                            skip(lexer);
                        }
                        return lexer->lookahead != ')';
                    }
                    return !(lexer->lookahead == '\n' || lexer->lookahead == '\r' || lexer->lookahead == ';' ||
                             lexer->lookahead == '}' || lexer->lookahead == '/' || lexer->eof(lexer));
                }
                // If `set` was found and the keyword is not valid, return a semi since it's being used as an identifier
                else if (index == 4 && (!valid_symbols[SET] || lexer->lookahead == '[' || lexer->lookahead == '(' ||
                                        lexer->lookahead == '.')) {
                    if (lexer->lookahead == '(' && valid_symbols[SET]) {
                        return set_paren_is_call(lexer);
                    }
                    return true;
                }
                // A setter is `set` alone on its line or `set(name)`: `set {` is a call on a new statement.
                else if (index == 4) {
                    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                        skip(lexer);
                    }
                    if (lexer->lookahead == '(') {
                        return set_paren_is_call(lexer);
                    }
                    return !(lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
                             lexer->lookahead == ';' || lexer->lookahead == '}' || lexer->lookahead == '/' ||
                             lexer->eof(lexer));
                }
                // If `in` was found and this specific external keyword is valid,
                // return a semi since it's being used in a range test
                else if (index == 1 && valid_symbols[IN]) {
                    return true;
                }
                // `by` continues a declaration only when an expression follows it (`by lazy { ... }`). Followed by a
                // character that cannot start an expression it is an ordinary name at the start of a new statement
                // (`by.write(x)`, `by = 1`, `by?.f()`, `by[0]`).
                else if (index == 6) {
                    while (iswspace(lexer->lookahead)) {
                        skip(lexer);
                    }
                    switch (lexer->lookahead) {
                        case '.':
                        case '?':
                        case '=':
                        case '[':
                        case ',':
                        case ')':
                        case ']':
                        case '}':
                        case ';':
                            return true;
                        default:
                            break;
                    }
                }
                // `where` continues a declaration only as a type-constraints
                // clause; followed by `?` `.` `=` `(` it is an ordinary
                // identifier (`where?.let { ... }`) — close the statement.
                else if (index == 8) {
                    while (iswspace(lexer->lookahead)) {
                        skip(lexer);
                    }
                    if (lexer->lookahead == '?' || lexer->lookahead == '.' ||
                        lexer->lookahead == '=' || lexer->lookahead == '(') {
                        return true;
                    }
                }
                return !res;
            case ';':
                advance(lexer);
                lexer->mark_end(lexer);
                return true;
            case '@':
                // An annotation line may belong to what follows: a primary constructor, or an accessor of the
                // property above (`val size` / `@JvmName("size")` / `get() = ...`). Skip the annotation and decide
                // on the next word. Anything else starts a new declaration.
                if (valid_symbols[CONSTRUCTOR] || valid_symbols[GET] || valid_symbols[SET]) {
                    bool saw_paren = false;
                    // At EOF lookahead is 0, which is neither '\n' nor whitespace, so neither arm
                    // of this condition can end the loop; it has to stop at end of input explicitly,
                    // otherwise skip() has nothing left to consume and the condition stays true forever.
                    while (!lexer->eof(lexer) &&
                           (saw_paren ? lexer->lookahead != '\n' : !iswspace(lexer->lookahead))) {
                        skip(lexer);
                        if (lexer->lookahead == '(') {
                            saw_paren = true;
                        }
                        if (lexer->lookahead == ')') {
                            saw_paren = false;
                        }
                    }
                    while (iswspace(lexer->lookahead)) {
                        skip(lexer);
                    }
                    if (lexer->lookahead == '/') {
                        return true;
                    }
                    goto _switch;
                }
                return true;

            default:
                return true;
        }
    }

    while (iswspace(lexer->lookahead)) {
        skip(lexer);
    }

    if (valid_symbols[MD_STRING_START] && !valid_symbols[ERROR_SENTINEL] && lexer->lookahead == '$') {
        return scan_multi_dollar_start((Scanner *)payload, lexer);
    }

    if (valid_symbols[NOT_IS]) {
        if (lexer->lookahead == '!') {
            advance(lexer);
        continue_not_is_from_semi:
            if (lexer->lookahead == 'i') {
                advance(lexer);
                if (lexer->lookahead == 's') {
                    advance(lexer);
                    lexer->result_symbol = NOT_IS;
                    lexer->mark_end(lexer);
                    return !iswalnum(lexer->lookahead);
                }
            }
        }
    }

    if (valid_symbols[IN]) {
        if (lexer->lookahead == 'i') {
            advance(lexer);
            if (lexer->lookahead == 'n') {
                advance(lexer);
                lexer->result_symbol = IN;
                lexer->mark_end(lexer);
                return !iswalnum(lexer->lookahead);
            }
        }
    }

q_dot_from_semi:
    if (valid_symbols[Q_DOT]) {
        while (iswspace(lexer->lookahead)) {
            skip(lexer);
        }
        if (lexer->lookahead == '?') {
            advance(lexer);
            while (iswspace(lexer->lookahead)) {
                skip(lexer);
            }
            if (lexer->lookahead == '.') {
                advance(lexer);
                lexer->result_symbol = Q_DOT;
                lexer->mark_end(lexer);
                return true;
            }
        }
    }

comment:
    if (valid_symbols[DOLLAR]) {
        return false;
    }

    if (lexer->lookahead == '/') {
        advance(lexer);
        if (lexer->lookahead != '*') {
            return false;
        }
        advance(lexer);

        bool after_star = false;
        unsigned nesting_depth = 1;
        for (;;) {
            switch (lexer->lookahead) {
                case '\0':
                    // A NUL byte inside the comment is ordinary text; only the end of the input ends the scan.
                    if (lexer->eof(lexer)) {
                        return false;
                    }
                    advance(lexer);
                    after_star = false;
                    break;
                case '*':
                    advance(lexer);
                    after_star = true;
                    break;
                case '/':
                    if (after_star) {
                        advance(lexer);
                        after_star = false;
                        nesting_depth--;
                        if (nesting_depth == 0) {
                            lexer->result_symbol = BLOCK_COMMENT;
                            lexer->mark_end(lexer);
                            return true;
                        }
                    } else {
                        advance(lexer);
                        after_star = false;
                        if (lexer->lookahead == '*') {
                            nesting_depth++;
                            advance(lexer);
                        }
                    }
                    break;
                default:
                    advance(lexer);
                    after_star = false;
                    break;
            }
        }
    }

    return false;
}
