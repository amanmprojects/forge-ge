// test_chess — headless rules tests for the 0x88 chess core.
//
// Ported from amanmprojects/chess (test/perft.test.mjs, test/rules.test.mjs):
// the published perft counts are the yardstick for move generation, and the
// rules tests cover the situations that trip implementations up (castling
// legality, en passant, promotion, pins, draw rules, SAN).
//
//   ./test_chess          fast suite (runs in a fraction of a second)
//   ./test_chess --deep   adds the multi-million-node perft depths
//
// Returns nonzero when any check fails.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "examples/chess/ai.hpp"
#include "examples/chess/chess.hpp"

using chess::Move;
using chess::MoveList;
using chess::Position;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)
#define CHECK_EQ_STR(a, b) do { \
    const std::string va = (a), vb = (b); \
    if (va != vb) { \
        std::printf("FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b, \
                    va.c_str(), vb.c_str()); \
        ++failures; \
    } \
} while (0)
#define CHECK_EQ_INT(a, b) do { \
    const long long va = (long long)(a), vb = (long long)(b); \
    if (va != vb) { \
        std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va, vb); \
        ++failures; \
    } \
} while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Sorted SANs of every legal move in the position. */
static std::vector<std::string> sans(Position& pos) {
    MoveList moves = pos.generateMoves();
    std::vector<std::string> out;
    out.reserve((size_t)moves.size());
    for (int i = 0; i < moves.size(); ++i) out.push_back(pos.toSan(moves[i]));
    std::sort(out.begin(), out.end());
    return out;
}

/** Sorted UCI strings of the legal moves originating from `sq`. */
static std::vector<std::string> ucis(Position& pos, int sq) {
    MoveList moves = pos.generateMoves(true, sq);
    std::vector<std::string> out;
    out.reserve((size_t)moves.size());
    for (int i = 0; i < moves.size(); ++i) out.push_back(chess::moveToUci(moves[i]));
    std::sort(out.begin(), out.end());
    return out;
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

/** Whitespace-separated FEN field (0-based); "?" when missing. */
static std::string fenField(const std::string& fen, int index) {
    size_t pos = 0;
    for (int i = 0; i < index; ++i) {
        pos = fen.find(' ', pos);
        if (pos == std::string::npos) return "?";
        ++pos;
    }
    const size_t end = fen.find(' ', pos);
    return fen.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

/** Castling moves available, with any check/mate suffix stripped. */
static std::vector<std::string> castles(Position& pos) {
    std::vector<std::string> out;
    for (const std::string& san : sans(pos)) {
        std::string s = san;
        while (!s.empty() && (s.back() == '+' || s.back() == '#')) s.pop_back();
        if (s == "O-O" || s == "O-O-O") out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------------------
// FEN round-tripping
// ---------------------------------------------------------------------------

static void testFenRoundTrip() {
    const char* fens[] = {
        chess::START_FEN,
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 b - - 12 47",
        "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1",
        "rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3",
    };
    for (const char* fen : fens) {
        Position p;
        CHECK(p.load(fen));
        CHECK_EQ_STR(p.fen(), fen);
    }

    // Malformed input is rejected, not fatal.
    // Malformed input is rejected, not fatal, and leaves the position alone.
    Position p;
    CHECK(!p.load("not a fen"));
    CHECK(!p.load("8/8/8/8/8/8/8/8 w - - 0 1"));  // no kings
    CHECK(!p.load("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq")); // too few fields
    CHECK_EQ_STR(p.fen(), chess::START_FEN);
    // Four fields is a valid FEN; the move counters default to 0 1.
    Position q;
    CHECK(q.load("4k3/8/8/8/8/8/8/4K3 w - -"));
    CHECK_EQ_STR(q.fen(), "4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    std::printf("fen ok\n");
}

// ---------------------------------------------------------------------------
// Castling
// ---------------------------------------------------------------------------

static void testCastling() {
    {
        Position game("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
        CHECK_EQ_INT(castles(game).size(), 2);

        CHECK(game.play("O-O"));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("g1")), chess::piece(chess::WHITE, chess::KING));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("f1")), chess::piece(chess::WHITE, chess::ROOK));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("h1")), chess::EMPTY);
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("e1")), chess::EMPTY);

        CHECK(game.play("O-O-O")); // black castles long
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("c8")), chess::piece(chess::BLACK, chess::KING));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("d8")), chess::piece(chess::BLACK, chess::ROOK));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("a8")), chess::EMPTY);
    }
    {
        // Cannot castle out of check.
        Position game("4r2k/8/8/8/8/8/8/R3K2R w KQ - 0 1");
        CHECK_EQ_INT(castles(game).size(), 0);
    }
    {
        // Cannot pass through an attacked square...
        Position game("5r1k/8/8/8/8/8/8/R3K2R w KQ - 0 1");
        CHECK(!has(castles(game), "O-O"));
        CHECK(has(castles(game), "O-O-O"));
    }
    {
        // ...nor land on one.
        Position game("k5r1/8/8/8/8/8/8/R3K2R w KQ - 0 1");
        CHECK(!has(castles(game), "O-O"));
    }
    {
        // d1 attacked forbids queenside; kingside is unaffected.
        Position game("3r3k/8/8/8/8/8/8/R3K2R w KQ - 0 1");
        CHECK(!has(castles(game), "O-O-O"));
        CHECK(has(castles(game), "O-O"));
    }
    {
        // b1 attacked is fine: the king never stands there.
        Position game("2k5/8/8/8/8/8/p7/R3K2R w KQ - 0 1");
        CHECK(has(castles(game), "O-O-O"));
    }
    {
        // Occupied transit squares block castling.
        Position game("r3k2r/8/8/8/8/8/8/R2QK1NR w KQkq - 0 1");
        CHECK_EQ_INT(castles(game).size(), 0);
    }
    {
        // Moving the rook forfeits only that side's right.
        Position game("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
        CHECK(game.play("Rh1g1"));
        CHECK_EQ_STR(fenField(game.fen(), 2), "Qkq");
    }
    {
        // Moving the king forfeits both.
        Position game("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
        CHECK(game.play("Ke1d1"));
        CHECK_EQ_STR(fenField(game.fen(), 2), "kq");
    }
    {
        // Capturing a rook on its home square clears those rights.
        Position game("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
        CHECK(game.play("Ra1a8"));
        CHECK_EQ_STR(fenField(game.fen(), 2), "Kk");
    }
    std::printf("castling ok\n");
}

// ---------------------------------------------------------------------------
// En passant
// ---------------------------------------------------------------------------

static void testEnPassant() {
    {
        Position game("rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 3");
        const int d4 = chess::squareFromAlgebraic("d4");
        CHECK(has(ucis(game, d4), "d4e3"));
        CHECK(game.play("dxe3"));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("e3")), chess::piece(chess::BLACK, chess::PAWN));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("e4")), chess::EMPTY);
    }
    {
        // The right expires after any other move.
        Position game("rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 3");
        CHECK(game.play("Nc6"));
        CHECK(game.play("Nf3"));
        CHECK(!has(ucis(game, chess::squareFromAlgebraic("d4")), "d4e3"));
    }
    {
        // A double push sets the ep square only for one ply.
        Position game(chess::START_FEN);
        CHECK(game.play("e4"));
        CHECK_EQ_STR(fenField(game.fen(), 3), "e3");
        CHECK(game.play("e5"));
        CHECK_EQ_STR(fenField(game.fen(), 3), "e6");
        CHECK(game.play("Nf3"));
        CHECK_EQ_STR(fenField(game.fen(), 3), "-");
    }
    {
        // An ep capture that would expose the king is illegal.
        Position game("8/2p5/8/KP5r/8/8/8/7k b - - 0 1");
        CHECK(game.play("c5"));
        CHECK_EQ_STR(fenField(game.fen(), 3), "c6");
        CHECK(!has(ucis(game, chess::squareFromAlgebraic("b5")), "b5c6"));
    }
    std::printf("en passant ok\n");
}

// ---------------------------------------------------------------------------
// Promotion
// ---------------------------------------------------------------------------

static void testPromotion() {
    {
        Position game("8/4P3/8/8/8/8/8/K6k w - - 0 1");
        const std::vector<std::string> promos = ucis(game, chess::squareFromAlgebraic("e7"));
        CHECK_EQ_INT(promos.size(), 4);
        CHECK(has(promos, "e7e8n"));
        CHECK(has(promos, "e7e8q"));
    }
    {
        Position game("8/4P3/8/8/8/8/8/K6k w - - 0 1");
        CHECK(game.play("e8=N"));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("e8")), chess::piece(chess::WHITE, chess::KNIGHT));
    }
    {
        // Object-style input without a piece defaults to a queen: "e7e8".
        Position game("8/4P3/8/8/8/8/8/K6k w - - 0 1");
        CHECK(game.playMove(game.resolve("e7e8")));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("e8")), chess::piece(chess::WHITE, chess::QUEEN));
    }
    {
        // Capture-promotion, and undo restores both pieces.
        Position game("3r4/4P3/8/8/8/8/8/K6k w - - 0 1");
        const std::string before = game.fen();
        CHECK(game.play("exd8=Q"));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("d8")), chess::piece(chess::WHITE, chess::QUEEN));
        CHECK(game.undoPlayed());
        CHECK_EQ_STR(game.fen(), before);
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("e7")), chess::piece(chess::WHITE, chess::PAWN));
        CHECK_EQ_INT(game.at(chess::squareFromAlgebraic("d8")), chess::piece(chess::BLACK, chess::ROOK));
    }
    std::printf("promotion ok\n");
}

// ---------------------------------------------------------------------------
// Check, pins, mate
// ---------------------------------------------------------------------------

static void testCheck() {
    {
        // A pinned piece may not abandon the king.
        Position game("4r2k/8/8/8/8/8/4N3/4K3 w - - 0 1");
        CHECK_EQ_INT(ucis(game, chess::squareFromAlgebraic("e2")).size(), 0);
    }
    {
        // ...but may capture along the pin line.
        Position game("4r2k/8/8/8/8/8/4R3/4K3 w - - 0 1");
        CHECK(has(ucis(game, chess::squareFromAlgebraic("e2")), "e2e8"));
    }
    {
        // In check, only resolving moves are legal.
        Position game("4r2k/8/8/8/1B6/8/8/4K3 w - - 0 1");
        CHECK(game.inCheck());
        MoveList moves = game.generateMoves();
        for (int i = 0; i < moves.size(); ++i) {
            Position probe(game.fen());
            CHECK(probe.playMove(moves[i]));
            CHECK(!probe.inCheck(chess::WHITE));
        }
        CHECK(has(sans(game), "Be7"));
    }
    {
        // Back-rank mate.
        Position game("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
        CHECK(game.play("Ra8"));
        CHECK(game.isCheckmate());
        Position::Status st = game.status();
        CHECK(st.over);
        CHECK_EQ_STR(st.result, "1-0");
        CHECK_EQ_STR(st.reason, "checkmate");
    }
    {
        // Fool's mate.
        Position game;
        CHECK(game.play("f3"));
        CHECK(game.play("e5"));
        CHECK(game.play("g4"));
        CHECK(game.play("Qh4#"));
        CHECK(game.isCheckmate());
        CHECK_EQ_STR(game.status().result, "0-1");
    }
    {
        // Smothered stalemate.
        Position game("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
        CHECK(game.isStalemate());
        CHECK(!game.isCheckmate());
        CHECK_EQ_STR(game.status().reason, "stalemate");
    }
    {
        // The king may not step into an attacked square.
        Position game("7k/8/8/8/8/8/5q2/4K3 w - - 0 1");
        const std::vector<std::string> dests = ucis(game, chess::squareFromAlgebraic("e1"));
        CHECK(!has(dests, "e1d2"));
        CHECK(!has(dests, "e1e2"));
        CHECK(!has(dests, "e1f1"));
    }
    {
        // Fleeing along the checking ray is not fleeing.
        Position game("4r2k/8/8/8/8/8/8/4K3 w - - 0 1");
        const std::vector<std::string> dests = ucis(game, chess::squareFromAlgebraic("e1"));
        CHECK_EQ_INT(dests.size(), 4);
        CHECK(has(dests, "e1d1"));
        CHECK(has(dests, "e1d2"));
        CHECK(has(dests, "e1f1"));
        CHECK(has(dests, "e1f2"));
    }
    std::printf("check/pins/mate ok\n");
}

// ---------------------------------------------------------------------------
// Draws
// ---------------------------------------------------------------------------

static void testDraws() {
    {
        const char* draws[] = {
            "8/8/4k3/8/8/4K3/8/8 w - - 0 1",      // K vs K
            "8/8/4k3/8/8/4K3/8/5N2 w - - 0 1",    // K+N vs K
            "8/8/4k3/8/8/4K3/8/5B2 w - - 0 1",    // K+B vs K
            "2b5/8/4k3/8/8/4K3/8/5B2 w - - 0 1",  // bishops, both on light squares
        };
        for (const char* fen : draws) {
            Position p(fen);
            CHECK(p.isInsufficientMaterial());
        }
        const char* notDraws[] = {
            "8/8/4k3/8/8/4K3/8/4BB2 w - - 0 1", // two bishops, opposite colours
            "8/8/4k3/8/8/4K3/8/4NN2 w - - 0 1", // two knights
            "8/8/4k3/8/8/4K3/8/5R2 w - - 0 1",  // rook
            "8/8/4k3/8/8/4K3/4P3/8 w - - 0 1",  // pawn
        };
        for (const char* fen : notDraws) {
            Position p(fen);
            CHECK(!p.isInsufficientMaterial());
        }
    }
    {
        // Fifty-move rule.
        Position game("4k3/8/8/8/8/8/8/R3K3 w - - 99 60");
        CHECK(!game.isFiftyMoveDraw());
        CHECK(game.play("Ra2"));
        CHECK(game.isFiftyMoveDraw());
        CHECK_EQ_STR(game.status().reason, "fifty-move rule");
    }
    {
        // A capture or pawn move resets the halfmove clock.
        Position game("4k3/8/8/8/8/8/P7/R3K3 w - - 40 60");
        CHECK(game.play("a4"));
        CHECK_EQ_INT(game.halfmoves(), 0);
    }
    {
        // Threefold repetition by shuffling the rooks.
        Position game("4k3/8/8/8/8/8/8/R3K2R w - - 0 1");
        const char* line[] = {"Ra2", "Ke7", "Ra1", "Ke8", "Ra2", "Ke7", "Ra1", "Ke8"};
        for (const char* san : line) CHECK(game.play(san));
        CHECK(game.isThreefoldRepetition());
        CHECK_EQ_STR(game.status().reason, "threefold repetition");
    }
    std::printf("draws ok\n");
}

// ---------------------------------------------------------------------------
// SAN
// ---------------------------------------------------------------------------

static void testSan() {
    {
        // Disambiguation by file, rank and full square.
        Position game("4k3/8/8/8/8/8/8/1N2KN2 w - - 0 1");
        CHECK(has(sans(game), "Nbd2"));
        CHECK(has(sans(game), "Nfd2"));
    }
    {
        Position game("4k3/8/8/8/8/R7/8/R3K3 w - - 0 1");
        CHECK(has(sans(game), "R1a2"));
        CHECK(has(sans(game), "R3a2"));
    }
    {
        Position game("Q2Q4/8/7k/8/8/8/8/3QK3 w - - 0 1");
        const std::vector<std::string> found = sans(game);
        CHECK(has(found, "Qad5"));
        CHECK(has(found, "Qd8d5"));
        CHECK(has(found, "Q1d5"));
    }
    {
        // Check and mate markers.
        Position game("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
        CHECK(has(sans(game), "Ra8#"));
        Position game2("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
        CHECK(has(sans(game2), "Ra8+"));
    }
    {
        // SAN and UCI parse back to the same move, and SAN round-trips.
        Position game;
        const char* line[] = {"e4", "e5", "Nf3", "Nc6", "Bb5", "a6", "Ba4", "Nf6", "O-O", "Be7"};
        for (const char* san : line) {
            Move m = game.resolve(san);
            CHECK(m != chess::MOVE_NONE);
            CHECK_EQ_STR(game.toSan(m), san);
            CHECK(game.playMove(m));
        }
        CHECK_EQ_STR(game.fen(),
                     "r1bqk2r/1pppbppp/p1n2n2/4p3/B3P3/5N2/PPPP1PPP/RNBQ1RK1 w kq - 4 6");
    }
    {
        // Illegal and malformed input is rejected, not thrown.
        Position game;
        CHECK(!game.play("e5"));
        CHECK(!game.play("Ke2"));
        CHECK(!game.play("garbage"));
        CHECK(!game.play("a1a1"));
        CHECK(!game.play("e2e9"));
        CHECK_EQ_STR(game.fen(), chess::START_FEN);
    }
    std::printf("san ok\n");
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

static void testUndo() {
    {
        Position game;
        std::vector<std::string> seen{game.fen()};
        const char* line[] = {"e4", "c5", "Nf3", "d6", "d4", "cxd4", "Nxd4", "Nf6", "Nc3", "a6"};
        for (const char* san : line) {
            CHECK(game.play(san));
            seen.push_back(game.fen());
        }
        for (size_t i = seen.size(); i-- > 1;) {
            CHECK_EQ_STR(game.fen(), seen[i]);
            CHECK(game.undoPlayed());
        }
        CHECK_EQ_STR(game.fen(), chess::START_FEN);
        CHECK(!game.undoPlayed()); // no-op past the start
    }
    {
        // Undoing castling and en passant restores both moved pieces.
        Position game("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
        const std::string before = game.fen();
        CHECK(game.play("O-O-O"));
        CHECK(game.undoPlayed());
        CHECK_EQ_STR(game.fen(), before);

        Position ep("rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 3");
        const std::string epBefore = ep.fen();
        CHECK(ep.play("dxe3"));
        CHECK(ep.undoPlayed());
        CHECK_EQ_STR(ep.fen(), epBefore);
        CHECK_EQ_INT(ep.at(chess::squareFromAlgebraic("e4")), chess::piece(chess::WHITE, chess::PAWN));
    }
    {
        // Turn and fullmove counters advance correctly.
        Position game;
        CHECK_EQ_INT(game.turn(), chess::WHITE);
        CHECK(game.play("e4"));
        CHECK_EQ_INT(game.turn(), chess::BLACK);
        CHECK_EQ_INT(game.fullmoves(), 1);
        CHECK(game.play("e5"));
        CHECK_EQ_INT(game.turn(), chess::WHITE);
        CHECK_EQ_INT(game.fullmoves(), 2);
    }
    std::printf("undo ok\n");
}

// ---------------------------------------------------------------------------
// Perft
// ---------------------------------------------------------------------------

struct PerftCase {
    const char* name;
    const char* fen;
    uint64_t counts[6]; // index 0 = depth 1; 0 = not checked
    int defaultDepth;
    int deepDepth;
};

static const PerftCase kPerft[] = {
    {"initial position", chess::START_FEN,
     {20, 400, 8902, 197281, 4865609, 119060324}, 4, 5},
    {"kiwipete (castling, pins, checks)",
     "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
     {48, 2039, 97862, 4085603, 193690690, 0}, 3, 4},
    {"en passant discovered check", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
     {14, 191, 2812, 43238, 674624, 0}, 4, 5},
    {"promotion and underpromotion",
     "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
     {6, 264, 9467, 422333, 15833292, 0}, 3, 4},
    {"mirrored promotion position", "r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1",
     {6, 264, 9467, 422333, 15833292, 0}, 3, 3},
    {"tactical middlegame", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
     {44, 1486, 62379, 2103487, 89941194, 0}, 3, 4},
    {"symmetric position", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
     {46, 2079, 89890, 3894594, 164075551, 0}, 3, 4},
};

static void testPerft(bool deep) {
    uint64_t totalNodes = 0;
    for (const PerftCase& pc : kPerft) {
        Position game(pc.fen);
        const int last = deep ? pc.deepDepth : pc.defaultDepth;
        for (int depth = 1; depth <= last; ++depth) {
            const uint64_t expected = pc.counts[depth - 1];
            if (expected == 0) break;
            const uint64_t nodes = game.perft(depth);
            totalNodes += nodes;
            if (nodes != expected) {
                std::printf("FAIL perft %s depth %d: %llu (expected %llu)\n", pc.name, depth,
                            (unsigned long long)nodes, (unsigned long long)expected);
                ++failures;
            }
            // The board must be pristine after a perft run.
            CHECK_EQ_STR(game.fen(), pc.fen);
        }
    }
    std::printf("perft ok (%llu nodes)\n", (unsigned long long)totalNodes);
}

/** Recomputing the hash from scratch must agree with the incremental update. */
static void hashWalk(Position& game, int depth) {
    if (depth == 0) return;
    MoveList moves = game.generateMoves();
    for (int i = 0; i < moves.size(); ++i) {
        const uint64_t before = game.hash();
        const std::string fen = game.fen();
        game.makeMove(moves[i]);
        const uint64_t incremental = game.hash();
        Position fresh(game.fen());
        if (fresh.hash() != incremental) {
            std::printf("FAIL hash drift after %s\n", chess::moveToUci(moves[i]).c_str());
            ++failures;
        }
        hashWalk(game, depth - 1);
        game.undoMove();
        if (game.hash() != before || game.fen() != fen) {
            std::printf("FAIL make/unmake did not restore state at %s\n",
                        chess::moveToUci(moves[i]).c_str());
            ++failures;
        }
    }
}

static void testHash() {
    Position game("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    hashWalk(game, 3);
    std::printf("hash ok\n");
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

/** Search `fen` at `depth` with no time limit; returns the best move found. */
static chess::ai::Result think(const char* fen, int depth) {
    Position pos(fen);
    chess::ai::Search search(size_t(1) << 16);
    chess::ai::Limits limits;
    limits.depth = depth;
    limits.movetimeMs = 0;
    return search.findBestMove(pos, limits);
}

static void testAi() {
    {
        // The evaluation is symmetric in the start position: only tempo remains.
        Position start;
        CHECK_EQ_INT(chess::ai::evaluate(start), 10);
    }
    {
        // Back-rank mate in one is found and scored as a mate.
        const chess::ai::Result r = think("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", 4);
        Position pos("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
        CHECK_EQ_INT(r.move, pos.resolve("Ra8"));
        CHECK(std::abs(r.score) > chess::ai::kMateThreshold);
    }
    {
        // A free queen is captured (and not a random developing move).
        Position pos("k7/8/8/3q4/8/8/3Q4/3K4 w - - 0 1");
        chess::ai::Search search(size_t(1) << 16);
        chess::ai::Limits limits;
        limits.depth = 5;
        const chess::ai::Result r = search.findBestMove(pos, limits);
        CHECK_EQ_INT(r.move, pos.resolve("Qxd5"));
    }
    {
        // Mate in two (several first moves work: Qa8+ or Kf7+). Verify that
        // the move the engine picked really forces mate in two against every
        // legal reply, rather than pinning one specific move.
        Position pos("7k/8/5K2/8/8/8/8/Q7 w - - 0 1");
        chess::ai::Search search(size_t(1) << 16);
        chess::ai::Limits limits;
        limits.depth = 6;
        const chess::ai::Result r = search.findBestMove(pos, limits);
        CHECK(std::abs(r.score) > chess::ai::kMateThreshold - 10);

        Position replay(pos.fen());
        CHECK(replay.playMove(r.move));
        MoveList replies = replay.generateMoves();
        CHECK(!replies.empty());
        for (int i = 0; i < replies.size(); ++i) {
            replay.makeMove(replies[i]);
            bool mateInOne = false;
            MoveList white = replay.generateMoves();
            for (int j = 0; j < white.size() && !mateInOne; ++j) {
                replay.makeMove(white[j]);
                if (replay.isCheckmate()) mateInOne = true;
                replay.undoMove();
            }
            replay.undoMove();
            CHECK(mateInOne); // every reply runs into mate
        }

        // The reported PV has to be playable from the root.
        Position pvReplay(pos.fen());
        for (Move m : r.pv) CHECK(pvReplay.playMove(m));
    }
    {
        // Every level returns a legal move; same seed = same choice.
        CHECK_EQ_INT(chess::ai::levels().size(), 5);
        const chess::ai::Level custom[] = {
            {"quick1", 1, 2000, 0.0, 180},
            {"quick2", 2, 5000, 0.0, 90},
            {"quick3", 3, 20000, 0.0, 30},
            {"quick4", 3, 20000, 0.0, 0},
        };
        for (const chess::ai::Level& level : custom) {
            Position a(chess::START_FEN);
            Position b(chess::START_FEN);
            chess::ai::Search s1(size_t(1) << 16), s2(size_t(1) << 16);
            const Move m1 = s1.chooseMove(a, level, 0xC0FFEE).move;
            const Move m2 = s2.chooseMove(b, level, 0xC0FFEE).move;
            CHECK(m1 != chess::MOVE_NONE);
            CHECK_EQ_INT(m1, m2);
        }
    }
    {
        // An out-of-time search still returns a legal best move from a
        // completed shallower iteration.
        Position pos("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4");
        chess::ai::Search search(size_t(1) << 14);
        chess::ai::Limits limits;
        limits.depth = 20;
        limits.movetimeMs = 5.0; // tiny budget: stops mid-iteration
        const chess::ai::Result r = search.findBestMove(pos, limits);
        CHECK(r.move != chess::MOVE_NONE);
        CHECK(pos.generateMoves().contains(r.move));
    }
    std::printf("ai ok\n");
}

int main(int argc, char** argv) {
    bool deep = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--deep") == 0) deep = true;

    testFenRoundTrip();
    testCastling();
    testEnPassant();
    testPromotion();
    testCheck();
    testDraws();
    testSan();
    testUndo();
    testHash();
    testAi();
    testPerft(deep);

    if (failures) {
        std::printf("chess tests: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("chess tests: all passed%s\n", deep ? " (deep perft)" : "");
    return 0;
}
