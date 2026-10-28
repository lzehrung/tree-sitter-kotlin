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
//
// Only the last dollars of a run that ends in a template start it; the dollars before them are text. Likewise the last
// three quotes of a run end a triple-quoted string and the quotes before them are text. The lexer can mark the end of
// a token only where it stands, so a call that has read the whole run cannot end a token before the run's last
// characters. It returns the first character alone and keeps in `rest` how many text characters follow it, and the
// next call returns those as one token without reading the run again. A run of n characters then takes O(n) work,
// where one character per call would read the rest of the run again every time: O(n^2).
#define MD_MAX_DEPTH 32
#define MD_RAW 0x80
#define MD_MAX_DOLLARS 0x7f

typedef struct {
    uint8_t depth;
    uint8_t open[MD_MAX_DEPTH];
    uint32_t rest;
} Scanner;

void *tree_sitter_kotlin_external_scanner_create() { return ts_calloc(1, sizeof(Scanner)); }

void tree_sitter_kotlin_external_scanner_destroy(void *payload) { ts_free(payload); }

// The serialized state is the number of open strings, their entries and, between the two tokens of a run, `rest`. It is
// empty outside a multi-dollar string.
unsigned tree_sitter_kotlin_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    if (scanner->depth == 0) {
        return 0;
    }
    unsigned length = 0;
    buffer[length++] = (char)scanner->depth;
    memcpy(buffer + length, scanner->open, scanner->depth);
    length += scanner->depth;
    if (scanner->rest > 0) {
        memcpy(buffer + length, &scanner->rest, sizeof(scanner->rest));
        length += sizeof(scanner->rest);
    }
    return length;
}

void tree_sitter_kotlin_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->depth = 0;
    scanner->rest = 0;
    if (length == 0) {
        return;
    }
    const uint8_t depth = (uint8_t)buffer[0];
    if (depth > MD_MAX_DEPTH || length < 1u + depth) {
        return;
    }
    memcpy(scanner->open, buffer + 1, depth);
    scanner->depth = depth;
    if (length >= 1u + depth + sizeof(scanner->rest)) {
        memcpy(&scanner->rest, buffer + 1 + depth, sizeof(scanner->rest));
    }
}

// The letters above ASCII that start a template name: the characters the generated `identifier` token accepts as
// its first character, read from the lexer in parser.c one code point at a time. `cargo test` checks the table
// against the lexer and prints a replacement when they differ. It is not the whole Unicode letter class. The
// tree-sitter 0.24.6 generator that wrote parser.c keeps only the end points of the large ranges (CJK ideographs,
// Hangul syllables) and nothing above U+10915, and a table with those letters would start a template that the
// grammar then fails to read. ASCII is handled in `is_identifier_start`.
// BEGIN LETTER RANGES
static const uint32_t LETTER_RANGES[][2] = {
    {0xaa, 0xaa}, {0xb5, 0xb5}, {0xba, 0xba}, {0xc0, 0xd6}, {0xd8, 0xf6},
    {0xf8, 0x2c1}, {0x2c6, 0x2d1}, {0x2e0, 0x2e4}, {0x2ec, 0x2ec}, {0x2ee, 0x2ee},
    {0x370, 0x374}, {0x376, 0x377}, {0x37a, 0x37d}, {0x37f, 0x37f}, {0x386, 0x386},
    {0x388, 0x38a}, {0x38c, 0x38c}, {0x38e, 0x3a1}, {0x3a3, 0x3f5}, {0x3f7, 0x481},
    {0x48a, 0x52f}, {0x531, 0x556}, {0x559, 0x559}, {0x560, 0x588}, {0x5d0, 0x5ea},
    {0x5ef, 0x5f2}, {0x620, 0x64a}, {0x66e, 0x66f}, {0x671, 0x6d3}, {0x6d5, 0x6d5},
    {0x6e5, 0x6e6}, {0x6ee, 0x6ef}, {0x6fa, 0x6fc}, {0x6ff, 0x6ff}, {0x710, 0x710},
    {0x712, 0x72f}, {0x74d, 0x7a5}, {0x7b1, 0x7b1}, {0x7ca, 0x7ea}, {0x7f4, 0x7f5},
    {0x7fa, 0x7fa}, {0x800, 0x815}, {0x81a, 0x81a}, {0x824, 0x824}, {0x828, 0x828},
    {0x840, 0x858}, {0x860, 0x86a}, {0x870, 0x887}, {0x889, 0x88e}, {0x8a0, 0x8c9},
    {0x904, 0x939}, {0x93d, 0x93d}, {0x950, 0x950}, {0x958, 0x961}, {0x971, 0x980},
    {0x985, 0x98c}, {0x98f, 0x990}, {0x993, 0x9a8}, {0x9aa, 0x9b0}, {0x9b2, 0x9b2},
    {0x9b6, 0x9b9}, {0x9bd, 0x9bd}, {0x9ce, 0x9ce}, {0x9dc, 0x9dd}, {0x9df, 0x9e1},
    {0x9f0, 0x9f1}, {0x9fc, 0x9fc}, {0xa05, 0xa0a}, {0xa0f, 0xa10}, {0xa13, 0xa28},
    {0xa2a, 0xa30}, {0xa32, 0xa33}, {0xa35, 0xa36}, {0xa38, 0xa39}, {0xa59, 0xa5c},
    {0xa5e, 0xa5e}, {0xa72, 0xa74}, {0xa85, 0xa8d}, {0xa8f, 0xa91}, {0xa93, 0xaa8},
    {0xaaa, 0xab0}, {0xab2, 0xab3}, {0xab5, 0xab9}, {0xabd, 0xabd}, {0xad0, 0xad0},
    {0xae0, 0xae1}, {0xaf9, 0xaf9}, {0xb05, 0xb0c}, {0xb0f, 0xb10}, {0xb13, 0xb28},
    {0xb2a, 0xb30}, {0xb32, 0xb33}, {0xb35, 0xb39}, {0xb3d, 0xb3d}, {0xb5c, 0xb5d},
    {0xb5f, 0xb61}, {0xb71, 0xb71}, {0xb83, 0xb83}, {0xb85, 0xb8a}, {0xb8e, 0xb90},
    {0xb92, 0xb95}, {0xb99, 0xb9a}, {0xb9c, 0xb9c}, {0xb9e, 0xb9f}, {0xba3, 0xba4},
    {0xba8, 0xbaa}, {0xbae, 0xbb9}, {0xbd0, 0xbd0}, {0xc05, 0xc0c}, {0xc0e, 0xc10},
    {0xc12, 0xc28}, {0xc2a, 0xc39}, {0xc3d, 0xc3d}, {0xc58, 0xc5a}, {0xc5d, 0xc5d},
    {0xc60, 0xc61}, {0xc80, 0xc80}, {0xc85, 0xc8c}, {0xc8e, 0xc90}, {0xc92, 0xca8},
    {0xcaa, 0xcb3}, {0xcb5, 0xcb9}, {0xcbd, 0xcbd}, {0xcdd, 0xcde}, {0xce0, 0xce1},
    {0xcf1, 0xcf2}, {0xd04, 0xd0c}, {0xd0e, 0xd10}, {0xd12, 0xd3a}, {0xd3d, 0xd3d},
    {0xd4e, 0xd4e}, {0xd54, 0xd56}, {0xd5f, 0xd61}, {0xd7a, 0xd7f}, {0xd85, 0xd96},
    {0xd9a, 0xdb1}, {0xdb3, 0xdbb}, {0xdbd, 0xdbd}, {0xdc0, 0xdc6}, {0xe01, 0xe30},
    {0xe32, 0xe33}, {0xe40, 0xe46}, {0xe81, 0xe82}, {0xe84, 0xe84}, {0xe86, 0xe8a},
    {0xe8c, 0xea3}, {0xea5, 0xea5}, {0xea7, 0xeb0}, {0xeb2, 0xeb3}, {0xebd, 0xebd},
    {0xec0, 0xec4}, {0xec6, 0xec6}, {0xedc, 0xedf}, {0xf00, 0xf00}, {0xf40, 0xf47},
    {0xf49, 0xf6c}, {0xf88, 0xf8c}, {0x1000, 0x102a}, {0x103f, 0x103f}, {0x1050, 0x1055},
    {0x105a, 0x105d}, {0x1061, 0x1061}, {0x1065, 0x1066}, {0x106e, 0x1070}, {0x1075, 0x1081},
    {0x108e, 0x108e}, {0x10a0, 0x10c5}, {0x10c7, 0x10c7}, {0x10cd, 0x10cd}, {0x10d0, 0x10fa},
    {0x10fc, 0x1248}, {0x124a, 0x124d}, {0x1250, 0x1256}, {0x1258, 0x1258}, {0x125a, 0x125d},
    {0x1260, 0x1288}, {0x128a, 0x128d}, {0x1290, 0x12b0}, {0x12b2, 0x12b5}, {0x12b8, 0x12be},
    {0x12c0, 0x12c0}, {0x12c2, 0x12c5}, {0x12c8, 0x12d6}, {0x12d8, 0x1310}, {0x1312, 0x1315},
    {0x1318, 0x135a}, {0x1380, 0x138f}, {0x13a0, 0x13f5}, {0x13f8, 0x13fd}, {0x1401, 0x166c},
    {0x166f, 0x167f}, {0x1681, 0x169a}, {0x16a0, 0x16ea}, {0x16f1, 0x16f8}, {0x1700, 0x1711},
    {0x171f, 0x1731}, {0x1740, 0x1751}, {0x1760, 0x176c}, {0x176e, 0x1770}, {0x1780, 0x17b3},
    {0x17d7, 0x17d7}, {0x17dc, 0x17dc}, {0x1820, 0x1878}, {0x1880, 0x1884}, {0x1887, 0x18a8},
    {0x18aa, 0x18aa}, {0x18b0, 0x18f5}, {0x1900, 0x191e}, {0x1950, 0x196d}, {0x1970, 0x1974},
    {0x1980, 0x19ab}, {0x19b0, 0x19c9}, {0x1a00, 0x1a16}, {0x1a20, 0x1a54}, {0x1aa7, 0x1aa7},
    {0x1b05, 0x1b33}, {0x1b45, 0x1b4c}, {0x1b83, 0x1ba0}, {0x1bae, 0x1baf}, {0x1bba, 0x1be5},
    {0x1c00, 0x1c23}, {0x1c4d, 0x1c4f}, {0x1c5a, 0x1c7d}, {0x1c80, 0x1c88}, {0x1c90, 0x1cba},
    {0x1cbd, 0x1cbf}, {0x1ce9, 0x1cec}, {0x1cee, 0x1cf3}, {0x1cf5, 0x1cf6}, {0x1cfa, 0x1cfa},
    {0x1d00, 0x1dbf}, {0x1e00, 0x1f15}, {0x1f18, 0x1f1d}, {0x1f20, 0x1f45}, {0x1f48, 0x1f4d},
    {0x1f50, 0x1f57}, {0x1f59, 0x1f59}, {0x1f5b, 0x1f5b}, {0x1f5d, 0x1f5d}, {0x1f5f, 0x1f7d},
    {0x1f80, 0x1fb4}, {0x1fb6, 0x1fbc}, {0x1fbe, 0x1fbe}, {0x1fc2, 0x1fc4}, {0x1fc6, 0x1fcc},
    {0x1fd0, 0x1fd3}, {0x1fd6, 0x1fdb}, {0x1fe0, 0x1fec}, {0x1ff2, 0x1ff4}, {0x1ff6, 0x1ffc},
    {0x2071, 0x2071}, {0x207f, 0x207f}, {0x2090, 0x209c}, {0x2102, 0x2102}, {0x2107, 0x2107},
    {0x210a, 0x2113}, {0x2115, 0x2115}, {0x2119, 0x211d}, {0x2124, 0x2124}, {0x2126, 0x2126},
    {0x2128, 0x2128}, {0x212a, 0x212d}, {0x212f, 0x2139}, {0x213c, 0x213f}, {0x2145, 0x2149},
    {0x214e, 0x214e}, {0x2183, 0x2184}, {0x2c00, 0x2ce4}, {0x2ceb, 0x2cee}, {0x2cf2, 0x2cf3},
    {0x2d00, 0x2d25}, {0x2d27, 0x2d27}, {0x2d2d, 0x2d2d}, {0x2d30, 0x2d67}, {0x2d6f, 0x2d6f},
    {0x2d80, 0x2d96}, {0x2da0, 0x2da6}, {0x2da8, 0x2dae}, {0x2db0, 0x2db6}, {0x2db8, 0x2dbe},
    {0x2dc0, 0x2dc6}, {0x2dc8, 0x2dce}, {0x2dd0, 0x2dd6}, {0x2dd8, 0x2dde}, {0x2e2f, 0x2e2f},
    {0x3005, 0x3006}, {0x3031, 0x3035}, {0x303b, 0x303c}, {0x3041, 0x3096}, {0x309d, 0x309f},
    {0x30a1, 0x30fa}, {0x30fc, 0x30ff}, {0x3105, 0x312f}, {0x3131, 0x318e}, {0x31a0, 0x31bf},
    {0x31f0, 0x31ff}, {0x3400, 0x3400}, {0x4dbf, 0x4dbf}, {0x4e00, 0x4e00}, {0x9fff, 0xa48c},
    {0xa4d0, 0xa4fd}, {0xa500, 0xa60c}, {0xa610, 0xa61f}, {0xa62a, 0xa62b}, {0xa640, 0xa66e},
    {0xa67f, 0xa69d}, {0xa6a0, 0xa6e5}, {0xa717, 0xa71f}, {0xa722, 0xa788}, {0xa78b, 0xa7ca},
    {0xa7d0, 0xa7d1}, {0xa7d3, 0xa7d3}, {0xa7d5, 0xa7d9}, {0xa7f2, 0xa801}, {0xa803, 0xa805},
    {0xa807, 0xa80a}, {0xa80c, 0xa822}, {0xa840, 0xa873}, {0xa882, 0xa8b3}, {0xa8f2, 0xa8f7},
    {0xa8fb, 0xa8fb}, {0xa8fd, 0xa8fe}, {0xa90a, 0xa925}, {0xa930, 0xa946}, {0xa960, 0xa97c},
    {0xa984, 0xa9b2}, {0xa9cf, 0xa9cf}, {0xa9e0, 0xa9e4}, {0xa9e6, 0xa9ef}, {0xa9fa, 0xa9fe},
    {0xaa00, 0xaa28}, {0xaa40, 0xaa42}, {0xaa44, 0xaa4b}, {0xaa60, 0xaa76}, {0xaa7a, 0xaa7a},
    {0xaa7e, 0xaaaf}, {0xaab1, 0xaab1}, {0xaab5, 0xaab6}, {0xaab9, 0xaabd}, {0xaac0, 0xaac0},
    {0xaac2, 0xaac2}, {0xaadb, 0xaadd}, {0xaae0, 0xaaea}, {0xaaf2, 0xaaf4}, {0xab01, 0xab06},
    {0xab09, 0xab0e}, {0xab11, 0xab16}, {0xab20, 0xab26}, {0xab28, 0xab2e}, {0xab30, 0xab5a},
    {0xab5c, 0xab69}, {0xab70, 0xabe2}, {0xac00, 0xac00}, {0xd7a3, 0xd7a3}, {0xd7b0, 0xd7c6},
    {0xd7cb, 0xd7fb}, {0xf900, 0xfa6d}, {0xfa70, 0xfad9}, {0xfb00, 0xfb06}, {0xfb13, 0xfb17},
    {0xfb1d, 0xfb1d}, {0xfb1f, 0xfb28}, {0xfb2a, 0xfb36}, {0xfb38, 0xfb3c}, {0xfb3e, 0xfb3e},
    {0xfb40, 0xfb41}, {0xfb43, 0xfb44}, {0xfb46, 0xfbb1}, {0xfbd3, 0xfd3d}, {0xfd50, 0xfd8f},
    {0xfd92, 0xfdc7}, {0xfdf0, 0xfdfb}, {0xfe70, 0xfe74}, {0xfe76, 0xfefc}, {0xff21, 0xff3a},
    {0xff41, 0xff5a}, {0xff66, 0xffbe}, {0xffc2, 0xffc7}, {0xffca, 0xffcf}, {0xffd2, 0xffd7},
    {0xffda, 0xffdc}, {0x10000, 0x1000b}, {0x1000d, 0x10026}, {0x10028, 0x1003a}, {0x1003c, 0x1003d},
    {0x1003f, 0x1004d}, {0x10050, 0x1005d}, {0x10080, 0x100fa}, {0x10280, 0x1029c}, {0x102a0, 0x102d0},
    {0x10300, 0x1031f}, {0x1032d, 0x10340}, {0x10342, 0x10349}, {0x10350, 0x10375}, {0x10380, 0x1039d},
    {0x103a0, 0x103c3}, {0x103c8, 0x103cf}, {0x10400, 0x1049d}, {0x104b0, 0x104d3}, {0x104d8, 0x104fb},
    {0x10500, 0x10527}, {0x10530, 0x10563}, {0x10570, 0x1057a}, {0x1057c, 0x1058a}, {0x1058c, 0x10592},
    {0x10594, 0x10595}, {0x10597, 0x105a1}, {0x105a3, 0x105b1}, {0x105b3, 0x105b9}, {0x105bb, 0x105bc},
    {0x10600, 0x10736}, {0x10740, 0x10755}, {0x10760, 0x10767}, {0x10780, 0x10785}, {0x10787, 0x107b0},
    {0x107b2, 0x107ba}, {0x10800, 0x10805}, {0x10808, 0x10808}, {0x1080a, 0x10835}, {0x10837, 0x10838},
    {0x1083c, 0x1083c}, {0x1083f, 0x10855}, {0x10860, 0x10876}, {0x10880, 0x1089e}, {0x108e0, 0x108f2},
    {0x108f4, 0x108f5}, {0x10900, 0x10915},
};
// END LETTER RANGES

static bool is_letter(uint32_t c) {
    size_t low = 0;
    size_t high = sizeof(LETTER_RANGES) / sizeof(LETTER_RANGES[0]);
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (c < LETTER_RANGES[middle][0]) {
            high = middle;
        } else if (c > LETTER_RANGES[middle][1]) {
            low = middle + 1;
        } else {
            return true;
        }
    }
    return false;
}

// The first character of a name after the dollars of a template: `_` or a letter. It gives the same answer on every
// platform, which `iswalpha` does not: that depends on the C library and its locale.
static inline bool is_identifier_start(int32_t c) {
    if (c < 0x80) {
        return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }
    return is_letter((uint32_t)c);
}

// With the opening backtick of a quoted name read: true when the name is complete, one or more characters that are
// not a backtick or a line break and then the closing backtick. Kotlin reads `$` and a quoted name as a template, and
// the grammar's `identifier` token reads the same text. A name that does not close is read once per line, because a
// later backtick on the line would close it.
static bool quoted_name_follows(TSLexer *lexer) {
    unsigned length = 0;
    while (!lexer->eof(lexer) && lexer->lookahead != '`' && lexer->lookahead != '\n' && lexer->lookahead != '\r') {
        advance(lexer);
        length++;
    }
    return length > 0 && lexer->lookahead == '`';
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
// when its leading characters are text and the rest is not, the first is returned alone and the others, counted in
// `rest`, by the next call.
static bool scan_multi_dollar_part(Scanner *scanner, TSLexer *lexer) {
    const uint8_t open = scanner->open[scanner->depth - 1];
    const unsigned dollars = open & MD_MAX_DOLLARS;
    const bool raw = (open & MD_RAW) != 0;
    bool has_text = false;

    if (scanner->rest > 0) {
        // The others of a run whose first character was just returned. The run was measured then and they are all text,
        // so they are one token. If the next character is neither `$` nor `"` the state does not fit the text, and
        // the call reads on as usual.
        const int32_t kind = lexer->lookahead;
        uint32_t rest = scanner->rest;
        scanner->rest = 0;
        if (kind == '$' || kind == '"') {
            while (rest > 0 && lexer->lookahead == kind) {
                advance(lexer);
                rest--;
            }
            lexer->mark_end(lexer);
            lexer->result_symbol = MD_STRING_CONTENT;
            return true;
        }
    }

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
            const bool quoted = lexer->lookahead == '`';
            if (run >= dollars && (lexer->lookahead == '{' || quoted || is_identifier_start(lexer->lookahead))) {
                if (has_text || run > dollars) {
                    // The text before the dollars, or the dollars before the last ones, ends here. Those dollars are
                    // text: the first is the token (the end was marked after it), the next call returns the others.
                    if (!has_text) {
                        scanner->rest = run - dollars - 1;
                    }
                    lexer->result_symbol = MD_STRING_CONTENT;
                    return true;
                }
                if (quoted) {
                    // A quoted name is a template only when it closes on the line. Either way the token is the
                    // dollars, so its end is marked before the name is read.
                    lexer->mark_end(lexer);
                    advance(lexer);
                    lexer->result_symbol = quoted_name_follows(lexer) ? MD_INTERPOLATION : MD_STRING_CONTENT;
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
                    // The text before the quotes, or the quotes before the last three, ends here; as for dollars.
                    if (!has_text) {
                        scanner->rest = run - 3 - 1;
                    }
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
    ((Scanner *)payload)->rest = 0;

    if (valid_symbols[MULTILINE_STRING_CONTENT]) {
        bool did_advance = false;
        lexer->result_symbol = MULTILINE_STRING_CONTENT;
        while (!lexer->eof(lexer)) {
            switch (lexer->lookahead) {
                case '$':
                    lexer->mark_end(lexer);
                    advance(lexer);
                    if (lexer->lookahead == '{' || is_identifier_start(lexer->lookahead)) {
                        return did_advance;
                    }
                    if (lexer->lookahead == '`') {
                        // A quoted name after the dollar is a template when it closes on the line. After text, the
                        // text ends here and the next call decides.
                        if (did_advance) {
                            return true;
                        }
                        advance(lexer);
                        lexer->mark_end(lexer);
                        // A template is left to the grammar, which reads the dollar and the name; otherwise the
                        // dollar and the backtick are text.
                        return !quoted_name_follows(lexer);
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
