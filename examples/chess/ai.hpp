#pragma once
// ai.hpp — negamax search with alpha-beta pruning.
//
// Ported from amanmprojects/chess (src/ai.js): iterative deepening, a
// transposition table, quiescence search, MVV-LVA capture ordering, killer and
// history heuristics, null-move pruning, late-move reductions, aspiration
// windows and a tapered piece-square evaluation. The difficulty levels weaken
// the bot by sampling among near-best moves instead of playing nonsense.
//
// The search never touches the engine, so a game may run it on a worker thread
// (see ChessBot in 09_chess) while the main thread keeps rendering. Limits are
// depth + node count + wall-clock; node or depth limits alone make a run
// deterministic, which the headless capture and CI smoke modes rely on.

#include "chess.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace chess {
namespace ai {

constexpr int kMate = 30000;
constexpr int kMateThreshold = kMate - 1000;
constexpr int kInfinity = 1 << 20;
constexpr int kMaxPly = 64;

struct Limits {
    int depth = 64;        // maximum search depth
    uint64_t nodes = 0;    // stop after this many nodes (0 = no limit)
    double movetimeMs = 0; // stop after this long (0 = no limit)
};

struct Result {
    Move move = MOVE_NONE;
    int score = 0;      // centipawns, side to move at the root; MATE-ish when forced
    int depth = 0;      // deepest fully completed iteration
    uint64_t nodes = 0; // nodes visited in this search
    std::vector<Move> pv;
};

using ProgressFn = std::function<void(const Result&)>;

/** A difficulty preset; `randomness` is the centipawn window for sampling. */
struct Level {
    const char* name;
    int depth;
    uint64_t nodes;
    double movetimeMs;
    int randomness;
};

inline const std::array<Level, 5>& levels() {
    static const std::array<Level, 5> k = {{
        // name            depth      nodes  movetime  randomness
        {"Beginner",         1,       3000,     60.0,        180},
        {"Casual",           2,      25000,    150.0,         90},
        {"Intermediate",     4,     120000,    400.0,         30},
        {"Strong",           8,     600000,   1200.0,          0},
        {"Expert",          20,    3000000,   3000.0,          0},
    }};
    return k;
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

/** Middlegame and endgame material values, in centipawns. */
inline constexpr int kMgValue[7] = {0, 82, 337, 365, 477, 1025, 0};
inline constexpr int kEgValue[7] = {0, 94, 281, 297, 512, 936, 0};

namespace detail {

using Table64 = std::array<int16_t, 64>;

/** rows[0] is rank 8 (so the tables read like a board); index 0 of the result
 *  is a1, exactly like the JS `table()` helper. */
constexpr Table64 makeTable(const int rows[8][8]) {
    Table64 t{};
    for (int rank = 0; rank < 8; ++rank)
        for (int file = 0; file < 8; ++file)
            t[(size_t)((7 - rank) * 8 + file)] = (int16_t)rows[rank][file];
    return t;
}

constexpr int kMgPawn[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {98, 134, 61, 95, 68, 126, 34, -11},
    {-6, 7, 26, 31, 65, 56, 25, -20},
    {-14, 13, 6, 21, 23, 12, 17, -23},
    {-27, -2, -5, 12, 17, 6, 10, -25},
    {-26, -4, -4, -10, 3, 3, 33, -12},
    {-35, -1, -20, -23, -15, 24, 38, -22},
    {0, 0, 0, 0, 0, 0, 0, 0},
};
constexpr int kMgKnight[8][8] = {
    {-167, -89, -34, -49, 61, -97, -15, -107},
    {-73, -41, 72, 36, 23, 62, 7, -17},
    {-47, 60, 37, 65, 84, 129, 73, 44},
    {-9, 17, 19, 53, 37, 69, 18, 22},
    {-13, 4, 16, 13, 28, 19, 21, -8},
    {-23, -9, 12, 10, 19, 17, 25, -16},
    {-29, -53, -12, -3, -1, 18, -14, -19},
    {-105, -21, -58, -33, -17, -28, -19, -23},
};
constexpr int kMgBishop[8][8] = {
    {-29, 4, -82, -37, -25, -42, 7, -8},
    {-26, 16, -18, -13, 30, 59, 18, -47},
    {-16, 37, 43, 40, 35, 50, 37, -2},
    {-4, 5, 19, 50, 37, 37, 7, -2},
    {-6, 13, 13, 26, 34, 12, 10, 4},
    {0, 15, 15, 15, 14, 27, 18, 10},
    {4, 15, 16, 0, 7, 21, 33, 1},
    {-33, -3, -14, -21, -13, -12, -39, -21},
};
constexpr int kMgRook[8][8] = {
    {32, 42, 32, 51, 63, 9, 31, 43},
    {27, 32, 58, 62, 80, 67, 26, 44},
    {-5, 19, 26, 36, 17, 45, 61, 16},
    {-24, -11, 7, 26, 24, 35, -8, -20},
    {-36, -26, -12, -1, 9, -7, 6, -23},
    {-45, -25, -16, -17, 3, 0, -5, -33},
    {-44, -16, -20, -9, -1, 11, -6, -71},
    {-19, -13, 1, 17, 16, 7, -37, -26},
};
constexpr int kMgQueen[8][8] = {
    {-28, 0, 29, 12, 59, 44, 43, 45},
    {-24, -39, -5, 1, -16, 57, 28, 54},
    {-13, -17, 7, 8, 29, 56, 47, 57},
    {-27, -27, -16, -16, -1, 17, -2, 1},
    {-9, -26, -9, -10, -2, -4, 3, -3},
    {-14, 2, -11, -2, -5, 2, 14, 5},
    {-35, -8, 11, 2, 8, 15, -3, 1},
    {-1, -18, -9, 10, -15, -25, -31, -50},
};
constexpr int kMgKing[8][8] = {
    {-65, 23, 16, -15, -56, -34, 2, 13},
    {29, -1, -20, -7, -8, -4, -38, -29},
    {-9, 24, 2, -16, -20, 6, 22, -22},
    {-17, -20, -12, -27, -30, -25, -14, -36},
    {-49, -1, -27, -39, -46, -44, -33, -51},
    {-14, -14, -22, -46, -44, -30, -15, -27},
    {1, 7, -8, -64, -43, -16, 9, 8},
    {-15, 36, 12, -54, 8, -28, 24, 14},
};
constexpr int kEgPawn[8][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0},
    {178, 173, 158, 134, 147, 132, 165, 187},
    {94, 100, 85, 67, 56, 53, 82, 84},
    {32, 24, 13, 5, -2, 4, 17, 17},
    {13, 9, -3, -7, -7, -8, 3, -1},
    {4, 7, -6, 1, 0, -5, -1, -8},
    {13, 8, 8, 10, 13, 0, 2, -7},
    {0, 0, 0, 0, 0, 0, 0, 0},
};
constexpr int kEgKnight[8][8] = {
    {-58, -38, -13, -28, -31, -27, -63, -99},
    {-25, -8, -25, -2, -9, -25, -24, -52},
    {-24, -20, 10, 9, -1, -9, -19, -41},
    {-17, 3, 22, 22, 22, 11, 8, -18},
    {-18, -6, 16, 25, 16, 17, 4, -18},
    {-23, -3, -1, 15, 10, -3, -20, -22},
    {-42, -20, -10, -5, -2, -20, -23, -44},
    {-29, -51, -23, -15, -22, -18, -50, -64},
};
constexpr int kEgBishop[8][8] = {
    {-14, -21, -11, -8, -7, -9, -17, -24},
    {-8, -4, 7, -12, -3, -13, -4, -14},
    {2, -8, 0, -1, -2, 6, 0, 4},
    {-3, 9, 12, 9, 14, 10, 3, 2},
    {-6, 3, 13, 19, 7, 10, -3, -9},
    {-12, -3, 8, 10, 13, 3, -7, -15},
    {-14, -18, -7, -1, 4, -9, -15, -27},
    {-23, -9, -23, -5, -9, -16, -5, -17},
};
constexpr int kEgRook[8][8] = {
    {13, 10, 18, 15, 12, 12, 8, 5},
    {11, 13, 13, 11, -3, 3, 8, 3},
    {7, 7, 7, 5, 4, -3, -5, -3},
    {4, 3, 13, 1, 2, 1, -1, 2},
    {3, 5, 8, 4, -5, -6, -8, -11},
    {-4, 0, -5, -1, -7, -12, -8, -16},
    {-6, -6, 0, 2, -9, -9, -11, -3},
    {-9, 2, 3, -1, -5, -13, 4, -20},
};
constexpr int kEgQueen[8][8] = {
    {-9, 22, 22, 27, 27, 19, 10, 20},
    {-17, 20, 32, 41, 58, 25, 30, 0},
    {-20, 6, 9, 49, 47, 35, 19, 9},
    {3, 22, 24, 45, 57, 40, 57, 36},
    {-18, 28, 19, 47, 31, 34, 39, 23},
    {-16, -27, 15, 6, 9, 17, 10, 5},
    {-22, -23, -30, -16, -16, -23, -36, -32},
    {-33, -28, -22, -43, -5, -32, -20, -41},
};
constexpr int kEgKing[8][8] = {
    {-74, -35, -18, -18, -11, 15, 4, -17},
    {-12, 17, 14, 17, 17, 38, 23, 11},
    {10, 17, 23, 15, 20, 45, 44, 13},
    {-8, 22, 24, 27, 26, 33, 26, 3},
    {-18, -4, 21, 24, 27, 23, 9, -11},
    {-19, -3, 11, 21, 23, 16, 7, -9},
    {-27, -11, 4, 13, 14, 4, -5, -17},
    {-53, -34, -21, -11, -28, -14, -24, -43},
};

struct EvalTables {
    Table64 mg[7], eg[7];
    EvalTables() {
        mg[PAWN] = makeTable(kMgPawn);
        mg[KNIGHT] = makeTable(kMgKnight);
        mg[BISHOP] = makeTable(kMgBishop);
        mg[ROOK] = makeTable(kMgRook);
        mg[QUEEN] = makeTable(kMgQueen);
        mg[KING] = makeTable(kMgKing);
        eg[PAWN] = makeTable(kEgPawn);
        eg[KNIGHT] = makeTable(kEgKnight);
        eg[BISHOP] = makeTable(kEgBishop);
        eg[ROOK] = makeTable(kEgRook);
        eg[QUEEN] = makeTable(kEgQueen);
        eg[KING] = makeTable(kEgKing);
    }
};

inline const EvalTables& evalTables() {
    static const EvalTables t;
    return t;
}

constexpr int kPassedPawnBonus[8] = {0, 10, 17, 15, 62, 168, 276, 0};
constexpr int kIsolatedPawnPenalty = -15;
constexpr int kDoubledPawnPenalty = -10;
constexpr int kRookOpenFile = 25;
constexpr int kRookSemiOpenFile = 12;
constexpr int kTempo = 10;
constexpr int kTotalPhase = 24;
constexpr int kPhaseWeight[7] = {0, 0, 1, 1, 2, 4, 0};

constexpr int kMvvLvaValue[7] = {0, 100, 300, 320, 500, 900, 0};

/** Mirror a square index vertically, to read White's tables for Black. */
constexpr int flip64(int i) { return i ^ 56; }

} // namespace detail

/** Static evaluation, in centipawns, from the side-to-move's point of view. */
inline int evaluate(const Position& pos) {
    using namespace detail;
    const EvalTables& tb = evalTables();

    int mg[2] = {0, 0};
    int eg[2] = {0, 0};
    int phase = 0;
    int bishops[2] = {0, 0};
    int pawnFiles[2][8] = {};
    int pawnMaxRank[2][8]; // most advanced pawn per file (for passed detection)
    int pawnMinRank[2][8];
    for (int c = 0; c < 2; ++c)
        for (int f = 0; f < 8; ++f) {
            pawnMaxRank[c][f] = -1;
            pawnMinRank[c][f] = 8;
        }

    for (int sq = 0; sq < 128; ++sq) {
        if (sq & 0x88) {
            sq += 7;
            continue;
        }
        const int p = pos.at(sq);
        if (p == EMPTY) continue;
        const int color = colorOf(p);
        const int type = typeOf(p);
        const int idx = color == WHITE ? to64(sq) : flip64(to64(sq));

        mg[color] += kMgValue[type] + tb.mg[type][(size_t)idx];
        eg[color] += kEgValue[type] + tb.eg[type][(size_t)idx];
        phase += kPhaseWeight[type];

        if (type == PAWN) {
            ++pawnFiles[color][fileOf(sq)];
            pawnMaxRank[color][fileOf(sq)] = std::max(pawnMaxRank[color][fileOf(sq)], rankOf(sq));
            pawnMinRank[color][fileOf(sq)] = std::min(pawnMinRank[color][fileOf(sq)], rankOf(sq));
        } else if (type == BISHOP) {
            ++bishops[color];
        }
    }

    // Passed pawns: no enemy pawn ahead on this or an adjacent file.
    for (int rank = 0; rank < 8; ++rank) {
        for (int file = 0; file < 8; ++file) {
            const int p = pos.at(rank * 16 + file);
            if (p == EMPTY || typeOf(p) != PAWN) continue;
            const int color = colorOf(p);
            const int them = color ^ 1;
            bool passed = true;
            for (int f = std::max(0, file - 1); f <= std::min(7, file + 1) && passed; ++f) {
                if (color == WHITE ? pawnMaxRank[them][f] > rank : pawnMinRank[them][f] < rank)
                    passed = false;
            }
            if (!passed) continue;
            const int relRank = color == WHITE ? rank : 7 - rank;
            mg[color] += kPassedPawnBonus[relRank] >> 1;
            eg[color] += kPassedPawnBonus[relRank];
        }
    }

    for (int color = 0; color < 2; ++color) {
        if (bishops[color] >= 2) {
            mg[color] += 30; // bishop pair
            eg[color] += 30;
        }

        for (int file = 0; file < 8; ++file) {
            const int count = pawnFiles[color][file];
            if (count == 0) continue;

            if (count > 1) {
                const int penalty = kDoubledPawnPenalty * (count - 1);
                mg[color] += penalty;
                eg[color] += penalty;
            }

            const bool leftEmpty = file == 0 || pawnFiles[color][file - 1] == 0;
            const bool rightEmpty = file == 7 || pawnFiles[color][file + 1] == 0;
            if (leftEmpty && rightEmpty) {
                mg[color] += kIsolatedPawnPenalty;
                eg[color] += kIsolatedPawnPenalty;
            }
        }
    }

    // Rooks on open and half-open files.
    for (int sq = 0; sq < 128; ++sq) {
        if (sq & 0x88) {
            sq += 7;
            continue;
        }
        const int p = pos.at(sq);
        if (p == EMPTY || typeOf(p) != ROOK) continue;
        const int color = colorOf(p);
        const int file = fileOf(sq);
        if (pawnFiles[color][file] == 0) {
            const int bonus = pawnFiles[color ^ 1][file] == 0 ? kRookOpenFile : kRookSemiOpenFile;
            mg[color] += bonus;
            eg[color] += bonus;
        }
    }

    // Taper between the middlegame and endgame scores.
    const int clamped = std::min(phase, kTotalPhase);
    const int mgScore = mg[WHITE] - mg[BLACK];
    const int egScore = eg[WHITE] - eg[BLACK];
    const double score =
        (double)(mgScore * clamped + egScore * (kTotalPhase - clamped)) / kTotalPhase;
    const int sideScore = pos.turn() == WHITE ? (int)std::lround(score) : -(int)std::lround(score);
    return sideScore + kTempo;
}

// ---------------------------------------------------------------------------
// Transposition table
// ---------------------------------------------------------------------------

class TranspositionTable {
public:
    enum Flag : uint8_t { Exact = 0, Lower = 1, Upper = 2 };

    struct Entry {
        bool found = false;
        Move move = MOVE_NONE;
        int score = 0;
        int depth = 0;
        uint8_t flag = Exact;
    };

    /** `entries` is rounded down to a power of two (minimum 1024). */
    explicit TranspositionTable(size_t entries)
        : keys_(roundUp(entries)), moves_(roundUp(entries)), scores_(roundUp(entries)),
          depths_(roundUp(entries)), flags_(roundUp(entries)), used_(roundUp(entries)),
          mask_(roundUp(entries) - 1) {}

    void clear() { std::fill(used_.begin(), used_.end(), 0); }

    Entry probe(uint64_t key) const {
        const size_t i = index(key);
        Entry e;
        if (!used_[i] || keys_[i] != key) return e;
        e.found = true;
        e.move = moves_[i];
        e.score = scores_[i];
        e.depth = depths_[i];
        e.flag = flags_[i];
        return e;
    }

    void store(uint64_t key, Move move, int score, int depth, uint8_t flag) {
        const size_t i = index(key);
        // Depth-preferred replacement, but always overwrite a different position.
        if (used_[i] && keys_[i] == key && depths_[i] > depth && flag != Exact) return;
        keys_[i] = key;
        moves_[i] = move;
        scores_[i] = score;
        depths_[i] = (int8_t)depth;
        flags_[i] = flag;
        used_[i] = 1;
    }

    size_t size() const { return keys_.size(); }

private:
    static size_t roundUp(size_t n) {
        size_t p = 1024;
        while (p < n && p < (size_t(1) << 22)) p <<= 1;
        return p;
    }
    size_t index(uint64_t key) const { return (size_t)((key ^ (key >> 32)) & mask_); }

    std::vector<uint64_t> keys_;
    std::vector<uint32_t> moves_;
    std::vector<int32_t> scores_;
    std::vector<int8_t> depths_;
    std::vector<uint8_t> flags_;
    std::vector<uint8_t> used_;
    size_t mask_;
};

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

class Search {
public:
    /** 1M entries ≈ 18 MB; enough for meaningful play without a big footprint. */
    explicit Search(size_t ttEntries = size_t(1) << 20) : tt_(ttEntries) {
        history_.assign(128 * 128, 0);
    }

    /** Forget everything learned in previous games (keeps runs reproducible). */
    void newGame() { tt_.clear(); }

    /** Ask the running search to abort soon. Safe to call from any thread. */
    void stop() { stopRequest_.store(true, std::memory_order_relaxed); }
    bool aborted() const { return stopped_; }
    uint64_t nodeCount() const { return nodes_; }

    /** Iterative deepening under `limits`; onIteration fires per completed depth. */
    Result findBestMove(Position& pos, const Limits& limits, const ProgressFn& onIteration = {});

    /** Pick a move at a difficulty level — the ported LEVELS/chooseMove logic. */
    Result chooseMove(Position& pos, const Level& level, uint64_t seed,
                      const ProgressFn& onIteration = {});

private:
    struct ScoredMove {
        int score;
        Move move;
    };

    int quiescence(Position& pos, int alpha, int beta, int ply);
    int negamax(Position& pos, int depth, int alpha, int beta, int ply, bool canNull);
    void orderMoves(const Position& pos, MoveList& moves, int ply, Move ttMove) const;
    int scoreMove(const Position& pos, Move m, int ply, Move ttMove) const;
    bool outOfLimits() const;
    bool isRepetition(const Position& pos) const;
    void setupRootKeys(const Position& pos);
    bool hasNonPawnMaterial(const Position& pos) const;
    std::vector<Move> extractPv(Position& pos, int maxLength);

    TranspositionTable tt_;
    std::array<std::array<Move, 2>, kMaxPly> killers_{};
    std::vector<int> history_;
    std::vector<uint64_t> rootKeys_;

    std::atomic<bool> stopRequest_{false};
    bool stopped_ = false;
    uint64_t nodes_ = 0;
    Limits limits_{};
    std::chrono::steady_clock::time_point deadline_{};
    bool hasDeadline_ = false;
};

inline bool Search::outOfLimits() const {
    if (stopRequest_.load(std::memory_order_relaxed)) return true;
    if (limits_.nodes && nodes_ >= limits_.nodes) return true;
    if (hasDeadline_ && std::chrono::steady_clock::now() >= deadline_) return true;
    return false;
}

inline int Search::scoreMove(const Position& pos, Move m, int ply, Move ttMove) const {
    using namespace detail;
    if (m == ttMove) return 1000000;

    const int flags = moveFlags(m);
    const int to = moveTo(m);
    const int from = moveFrom(m);

    if (flags & FLAG_PROMO) return 900000 + kMvvLvaValue[movePromo(m)];

    if (flags & FLAG_CAPTURE) {
        const int victim = pos.at(to);
        const int victimType = victim == EMPTY ? PAWN : typeOf(victim); // ep captures a pawn
        const int attackerType = typeOf(pos.at(from));
        return 800000 + kMvvLvaValue[victimType] * 10 - kMvvLvaValue[attackerType] / 100;
    }

    if (killers_[(size_t)ply][0] == m) return 700000;
    if (killers_[(size_t)ply][1] == m) return 600000;

    return history_[(size_t)(from * 128 + to)];
}

inline void Search::orderMoves(const Position& pos, MoveList& moves, int ply, Move ttMove) const {
    const int n = moves.size();
    if (n <= 1) return;
    std::array<ScoredMove, MoveList::CAP> buf{};
    for (int i = 0; i < n; ++i)
        buf[(size_t)i] = ScoredMove{scoreMove(pos, moves[i], ply, ttMove), moves[i]};
    // Insertion sort, largest score first (stable, and the list is short).
    for (int i = 1; i < n; ++i) {
        const ScoredMove key = buf[(size_t)i];
        int j = i - 1;
        while (j >= 0 && buf[(size_t)j].score < key.score) {
            buf[(size_t)(j + 1)] = buf[(size_t)j];
            --j;
        }
        buf[(size_t)(j + 1)] = key;
    }
    for (int i = 0; i < n; ++i) moves.moves[(size_t)i] = buf[(size_t)i].move;
}

inline bool Search::hasNonPawnMaterial(const Position& pos) const {
    const int us = pos.turn();
    for (int sq = 0; sq < 128; ++sq) {
        if (sq & 0x88) {
            sq += 7;
            continue;
        }
        const int p = pos.at(sq);
        if (p == EMPTY || colorOf(p) != us) continue;
        const int t = typeOf(p);
        if (t != PAWN && t != KING) return true;
    }
    return false;
}

inline bool Search::isRepetition(const Position& pos) const {
    // Only positions since the last irreversible move can repeat, and the
    // halfmove clock counts exactly those plies. It spans the in-tree undo
    // stack first, then continues into the pre-root game history.
    int remaining = pos.halfmoves();
    if (remaining < 4) return false; // a repetition needs at least four plies

    const std::vector<Position::HistoryEntry>& hist = pos.history();
    for (int i = (int)hist.size() - 1; i >= 0 && remaining > 0; --i, --remaining) {
        const Position::HistoryEntry& state = hist[(size_t)i];
        // A null move fabricates a position that real play cannot reach, so
        // anything beyond it is not a genuine ancestor of this node.
        if (state.isNull) return false;
        if (state.hash == pos.hash()) return true;
    }
    for (int i = (int)rootKeys_.size() - 1; i >= 0 && remaining > 0; --i, --remaining) {
        if (rootKeys_[(size_t)i] == pos.hash()) return true;
    }
    return false;
}

inline void Search::setupRootKeys(const Position& pos) {
    rootKeys_.clear();
    const std::vector<uint64_t>& keys = pos.positions();
    if (keys.empty()) return;
    // Drop the trailing entry when it is the root — the in-tree walk covers it.
    size_t end = keys.size();
    if (keys[end - 1] == pos.hash()) --end;
    rootKeys_.assign(keys.begin(), keys.begin() + (std::ptrdiff_t)end);
}

inline int Search::quiescence(Position& pos, int alpha, int beta, int ply) {
    if ((nodes_ & 2047) == 0 && outOfLimits()) {
        stopped_ = true;
        return 0;
    }
    ++nodes_;

    const int standPat = evaluate(pos);
    if (standPat >= beta) return beta;
    if (standPat > alpha) alpha = standPat;
    if (ply >= kMaxPly - 1) return standPat;

    // Delta pruning: if even capturing a queen cannot reach alpha, give up.
    if (standPat + kMgValue[QUEEN] < alpha) return alpha;

    MoveList moves = pos.generateMoves(false, -1, true);
    orderMoves(pos, moves, ply, MOVE_NONE);
    const int us = pos.turn();

    for (int i = 0; i < moves.size(); ++i) {
        pos.makeMove(moves[i]);
        if (pos.isSquareAttacked(pos.kingSquare(us), us ^ 1)) {
            pos.undoMove();
            continue;
        }
        const int score = -quiescence(pos, -beta, -alpha, ply + 1);
        pos.undoMove();

        if (stopped_) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }

    return alpha;
}

inline int Search::negamax(Position& pos, int depth, int alpha, int beta, int ply, bool canNull) {
    if ((nodes_ & 2047) == 0 && outOfLimits()) {
        stopped_ = true;
        return 0;
    }

    // Draw detection inside the tree — a repetition or the fifty-move rule is
    // worth exactly zero, and missing it makes the engine play into draws.
    if (ply > 0) {
        if (pos.halfmoves() >= 100 || isRepetition(pos)) return 0;
        if (pos.isInsufficientMaterial()) return 0;
    }

    const bool inCheck = pos.inCheck();
    if (inCheck) ++depth; // check extension
    if (depth <= 0) return quiescence(pos, alpha, beta, ply);

    ++nodes_;

    const int alphaOrig = alpha;
    Move ttMove = MOVE_NONE;
    const TranspositionTable::Entry entry = tt_.probe(pos.hash());
    if (entry.found) {
        ttMove = entry.move;
        if (ply > 0 && entry.depth >= depth) {
            int score = entry.score;
            // Mate scores are stored relative to the root; re-anchor them here.
            if (score > kMateThreshold) score -= ply;
            else if (score < -kMateThreshold) score += ply;

            if (entry.flag == TranspositionTable::Exact) return score;
            if (entry.flag == TranspositionTable::Lower && score > alpha) alpha = score;
            else if (entry.flag == TranspositionTable::Upper && score < beta) beta = score;
            if (alpha >= beta) return score;
        }
    }

    // Null-move pruning: give the opponent a free move; if we are still winning
    // comfortably, this node is unlikely to matter. Skipped in check, in the
    // endgame (zugzwang), and when the position is already losing.
    if (canNull && !inCheck && depth >= 3 && ply > 0 && hasNonPawnMaterial(pos)) {
        const int R = 2 + (depth > 6 ? 1 : 0);
        pos.makeNullMove();
        const int score = -negamax(pos, depth - 1 - R, -beta, -beta + 1, ply + 1, false);
        pos.undoNullMove();
        if (stopped_) return 0;
        if (score >= beta && std::abs(score) < kMateThreshold) return beta;
    }

    MoveList moves = pos.generateMoves(false);
    orderMoves(pos, moves, ply, ttMove);
    const int us = pos.turn();
    Move bestMove = MOVE_NONE;
    int bestScore = -kInfinity;
    int legalCount = 0;

    for (int i = 0; i < moves.size(); ++i) {
        const Move m = moves[i];
        pos.makeMove(m);
        if (pos.isSquareAttacked(pos.kingSquare(us), us ^ 1)) {
            pos.undoMove();
            continue;
        }
        ++legalCount;

        const bool isQuiet = (moveFlags(m) & (FLAG_CAPTURE | FLAG_PROMO)) == 0;
        int score;
        if (legalCount == 1) {
            score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, true);
        } else {
            // Late move reductions: quiet moves late in a well-ordered list are
            // searched shallower first, and re-searched only if they beat alpha.
            int reduction = 0;
            if (depth >= 3 && legalCount > 3 && isQuiet && !inCheck)
                reduction = legalCount > 6 ? 2 : 1;
            score = -negamax(pos, depth - 1 - reduction, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha && reduction > 0)
                score = -negamax(pos, depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha && score < beta)
                score = -negamax(pos, depth - 1, -beta, -alpha, ply + 1, true);
        }

        pos.undoMove();
        if (stopped_) return 0;

        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
        }

        if (score > alpha) {
            alpha = score;
            if (score >= beta) {
                // A quiet move that causes a cutoff is worth remembering.
                if (isQuiet) {
                    if (killers_[(size_t)ply][0] != m) {
                        killers_[(size_t)ply][1] = killers_[(size_t)ply][0];
                        killers_[(size_t)ply][0] = m;
                    }
                    history_[(size_t)(moveFrom(m) * 128 + moveTo(m))] += depth * depth;
                }
                break;
            }
        }
    }

    // No legal moves: checkmate (scored by distance, so shorter mates win) or
    // stalemate.
    if (legalCount == 0) return inCheck ? -kMate + ply : 0;

    int stored = bestScore;
    if (stored > kMateThreshold) stored += ply;
    else if (stored < -kMateThreshold) stored -= ply;

    const uint8_t flag = bestScore <= alphaOrig  ? TranspositionTable::Upper
                         : bestScore >= beta     ? TranspositionTable::Lower
                                                 : TranspositionTable::Exact;
    tt_.store(pos.hash(), bestMove, stored, depth, flag);

    return bestScore;
}

inline std::vector<Move> Search::extractPv(Position& pos, int maxLength) {
    std::vector<Move> pv;
    std::vector<uint64_t> seen;
    for (int i = 0; i < maxLength; ++i) {
        const TranspositionTable::Entry entry = tt_.probe(pos.hash());
        if (!entry.found || !entry.move) break;
        bool loop = false;
        for (uint64_t k : seen)
            if (k == pos.hash()) loop = true;
        if (loop) break; // a repetition loop in the table
        seen.push_back(pos.hash());
        // Confirm the stored move is legal here before trusting it.
        if (!pos.generateMoves().contains(entry.move)) break;
        pv.push_back(entry.move);
        pos.makeMove(entry.move);
    }
    for (size_t i = 0; i < pv.size(); ++i) pos.undoMove();
    return pv;
}

inline Result Search::findBestMove(Position& pos, const Limits& limits, const ProgressFn& onIteration) {
    nodes_ = 0;
    stopped_ = false;
    stopRequest_.store(false, std::memory_order_relaxed);
    limits_ = limits;
    hasDeadline_ = limits.movetimeMs > 0;
    if (hasDeadline_)
        deadline_ = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds((int64_t)limits.movetimeMs);
    killers_ = {};
    std::fill(history_.begin(), history_.end(), 0);
    setupRootKeys(pos);

    MoveList rootMoves = pos.generateMoves();
    Result best;
    if (rootMoves.empty()) return best;
    best.move = rootMoves[0];
    best.score = -kInfinity;
    best.pv = {rootMoves[0]};
    if (rootMoves.size() == 1) {
        best.depth = 1;
        best.nodes = 1;
        return best;
    }

    int alpha = -kInfinity;
    int beta = kInfinity;

    for (int d = 1; d <= limits.depth; ++d) {
        const int score = negamax(pos, d, alpha, beta, 0, true);
        if (stopped_) break;

        // Aspiration windows: retry with a wider window if we fell outside it.
        if (score <= alpha || score >= beta) {
            alpha = -kInfinity;
            beta = kInfinity;
            --d;
            continue;
        }

        alpha = score - 50;
        beta = score + 50;

        std::vector<Move> pv = extractPv(pos, d);
        Result candidate = best; // keeps the previous move when the PV is empty
        candidate.score = score;
        candidate.depth = d;
        candidate.nodes = nodes_;
        candidate.pv = pv;
        if (!pv.empty()) candidate.move = pv[0];
        best = candidate;

        if (onIteration) onIteration(best);

        // A forced mate has been found — no need to search deeper.
        if (std::abs(score) > kMateThreshold) break;
    }

    return best;
}

namespace detail {
inline uint64_t splitmix64(uint64_t& x) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
} // namespace detail

inline Result Search::chooseMove(Position& pos, const Level& level, uint64_t seed,
                                 const ProgressFn& onIteration) {
    Limits limits;
    limits.depth = level.depth;
    limits.nodes = level.nodes;
    limits.movetimeMs = level.movetimeMs;
    Result result = findBestMove(pos, limits, onIteration);
    if (level.randomness <= 0 || result.move == MOVE_NONE) return result;

    // The root search may have just stopped on its node or time budget. The
    // re-scoring below is shallow, so run it without limits: an aborted search
    // returns 0, which would flatten every score and turn the near-best
    // sampling into a completely random move.
    stopped_ = false;
    stopRequest_.store(false, std::memory_order_relaxed);
    limits_ = Limits{};
    hasDeadline_ = false;

    // Re-score the root moves shallowly and sample from the near-best set.
    MoveList rootMoves = pos.generateMoves();
    std::vector<ScoredMove> scored;
    scored.reserve((size_t)rootMoves.size());
    for (int i = 0; i < rootMoves.size(); ++i) {
        pos.makeMove(rootMoves[i]);
        const int score =
            -negamax(pos, std::max(0, level.depth - 1), -kInfinity, kInfinity, 1, true);
        pos.undoMove();
        scored.push_back(ScoredMove{score, rootMoves[i]});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const ScoredMove& a, const ScoredMove& b) { return a.score > b.score; });

    const int cutoff = scored[0].score - level.randomness;
    std::vector<ScoredMove> pool;
    for (const ScoredMove& s : scored)
        if (s.score >= cutoff) pool.push_back(s);

    uint64_t state = seed;
    const size_t pickIndex = (size_t)(detail::splitmix64(state) % pool.size());
    result.move = pool[pickIndex].move;
    result.score = pool[pickIndex].score;
    return result;
}

} // namespace ai
} // namespace chess
