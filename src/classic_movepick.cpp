/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>

#include "bitboard.h"
#include "movepick.h"

namespace Stockfish::Search::Classic {

    namespace {

        enum Stages {
            // generate main search moves
            MAIN_TT,
            CAPTURE_INIT,
            GOOD_CAPTURE,
            REFUTATION,
            QUIET_INIT,
            QUIET,
            BAD_CAPTURE,

            // generate evasion moves
            EVASION_TT,
            EVASION_INIT,
            EVASION,

            // generate probcut moves
            PROBCUT_TT,
            PROBCUT_INIT,
            PROBCUT,

            // generate qsearch moves
            QSEARCH_TT,
            QCAPTURE_INIT,
            QCAPTURE,
            QCHECK_INIT,
            QCHECK
        };

        // partial_insertion_sort() sorts moves in descending order up to and including a given limit.
        // The order of moves smaller than the limit is left unspecified.
        void partial_insertion_sort(ExtMove* begin, ExtMove* end, int limit) {

            for (ExtMove* sortedEnd = begin, *p = begin + 1; p < end; ++p)
                if (p->value >= limit)
                {
                    ExtMove tmp = *p, * q;
                    *p = *++sortedEnd;
                    for (q = sortedEnd; q != begin && *(q - 1) < tmp; --q)
                        *q = *(q - 1);
                    *q = tmp;
                }
        }

    } // namespace


    // Constructors of the MovePicker class. As arguments we pass information
    // to help it to return the (presumably) good moves first, to decide which
    // moves to return (in the quiescence search, for instance, we only want to
    // search captures, promotions, and some checks) and how important good move
    // ordering is at the current node.

    // MovePicker constructor for the main search
    MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh,
        const CapturePieceToHistory* cph,
        const PieceToHistory** ch,
        Move cm,
        const Move* killers)
        : pos(p), mainHistory(mh), captureHistory(cph), continuationHistory(ch),
        ttMove(ttm), refutations{ {killers[0], 0}, {killers[1], 0}, {cm, 0} }, depth(d)
    {
        assert(d > 0);

        stage = (pos.checkers() ? EVASION_TT : MAIN_TT) + !(ttm && pos.pseudo_legal(ttm));
        threatenedPieces = 0;
    }

    // MovePicker constructor for quiescence search
    MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh,
        const CapturePieceToHistory* cph,
        const PieceToHistory** ch,
        Square rs)
        : pos(p), mainHistory(mh), captureHistory(cph), continuationHistory(ch), ttMove(ttm), recaptureSquare(rs), depth(d)
    {
        assert(d <= 0);

        stage = (pos.checkers() ? EVASION_TT : QSEARCH_TT) + !(ttm && pos.pseudo_legal(ttm));
    }

    // MovePicker constructor for ProbCut: we generate captures with SEE greater
    // than or equal to the given threshold.
    MovePicker::MovePicker(const Position& p, Move ttm, int th, const CapturePieceToHistory* cph)
        : pos(p), captureHistory(cph), ttMove(ttm), threshold(th)
    {
        assert(!pos.checkers());

        stage = PROBCUT_TT + !(ttm && pos.capture_stage(ttm) && pos.pseudo_legal(ttm) && pos.see_ge<true>(ttm, threshold));
    }

    // MovePicker::score() assigns a numerical value to each move in a list, used
    // for sorting. Captures are ordered by Most Valuable Victim (MVV), preferring
    // captures with a good history. Quiets moves are ordered using the histories.
    template<GenType Type, bool SearchMate>
    void MovePicker::score() {

        static_assert(Type == CAPTURES || Type == QUIETS || Type == EVASIONS, "Wrong type");

        [[maybe_unused]] Bitboard threatenedByPawn, threatenedByMinor, threatenedByRook, threatenedPieces;
        if constexpr (Type == QUIETS)
        {
            Color us = pos.side_to_move();

            threatenedByPawn = pos.attacks_by<PAWN>(~us);
            threatenedByMinor = pos.attacks_by<KNIGHT>(~us) | pos.attacks_by<BISHOP>(~us) | threatenedByPawn;
            threatenedByRook = pos.attacks_by<ROOK>(~us) | threatenedByMinor;

            // Pieces threatened by pieces of lesser material value
            threatenedPieces = (pos.pieces(us, QUEEN) & threatenedByRook)
                | (pos.pieces(us, ROOK) & threatenedByMinor)
                | (pos.pieces(us, KNIGHT, BISHOP) & threatenedByPawn);
        }

        for (auto& m : *this)
        {
            Piece movedPiece = pos.moved_piece(m);
            Square to = m.to_sq();

            if constexpr (Type == CAPTURES)
            {
                Piece pto = pos.piece_on(to);
                m.value = (7 * int(PieceValueME[MG][pto]) + (*captureHistory)[movedPiece][to][type_of(pto)]) / 16;
            }

            else if constexpr (Type == QUIETS)
            {
                PieceType pt = type_of(movedPiece);
                Square from = m.from_sq();

                // histories
                m.value = (*mainHistory)[pos.side_to_move()][m.from_to()] * 2;
                m.value += (*continuationHistory[0])[movedPiece][to] * 2;
                m.value += (*continuationHistory[1])[movedPiece][to];
                m.value += (*continuationHistory[3])[movedPiece][to];
                m.value += (*continuationHistory[5])[movedPiece][to];

                // bonus for escaping from capture
                m.value += threatenedPieces & from
                    ? (pt == QUEEN && !(to & threatenedByRook) ? 50000
                        : pt == ROOK && !(to & threatenedByMinor) ? 25000
                        : !(to & threatenedByPawn) ? 15000
                        : 0)
                    : 0;

                if constexpr (!SearchMate)
                {
                    m.value += (*continuationHistory[2])[movedPiece][to] / 4;

                    // bonus for checks
                    m.value += bool(pos.check_squares(pt) & to) * 16384;

                    // malus for putting piece en prise
                    m.value -= !(threatenedPieces & from)
                        ? (pt == QUEEN ? bool(to & threatenedByRook) * 50000
                            + bool(to & threatenedByMinor) * 10000
                            + bool(to & threatenedByPawn) * 20000
                            : pt == ROOK ? bool(to & threatenedByMinor) * 25000
                            + bool(to & threatenedByPawn) * 10000
                            : pt != PAWN ? bool(to & threatenedByPawn) * 15000
                            : 0)
                        : 0;
                }
            }

            else // Type == EVASIONS
            {
                if (pos.capture_stage(m))
                    m.value = PieceValueME[MG][pos.piece_on(to)] - Value(type_of(movedPiece)) + (1 << 28);
                else
                    m.value = (*mainHistory)[pos.side_to_move()][m.from_to()] + (*continuationHistory[0])[movedPiece][to];
            }

            if constexpr (SearchMate && (Type == CAPTURES || Type == QUIETS))
            {
                Color us = pos.side_to_move();
                Square theirKing = pos.square<KING>(~us);
                Bitboard kingRing = pos.attacks_from<KING>(theirKing);

                if (pos.gives_check(m))
                {
                    m.value += 20000 - 400 * distance(theirKing, to);

                    // Bonus for a knight check
                    if (type_of(movedPiece) == KNIGHT)
                        m.value += 3000;

                    // Bonus for queen/rook contact checks
                    else if ((type_of(movedPiece) == QUEEN || type_of(movedPiece) == ROOK) && distance(theirKing, to) == 1)
                        m.value += 4000;
                }

                // Bonus for pawns
                if (type_of(movedPiece) == PAWN)
                {
                    m.value += 640 * edge_distance(file_of(to)) + 1280 * relative_rank(us, to);

                    // Extra bonus for double push
                    m.value += 4000 * (distance<Rank>(to, m.from_sq()) == 2);
                }

                // Bonus for a knight eventually able to give check on the next move
                if (type_of(movedPiece) == KNIGHT)
                {
                    if (pos.attacks_from<KNIGHT>(to) & pos.check_squares(KNIGHT))
                        m.value += 6000;

                    m.value += 2560 * popcount(PseudoAttacks[KNIGHT][to] & kingRing);
                }

                // Bonus for a queen eventually able to give check on the next move
                else if (type_of(movedPiece) == QUEEN)
                {
                    if (pos.attacks_from<QUEEN>(to) & pos.check_squares(QUEEN))
                        m.value += 5000;

                    m.value += 1280 * popcount(PseudoAttacks[QUEEN][to] & kingRing);
                }

                // Bonus for a rook eventually able to give check on the next move
                else if (type_of(movedPiece) == ROOK)
                {
                    if (pos.attacks_from<ROOK>(to) & pos.check_squares(ROOK))
                        m.value += 4000;

                    m.value += 960 * popcount(PseudoAttacks[ROOK][to] & kingRing);
                }

                // Bonus for a bishop eventually able to give check on the next move
                else if (type_of(movedPiece) == BISHOP)
                {
                    if (pos.attacks_from<BISHOP>(to) & pos.check_squares(BISHOP))
                        m.value += 3000;

                    m.value += 640 * popcount(PseudoAttacks[BISHOP][to] & kingRing);
                }
            }
        }
    }

    // MovePicker::select() returns the next move satisfying a predicate function.
    // It never returns the TT move.
    template<MovePicker::PickType T, typename Pred>
    Move MovePicker::select(Pred filter) {

        while (cur < endMoves)
        {
            if (T == Best)
                std::swap(*cur, *std::max_element(cur, endMoves));

            if (*cur != ttMove && filter())
                return *cur++;

            cur++;
        }
        return Move::none();
    }

    // MovePicker::next_move() is the most important method of the MovePicker class. It
    // returns a new pseudo-legal move every time it is called until there are no more
    // moves left, picking the move with the highest score from a list of generated moves.
    template<bool SearchMate>
    Move MovePicker::next_move(bool skipQuiets) {

    top:
        switch (stage) {

        case MAIN_TT:
        case EVASION_TT:
        case QSEARCH_TT:
        case PROBCUT_TT:
            ++stage;
            return ttMove;

        case CAPTURE_INIT:
        case PROBCUT_INIT:
        case QCAPTURE_INIT:
            cur = endBadCaptures = moves;
            endMoves = generate<CAPTURES>(pos, cur);

            score<CAPTURES, SearchMate>();
            partial_insertion_sort(cur, endMoves, std::numeric_limits<int>::min());
            ++stage;
            goto top;

        case GOOD_CAPTURE:
            if (select<Next>([&]() {
                // Move losing capture to endBadCaptures to be tried later
                return pos.see_ge<true>(*cur, Value(-69 * cur->value / 1024)) ? true : (*endBadCaptures++ = *cur, false);
                }))
                return *(cur - 1);

                // Prepare the pointers to loop over the refutations array
                cur = std::begin(refutations);
                endMoves = std::end(refutations);

                // If the countermove is the same as a killer, skip it
                if (refutations[0] == refutations[2] || refutations[1] == refutations[2])
                    --endMoves;

                ++stage;
                [[fallthrough]];

        case REFUTATION:
            if (select<Next>([&]() { return *cur && !pos.capture(*cur) && pos.pseudo_legal(*cur); }))
                return *(cur - 1);
                ++stage;
                [[fallthrough]];

        case QUIET_INIT:
            if (!skipQuiets)
            {
                cur = endBadCaptures;
                endMoves = generate<QUIETS>(pos, cur);

                score<QUIETS, SearchMate>();
                partial_insertion_sort(cur, endMoves, -3000 * depth);
            }

            ++stage;
            [[fallthrough]];

        case QUIET:
            if (!skipQuiets && select<Next>([&]() {
                return *cur != refutations[0] && *cur != refutations[1] && *cur != refutations[2];
                }))
                return *(cur - 1);

                // Prepare the pointers to loop over the bad captures
                cur = moves;
                endMoves = endBadCaptures;

                ++stage;
                [[fallthrough]];

        case BAD_CAPTURE:
            return select<Next>([]() { return true; });

        case EVASION_INIT:
            cur = moves;
            endMoves = generate<EVASIONS>(pos, cur);

            score<EVASIONS, SearchMate>();
            ++stage;
            [[fallthrough]];

        case EVASION:
            return select<Best>([]() { return true; });

        case PROBCUT:
            return select<Next>([&]() { return pos.see_ge<true>(*cur, threshold); });

        case QCAPTURE:
            if (select<Next>([&]() { return depth > DEPTH_QS_RECAPTURES || cur->to_sq() == recaptureSquare; }))
                return *(cur - 1);

            // If we did not find any move and we do not try checks, we have finished
            if (depth != DEPTH_QS_CHECKS)
                return Move::none();

            ++stage;
            [[fallthrough]];

        case QCHECK_INIT:
            cur = moves;
            endMoves = generate<QUIET_CHECKS>(pos, cur);

            ++stage;
            [[fallthrough]];

        case QCHECK:
            return select<Next>([]() { return true; });
        }

        assert(false);
        return Move::none(); // Silence warning
    }

    template Move MovePicker::next_move<false>(bool skipQuiets);
    template Move MovePicker::next_move<true>(bool skipQuiets);

} // namespace Stockfish
