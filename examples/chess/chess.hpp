#pragma once
// chess.hpp — the complete rules of chess on a 0x88 board.
//
// Ported from amanmprojects/chess (browser chess in vanilla JS): the same
// board representation, packed move encoding, perft-verified move generation
// (castling, en passant, promotion, pins, discovered checks) and the full set
// of draw rules (fifty-move, threefold repetition, insufficient material).
//
// Header-only and engine-free on purpose: the 09_chess example includes it for
// gameplay and tests/test_chess.cpp includes it for the headless perft suite.
//
// Board: 128 bytes, `sq = rank * 16 + file`, a1 = 0, h8 = 119. Squares with
// the 0x88 bit set do not exist. A piece is `color << 3 | type`.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace chess {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr int WHITE = 0;
constexpr int BLACK = 1;

enum PieceType : int { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };

/** A piece is `color << 3 | type`, so a white pawn is 1 and a black rook 12. */
constexpr int piece(int color, int type) { return (color << 3) | type; }
constexpr int colorOf(int p) { return p >> 3; }
constexpr int typeOf(int p) { return p & 7; }

inline constexpr const char* START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Castling-rights bitmask.
enum CastlingRights : int { CR_WK = 1, CR_WQ = 2, CR_BK = 4, CR_BQ = 8 };

// Move flags.
enum MoveFlags : int {
    FLAG_CAPTURE = 1,
    FLAG_EP = 2,
    FLAG_DOUBLE = 4,
    FLAG_KCASTLE = 8,
    FLAG_QCASTLE = 16,
    FLAG_PROMO = 32,
};

// ---------------------------------------------------------------------------
// Square helpers
// ---------------------------------------------------------------------------

constexpr bool onBoard(int sq) { return (sq & 0x88) == 0; }
constexpr int rankOf(int sq) { return sq >> 4; }
constexpr int fileOf(int sq) { return sq & 15; }
/** Square index used by 8x8 tables (0 = a1 … 63 = h8). */
constexpr int to64(int sq) { return rankOf(sq) * 8 + (sq & 7); }
constexpr int from64(int i) { return (i / 8) * 16 + (i % 8); }
/** True if the square is light-coloured (a1 is dark). */
constexpr bool isLightSquare(int sq) { return ((rankOf(sq) + fileOf(sq)) & 1) == 1; }

/** "e4"-style name of a square. */
inline std::string algebraic(int sq) {
    return std::string(1, (char)('a' + fileOf(sq))) + (char)('1' + rankOf(sq));
}

/** Square index for "e4"; -1 when the text is not a square. */
inline int squareFromAlgebraic(std::string_view s) {
    if (s.size() < 2) return -1;
    const int file = s[0] - 'a';
    const int rank = s[1] - '1';
    if (file < 0 || file > 7 || rank < 0 || rank > 7) return -1;
    return rank * 16 + file;
}

// ---------------------------------------------------------------------------
// Move encoding — packed into one 32-bit integer
//   bits  0.. 6  from square
//   bits  7..13  to square
//   bits 14..16  promotion piece type (0 = none)
//   bits 17..22  flags
// ---------------------------------------------------------------------------

using Move = uint32_t;
/** A move that is never legal: from and to are both a1. Used as "no move". */
inline constexpr Move MOVE_NONE = 0;

constexpr Move encodeMove(int from, int to, int promo = 0, int flags = 0) {
    return (Move)from | ((Move)to << 7) | ((Move)promo << 14) | ((Move)flags << 17);
}
constexpr int moveFrom(Move m) { return (int)(m & 0x7f); }
constexpr int moveTo(Move m) { return (int)((m >> 7) & 0x7f); }
constexpr int movePromo(Move m) { return (int)((m >> 14) & 7); }
constexpr int moveFlags(Move m) { return (int)((m >> 17) & 0x3f); }

/** Long algebraic / UCI form, e.g. "e2e4" or "e7e8q". */
inline std::string moveToUci(Move m) {
    static constexpr char kLetters[] = " pnbrqk";
    std::string s = algebraic(moveFrom(m)) + algebraic(moveTo(m));
    if (movePromo(m)) s += kLetters[movePromo(m)];
    return s;
}

/** 'p'..'k' for a piece type; '?' when out of range. */
constexpr char pieceLetter(int type) {
    switch (type) {
        case PAWN: return 'p';
        case KNIGHT: return 'n';
        case BISHOP: return 'b';
        case ROOK: return 'r';
        case QUEEN: return 'q';
        case KING: return 'k';
        default: return '?';
    }
}
constexpr int pieceFromLetter(char ch) {
    switch (ch) {
        case 'p': case 'P': return PAWN;
        case 'n': case 'N': return KNIGHT;
        case 'b': case 'B': return BISHOP;
        case 'r': case 'R': return ROOK;
        case 'q': case 'Q': return QUEEN;
        case 'k': case 'K': return KING;
        default: return EMPTY;
    }
}

// ---------------------------------------------------------------------------
// Fixed-capacity move list (no per-node allocation in the search)
// ---------------------------------------------------------------------------

struct MoveList {
    static constexpr int CAP = 512; // generous: the legal maximum is 218
    std::array<Move, CAP> moves{};
    int count = 0;

    void push(Move m) {
        if (count < CAP) moves[(size_t)count++] = m;
    }
    int size() const { return count; }
    bool empty() const { return count == 0; }
    Move operator[](int i) const { return moves[(size_t)i]; }
    Move* begin() { return moves.data(); }
    Move* end() { return moves.data() + count; }
    const Move* begin() const { return moves.data(); }
    const Move* end() const { return moves.data() + count; }
    bool contains(Move m) const {
        for (int i = 0; i < count; ++i)
            if (moves[(size_t)i] == m) return true;
        return false;
    }
};

// ---------------------------------------------------------------------------
// Zobrist hashing (64-bit, deterministic, no coordination with the JS build)
// ---------------------------------------------------------------------------

namespace detail {

struct Zobrist {
    std::array<std::array<uint64_t, 128>, 16> pieceSquare{};
    std::array<uint64_t, 16> castle{};
    std::array<uint64_t, 8> epFile{};
    uint64_t side = 0;

    Zobrist() {
        uint64_t x = 0x9E3779B97F4A7C15ull;
        auto next = [&x]() { // splitmix64
            x += 0x9E3779B97F4A7C15ull;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        };
        for (auto& row : pieceSquare)
            for (auto& v : row) v = next();
        for (auto& v : castle) v = next();
        for (auto& v : epFile) v = next();
        side = next();
    }
};

inline const Zobrist& zobrist() {
    static const Zobrist z;
    return z;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Position
// ---------------------------------------------------------------------------

class Position {
public:
    struct Status {
        bool over = false;
        const char* result = nullptr; // "1-0", "0-1", "1/2-1/2" or null
        const char* reason = nullptr; // "checkmate", "stalemate", …
    };

    /** One entry per applied move, enough to undo it exactly. */
    struct HistoryEntry {
        Move move = MOVE_NONE;
        int captured = EMPTY;
        int capturedSq = -1;
        int castling = 0;
        int ep = -1;
        int halfmoves = 0;
        uint64_t hash = 0;
        int kingSq = -1;
        bool isNull = false;
    };

    Position() { load(START_FEN); }
    explicit Position(std::string_view fen) { load(fen); }

    /** Parse a FEN. Returns false (and leaves the position untouched) if bad. */
    bool load(std::string_view fen);
    std::string fen() const;

    // --- queries -----------------------------------------------------------
    int at(int sq) const { return board_[(size_t)sq]; }
    int turn() const { return turn_; }
    int castling() const { return castling_; }
    int ep() const { return ep_; }
    int halfmoves() const { return halfmoves_; }
    int fullmoves() const { return fullmoves_; }
    uint64_t hash() const { return hash_; }
    int kingSquare(int color) const { return kings_[(size_t)color]; }
    const std::vector<uint64_t>& positions() const { return positions_; } // repetition keys
    const std::vector<HistoryEntry>& history() const { return history_; } // raw undo entries

    /** True if `color` attacks `sq` (pins are ignored — that is what "attacks" means). */
    bool isSquareAttacked(int sq, int color) const;
    /** Is `color` (-1 = side to move) currently in check? */
    bool inCheck(int color = -1) const {
        const int c = color < 0 ? turn_ : color;
        return isSquareAttacked(kings_[(size_t)c], c ^ 1);
    }

    // --- move generation ---------------------------------------------------
    /** Legal (or pseudo-legal) moves; optionally restricted to one square. */
    MoveList generateMoves(bool legal = true, int square = -1, bool capturesOnly = false);

    // --- make / unmake -----------------------------------------------------
    void makeMove(Move m);
    Move undoMove();
    void makeNullMove();
    void undoNullMove();

    // --- playing moves (with repetition bookkeeping) -----------------------
    /** Play a move given as SAN, UCI or an encoded Move. False when illegal. */
    bool play(const std::string& text, Move* played = nullptr);
    bool playMove(Move m, Move* played = nullptr);
    /** Undo the last played move. False when there is nothing to undo. */
    bool undoPlayed();
    void reset() { load(START_FEN); }

    /** Turn SAN/UCI text into a legal encoded move, or MOVE_NONE. */
    Move resolve(const std::string& text);
    /** Standard Algebraic Notation for a legal move in this position. */
    std::string toSan(Move m);

    // --- game state --------------------------------------------------------
    bool isCheckmate() { return inCheck() && generateMoves().empty(); }
    bool isStalemate() { return !inCheck() && generateMoves().empty(); }
    bool isFiftyMoveDraw() const { return halfmoves_ >= 100; }
    bool isThreefoldRepetition() const;
    bool isInsufficientMaterial() const;
    Status status();

    // --- correctness yardstick ---------------------------------------------
    /** Leaf-node count of the legal move tree; the position is restored. */
    uint64_t perft(int depth);

private:
    void computeHash();
    std::string disambiguate(Move m, int type, int from, int to);
    std::string withCheckSuffix(Move m, std::string san);

    std::array<int8_t, 128> board_{};
    std::array<int, 2> kings_{-1, -1};
    int turn_ = WHITE;
    int castling_ = 0;
    int ep_ = -1;
    int halfmoves_ = 0;
    int fullmoves_ = 1;
    uint64_t hash_ = 0;
    std::vector<HistoryEntry> history_;
    std::vector<uint64_t> positions_; // Zobrist keys of every played position
};

// ---------------------------------------------------------------------------
// FEN
// ---------------------------------------------------------------------------

inline bool Position::load(std::string_view fen) {
    std::array<int8_t, 128> board{};
    std::array<int, 2> kings{-1, -1};

    // Split into up to six whitespace-separated fields.
    std::string parts[6];
    int count = 0;
    size_t i = 0;
    while (i < fen.size() && count < 6) {
        while (i < fen.size() && (fen[i] == ' ' || fen[i] == '\t')) ++i;
        size_t start = i;
        while (i < fen.size() && fen[i] != ' ' && fen[i] != '\t') ++i;
        if (i > start) parts[count++] = std::string(fen.substr(start, i - start));
    }
    if (count < 4) return false;

    int sq = 112; // a8
    for (char ch : parts[0]) {
        if (ch == '/') {
            sq -= 24;
        } else if (ch >= '1' && ch <= '8') {
            sq += ch - '0';
        } else {
            const int type = pieceFromLetter(ch);
            if (type == EMPTY || !onBoard(sq)) return false;
            const int color = (ch >= 'A' && ch <= 'Z') ? WHITE : BLACK;
            board[(size_t)sq] = (int8_t)piece(color, type);
            if (type == KING) {
                if (kings[(size_t)color] >= 0) return false;
                kings[(size_t)color] = sq;
            }
            ++sq;
        }
    }
    // A position needs both kings: every check query below assumes they exist.
    if (kings[0] < 0 || kings[1] < 0) return false;

    // Commit.
    board_ = board;
    kings_ = kings;
    turn_ = parts[1] == "b" ? BLACK : WHITE;

    castling_ = 0;
    if (parts[2].find('K') != std::string::npos) castling_ |= CR_WK;
    if (parts[2].find('Q') != std::string::npos) castling_ |= CR_WQ;
    if (parts[2].find('k') != std::string::npos) castling_ |= CR_BK;
    if (parts[2].find('q') != std::string::npos) castling_ |= CR_BQ;

    ep_ = parts[3] == "-" ? -1 : squareFromAlgebraic(parts[3]);
    halfmoves_ = count > 4 ? std::atoi(parts[4].c_str()) : 0;
    fullmoves_ = count > 5 ? std::atoi(parts[5].c_str()) : 1;

    history_.clear();
    computeHash();
    positions_.assign(1, hash_);
    return true;
}

inline std::string Position::fen() const {
    std::string placement;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const int p = board_[(size_t)(rank * 16 + file)];
            if (p == EMPTY) {
                ++empty;
                continue;
            }
            if (empty) {
                placement += (char)('0' + empty);
                empty = 0;
            }
            char letter = pieceLetter(typeOf(p));
            placement += colorOf(p) == WHITE ? (char)(letter - 32) : letter;
        }
        if (empty) placement += (char)('0' + empty);
        if (rank > 0) placement += '/';
    }

    std::string rights;
    if (castling_ & CR_WK) rights += 'K';
    if (castling_ & CR_WQ) rights += 'Q';
    if (castling_ & CR_BK) rights += 'k';
    if (castling_ & CR_BQ) rights += 'q';

    return placement + ' ' + (turn_ == WHITE ? "w" : "b") + ' ' +
           (rights.empty() ? "-" : rights) + ' ' +
           (ep_ >= 0 ? algebraic(ep_) : "-") + ' ' + std::to_string(halfmoves_) +
           ' ' + std::to_string(fullmoves_);
}

// ---------------------------------------------------------------------------
// Attacks
// ---------------------------------------------------------------------------

namespace detail {
// Offsets are relative to the square under attack.
inline constexpr int kKnightOffsets[8] = {33, 31, 18, 14, -14, -18, -31, -33};
inline constexpr int kBishopOffsets[4] = {17, 15, -15, -17};
inline constexpr int kRookOffsets[4] = {16, 1, -1, -16};
inline constexpr int kKingOffsets[8] = {17, 16, 15, 1, -1, -15, -16, -17};

// Moving from or to one of these squares clears the listed castling rights.
// Every other square leaves rights untouched, hence the default of 15.
inline const std::array<int8_t, 128>& castlingMask() {
    static const std::array<int8_t, 128> mask = [] {
        std::array<int8_t, 128> m{};
        m.fill(15);
        m[0 /*a1*/] = 15 & ~CR_WQ;
        m[7 /*h1*/] = 15 & ~CR_WK;
        m[4 /*e1*/] = 15 & ~(CR_WK | CR_WQ);
        m[112 /*a8*/] = 15 & ~CR_BQ;
        m[119 /*h8*/] = 15 & ~CR_BK;
        m[116 /*e8*/] = 15 & ~(CR_BK | CR_BQ);
        return m;
    }();
    return mask;
}
} // namespace detail

inline bool Position::isSquareAttacked(int sq, int color) const {
    using namespace detail;

    // Pawns: a white pawn on sq-17/sq-15 attacks sq.
    const int pawn = piece(color, PAWN);
    const int dir = color == WHITE ? -16 : 16;
    for (int side = -1; side <= 1; side += 2) {
        const int from = sq + dir + side;
        if (onBoard(from) && board_[(size_t)from] == pawn) return true;
    }

    const int knight = piece(color, KNIGHT);
    for (int off : kKnightOffsets) {
        const int from = sq + off;
        if (onBoard(from) && board_[(size_t)from] == knight) return true;
    }

    const int king = piece(color, KING);
    for (int off : kKingOffsets) {
        const int from = sq + off;
        if (onBoard(from) && board_[(size_t)from] == king) return true;
    }

    const int queen = piece(color, QUEEN);
    const int bishop = piece(color, BISHOP);
    for (int off : kBishopOffsets) {
        for (int from = sq + off; onBoard(from); from += off) {
            const int p = board_[(size_t)from];
            if (p == EMPTY) continue;
            if (p == bishop || p == queen) return true;
            break;
        }
    }

    const int rook = piece(color, ROOK);
    for (int off : kRookOffsets) {
        for (int from = sq + off; onBoard(from); from += off) {
            const int p = board_[(size_t)from];
            if (p == EMPTY) continue;
            if (p == rook || p == queen) return true;
            break;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Move generation
// ---------------------------------------------------------------------------

inline MoveList Position::generateMoves(bool legal, int square, bool capturesOnly) {
    using namespace detail;

    MoveList moves;
    const int us = turn_;
    const int them = us ^ 1;

    const int first = square >= 0 ? square : 0;
    const int last = square >= 0 ? square : 119;

    for (int from = first; from <= last; ++from) {
        if (from & 0x88) {
            from += 7;
            continue;
        }
        const int p = board_[(size_t)from];
        if (p == EMPTY || colorOf(p) != us) continue;
        const int type = typeOf(p);

        if (type == PAWN) {
            const int dir = us == WHITE ? 16 : -16;
            const int startRank = us == WHITE ? 1 : 6;
            const int promoRank = us == WHITE ? 7 : 0;

            // Pushes.
            const int one = from + dir;
            if (!capturesOnly && onBoard(one) && board_[(size_t)one] == EMPTY) {
                if (rankOf(one) == promoRank) {
                    for (int promo : {QUEEN, ROOK, BISHOP, KNIGHT})
                        moves.push(encodeMove(from, one, promo, FLAG_PROMO));
                } else {
                    moves.push(encodeMove(from, one));
                    const int two = from + dir * 2;
                    if (rankOf(from) == startRank && board_[(size_t)two] == EMPTY)
                        moves.push(encodeMove(from, two, 0, FLAG_DOUBLE));
                }
            } else if (capturesOnly && onBoard(one) && board_[(size_t)one] == EMPTY &&
                       rankOf(one) == promoRank) {
                // Quiescence still wants promotions, which change material.
                moves.push(encodeMove(from, one, QUEEN, FLAG_PROMO));
            }

            // Captures, including en passant.
            for (int side = -1; side <= 1; side += 2) {
                const int to = from + dir + side;
                if (!onBoard(to)) continue;
                const int target = board_[(size_t)to];
                if (target != EMPTY && colorOf(target) == them) {
                    if (rankOf(to) == promoRank) {
                        for (int promo : {QUEEN, ROOK, BISHOP, KNIGHT})
                            moves.push(encodeMove(from, to, promo, FLAG_PROMO | FLAG_CAPTURE));
                    } else {
                        moves.push(encodeMove(from, to, 0, FLAG_CAPTURE));
                    }
                } else if (to == ep_) {
                    moves.push(encodeMove(from, to, 0, FLAG_CAPTURE | FLAG_EP));
                }
            }
            continue;
        }

        if (type == KNIGHT || type == KING) {
            const int* offsets = type == KNIGHT ? kKnightOffsets : kKingOffsets;
            const int n = 8;
            for (int i = 0; i < n; ++i) {
                const int to = from + offsets[i];
                if (!onBoard(to)) continue;
                const int target = board_[(size_t)to];
                if (target == EMPTY) {
                    if (!capturesOnly) moves.push(encodeMove(from, to));
                } else if (colorOf(target) == them) {
                    moves.push(encodeMove(from, to, 0, FLAG_CAPTURE));
                }
            }
            continue;
        }

        // Sliding pieces: bishop → diagonals, rook → ranks/files, queen → both.
        const int* slide = type == BISHOP ? kBishopOffsets
                           : type == ROOK ? kRookOffsets
                                          : kKingOffsets;
        const int n = type == QUEEN ? 8 : 4;
        for (int i = 0; i < n; ++i) {
            for (int to = from + slide[i]; onBoard(to); to += slide[i]) {
                const int target = board_[(size_t)to];
                if (target == EMPTY) {
                    if (!capturesOnly) moves.push(encodeMove(from, to));
                    continue;
                }
                if (colorOf(target) == them) moves.push(encodeMove(from, to, 0, FLAG_CAPTURE));
                break;
            }
        }
    }

    // Castling. The king may not start in check, pass through an attacked
    // square, or land on one; the squares between king and rook must be empty.
    if (!capturesOnly) {
        const int kingSq = kings_[(size_t)us];
        const bool generateCastles = square < 0 || square == kingSq;
        if (generateCastles && kingSq >= 0) {
            const int kingSide = us == WHITE ? CR_WK : CR_BK;
            const int queenSide = us == WHITE ? CR_WQ : CR_BQ;
            const int e = us == WHITE ? 4 : 116;   // e1 / e8
            const int f = us == WHITE ? 5 : 117;   // f1 / f8
            const int g = us == WHITE ? 6 : 118;   // g1 / g8
            const int d = us == WHITE ? 3 : 115;   // d1 / d8
            const int c = us == WHITE ? 2 : 114;   // c1 / c8
            const int b = us == WHITE ? 1 : 113;   // b1 / b8

            if ((castling_ & kingSide) && board_[(size_t)f] == EMPTY &&
                board_[(size_t)g] == EMPTY && !isSquareAttacked(e, them) &&
                !isSquareAttacked(f, them) && !isSquareAttacked(g, them))
                moves.push(encodeMove(e, g, 0, FLAG_KCASTLE));

            if ((castling_ & queenSide) && board_[(size_t)d] == EMPTY &&
                board_[(size_t)c] == EMPTY && board_[(size_t)b] == EMPTY &&
                !isSquareAttacked(e, them) && !isSquareAttacked(d, them) &&
                !isSquareAttacked(c, them))
                moves.push(encodeMove(e, c, 0, FLAG_QCASTLE));
        }
    }

    if (!legal) return moves;

    MoveList legalMoves;
    for (int i = 0; i < moves.size(); ++i) {
        const Move m = moves[i];
        makeMove(m);
        if (!isSquareAttacked(kings_[(size_t)us], them)) legalMoves.push(m);
        undoMove();
    }
    return legalMoves;
}

// ---------------------------------------------------------------------------
// Make / unmake
// ---------------------------------------------------------------------------

inline void Position::makeMove(Move m) {
    const auto& zob = detail::zobrist();
    const int from = moveFrom(m);
    const int to = moveTo(m);
    const int flags = moveFlags(m);
    const int promo = movePromo(m);
    const int us = turn_;
    const int them = us ^ 1;
    const int moving = board_[(size_t)from];
    const int movingType = typeOf(moving);

    int captured = EMPTY;
    int capturedSq = to;
    if (flags & FLAG_EP) {
        capturedSq = us == WHITE ? to - 16 : to + 16;
        captured = board_[(size_t)capturedSq];
    } else if (board_[(size_t)to] != EMPTY) {
        captured = board_[(size_t)to];
    }

    history_.push_back(HistoryEntry{m, captured, capturedSq, castling_, ep_, halfmoves_,
                                    hash_, kings_[(size_t)us], false});

    // Remove old castling/ep from the hash before mutating them.
    hash_ ^= zob.castle[(size_t)castling_];
    if (ep_ >= 0) hash_ ^= zob.epFile[(size_t)fileOf(ep_)];

    // The captured piece leaves the board.
    if (captured != EMPTY) {
        board_[(size_t)capturedSq] = EMPTY;
        hash_ ^= zob.pieceSquare[(size_t)captured][(size_t)capturedSq];
    }

    // Move the piece (promoting if required).
    const int placed = promo ? piece(us, promo) : moving;
    board_[(size_t)from] = EMPTY;
    hash_ ^= zob.pieceSquare[(size_t)moving][(size_t)from];
    board_[(size_t)to] = (int8_t)placed;
    hash_ ^= zob.pieceSquare[(size_t)placed][(size_t)to];

    // Rook hop when castling.
    if (flags & (FLAG_KCASTLE | FLAG_QCASTLE)) {
        const int rookFrom = (flags & FLAG_KCASTLE) ? to + 1 : to - 2;
        const int rookTo = (flags & FLAG_KCASTLE) ? to - 1 : to + 1;
        const int rook = board_[(size_t)rookFrom];
        board_[(size_t)rookFrom] = EMPTY;
        board_[(size_t)rookTo] = (int8_t)rook;
        hash_ ^= zob.pieceSquare[(size_t)rook][(size_t)rookFrom] ^
                 zob.pieceSquare[(size_t)rook][(size_t)rookTo];
    }

    if (movingType == KING) kings_[(size_t)us] = to;

    // New castling rights and en passant square.
    castling_ &= detail::castlingMask()[(size_t)from] & detail::castlingMask()[(size_t)to];
    ep_ = (flags & FLAG_DOUBLE) ? (us == WHITE ? from + 16 : from - 16) : -1;

    hash_ ^= zob.castle[(size_t)castling_];
    if (ep_ >= 0) hash_ ^= zob.epFile[(size_t)fileOf(ep_)];

    halfmoves_ = (movingType == PAWN || captured != EMPTY) ? 0 : halfmoves_ + 1;
    if (us == BLACK) ++fullmoves_;

    turn_ = them;
    hash_ ^= zob.side;
}

inline Move Position::undoMove() {
    if (history_.empty()) return MOVE_NONE;
    const HistoryEntry state = history_.back();
    history_.pop_back();

    const Move m = state.move;
    const int from = moveFrom(m);
    const int to = moveTo(m);
    const int flags = moveFlags(m);
    const int promo = movePromo(m);

    turn_ ^= 1;
    const int us = turn_;
    if (us == BLACK) --fullmoves_;

    const int placed = board_[(size_t)to];
    board_[(size_t)to] = EMPTY;
    board_[(size_t)from] = (int8_t)(promo ? piece(us, PAWN) : placed);

    if (flags & (FLAG_KCASTLE | FLAG_QCASTLE)) {
        const int rookFrom = (flags & FLAG_KCASTLE) ? to + 1 : to - 2;
        const int rookTo = (flags & FLAG_KCASTLE) ? to - 1 : to + 1;
        board_[(size_t)rookFrom] = board_[(size_t)rookTo];
        board_[(size_t)rookTo] = EMPTY;
    }

    if (state.captured != EMPTY) board_[(size_t)state.capturedSq] = (int8_t)state.captured;
    if (typeOf(placed) == KING) kings_[(size_t)us] = state.kingSq;

    castling_ = state.castling;
    ep_ = state.ep;
    halfmoves_ = state.halfmoves;
    hash_ = state.hash;

    return m;
}

inline void Position::makeNullMove() {
    const auto& zob = detail::zobrist();
    history_.push_back(HistoryEntry{MOVE_NONE, EMPTY, -1, castling_, ep_, halfmoves_,
                                    hash_, kings_[(size_t)turn_], true});
    if (ep_ >= 0) {
        hash_ ^= zob.epFile[(size_t)fileOf(ep_)];
        ep_ = -1;
    }
    turn_ ^= 1;
    hash_ ^= zob.side;
    ++halfmoves_;
}

inline void Position::undoNullMove() {
    if (history_.empty()) return;
    const HistoryEntry state = history_.back();
    history_.pop_back();
    turn_ ^= 1;
    ep_ = state.ep;
    halfmoves_ = state.halfmoves;
    hash_ = state.hash;
}

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------

inline void Position::computeHash() {
    const auto& zob = detail::zobrist();
    uint64_t h = 0;
    for (int sq = 0; sq < 128; ++sq) {
        if (sq & 0x88) {
            sq += 7;
            continue;
        }
        const int p = board_[(size_t)sq];
        if (p != EMPTY) h ^= zob.pieceSquare[(size_t)p][(size_t)sq];
    }
    h ^= zob.castle[(size_t)castling_];
    if (ep_ >= 0) h ^= zob.epFile[(size_t)fileOf(ep_)];
    if (turn_ == BLACK) h ^= zob.side;
    hash_ = h;
}

// ---------------------------------------------------------------------------
// Playing moves
// ---------------------------------------------------------------------------

inline bool Position::playMove(Move m, Move* played) {
    // Only legal moves are accepted; the caller may pass any encoding.
    if (!generateMoves().contains(m)) return false;
    makeMove(m);
    positions_.push_back(hash_);
    if (played) *played = m;
    return true;
}

inline bool Position::play(const std::string& text, Move* played) {
    const Move m = resolve(text);
    if (m == MOVE_NONE) return false;
    return playMove(m, played);
}

inline bool Position::undoPlayed() {
    if (history_.empty()) return false;
    positions_.pop_back();
    undoMove();
    return true;
}

inline Move Position::resolve(const std::string& text) {
    MoveList legal = generateMoves();

    // Trim whitespace.
    size_t begin = 0, end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    std::string s = text.substr(begin, end - begin);
    if (s.empty()) return MOVE_NONE;

    // UCI first — it is unambiguous.
    if (s.size() == 4 || s.size() == 5) {
        const int from = squareFromAlgebraic(s);
        const int to = squareFromAlgebraic(std::string_view(s).substr(2));
        if (from >= 0 && to >= 0) {
            const int promo = s.size() == 5 ? pieceFromLetter(s[4]) : 0;
            if (s.size() == 4 || promo != EMPTY) {
                for (int i = 0; i < legal.size(); ++i)
                    if (moveFrom(legal[i]) == from && moveTo(legal[i]) == to &&
                        movePromo(legal[i]) == promo)
                        return legal[i];
                // A promotion given without its piece ("e7e8") is not a UCI
                // match; fall through — the SAN parse defaults it to a queen.
            }
        }
    }

    // Normalise SAN: drop annotations, accept 0-0 for O-O.
    std::string want;
    for (char ch : s) {
        if (ch == '+' || ch == '#' || ch == '!' || ch == '?') break;
        want += ch == '0' ? 'O' : ch;
    }
    if (want.empty()) return MOVE_NONE;

    // Exact SAN match.
    for (int i = 0; i < legal.size(); ++i) {
        std::string san = toSan(legal[i]);
        std::string norm;
        for (char ch : san) {
            if (ch == '+' || ch == '#' || ch == '!' || ch == '?') break;
            norm += ch;
        }
        if (norm == want) return legal[i];
    }

    // Castling by spelling.
    if (want == "O-O" || want == "O-O-O") {
        const int flag = want == "O-O" ? FLAG_KCASTLE : FLAG_QCASTLE;
        for (int i = 0; i < legal.size(); ++i)
            if (moveFlags(legal[i]) & flag) return legal[i];
        return MOVE_NONE;
    }

    // Structural parse, which also accepts over-specified SAN such as "Rh1g1"
    // or "e7e8=Q" that a strict generator would never emit:
    //   [piece] [file] [rank] [x] target [= promo]
    // The optional file/rank pair is disambiguation; "e4" backtracks to no
    // disambiguator because "e4" is all target.
    size_t pos = 0;
    int type = PAWN;
    const int letter = pos < want.size() ? pieceFromLetter(want[pos]) : EMPTY;
    if (letter == KNIGHT || letter == BISHOP || letter == ROOK || letter == QUEEN ||
        letter == KING) {
        type = letter;
        ++pos;
    }

    // Two structural attempts: greedy (consume a file/rank disambiguator) and
    // plain (the first two characters are the target square).
    int fromFile = -1, fromRank = -1, to = -1, promo = 0;
    size_t after = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        size_t p = pos;
        fromFile = fromRank = -1;
        if (attempt == 0) {
            if (p < want.size() && want[p] >= 'a' && want[p] <= 'h') {
                fromFile = want[p] - 'a';
                ++p;
                if (p < want.size() && want[p] >= '1' && want[p] <= '8') {
                    fromRank = want[p] - '1';
                    ++p;
                }
            } else if (p < want.size() && want[p] >= '1' && want[p] <= '8') {
                fromRank = want[p] - '1';
                ++p;
            }
        }
        if (p < want.size() && want[p] == 'x') ++p;
        if (p + 2 > want.size()) continue;
        const int target = squareFromAlgebraic(std::string_view(want).substr(p, 2));
        if (target < 0) continue;
        p += 2;
        int promoT = 0;
        if (p < want.size() && want[p] == '=') ++p;
        if (p < want.size()) {
            promoT = pieceFromLetter(want[p]);
            if (promoT == EMPTY) continue;
            ++p;
        }
        if (p != want.size()) continue;
        to = target;
        promo = promoT;
        after = p;
        break;
    }
    if (to < 0 || after != want.size()) return MOVE_NONE;

    Move found = MOVE_NONE;
    int candidates = 0;
    for (int i = 0; i < legal.size(); ++i) {
        const Move m = legal[i];
        if (moveTo(m) != to) continue;
        const int from = moveFrom(m);
        if (typeOf(board_[(size_t)from]) != type) continue;
        if (fromFile >= 0 && fileOf(from) != fromFile) continue;
        if (fromRank >= 0 && rankOf(from) != fromRank) continue;
        if (promo) {
            if (movePromo(m) != promo) continue;
        } else if (movePromo(m) != 0 && movePromo(m) != QUEEN) {
            // An unspecified promotion piece means a queen.
            continue;
        }
        if (++candidates > 1) return MOVE_NONE; // ambiguous without a disambiguator
        found = m;
    }
    return candidates == 1 ? found : MOVE_NONE;
}

// ---------------------------------------------------------------------------
// Standard Algebraic Notation
// ---------------------------------------------------------------------------

inline std::string Position::toSan(Move m) {
    const int flags = moveFlags(m);
    if (flags & FLAG_KCASTLE) return withCheckSuffix(m, "O-O");
    if (flags & FLAG_QCASTLE) return withCheckSuffix(m, "O-O-O");

    const int from = moveFrom(m);
    const int to = moveTo(m);
    const int type = typeOf(board_[(size_t)from]);
    const bool isCapture = (flags & FLAG_CAPTURE) != 0;
    std::string san;

    if (type == PAWN) {
        if (isCapture) {
            san += (char)('a' + fileOf(from));
            san += 'x';
        }
        san += algebraic(to);
    } else {
        san += (char)(pieceLetter(type) - 32); // uppercase
        san += disambiguate(m, type, from, to);
        if (isCapture) san += 'x';
        san += algebraic(to);
    }

    if (movePromo(m)) {
        san += '=';
        san += (char)(pieceLetter(movePromo(m)) - 32);
    }

    return withCheckSuffix(m, san);
}

inline std::string Position::disambiguate(Move m, int type, int from, int to) {
    MoveList list = generateMoves();
    int rivals = 0;
    bool sameFile = false, sameRank = false;
    for (int i = 0; i < list.size(); ++i) {
        const Move o = list[i];
        if (o == m || moveTo(o) != to) continue;
        const int f = moveFrom(o);
        if (f == from || typeOf(board_[(size_t)f]) != type) continue;
        ++rivals;
        sameFile = sameFile || fileOf(f) == fileOf(from);
        sameRank = sameRank || rankOf(f) == rankOf(from);
    }
    if (rivals == 0) return {};
    if (!sameFile) return std::string(1, (char)('a' + fileOf(from)));
    if (!sameRank) return std::to_string(rankOf(from) + 1);
    return algebraic(from);
}

inline std::string Position::withCheckSuffix(Move m, std::string san) {
    makeMove(m);
    std::string suffix;
    if (inCheck()) {
        suffix = generateMoves().empty() ? "#" : "+";
    }
    undoMove();
    return san + suffix;
}

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------

inline bool Position::isThreefoldRepetition() const {
    const uint64_t key = hash_;
    int count = 0;
    for (uint64_t k : positions_)
        if (k == key) ++count;
    return count >= 3;
}

/** K vs K, K+minor vs K, and K+B vs K+B with same-coloured bishops. */
inline bool Position::isInsufficientMaterial() const {
    int bishops = 0, knights = 0, others = 0;
    bool bishopLight = false;
    bool bishopsSameColor = true;

    for (int sq = 0; sq < 128; ++sq) {
        if (sq & 0x88) {
            sq += 7;
            continue;
        }
        const int p = board_[(size_t)sq];
        if (p == EMPTY) continue;
        const int type = typeOf(p);
        if (type == KING) continue;
        if (type == BISHOP) {
            const bool light = isLightSquare(sq);
            if (bishops == 0) bishopLight = light;
            else if (light != bishopLight) bishopsSameColor = false;
            ++bishops;
        } else if (type == KNIGHT) {
            ++knights;
        } else {
            ++others;
        }
    }

    if (others > 0) return false;
    if (knights == 0 && bishops == 0) return true; // K vs K
    if (knights == 1 && bishops == 0) return true; // K+N vs K
    if (knights == 0 && bishops == 1) return true; // K+B vs K
    if (knights == 0 && bishops > 1) return bishopsSameColor; // same-colour bishops
    return false;
}

inline Position::Status Position::status() {
    if (generateMoves().empty()) {
        if (inCheck())
            return {true, turn_ == WHITE ? "0-1" : "1-0", "checkmate"};
        return {true, "1/2-1/2", "stalemate"};
    }
    if (isInsufficientMaterial()) return {true, "1/2-1/2", "insufficient material"};
    if (isFiftyMoveDraw()) return {true, "1/2-1/2", "fifty-move rule"};
    if (isThreefoldRepetition()) return {true, "1/2-1/2", "threefold repetition"};
    return {false, nullptr, nullptr};
}

// ---------------------------------------------------------------------------
// Perft — the correctness yardstick for move generation
// ---------------------------------------------------------------------------

inline uint64_t Position::perft(int depth) {
    if (depth == 0) return 1;
    const MoveList moves = generateMoves(false);
    const int us = turn_;
    const int them = us ^ 1;
    uint64_t nodes = 0;
    for (int i = 0; i < moves.size(); ++i) {
        makeMove(moves[i]);
        if (!isSquareAttacked(kings_[(size_t)us], them))
            nodes += depth == 1 ? 1 : perft(depth - 1);
        undoMove();
    }
    return nodes;
}

} // namespace chess
