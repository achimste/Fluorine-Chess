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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>   // For std::memset
#if _DEBUG
#include <fstream>
#endif
#include <iostream>
#include <sstream>

#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

#include "san.h"

namespace Stockfish {

    namespace Search {

        extern LimitsType Limits;
    }

    namespace Tablebases {

        extern int Cardinality;
        extern bool RootInTB;
        extern bool UseRule50;
        extern Depth ProbeDepth;
    }

    namespace TB = Tablebases;

    using std::string;
    using Classic::Eval::evaluate;

    namespace Search::Classic {

        // Futility margin
        template <bool SearchMate>
        Value futility_margin(Depth d, bool improving) {
            return (SearchMate ? 140 : 165) * (d - improving);
        }

        // Reductions lookup table, initialized at startup
        int Reductions[MAX_MOVES]; // [depth or moveNumber]

        template <bool SearchMate>
        Depth reduction(bool i, Depth d, int mn, int delta, int rootDelta) {
            int r = Reductions[d] * Reductions[mn];
            if constexpr (!SearchMate)
                return (r + 1642 - delta * 1024 / rootDelta) / 1024 + (!i && r > 916);
            else
                return (r + 1372 - delta * 1073 / rootDelta) / 1024 + (!i && r > 936);
        }

        constexpr int futility_move_count(bool improving, Depth depth) {
            return improving ? (3 + depth * depth) : (3 + depth * depth) / 2;
        }

        // History and stats update bonus, based on depth
        template <bool SearchMate>
        int stat_bonus(Depth d) {
            if constexpr (!SearchMate)
                return std::min((12 * d + 282) * d - 349, 1594);
            else
                return std::min(336 * d - 547, 1561);
        }

        // Add a small random component to draw evaluations to avoid 3-fold blindness
        Value value_draw(const Thread* thisThread) {
            return VALUE_DRAW - 1 + Value(thisThread->nodes & 0x2);
        }

        template <NodeType nodeType, bool SearchMate>
        Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth = 0);

        Value value_to_tt(Value v, int ply);
        template <bool SearchMate> Value value_from_tt(Value v, int ply, int r50c);
        void update_pv(Move* pv, Move move, const Move* childPv);
        void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
        void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus);
        template <bool SearchMate> void update_all_stats(const Position& pos, Stack* ss, Move bestMove,
            Value bestValue, Value beta, Square prevSq,
            Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth);

#if _DEBUG
        // std::ofstream log("1.txt");
        // static std::atomic<int>counter = 0;
#endif

        template <NodeType nodeType, bool SearchMate>
        Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

#if _DEBUG
            auto debug = [&]()
                {
                    for (int i = 0; i < pos.this_thread()->rootDepth; ++i) log << " ";
                    log
                        << " " << ++counter
                        << " " << depth
                        << " " << alpha
                        << " " << beta
                        << " " << std::hex << pos.key() << std::dec
                        << std::endl;
                };

            // debug();
#endif

            constexpr bool PvNode = nodeType != NonPV;
            constexpr bool rootNode = nodeType == Root;

            if constexpr (!SearchMate)
            {
                const Depth maxNextDepth = rootNode ? depth : depth + 1;

                // Check if we have an upcoming move which draws by repetition, or
                // if the opponent had an alternative move earlier to this position.
                if (!rootNode && pos.rule50_count() >= 3 && alpha < VALUE_DRAW && pos.has_game_cycle(ss->ply))
                {
                    alpha = value_draw(pos.this_thread());
                    if (alpha >= beta)
                        return alpha;
                }

                // Dive into quiescence search when the depth reaches zero
                if (depth <= 0)
                    return qsearch<PvNode ? PV : NonPV, SearchMate>(pos, ss, alpha, beta);

                assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
                assert(PvNode || (alpha == beta - 1));
                assert(0 < depth && depth < MAX_PLY);
                assert(!(PvNode && cutNode));

                Move pv[MAX_PLY + 1], capturesSearched[32], quietsSearched[64];
                StateInfo st;
                TTEntry* tte;
                Key posKey;
                Move ttMove, move, excludedMove, bestMove;
                Depth extension, newDepth;
                Value bestValue, value, ttValue, eval, maxValue, probCutBeta;
                bool givesCheck, improving, priorCapture, singularQuietLMR;
                bool capture, moveCountPruning, ttCapture;
                Piece movedPiece;
                int moveCount, captureCount, quietCount, improvement, complexity;

                // Step 1. Initialize node
                Thread* thisThread = pos.this_thread();
                ss->inCheck = pos.checkers();
                priorCapture = pos.captured_piece();
                Color us = pos.side_to_move();
                moveCount = captureCount = quietCount = ss->moveCount = 0;
                bestValue = -VALUE_INFINITE;
                maxValue = VALUE_INFINITE;

                // Check for the available remaining time
                if (thisThread == Threads.main())
                    static_cast<MainThread*>(thisThread)->check_time();

                // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
                if (PvNode && thisThread->selDepth < ss->ply + 1)
                    thisThread->selDepth = ss->ply + 1;

                if (!rootNode)
                {
                    // Step 2. Check for aborted search and immediate draw
                    if (Threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
                        return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate<SearchMate>(pos) : value_draw(pos.this_thread());

                    // Step 3. Mate distance pruning. Even if we mate at the next move our score
                    // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
                    // a shorter mate was found upward in the tree then there is no need to search
                    // because we will never beat the current alpha. Same logic but with reversed
                    // signs applies also in the opposite condition of being mated instead of giving
                    // mate. In this case return a fail-high score.
                    alpha = std::max(mated_in(ss->ply), alpha);
                    beta = std::min(mate_in(ss->ply + 1), beta);
                    if (alpha >= beta)
                        return alpha;
                }
                else
                    thisThread->rootDelta = beta - alpha;

                assert(0 <= ss->ply && ss->ply < MAX_PLY);

                (ss + 1)->ttPv = false;
                (ss + 1)->excludedMove = bestMove = Move::none();
                (ss + 2)->killers[0] = (ss + 2)->killers[1] = Move::none();
                (ss + 2)->cutoffCnt = 0;
                ss->doubleExtensions = (ss - 1)->doubleExtensions;
                Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;

                // Initialize statScore to zero for the grandchildren of the current position.
                // So statScore is shared between all grandchildren and only the first grandchild
                // starts with statScore = 0. Later grandchildren start with the last calculated
                // statScore of the previous grandchild. This influences the reduction rules in
                // LMR which are based on the statScore of parent position.
                if (!rootNode)
                    (ss + 2)->statScore = 0;

                // Step 4. Transposition table lookup. We don't want the score of a partial
                // search to overwrite a previous full search TT value, so we use a different
                // position key in case of an excluded move.
                excludedMove = ss->excludedMove;
                posKey = !excludedMove ? pos.key() : pos.key() ^ make_key(excludedMove.raw());
                tte = TT.probe(posKey, ss->ttHit);
                ttValue = ss->ttHit ? value_from_tt<SearchMate>(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
                ttMove = rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0] : ss->ttHit ? tte->move() : Move::none();
                ttCapture = ttMove && pos.capture(ttMove);
                if (!excludedMove)
                    ss->ttPv = PvNode || (ss->ttHit && tte->is_pv());

                // At non-PV nodes we check for an early TT cutoff
                if (!PvNode
                    && ss->ttHit
                    && tte->depth() > depth - (tte->bound() == BOUND_EXACT)
                    && ttValue != VALUE_NONE // Possible in case of TT access race
                    && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
                {
                    // If ttMove is quiet, update move sorting heuristics on TT hit (~1 Elo)
                    if (ttMove)
                    {
                        if (ttValue >= beta)
                        {
                            // Bonus for a quiet ttMove that fails high (~3 Elo)
                            if (!ttCapture)
                                update_quiet_stats(pos, ss, ttMove, stat_bonus<SearchMate>(depth));

                            // Extra penalty for early quiet moves of the previous ply (~0 Elo)
                            if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !priorCapture)
                                update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                    -stat_bonus<SearchMate>(depth + 1));
                        }
                        // Penalty for a quiet ttMove that fails low (~1 Elo)
                        else if (!ttCapture)
                        {
                            int penalty = -stat_bonus<SearchMate>(depth);
                            thisThread->mainHistory[us][ttMove.from_to()] << penalty;
                            update_continuation_histories(ss, pos.moved_piece(ttMove), ttMove.to_sq(), penalty);
                        }
                    }

                    // Partial workaround for the graph history interaction problem
                    // For high rule50 counts don't produce transposition table cutoffs.
                    if (pos.rule50_count() < 90)
                        return ttValue;
                }

                // Step 5. Tablebases probe
                if (!rootNode && TB::Cardinality)
                {
                    int piecesCount = pos.count<ALL_PIECES>();

                    if (piecesCount <= TB::Cardinality
                        && (piecesCount < TB::Cardinality || depth >= TB::ProbeDepth)
                        && pos.rule50_count() == 0
                        && !pos.can_castle(ANY_CASTLING))
                    {
                        TB::ProbeState err;
                        TB::WDLScore wdl = Tablebases::probe_wdl(pos, &err);

                        // Force check of time on the next occasion
                        if (thisThread == Threads.main())
                            static_cast<MainThread*>(thisThread)->callsCnt = 0;

                        if (err != TB::ProbeState::FAIL)
                        {
                            thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);

                            int drawScore = TB::UseRule50 ? 1 : 0;

                            // use the range VALUE_MATE_IN_MAX_PLY to VALUE_TB_WIN_IN_MAX_PLY to score
                            value = wdl < -drawScore ? VALUE_MATED_IN_MAX_PLY + ss->ply + 1
                                : wdl >  drawScore ? VALUE_MATE_IN_MAX_PLY - ss->ply - 1
                                : VALUE_DRAW + 2 * wdl * drawScore;

                            Bound b = wdl < -drawScore ? BOUND_UPPER
                                : wdl >  drawScore ? BOUND_LOWER : BOUND_EXACT;

                            if (b == BOUND_EXACT
                                || (b == BOUND_LOWER ? value >= beta : value <= alpha))
                            {
                                tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, b,
                                    std::min(MAX_PLY - 1, depth + 6), Move::none(), VALUE_NONE);

                                return value;
                            }

                            if constexpr (PvNode)
                            {
                                if (b == BOUND_LOWER)
                                    bestValue = value, alpha = std::max(alpha, bestValue);
                                else
                                    maxValue = value;
                            }
                        }
                    }
                }

                CapturePieceToHistory& captureHistory = thisThread->captureHistory;

                // Step 6. Static evaluation of the position
                if (ss->inCheck)
                {
                    // Skip early pruning when in check
                    ss->staticEval = eval = VALUE_NONE;
                    improving = false;
                    improvement = 0;
                    complexity = 0;
                    goto moves_loop;
                }
                else if (ss->ttHit)
                {
                    // Never assume anything about values stored in TT
                    ss->staticEval = eval = tte->eval();
                    if (eval == VALUE_NONE)
                        ss->staticEval = eval = evaluate<SearchMate>(pos, &complexity);
                    else // Fall back to classical complexity for TT hits
                        complexity = std::abs(ss->staticEval - pos.psq_eg_stm());

                    // ttValue can be used as a better position evaluation (~4 Elo)
                    if (ttValue != VALUE_NONE && (tte->bound() & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
                        eval = ttValue;
                }
                else
                {
                    ss->staticEval = eval = evaluate<SearchMate>(pos, &complexity);

                    // Save static evaluation into transposition table
                    if (!excludedMove)
                        tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, Move::none(), eval);
                }

                thisThread->complexityAverage.update(complexity);

                // Use static evaluation difference to improve quiet move ordering (~3 Elo)
                if ((ss - 1)->currentMove.is_ok() && !(ss - 1)->inCheck && !priorCapture)
                {
                    int bonus = std::clamp(-19 * int((ss - 1)->staticEval + ss->staticEval), -1914, 1914);
                    thisThread->mainHistory[~us][(ss - 1)->currentMove.from_to()] << bonus;
                }

                // Set up the improvement variable, which is the difference between the current
                // static evaluation and the previous static evaluation at our turn (if we were
                // in check at our previous move we look at the move prior to it). The improvement
                // margin and the improving flag are used in various pruning heuristics.
                improvement = (ss - 2)->staticEval != VALUE_NONE ? ss->staticEval - (ss - 2)->staticEval
                            : (ss - 4)->staticEval != VALUE_NONE ? ss->staticEval - (ss - 4)->staticEval
                            : 168;
                improving = improvement > 0;

                // Step 7. Razoring.
                // If eval is really low check with qsearch if it can exceed alpha, if it can't,
                // return a fail low.
                if (eval < alpha - 369 - 254 * depth * depth)
                {
                    value = qsearch<NonPV, SearchMate>(pos, ss, alpha - 1, alpha);
                    if (value < alpha)
                        return value;
                }

                // Step 8. Futility pruning: child node (~25 Elo).
                // The depth condition is important for mate finding.
                if (!ss->ttPv
                    && depth < 8
                    && eval - futility_margin<SearchMate>(depth, improving) - (ss - 1)->statScore / 303 >= beta
                    && eval >= beta
                    && eval < 28031) // larger than VALUE_KNOWN_WIN, but smaller than TB wins
                    return eval;

                // Step 9. Null move search with verification search (~22 Elo)
                if (!PvNode
                    && (ss - 1)->currentMove != Move::null()
                    && (ss - 1)->statScore < 17139
                    && eval >= beta
                    && eval >= ss->staticEval
                    && ss->staticEval >= beta - 20 * depth - improvement / 13 + 233 + complexity / 25
                    && !excludedMove
                    && pos.non_pawn_material(us)
                    && (ss->ply >= thisThread->nmpMinPly || us != thisThread->nmpColor))
                {
                    assert(eval - beta >= 0);

                    // Null move dynamic reduction based on depth, eval and complexity of position
                    Depth R = std::min(int(eval - beta) / 168, 7) + depth / 3 + 4 - (complexity > 861);

                    ss->currentMove = Move::null();
                    ss->continuationHistory = &thisThread->continuationHistory[0][0][NO_PIECE][0];

                    pos.do_null_move(st);

                    Value nullValue = -search<NonPV, SearchMate>(pos, ss + 1, -beta, -beta + 1, depth - R, !cutNode);

                    pos.undo_null_move();

                    if (nullValue >= beta)
                    {
                        // Do not return unproven mate or TB scores
                        if (nullValue >= VALUE_TB_WIN_IN_MAX_PLY)
                            nullValue = beta;

                        if (thisThread->nmpMinPly || (std::abs(beta) < VALUE_KNOWN_WIN && depth < 14))
                            return nullValue;

                        assert(!thisThread->nmpMinPly); // Recursive verification is not allowed

                        // Do verification search at high depths, with null move pruning disabled
                        // for us, until ply exceeds nmpMinPly.
                        thisThread->nmpMinPly = ss->ply + 3 * (depth - R) / 4;
                        thisThread->nmpColor = us;

                        Value v = search<NonPV, SearchMate>(pos, ss, beta - 1, beta, depth - R, false);

                        thisThread->nmpMinPly = 0;

                        if (v >= beta)
                            return nullValue;
                    }
                }

                probCutBeta = beta + 191 - 54 * improving;

                // Step 10. ProbCut (~4 Elo)
                // If we have a good enough capture and a reduced search returns a value
                // much above beta, we can (almost) safely prune the previous move.
                if (!PvNode
                    && depth > 4
                    && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
                    // if value from transposition table is lower than probCutBeta, don't attempt probCut
                    // there and in further interactions with transposition table cutoff depth is set to depth - 3
                    // because probCut search has depth set to depth - 4 but we also do a move before it
                    // so effective depth is equal to depth - 3
                    && !(ss->ttHit
                        && tte->depth() >= depth - 3
                        && ttValue != VALUE_NONE
                        && ttValue < probCutBeta))
                {
                    assert(probCutBeta < VALUE_INFINITE);

                    MovePicker mp(pos, ttMove, probCutBeta - ss->staticEval, &captureHistory);

                    while ((move = mp.next_move<SearchMate>()))
                        if (move != excludedMove && pos.legal(move))
                        {
                            assert(pos.capture(move) || move.promotion_type() == QUEEN);

                            ss->currentMove = move;
                            ss->continuationHistory = &thisThread->
                                continuationHistory[ss->inCheck][true][pos.moved_piece(move)][move.to_sq()];

                            pos.do_move(move, st);

                            // Perform a preliminary qsearch to verify that the move holds
                            value = -qsearch<NonPV, SearchMate>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);

                            // If the qsearch held, perform the regular search
                            if (value >= probCutBeta)
                                value = -search<NonPV, SearchMate>(pos, ss + 1, -probCutBeta, -probCutBeta + 1, depth - 4, !cutNode);

                            pos.undo_move(move);

                            if (value >= probCutBeta)
                            {
                                // Save ProbCut data into transposition table
                                tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER, depth - 3, move, ss->staticEval);
                                return value;
                            }
                        }
                }

                // Step 11. If the position is not in TT, decrease depth by 3.
                // Use qsearch if depth is equal or below zero (~4 Elo)
                if (PvNode && !ttMove)
                    depth -= 3;

                if (depth <= 0)
                    return qsearch<PV, SearchMate>(pos, ss, alpha, beta);

                if (cutNode && depth >= 9 && !ttMove)
                    depth -= 2;

            moves_loop: // When in check, search starts here

                // Step 12. A small Probcut idea, when we are in check (~0 Elo)
                probCutBeta = beta + 417;
                if (!PvNode
                    && ss->inCheck
                    && depth >= 2
                    && ttCapture
                    && (tte->bound() & BOUND_LOWER)
                    && tte->depth() >= depth - 3
                    && ttValue >= probCutBeta
                    && std::abs(ttValue) <= VALUE_KNOWN_WIN
                    && std::abs(beta) <= VALUE_KNOWN_WIN)
                    return probCutBeta;

                const PieceToHistory* contHist[] = { (ss - 1)->continuationHistory,
                                                     (ss - 2)->continuationHistory,
                                                     (ss - 3)->continuationHistory,
                                                     (ss - 4)->continuationHistory,
                                                     nullptr,
                                                     (ss - 6)->continuationHistory };

                Move countermove = prevSq != SQ_NONE ? thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] : Move::none();

                MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory, &captureHistory, contHist, countermove, ss->killers);

                value = bestValue;
                moveCountPruning = singularQuietLMR = false;

                // Indicate PvNodes that will probably fail low if the node was searched
                // at a depth equal or greater than the current depth, and the result of this search was a fail low.
                bool likelyFailLow = PvNode && ttMove && (tte->bound() & BOUND_UPPER) && tte->depth() >= depth;

                // Step 13. Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs.
                while ((move = mp.next_move<SearchMate>(moveCountPruning)))
                {
                    assert(move.is_ok());

                    if (move == excludedMove)
                        continue;

                    // At root obey the "searchmoves" option and skip moves not listed in Root
                    // Move List. As a consequence any illegal move is also skipped. In MultiPV
                    // mode we also skip PV moves which have been already searched and those
                    // of lower "TB rank" if we are in a TB root position.
                    if (rootNode
                        && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                            thisThread->rootMoves.begin() + thisThread->pvLast, move))
                        continue;

                    // Check for legality
                    if (!rootNode && !pos.legal(move))
                        continue;

                    ss->moveCount = ++moveCount;

                    if (rootNode && bUCI && thisThread == Threads.main() && Time.elapsed() > 3000)
                        sync_cout << "info depth " << depth
                        << " currmove " << UCI::move(move, pos.is_chess960())
                        << " currmovenumber " << moveCount + thisThread->pvIdx << sync_endl;

                    if (PvNode)
                        (ss + 1)->pv = nullptr;

                    extension = 0;
                    capture = pos.capture(move);
                    movedPiece = pos.moved_piece(move);
                    givesCheck = pos.gives_check(move);

                    // Calculate new depth for this move
                    newDepth = depth - 1;

                    int delta = beta - alpha;

                    // Step 14. Pruning at shallow depth (~98 Elo). Depth conditions are important for mate finding.
                    if (!rootNode && pos.non_pawn_material(us) && bestValue > VALUE_TB_LOSS_IN_MAX_PLY)
                    {
                        // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold (~7 Elo)
                        moveCountPruning = moveCount >= futility_move_count(improving, depth);

                        // Reduced depth of the next LMR search
                        int lmrDepth = std::max(newDepth - reduction<SearchMate>(improving, depth, moveCount, delta, thisThread->rootDelta), 0);

                        if (capture || givesCheck)
                        {
                            // Futility pruning for captures (~0 Elo)
                            if (!PvNode
                                && !givesCheck
                                && lmrDepth < 7
                                && !ss->inCheck
                                && ss->staticEval + 180 + 201 * lmrDepth + PieceValueME[EG][pos.piece_on(move.to_sq())]
                                + captureHistory[movedPiece][move.to_sq()][type_of(pos.piece_on(move.to_sq()))] / 6 < alpha)
                                continue;

                            // SEE based pruning (~9 Elo)
                            if (!pos.see_ge<true>(move, -222 * depth))
                                continue;
                        }
                        else
                        {
                            int history = (*contHist[0])[movedPiece][move.to_sq()]
                                + (*contHist[1])[movedPiece][move.to_sq()]
                                + (*contHist[3])[movedPiece][move.to_sq()];

                            // Continuation history based pruning (~2 Elo)
                            if (lmrDepth < 5 && history < -3875 * (depth - 1))
                                continue;

                            history += 2 * thisThread->mainHistory[us][move.from_to()];

                            // Futility pruning: parent node (~9 Elo)
                            if (!ss->inCheck
                                && lmrDepth < 13
                                && ss->staticEval + 106 + 145 * lmrDepth + history / 52 <= alpha)
                                continue;

                            // Prune moves with negative SEE (~3 Elo)
                            if (!pos.see_ge<true>(move, (-24 * lmrDepth - 15) * lmrDepth))
                                continue;
                        }
                    }

                    // Step 15. Extensions (~66 Elo)
                    // We take care to not overdo to avoid search getting stuck.
                    if (ss->ply < thisThread->rootDepth * 2)
                    {
                        // Singular extension search (~58 Elo). If all moves but one fail low on a
                        // search of (alpha-s, beta-s), and just one fails high on (alpha, beta),
                        // then that move is singular and should be extended. To verify this we do
                        // a reduced search on all the other moves but the ttMove and if the
                        // result is lower than ttValue minus a margin, then we will extend the ttMove.
                        if (!rootNode
                            && depth >= 4 - (thisThread->previousDepth > 24) + 2 * (PvNode && tte->is_pv())
                            && move == ttMove
                            && !excludedMove // Avoid recursive singular search
                            && std::abs(ttValue) < VALUE_KNOWN_WIN
                            && (tte->bound() & BOUND_LOWER)
                            && tte->depth() >= depth - 3)
                        {
                            Value singularBeta = ttValue - (3 + (ss->ttPv && !PvNode)) * depth;
                            Depth singularDepth = (depth - 1) / 2;

                            ss->excludedMove = move;
                            value = search<NonPV, SearchMate>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
                            ss->excludedMove = Move::none();

                            if (value < singularBeta)
                            {
                                extension = 1;
                                singularQuietLMR = !ttCapture;

                                // Avoid search explosion by limiting the number of double extensions
                                if (!PvNode && value < singularBeta - 25 && ss->doubleExtensions <= 9)
                                    extension = 2;
                            }

                            // Multi-cut pruning
                            // Our ttMove is assumed to fail high, and now we failed high also on a reduced
                            // search without the ttMove. So we assume this expected Cut-node is not singular,
                            // that multiple moves fail high, and we can prune the whole subtree by returning
                            // a soft bound.
                            else if (singularBeta >= beta)
                                return singularBeta;

                            // If the eval of ttMove is greater than beta, we reduce it (negative extension)
                            else if (ttValue >= beta)
                                extension = -2;

                            // If the eval of ttMove is less than alpha and value, we reduce it (negative extension)
                            else if (ttValue <= alpha && ttValue <= value)
                                extension = -1;
                        }

                        // Check extensions (~1 Elo)
                        else if (givesCheck
                            && depth > 9
                            && std::abs(ss->staticEval) > 82)
                            extension = 1;

                        // Quiet ttMove extensions (~0 Elo)
                        else if (PvNode
                            && move == ttMove
                            && move == ss->killers[0]
                            && (*contHist[0])[movedPiece][move.to_sq()] >= 5177)
                            extension = 1;
                    }

                    // Add extension to new depth
                    newDepth += extension;
                    ss->doubleExtensions = (ss - 1)->doubleExtensions + (extension == 2);

                    // Speculative prefetch as early as possible
                    prefetch(TT.first_entry(pos.key_after(move)));

                    // Update the current move (this must be done after singular extension search)
                    ss->currentMove = move;
                    ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck][capture][movedPiece][move.to_sq()];

                    // Step 16. Make the move
                    pos.do_move(move, st, givesCheck);

                    // Step 17. Late moves reduction / extension (LMR, ~98 Elo)
                    // We use various heuristics for the sons of a node after the first son has
                    // been searched. In general we would like to reduce them, but there are many
                    // cases where we extend a son if it has good chances to be "interesting".
                    if (depth >= 2
                        && moveCount > 1 + (PvNode && ss->ply <= 1)
                        && (!ss->ttPv
                            || !capture
                            || (cutNode && (ss - 1)->moveCount > 1)))
                    {
                        Depth r = reduction<SearchMate>(improving, depth, moveCount, delta, thisThread->rootDelta);

                        // Decrease reduction if position is or has been on the PV
                        // and node is not likely to fail low. (~3 Elo)
                        if (ss->ttPv && !likelyFailLow)
                            r -= 2;

                        // Decrease reduction if opponent's move count is high (~1 Elo)
                        if ((ss - 1)->moveCount > 7)
                            r--;

                        // Increase reduction for cut nodes (~3 Elo)
                        if (cutNode)
                            r += 2;

                        // Increase reduction if ttMove is a capture (~3 Elo)
                        if (ttCapture)
                            r++;

                        // Decrease reduction for PvNodes based on depth
                        if constexpr (PvNode)
                            r -= 1 + 11 / (3 + depth);

                        // Decrease reduction if ttMove has been singularly extended (~1 Elo)
                        if (singularQuietLMR)
                            r--;

                        // Decrease reduction if we move a threatened piece (~1 Elo)
                        if (depth > 9 && (mp.threatenedPieces & move.from_sq()))
                            r--;

                        // Increase reduction if next ply has a lot of fail high
                        if ((ss + 1)->cutoffCnt > 3)
                            r++;

                        ss->statScore = 2 * thisThread->mainHistory[us][move.from_to()]
                            + (*contHist[0])[movedPiece][move.to_sq()]
                            + (*contHist[1])[movedPiece][move.to_sq()]
                            + (*contHist[3])[movedPiece][move.to_sq()]
                            - 4433;

                        // Decrease/increase reduction for moves with a good/bad history (~30 Elo)
                        r -= ss->statScore / (13628 + 4000 * (depth > 7 && depth < 19));

                        // In general we want to cap the LMR depth search at newDepth, but when
                        // reduction is negative, we allow this move a limited search extension
                        // beyond the first move depth. This may lead to hidden double extensions.
                        Depth d = std::max(1, std::min(newDepth - r, newDepth + 1));

                        value = -search<NonPV, SearchMate>(pos, ss + 1, -(alpha + 1), -alpha, d, true);

                        // Do full depth search when reduced LMR search fails high
                        if (value > alpha && d < newDepth)
                        {
                            // Adjust full depth search based on LMR results - if result
                            // was good enough search deeper, if it was bad enough search shallower
                            const bool doDeeperSearch = value > (alpha + 64 + 11 * (newDepth - d));
                            const bool doShallowerSearch = value < bestValue + newDepth;

                            newDepth += doDeeperSearch - doShallowerSearch;

                            if (newDepth > d)
                                value = -search<NonPV, SearchMate>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

                            int bonus = value > alpha ? stat_bonus<SearchMate>(newDepth) : -stat_bonus<SearchMate>(newDepth);

                            if (capture)
                                bonus /= 6;

                            update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
                        }
                    }

                    // Step 18. Full depth search when LMR is skipped
                    else if (!PvNode || moveCount > 1)
                    {
                        value = -search<NonPV, SearchMate>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);
                    }

                    // For PV nodes only, do a full PV search on the first move or after a fail
                    // high (in the latter case search only if value < beta), otherwise let the
                    // parent node fail low with value <= alpha and try another move.
                    if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
                    {
                        (ss + 1)->pv = pv;
                        (ss + 1)->pv[0] = Move::none();

                        value = -search<PV, SearchMate>(pos, ss + 1, -beta, -alpha, std::min(maxNextDepth, newDepth), false);
                    }

                    // Step 19. Undo move
                    pos.undo_move(move);

                    assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

                    // Step 20. Check for a new best move
                    // Finished searching the move. If a stop occurred, the return value of
                    // the search cannot be trusted, and we return immediately without
                    // updating best move, PV and TT.
                    if (Threads.stop.load(std::memory_order_relaxed))
                        return VALUE_ZERO;

                    if (rootNode)
                    {
                        RootMove& rm = *std::find(thisThread->rootMoves.begin(), thisThread->rootMoves.end(), move);

                        rm.averageScore = rm.averageScore != -VALUE_INFINITE ? (2 * value + rm.averageScore) / 3 : value;

                        // PV move or new best move?
                        if (moveCount == 1 || value > alpha)
                        {
                            rm.score = rm.uciScore = value;
                            rm.selDepth = thisThread->selDepth;
                            rm.scoreLowerbound = rm.scoreUpperbound = false;

                            if (value >= beta)
                            {
                                rm.scoreLowerbound = true;
                                rm.uciScore = beta;
                            }
                            else if (value <= alpha)
                            {
                                rm.scoreUpperbound = true;
                                rm.uciScore = alpha;
                            }

                            rm.pv.resize(1);

                            assert((ss + 1)->pv);

                            for (Move* m = (ss + 1)->pv; *m; ++m)
                                rm.pv.push_back(*m);

                            // We record how often the best move has been changed in each iteration.
                            // This information is used for time management. In MultiPV mode,
                            // we must take care to only do this for the first PV line.
                            if (moveCount > 1 && !thisThread->pvIdx)
                                ++thisThread->bestMoveChanges;
                        }
                        else
                            // All other moves but the PV are set to the lowest value: this
                            // is not a problem when sorting because the sort is stable and the
                            // move position in the list is preserved - just the PV is pushed up.
                            rm.score = -VALUE_INFINITE;
                    }

                    if (value > bestValue)
                    {
                        bestValue = value;

                        if (value > alpha)
                        {
                            bestMove = move;

                            if constexpr (PvNode && !rootNode) // Update pv even in fail-high case
                                update_pv(ss->pv, move, (ss + 1)->pv);

                            if (PvNode && value < beta) // Update alpha! Always alpha < beta
                            {
                                alpha = value;

                                // Reduce other moves if we have found at least one score improvement
                                if (depth > 1
                                    && depth < 6
                                    && beta  <  VALUE_KNOWN_WIN
                                    && alpha > -VALUE_KNOWN_WIN)
                                    depth -= 1;

                                assert(depth > 0);
                            }
                            else
                            {
                                ss->cutoffCnt++;
                                assert(value >= beta); // Fail high
                                break;
                            }
                        }
                    }


                    // If the move is worse than some previously searched move, remember it to update its stats later
                    if (move != bestMove)
                    {
                        if (capture && captureCount < 32)
                            capturesSearched[captureCount++] = move;

                        else if (!capture && quietCount < 64)
                            quietsSearched[quietCount++] = move;
                    }
                }

                // Step 21. Check for mate and stalemate
                // All legal moves have been searched and if there are no legal moves, it
                // must be a mate or a stalemate. If we are in a singular extension search then
                // return a fail low score.

                assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

                if (!moveCount)
                    bestValue = excludedMove ? alpha : ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;

                // If there is a move which produces search value greater than alpha we update stats of searched moves
                else if (bestMove)
                    update_all_stats<SearchMate>(pos, ss, bestMove, bestValue, beta, prevSq,
                        quietsSearched, quietCount, capturesSearched, captureCount, depth);

                // Bonus for prior countermove that caused the fail low
                else if ((depth >= 5 || PvNode) && !priorCapture && prevSq != SQ_NONE)
                {
                    //Assign extra bonus if current node is PvNode or cutNode or fail low was really bad
                    bool extraBonus = PvNode || cutNode || bestValue < alpha - 62 * depth;

                    update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, stat_bonus<SearchMate>(depth) * (1 + extraBonus));
                }

                if (PvNode)
                    bestValue = std::min(bestValue, maxValue);

                // If no good move is found and the previous position was ttPv, then the previous
                // opponent move is probably good and the new position is added to the search tree.
                if (bestValue <= alpha)
                    ss->ttPv = ss->ttPv || ((ss - 1)->ttPv && depth > 3);

                // Write gathered information in transposition table
                if (!excludedMove && !(rootNode && thisThread->pvIdx))
                    tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                        bestValue >= beta ? BOUND_LOWER :
                        PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
                        depth, bestMove, ss->staticEval);

                assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

                return bestValue;
            }
            else
            {
                // Dive into quiescence search when the depth reaches zero
                if (depth <= 0)
                    return qsearch<PvNode ? PV : NonPV, SearchMate>(pos, ss, alpha, beta);

                assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
                assert(PvNode || (alpha == beta - 1));
                assert(0 < depth && depth < MAX_PLY);
                assert(!(PvNode && cutNode));

                Move pv[MAX_PLY + 1], capturesSearched[32], quietsSearched[64];
                StateInfo st;

                TTEntry* tte;
                Key posKey;
                Move ttMove, move, excludedMove, bestMove;
                Depth extension, newDepth;
                Value bestValue, value, ttValue, eval, maxValue, probCutBeta;
                bool givesCheck, improving, priorCapture, singularQuietLMR;
                bool capture, moveCountPruning, ttCapture;
                Piece movedPiece;
                int moveCount, captureCount, quietCount, improvement;

                // Step 1. Initialize node
                Thread* thisThread = pos.this_thread();
                ss->inCheck = pos.checkers();
                priorCapture = pos.captured_piece();
                Color us = pos.side_to_move();
                moveCount = captureCount = quietCount = ss->moveCount = 0;
                bestValue = -VALUE_INFINITE;
                maxValue = VALUE_INFINITE;

                // Check for the available remaining time
                if (thisThread == Threads.main())
                    static_cast<MainThread*>(thisThread)->check_time();

                // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
                if (PvNode && thisThread->selDepth < ss->ply + 1)
                    thisThread->selDepth = ss->ply + 1;

                if (!rootNode)
                {
                    // Step 2. Check for aborted search and immediate draw
                    if (TB::UseRule50
                        && pos.rule50_count() > 99 && (!ss->inCheck || MoveList<LEGAL>(pos).size()))
                        return value_draw(thisThread);

                    if (pos.is_draw(ss->ply))
                        return value_draw(thisThread);

                    if (Threads.stop.load(std::memory_order_relaxed) || ss->ply >= MAX_PLY)
                        return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate<SearchMate>(pos) : VALUE_ZERO;

                    // Step 3. Mate distance pruning. Even if we mate at the next move our score
                    // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
                    // a shorter mate was found upward in the tree then there is no need to search
                    // because we will never beat the current alpha.
                    if (alpha >= mate_in(ss->ply + 1))
                        return alpha;
                }
                else
                    thisThread->rootDelta = beta - alpha;

                assert(0 <= ss->ply && ss->ply < MAX_PLY);

                (ss + 1)->excludedMove = bestMove = Move::none();
                (ss + 2)->killers[0] = (ss + 2)->killers[1] = Move::none();
                (ss + 2)->cutoffCnt = 0;
                ss->doubleExtensions = (ss - 1)->doubleExtensions;
                Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;
                ss->statScore = 0;

                // Step 4. Transposition table lookup.
                excludedMove = ss->excludedMove;
                posKey = pos.key();
                tte = TT.probe(posKey, ss->ttHit);
                ttValue = ss->ttHit ? value_from_tt<SearchMate>(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
                ttMove = rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0] : ss->ttHit ? tte->move() : Move::none();
                ttCapture = ttMove && pos.capture_stage(ttMove);

                // At this point, if excluded, skip straight to step 6, static eval. However,
                // to save indentation, we list the condition in all code between here and there.
                if (!excludedMove)
                    ss->ttPv = PvNode || (ss->ttHit && tte->is_pv());

                // At non-PV nodes we check for an early TT cutoff
                if (!PvNode
                    && !excludedMove
                    && ttValue != VALUE_NONE // Possible in case of TT access race or if !ttHit
                    && tte->depth() > depth - (tte->bound() == BOUND_EXACT)
                    && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
                {
                    // If ttMove is quiet, update move sorting heuristics on TT hit (~2 Elo)
                    if (ttMove)
                    {
                        if (ttValue >= beta)
                        {
                            // Bonus for a quiet ttMove that fails high (~2 Elo)
                            if (!ttCapture)
                                update_quiet_stats(pos, ss, ttMove, stat_bonus<SearchMate>(depth));

                            // Extra penalty for early quiet moves of the previous ply (~0 Elo on STC, ~2 Elo on LTC)
                            if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !priorCapture)
                                update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -stat_bonus<SearchMate>(depth + 1));
                        }
                        // Penalty for a quiet ttMove that fails low (~1 Elo)
                        else if (!ttCapture)
                        {
                            int penalty = -stat_bonus<SearchMate>(depth);
                            thisThread->mainHistory[us][ttMove.from_to()] << penalty;
                            update_continuation_histories(ss, pos.moved_piece(ttMove), ttMove.to_sq(), penalty);
                        }
                    }

                    return ttValue;
                }

                // Step 5. Tablebases probe
                if (!rootNode && !excludedMove && TB::Cardinality)
                {
                    int piecesCount = pos.count<ALL_PIECES>();

                    if (piecesCount <= TB::Cardinality
                        && (piecesCount < TB::Cardinality || depth >= TB::ProbeDepth)
                        && pos.rule50_count() == 0
                        && !pos.can_castle(ANY_CASTLING))
                    {
                        TB::ProbeState err;
                        TB::WDLScore wdl = Tablebases::probe_wdl(pos, &err);

                        // Force check of time on the next occasion
                        if (thisThread == Threads.main())
                            static_cast<MainThread*>(thisThread)->callsCnt = 0;

                        if (err != TB::ProbeState::FAIL)
                        {
                            thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);

                            int drawScore = TB::UseRule50 ? 1 : 0;

                            // use the range VALUE_MATE_IN_MAX_PLY to VALUE_TB_WIN_IN_MAX_PLY to score
                            value = wdl < -drawScore ? VALUE_MATED_IN_MAX_PLY + ss->ply + 1
                                : wdl >  drawScore ? VALUE_MATE_IN_MAX_PLY - ss->ply - 1
                                : VALUE_DRAW + 2 * wdl * drawScore;

                            Bound b = wdl < -drawScore ? BOUND_UPPER : wdl >  drawScore ? BOUND_LOWER : BOUND_EXACT;

                            if (b == BOUND_EXACT || (b == BOUND_LOWER ? value >= beta : value <= alpha))
                            {
                                tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, b,
                                    std::min(MAX_PLY - 1, depth + 6), Move::none(), VALUE_NONE);

                                return value;
                            }

                            if constexpr (PvNode)
                            {
                                if (b == BOUND_LOWER)
                                    bestValue = value, alpha = std::max(alpha, bestValue);
                                else
                                    maxValue = value;
                            }
                        }
                    }
                }

                CapturePieceToHistory& captureHistory = thisThread->captureHistory;

                // Step 6. Static evaluation of the position
                if (ss->inCheck)
                {
                    // Skip early pruning when in check
                    ss->staticEval = eval = VALUE_NONE;
                    improving = false;
                    improvement = 0;
                    goto moves_loop;
                }
                else if (excludedMove)
                {
                    // Providing the hint that this node's accumulator will be used often brings significant Elo gain (13 Elo)
                    eval = ss->staticEval;
                }
                else if (ss->ttHit)
                {
                    // Never assume anything about values stored in TT
                    ss->staticEval = eval = tte->eval();
                    if (eval == VALUE_NONE)
                        ss->staticEval = eval = evaluate<SearchMate>(pos);

                    // ttValue can be used as a better position evaluation (~7 Elo)
                    if (ttValue != VALUE_NONE && (tte->bound() & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
                        eval = ttValue;
                }
                else
                {
                    ss->staticEval = eval = evaluate<SearchMate>(pos);
                    // Save static evaluation into transposition table
                    tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, Move::none(), eval);
                }

                // Use static evaluation difference to improve quiet move ordering (~4 Elo)
                if ((ss - 1)->currentMove.is_ok() && !(ss - 1)->inCheck && !priorCapture)
                {
                    int bonus = std::clamp(-18 * int((ss - 1)->staticEval + ss->staticEval), -1817, 1817);
                    thisThread->mainHistory[~us][(ss - 1)->currentMove.from_to()] << bonus;
                }

                // Set up the improvement variable, which is the difference between the current
                // static evaluation and the previous static evaluation at our turn (if we were
                // in check at our previous move we look at the move prior to it). The improvement
                // margin and the improving flag are used in various pruning heuristics.
                improvement = (ss - 2)->staticEval != VALUE_NONE ? ss->staticEval - (ss - 2)->staticEval
                    : (ss - 4)->staticEval != VALUE_NONE ? ss->staticEval - (ss - 4)->staticEval
                    : 173;
                improving = improvement > 0;

                // Begin early pruning.
                if (!PvNode
                    && thisThread->rootDepth > 4
                    && thisThread->rootMoves[thisThread->pvIdx].previousScore < VALUE_MATE_IN_MAX_PLY)
                {
                    // Step 7. Razoring (~1 Elo).
                    // If eval is really low check with qsearch if it can exceed alpha, if it can't,
                    // return a fail low.
                    if (eval < alpha - 456 - 252 * depth * depth)
                    {
                        value = qsearch<NonPV, SearchMate>(pos, ss, alpha - 1, alpha);

                        if (value < alpha)
                            return value;
                    }

                    // Step 8. Futility pruning: child node (~40 Elo).
                    // The depth condition is important for mate finding.
                    if (!ss->ttPv
                        && depth < 9
                        && eval < Value(24923) // larger than VALUE_KNOWN_WIN, but smaller than TB wins
                        && eval >= beta + futility_margin<SearchMate>(depth, improving) + (ss - 1)->statScore / 306)
                        return eval;

                    // Step 9. Null move search with verification search (~35 Elo)
                    if (!excludedMove
                        && (ss - 1)->currentMove != Move::null()
                        && (ss - 1)->statScore < 17329
                        && beta > -VALUE_KNOWN_WIN
                        && eval >= beta
                        && eval >= ss->staticEval
                        && ss->staticEval >= beta - 21 * depth - improvement / 13 + 258
                        && pos.non_pawn_material(us)
                        && (ss->ply >= thisThread->nmpMinPly))
                    {
                        assert(eval - beta >= 0);

                        // Null move dynamic reduction based on depth and eval
                        Depth R = std::min(int(eval - beta) / 173, 6) + depth / 3 + 4;

                        ss->currentMove = Move::null();
                        ss->continuationHistory = &thisThread->continuationHistory[0][0][NO_PIECE][0];

                        pos.do_null_move(st);

                        Value nullValue = -search<NonPV, SearchMate>(pos, ss + 1, -beta, -beta + 1, depth - R, !cutNode);

                        pos.undo_null_move();

                        if (nullValue >= beta)
                        {
                            // Do not return unproven mate or TB scores
                            nullValue = std::min(nullValue, VALUE_TB_WIN_IN_MAX_PLY - 1);

                            if (thisThread->nmpMinPly || depth < 14)
                                return nullValue;

                            assert(!thisThread->nmpMinPly); // Recursive verification is not allowed

                            // Do verification search at high depths, with null move pruning disabled
                            // until ply exceeds nmpMinPly.
                            thisThread->nmpMinPly = ss->ply + 3 * (depth - R) / 4;

                            Value v = search<NonPV, SearchMate>(pos, ss, beta - 1, beta, depth - R, false);

                            thisThread->nmpMinPly = 0;

                            if (v >= beta)
                                return nullValue;
                        }
                    }

                    probCutBeta = beta + 168 - 61 * improving;

                    // Step 10. ProbCut (~10 Elo)
                    // If we have a good enough capture (or queen promotion) and a reduced search returns a value
                    // much above beta, we can (almost) safely prune the previous move.
                    if (depth > 3 && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
                        // if value from transposition table is lower than probCutBeta, don't attempt probCut
                        // there and in further interactions with transposition table cutoff depth is set to depth - 3
                        // because probCut search has depth set to depth - 4 but we also do a move before it
                        // so effective depth is equal to depth - 3
                        && !(tte->depth() >= depth - 3 && ttValue != VALUE_NONE && ttValue < probCutBeta))
                    {
                        assert(probCutBeta < VALUE_INFINITE);

                        MovePicker mp(pos, ttMove, probCutBeta - ss->staticEval, &captureHistory);

                        while ((move = mp.next_move<SearchMate>()))
                        {
                            if (move != excludedMove && pos.legal(move))
                            {
                                assert(pos.capture_stage(move));

                                ss->currentMove = move;
                                ss->continuationHistory = &thisThread->
                                    continuationHistory[ss->inCheck][true][pos.moved_piece(move)][move.to_sq()];

                                pos.do_move(move, st);

                                // Perform a preliminary qsearch to verify that the move holds
                                value = -qsearch<NonPV, SearchMate>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);

                                // If the qsearch held, perform the regular search
                                if (value >= probCutBeta)
                                    value = -search<NonPV, SearchMate>(pos, ss + 1, -probCutBeta, -probCutBeta + 1, depth - 4, !cutNode);

                                pos.undo_move(move);

                                if (value >= probCutBeta)
                                {
                                    // Save ProbCut data into transposition table
                                    tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv,
                                        BOUND_LOWER, depth - 3, move, ss->staticEval);

                                    return value;
                                }
                            }
                        }
                    }
                }

            moves_loop: // When in check, search starts here

                // Step 11. If the position doesn't have a ttMove, decrease depth by 2
                // (or by 4 if the TT entry for the current position was hit and the stored depth is greater than or equal to the current depth).
                // Use qsearch if depth is equal or below zero (~9 Elo)
                if (!ttMove)
                {
                    if (PvNode)
                        depth -= 2 + 2 * (ss->ttHit && tte->depth() >= depth);

                    if (depth <= 0)
                        return qsearch<PV, SearchMate>(pos, ss, alpha, beta);

                    if (cutNode
                        && depth >= 8)
                        depth -= 2;
                }

                // Step 12. A small Probcut idea, when we are in check (~4 Elo)
                if (!PvNode
                    && ss->inCheck
                    && ttCapture
                    && tte->depth() >= depth - 4
                    && tte->bound() & BOUND_LOWER
                    && ttValue <= 2 * VALUE_KNOWN_WIN
                    && alpha > -VALUE_KNOWN_WIN
                    && beta < VALUE_KNOWN_WIN)
                {
                    probCutBeta = beta + 413;

                    if (ttValue >= probCutBeta)
                        return probCutBeta;
                }

                const PieceToHistory* contHist[] = { (ss - 1)->continuationHistory,
                                                     (ss - 2)->continuationHistory,
                                                     (ss - 3)->continuationHistory,
                                                     (ss - 4)->continuationHistory,
                                                     nullptr,
                                                     (ss - 6)->continuationHistory };

                Move countermove = prevSq != SQ_NONE ? thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] : Move::none();

                MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory, &captureHistory, contHist, countermove, ss->killers);

                value = bestValue;
                moveCountPruning = singularQuietLMR = false;

                // Indicate PvNodes that will probably fail low if the node was searched
                // at a depth equal or greater than the current depth, and the result of this search was a fail low.
                bool likelyFailLow = PvNode && ttMove && (tte->bound() & BOUND_UPPER) && tte->depth() >= depth;

                // Step 13. Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs.
                while ((move = mp.next_move<SearchMate>(moveCountPruning)))
                {
                    assert(move.is_ok());

                    if (move == excludedMove)
                        continue;

                    // Check for legality
                    if (!pos.legal(move))
                        continue;

                    // At root obey the "searchmoves" option and skip moves not listed in Root
                    // Move List. As a consequence any illegal move is also skipped. In MultiPV
                    // mode we also skip PV moves which have been already searched and those
                    // of lower "TB rank" if we are in a TB root position.
                    if (rootNode && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                        thisThread->rootMoves.begin() + thisThread->pvLast, move))
                        continue;

                    ss->moveCount = ++moveCount;

                    if (rootNode && bUCI && thisThread == Threads.main() && Time.elapsed() > 3000)
                        sync_cout << "info depth " << depth
                        << " currmove " << UCI::move(move, pos.is_chess960())
                        << " currmovenumber " << moveCount + thisThread->pvIdx << sync_endl;

                    if (PvNode)
                        (ss + 1)->pv = nullptr;

                    extension = 0;
                    capture = pos.capture_stage(move);
                    movedPiece = pos.moved_piece(move);
                    givesCheck = pos.gives_check(move);

                    // Calculate new depth for this move
                    newDepth = depth - 1;

                    int delta = beta - alpha;

                    Depth r = reduction<SearchMate>(improving, depth, moveCount, delta, thisThread->rootDelta);

                    // Step 14. Pruning at shallow depth (~120 Elo). Depth conditions are important for mate finding.
                    if (!rootNode
                        && !ss->inCheck
                        && !givesCheck
                        && thisThread->rootDepth > 6
                        && pos.non_pawn_material(us)
                        && bestValue > VALUE_TB_LOSS_IN_MAX_PLY)
                    {
                        // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold (~8 Elo)
                        moveCountPruning = moveCount >= futility_move_count(improving, depth)
                            && thisThread->rootMoves[thisThread->pvIdx].previousScore < VALUE_MATE_IN_MAX_PLY;

                        // Reduced depth of the next LMR search
                        int lmrDepth = newDepth - r;

                        if (capture)
                        {
                            // Futility pruning for captures (~2 Elo)
                            if (move.type_of() == NORMAL
                                && lmrDepth < 7
                                && bestValue < VALUE_MATE_IN_MAX_PLY
                                && ss->staticEval + 197 + 248 * lmrDepth + PieceValueME[EG][pos.piece_on(move.to_sq())] +
                                captureHistory[movedPiece][move.to_sq()][type_of(pos.piece_on(move.to_sq()))] / 7 < alpha)
                                continue;

                            // SEE based pruning (~11 Elo)
                            if (!pos.see_ge<true>(move, -205 * depth))
                                continue;
                        }
                        else
                        {
                            int history = (*contHist[0])[movedPiece][move.to_sq()]
                                + (*contHist[1])[movedPiece][move.to_sq()]
                                + (*contHist[3])[movedPiece][move.to_sq()];

                            // Continuation history based pruning (~2 Elo)
                            if (lmrDepth < 6 && history < -3832 * depth)
                                continue;

                            history += 2 * thisThread->mainHistory[us][move.from_to()];

                            lmrDepth += history / 7011;
                            lmrDepth = std::max(lmrDepth, -2);

                            // Futility pruning: parent node (~13 Elo)
                            if (lmrDepth < 12 && ss->staticEval + 112 + 138 * lmrDepth <= alpha)
                                continue;

                            lmrDepth = std::max(lmrDepth, 0);

                            // Prune moves with negative SEE (~4 Elo)
                            if (!pos.see_ge<true>(move, (-27 * lmrDepth - 16) * lmrDepth))
                                continue;
                        }
                    }

                    // Step 15. Extensions (~100 Elo)
                    // We take care to not overdo to avoid search getting stuck.
                    if (ss->ply < thisThread->rootDepth * 2)
                    {
                        // Singular extension search (~94 Elo). If all moves but one fail low on a
                        // search of (alpha-s, beta-s), and just one fails high on (alpha, beta),
                        // then that move is singular and should be extended. To verify this we do
                        // a reduced search on all the other moves but the ttMove and if the
                        // result is lower than ttValue minus a margin, then we will extend the ttMove.
                        // Depth margin and singularBeta margin are known for having non-linear scaling.
                        // Their values are optimized to time controls of 180+1.8 and longer
                        // so changing them requires tests at this type of time controls.
                        if (!rootNode
                            && depth >= 4 - (thisThread->completedDepth > 22) + 2 * (PvNode && tte->is_pv())
                            && move == ttMove
                            && !excludedMove
                            && std::abs(ttValue) < VALUE_KNOWN_WIN
                            && (tte->bound() & BOUND_LOWER)
                            && tte->depth() >= depth - 3)
                        {
                            Value singularBeta = ttValue - (82 + 65 * (ss->ttPv && !PvNode)) * depth / 64;
                            Depth singularDepth = (depth - 1) / 2;

                            ss->excludedMove = move;
                            value = search<NonPV, SearchMate>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
                            ss->excludedMove = Move::none();

                            if (value < singularBeta)
                            {
                                extension = 1;
                                singularQuietLMR = !ttCapture;

                                // Avoid search explosion by limiting the number of double extensions
                                if (!PvNode && value < singularBeta - 21 && ss->doubleExtensions <= 11)
                                {
                                    extension = 2;
                                    depth += depth < 13;
                                }
                            }

                            // Multi-cut pruning
                            // Our ttMove is assumed to fail high, and now we failed high also on a reduced
                            // search without the ttMove. So we assume this expected Cut-node is not singular,
                            // that multiple moves fail high, and we can prune the whole subtree by returning
                            // a soft bound.
                            else if (singularBeta >= beta)
                                return singularBeta;

                            // If the eval of ttMove is greater than beta, we reduce it (negative extension) (~7 Elo)
                            else if (ttValue >= beta)
                                extension = -2 - !PvNode;

                            // If the eval of ttMove is less than value, we reduce it (negative extension) (~1 Elo)
                            else if (ttValue <= value)
                                extension = -1;

                            // If the eval of ttMove is less than alpha, we reduce it (negative extension) (~1 Elo)
                            else if (ttValue <= alpha)
                                extension = -1;
                        }

                        // Check extensions (~1 Elo)
                        else if (givesCheck && depth > 9)
                            extension = 1;

                        // Quiet ttMove extensions (~1 Elo)
                        else if (PvNode
                            && move == ttMove
                            && move == ss->killers[0]
                            && (*contHist[0])[movedPiece][move.to_sq()] >= 5168)
                            extension = 1;
                    }

                    // Add extension to new depth
                    newDepth += extension;
                    ss->doubleExtensions = (ss - 1)->doubleExtensions + (extension == 2);

                    // Speculative prefetch as early as possible
                    prefetch(TT.first_entry(pos.key_after(move)));

                    // Update the current move (this must be done after singular extension search)
                    ss->currentMove = move;
                    ss->continuationHistory = &thisThread->
                        continuationHistory[ss->inCheck][capture][movedPiece][move.to_sq()];

                    // Step 16. Make the move
                    pos.do_move(move, st, givesCheck);

                    // Decrease reduction if position is or has been on the PV
                    // and node is not likely to fail low. (~3 Elo)
                    // Decrease further on cutNodes. (~1 Elo)
                    if (ss->ttPv && !likelyFailLow)
                        r -= cutNode && tte->depth() >= depth + 3 ? 3 : 2;

                    // Decrease reduction if opponent's move count is high (~1 Elo)
                    if ((ss - 1)->moveCount > 8)
                        r--;

                    // Increase reduction for cut nodes (~3 Elo)
                    if (cutNode)
                        r += 2;

                    // Increase reduction if ttMove is a capture (~3 Elo)
                    if (ttCapture)
                        r++;

                    // Decrease reduction for PvNodes based on depth (~2 Elo)
                    if constexpr (PvNode)
                        r -= 1 + 12 / (3 + depth);

                    // Decrease reduction if ttMove has been singularly extended (~1 Elo)
                    if (singularQuietLMR)
                        r--;

                    // Increase reduction if next ply has a lot of fail high (~5 Elo)
                    if ((ss + 1)->cutoffCnt > 3)
                        r++;

                    // Decrease reduction for checking moves
                    if (givesCheck && r > 2)
                        r--;

                    else if (move == ttMove)
                        r--;

                    ss->statScore = 2 * thisThread->mainHistory[us][move.from_to()]
                        + (*contHist[0])[movedPiece][move.to_sq()]
                        + (*contHist[1])[movedPiece][move.to_sq()]
                        + (*contHist[3])[movedPiece][move.to_sq()]
                        - 4006;

                    // Decrease/increase reduction for moves with a good/bad history (~25 Elo)
                    r -= ss->statScore / (11124 + 4740 * (depth > 5 && depth < 22));

                    // Step 17. Late moves reduction / extension (LMR, ~117 Elo)
                    // We use various heuristics for the sons of a node after the first son has
                    // been searched. In general we would like to reduce them, but there are many
                    // cases where we extend a son if it has good chances to be "interesting".
                    if (depth >= 2
                        && moveCount > 1 + (PvNode && ss->ply <= 1)
                        && (!ss->ttPv
                            || !capture
                            || (cutNode && (ss - 1)->moveCount > 1)))
                    {
                        // In general we want to cap the LMR depth search at newDepth, but when
                        // reduction is negative, we allow this move a limited search extension
                        // beyond the first move depth. This may lead to hidden double extensions.
                        Depth d = std::clamp(newDepth - r, 1, newDepth + 1);

                        value = -search<NonPV, SearchMate>(pos, ss + 1, -(alpha + 1), -alpha, d, true);

                        // Do full depth search when reduced LMR search fails high
                        if (value > alpha && d < newDepth)
                        {
                            // Adjust full depth search based on LMR results - if result
                            // was good enough search deeper, if it was bad enough search shallower
                            const bool doDeeperSearch = value > (bestValue + 64 + 11 * (newDepth - d));
                            const bool doEvenDeeperSearch = value > alpha + 711 && ss->doubleExtensions <= 6;
                            const bool doShallowerSearch = value < bestValue + newDepth;

                            ss->doubleExtensions = ss->doubleExtensions + doEvenDeeperSearch;

                            newDepth += doDeeperSearch - doShallowerSearch + doEvenDeeperSearch;

                            if (newDepth > d)
                                value = -search<NonPV, SearchMate>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

                            int bonus = value <= alpha ? -stat_bonus<SearchMate>(newDepth)
                                : value >= beta ? stat_bonus<SearchMate>(newDepth)
                                : 0;

                            update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
                        }
                    }

                    // Step 18. Full depth search when LMR is skipped. If expected reduction is high, reduce its depth by 1.
                    else if (!PvNode || moveCount > 1)
                    {
                        // Increase reduction for cut nodes and not ttMove (~1 Elo)
                        if (!ttMove && cutNode)
                            r += 2;

                        value = -search<NonPV, SearchMate>(pos, ss + 1, -(alpha + 1), -alpha, newDepth - (r > 3), !cutNode);
                    }

                    // For PV nodes only, do a full PV search on the first move or after a fail
                    // high (in the latter case search only if value < beta), otherwise let the
                    // parent node fail low with value <= alpha and try another move.
                    if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
                    {
                        (ss + 1)->pv = pv;
                        (ss + 1)->pv[0] = Move::none();

                        value = -search<PV, SearchMate>(pos, ss + 1, -beta, -alpha, newDepth, false);
                    }

                    // Step 19. Undo move
                    pos.undo_move(move);

                    assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

                    // Step 20. Check for a new best move
                    // Finished searching the move. If a stop occurred, the return value of
                    // the search cannot be trusted, and we return immediately without
                    // updating best move, PV and TT.
                    if (Threads.stop.load(std::memory_order_relaxed))
                        return VALUE_ZERO;

                    if constexpr (rootNode)
                    {
                        RootMove& rm = *std::find(thisThread->rootMoves.begin(), thisThread->rootMoves.end(), move);

                        rm.averageScore = rm.averageScore != -VALUE_INFINITE ? (2 * value + rm.averageScore) / 3 : value;

                        // PV move or new best move?
                        if (moveCount == 1 || value > alpha)
                        {
                            rm.score = rm.uciScore = value;
                            rm.selDepth = thisThread->selDepth;
                            rm.scoreLowerbound = rm.scoreUpperbound = false;

                            if (value >= beta)
                            {
                                rm.scoreLowerbound = true;
                                rm.uciScore = beta;
                            }
                            else if (value <= alpha)
                            {
                                rm.scoreUpperbound = true;
                                rm.uciScore = alpha;
                            }

                            rm.pv.resize(1);

                            assert((ss + 1)->pv);

                            for (Move* m = (ss + 1)->pv; *m; ++m)
                                rm.pv.push_back(*m);

                            // We record how often the best move has been changed in each iteration.
                            // This information is used for time management. In MultiPV mode,
                            // we must take care to only do this for the first PV line.
                            if (moveCount > 1 && !thisThread->pvIdx)
                                ++thisThread->bestMoveChanges;
                        }
                        else
                            // All other moves but the PV are set to the lowest value: this
                            // is not a problem when sorting because the sort is stable and the
                            // move position in the list is preserved - just the PV is pushed up.
                            rm.score = -VALUE_INFINITE;
                    }

                    if (value > bestValue)
                    {
                        bestValue = value;

                        if (value > alpha)
                        {
                            bestMove = move;

                            if constexpr (PvNode && !rootNode) // Update pv even in fail-high case
                                update_pv(ss->pv, move, (ss + 1)->pv);

                            if (value >= beta)
                            {
                                ss->cutoffCnt += 1 + !ttMove;
                                assert(value >= beta); // Fail high
                                break;
                            }
                            else
                            {
                                // Reduce other moves if we have found at least one score improvement (~1 Elo)
                                // Reduce more for depth > 3 and depth < 12 (~1 Elo)
                                if (depth > 1
                                    && beta  <  14362
                                    && value > -12393)
                                    depth -= depth > 3 && depth < 12 ? 2 : 1;

                                assert(depth > 0);
                                alpha = value; // Update alpha! Always alpha < beta
                            }
                        }
                    }

                    // If we have found a mate within the specified limit, we can immediately break from the moves loop.
                    if ((Limits.mate > 0 && bestValue > VALUE_MATE - 2 * Limits.mate)
                        || (Limits.mate < 0 && bestValue < -VALUE_MATE - 2 * Limits.mate))
                        break;

                    // If the move is worse than some previously searched move, remember it to update its stats later
                    if (move != bestMove)
                    {
                        if (capture && captureCount < 32)
                            capturesSearched[captureCount++] = move;

                        else if (!capture && quietCount < 64)
                            quietsSearched[quietCount++] = move;
                    }
                }

                // Step 21. Check for mate and stalemate
                // All legal moves have been searched and if there are no legal moves, it
                // must be a mate or a stalemate. If we are in a singular extension search then
                // return a fail low score.

                assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

                if (!moveCount)
                    bestValue = excludedMove ? alpha : ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;

                // If there is a move which produces search value greater than alpha we update stats of searched moves
                else if (bestMove)
                    update_all_stats<SearchMate>(pos, ss, bestMove, bestValue, beta, prevSq,
                        quietsSearched, quietCount, capturesSearched, captureCount, depth);

                // Bonus for prior countermove that caused the fail low
                else if (!priorCapture && prevSq != SQ_NONE)
                {
                    int bonus = (depth > 5) + (PvNode || cutNode) + (bestValue < alpha - 113 * depth) + ((ss - 1)->moveCount > 12);
                    update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, stat_bonus<SearchMate>(depth) * bonus);
                }

                if constexpr (PvNode)
                    bestValue = std::min(bestValue, maxValue);

                // If no good move is found and the previous position was ttPv, then the previous
                // opponent move is probably good and the new position is added to the search tree. (~7 Elo)
                if (bestValue <= alpha)
                    ss->ttPv = ss->ttPv || ((ss - 1)->ttPv && depth > 3);

                // Write gathered information in transposition table
                if (!excludedMove && !(rootNode && thisThread->pvIdx))
                    tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                        bestValue >= beta ? BOUND_LOWER :
                        PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
                        depth, bestMove, ss->staticEval);

                assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

                return bestValue;
            }
        }

        template Value search<Root, false>(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode);
        template Value search<Root, true>(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode);


        // qsearch() is the quiescence search function, which is called by the main search
        // function with zero depth, or recursively with further decreasing depth per call.
        // (~155 elo)
        template <NodeType nodeType, bool SearchMate>
        Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

            static_assert(nodeType != Root);
            constexpr bool PvNode = nodeType != NonPV;

            assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
            assert(PvNode || (alpha == beta - 1));
            assert(depth <= 0);

            if constexpr (!SearchMate)
            {
                Move pv[MAX_PLY + 1];
                StateInfo st;
                TTEntry* tte;
                Key posKey;
                Move ttMove, move, bestMove;
                Depth ttDepth;
                Value bestValue, value, ttValue, futilityValue, futilityBase;
                bool pvHit, givesCheck, capture;
                int moveCount;

                // Step 1. Initialize node
                if constexpr (PvNode)
                {
                    (ss + 1)->pv = pv;
                    ss->pv[0] = Move::none();
                }

                Thread* thisThread = pos.this_thread();
                bestMove = Move::none();
                ss->inCheck = pos.checkers();
                moveCount = 0;

                // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
                if (PvNode && thisThread->selDepth < ss->ply + 1)
                    thisThread->selDepth = ss->ply + 1;

                // Step 2. Check for an immediate draw or maximum ply reached
                if (pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
                    return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate<SearchMate>(pos) : VALUE_DRAW;

                assert(0 <= ss->ply && ss->ply < MAX_PLY);

                // Decide whether or not to include checks: this fixes also the type of
                // TT entry depth that we are going to use. Note that in qsearch we use
                // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
                ttDepth = ss->inCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS : DEPTH_QS_NO_CHECKS;

                // Step 3. Transposition table lookup
                posKey = pos.key();
                tte = TT.probe(posKey, ss->ttHit);
                ttValue = ss->ttHit ? value_from_tt<SearchMate>(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
                ttMove = ss->ttHit ? tte->move() : Move::none();
                pvHit = ss->ttHit && tte->is_pv();

                // At non-PV nodes we check for an early TT cutoff
                if (!PvNode
                    && ss->ttHit
                    && tte->depth() >= ttDepth
                    && ttValue != VALUE_NONE // Only in case of TT access race
                    && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
                    return ttValue;

                // Step 4. Static evaluation of the position
                if (ss->inCheck)
                {
                    ss->staticEval = VALUE_NONE;
                    bestValue = futilityBase = -VALUE_INFINITE;
                }
                else
                {
                    if (ss->ttHit)
                    {
                        // Never assume anything about values stored in TT
                        if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE)
                            ss->staticEval = bestValue = evaluate<SearchMate>(pos);

                        // ttValue can be used as a better position evaluation (~7 Elo)
                        if (ttValue != VALUE_NONE
                            && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                            bestValue = ttValue;
                    }
                    else
                        // In case of null move search use previous static eval with a different sign
                        ss->staticEval = bestValue =
                        (ss - 1)->currentMove != Move::null() ? evaluate<SearchMate>(pos) : -(ss - 1)->staticEval;

                    // Stand pat. Return immediately if static value is at least beta
                    if (bestValue >= beta)
                    {
                        // Save gathered info in transposition table
                        if (!ss->ttHit)
                            tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER,
                                DEPTH_NONE, Move::none(), ss->staticEval);

                        return bestValue;
                    }

                    if (PvNode && bestValue > alpha)
                        alpha = bestValue;

                    futilityBase = bestValue + 153;
                }

                const PieceToHistory* contHist[] = { (ss - 1)->continuationHistory, (ss - 2)->continuationHistory };

                // Initialize a MovePicker object for the current position, and prepare
                // to search the moves. Because the depth is <= 0 here, only captures,
                // queen promotions, and other checks (only if depth >= DEPTH_QS_CHECKS)
                // will be generated.
                Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;
                MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory, &thisThread->captureHistory, contHist, prevSq);

                int quietCheckEvasions = 0;

                // Step 5. Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs.
                while ((move = mp.next_move<SearchMate>()))
                {
                    assert(move.is_ok());

                    // Check for legality
                    if (!pos.legal(move))
                        continue;

                    givesCheck = pos.gives_check(move);
                    capture = pos.capture(move);

                    moveCount++;

                    // Step 6. Pruning.
                    // Futility pruning and moveCount pruning (~5 Elo)
                    if (bestValue > VALUE_TB_LOSS_IN_MAX_PLY
                        && !givesCheck
                        && move.to_sq() != prevSq
                        && futilityBase > -VALUE_KNOWN_WIN
                        && move.type_of() != PROMOTION)
                    {

                        if (moveCount > 2)
                            continue;

                        futilityValue = futilityBase + PieceValueME[EG][pos.piece_on(move.to_sq())];

                        if (futilityValue <= alpha)
                        {
                            bestValue = std::max(bestValue, futilityValue);
                            continue;
                        }

                        if (futilityBase <= alpha && !pos.see_ge<true>(move, 1))
                        {
                            bestValue = std::max(bestValue, futilityBase);
                            continue;
                        }
                    }

                    // Do not search moves with negative SEE values (~5 Elo)
                    if (bestValue > VALUE_TB_LOSS_IN_MAX_PLY && !pos.see_ge<true>(move))
                        continue;

                    // Speculative prefetch as early as possible
                    prefetch(TT.first_entry(pos.key_after(move)));

                    ss->currentMove = move;
                    ss->continuationHistory = &thisThread->
                        continuationHistory[ss->inCheck][capture][pos.moved_piece(move)][move.to_sq()];

                    // Continuation history based pruning (~2 Elo)
                    if (!capture
                        && bestValue > VALUE_TB_LOSS_IN_MAX_PLY
                        && (*contHist[0])[pos.moved_piece(move)][move.to_sq()] < 0
                        && (*contHist[1])[pos.moved_piece(move)][move.to_sq()] < 0)
                        continue;

                    // We prune after 2nd quiet check evasion where being 'in check' is implicitly checked through the counter
                    // and being a 'quiet' apart from being a tt move is assumed after an increment because captures are pushed ahead.
                    if (bestValue > VALUE_TB_LOSS_IN_MAX_PLY && quietCheckEvasions > 1)
                        break;

                    quietCheckEvasions += !capture && ss->inCheck;

                    // Step 7. Make and search the move
                    pos.do_move(move, st, givesCheck);
                    value = -qsearch<nodeType, SearchMate>(pos, ss + 1, -beta, -alpha, depth - 1);
                    pos.undo_move(move);

                    assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

                    // Step 8. Check for a new best move
                    if (value > bestValue)
                    {
                        bestValue = value;

                        if (value > alpha)
                        {
                            bestMove = move;

                            if constexpr (PvNode) // Update pv even in fail-high case
                                update_pv(ss->pv, move, (ss + 1)->pv);

                            if (PvNode && value < beta) // Update alpha here!
                                alpha = value;
                            else
                                break; // Fail high
                        }
                    }
                }

                // Step 9. Check for mate
                // All legal moves have been searched. A special case: if we're in check
                // and no legal moves were found, it is checkmate.
                if (ss->inCheck && bestValue == -VALUE_INFINITE)
                {
                    assert(!MoveList<LEGAL>(pos).size());

                    return mated_in(ss->ply); // Plies to mate from the root
                }

                // Save gathered info in transposition table
                tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
                    bestValue >= beta ? BOUND_LOWER : BOUND_UPPER,
                    ttDepth, bestMove, ss->staticEval);

                assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

                return bestValue;
            }
            else
            {
                Move pv[MAX_PLY + 1];
                StateInfo st;
                TTEntry* tte;
                Key posKey;
                Move ttMove, move, bestMove;
                Depth ttDepth;
                Value bestValue, value, ttValue, futilityValue, futilityBase;
                bool pvHit, givesCheck, capture;
                int moveCount;

                // Step 1. Initialize node
                if constexpr (PvNode)
                {
                    (ss + 1)->pv = pv;
                    ss->pv[0] = Move::none();
                }

                Thread* thisThread = pos.this_thread();
                bestMove = Move::none();
                ss->inCheck = pos.checkers();
                moveCount = 0;

                // Step 2. Check for an immediate draw or maximum ply reached
                if (TB::UseRule50 && pos.rule50_count() > 99 && (!ss->inCheck || MoveList<LEGAL>(pos).size()))
                    return value_draw(thisThread);

                if (pos.is_draw(ss->ply))
                    return value_draw(thisThread);

                if (ss->ply >= MAX_PLY)
                    return !ss->inCheck ? evaluate<SearchMate>(pos) : VALUE_ZERO;

                assert(0 <= ss->ply && ss->ply < MAX_PLY);

                if (alpha >= mate_in(ss->ply + 1))
                    return alpha;

                // Decide whether or not to include checks: this fixes also the type of
                // TT entry depth that we are going to use. Note that in qsearch we use
                // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
                ttDepth = ss->inCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS : DEPTH_QS_NO_CHECKS;

                // Step 3. Transposition table lookup
                posKey = pos.key();
                tte = TT.probe(posKey, ss->ttHit);
                ttValue = ss->ttHit ? value_from_tt<SearchMate>(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
                ttMove = ss->ttHit ? tte->move() : Move::none();
                pvHit = ss->ttHit && tte->is_pv();

                // At non-PV nodes we check for an early TT cutoff
                if (!PvNode
                    && ttValue != VALUE_NONE // Only in case of TT access race or if !ttHit
                    && tte->depth() >= ttDepth
                    && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
                    return ttValue;

                // Step 4. Static evaluation of the position
                if (ss->inCheck)
                    bestValue = futilityBase = -VALUE_INFINITE;
                else
                {
                    if (ss->ttHit)
                    {
                        // Never assume anything about values stored in TT
                        if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE)
                            ss->staticEval = bestValue = evaluate<SearchMate>(pos);

                        // ttValue can be used as a better position evaluation (~13 Elo)
                        if (!PvNode
                            && ttValue != VALUE_NONE
                            && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                            bestValue = ttValue;
                    }
                    else
                        // In case of null move search use previous static eval with a different sign
                        ss->staticEval = bestValue =
                        (ss - 1)->currentMove != Move::null() ? evaluate<SearchMate>(pos) : -(ss - 1)->staticEval;

                    // Stand pat. Return immediately if static value is at least beta
                    if (bestValue >= beta)
                    {
                        // Save gathered info in transposition table
                        if (!ss->ttHit)
                            tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER,
                                DEPTH_NONE, Move::none(), ss->staticEval);

                        return bestValue;
                    }

                    if (PvNode && bestValue > alpha)
                        alpha = bestValue;

                    futilityBase = bestValue + 200;
                }

                const PieceToHistory* contHist[] = { (ss - 1)->continuationHistory, (ss - 2)->continuationHistory };

                // Initialize a MovePicker object for the current position, and prepare
                // to search the moves. Because the depth is <= 0 here, only captures,
                // queen promotions, and other checks (only if depth >= DEPTH_QS_CHECKS)
                // will be generated.
                Square prevSq = (ss - 1)->currentMove.is_ok() ? (ss - 1)->currentMove.to_sq() : SQ_NONE;
                MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory, &thisThread->captureHistory, contHist, prevSq);

                // Step 5. Loop through all pseudo-legal moves until no moves remain or a beta cutoff occurs.
                while ((move = mp.next_move<SearchMate>()))
                {
                    assert(move.is_ok());

                    // Check for legality
                    if (!pos.legal(move))
                        continue;

                    givesCheck = pos.gives_check(move);
                    capture = pos.capture_stage(move);

                    moveCount++;

                    // Step 6. Pruning.
                    if (bestValue > VALUE_TB_LOSS_IN_MAX_PLY)
                    {
                        // Futility pruning and moveCount pruning (~10 Elo)
                        if (!givesCheck
                            && move.to_sq() != prevSq
                            && futilityBase > -VALUE_KNOWN_WIN
                            && move.type_of() != PROMOTION)
                        {
                            if (moveCount > 2)
                                continue;

                            futilityValue = futilityBase + PieceValueME[EG][pos.piece_on(move.to_sq())];

                            if (futilityValue <= alpha)
                            {
                                bestValue = std::max(bestValue, futilityValue);
                                continue;
                            }

                            if (futilityBase <= alpha && !pos.see_ge<true>(move, 1))
                            {
                                bestValue = std::max(bestValue, futilityBase);
                                continue;
                            }
                        }

                        // Do not search moves with bad enough SEE values (~5 Elo)
                        if (!ss->inCheck && !pos.see_ge<true>(move, -95))
                            continue;
                    }

                    // Speculative prefetch as early as possible
                    prefetch(TT.first_entry(pos.key_after(move)));

                    // Update the current move
                    ss->currentMove = move;
                    ss->continuationHistory = &thisThread->
                        continuationHistory[ss->inCheck][capture][pos.moved_piece(move)][move.to_sq()];

                    // Step 7. Make and search the move
                    pos.do_move(move, st, givesCheck);
                    value = -qsearch<nodeType, SearchMate>(pos, ss + 1, -beta, -alpha, depth - 1);
                    pos.undo_move(move);

                    assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

                    // Step 8. Check for a new best move
                    if (value > bestValue)
                    {
                        bestValue = value;

                        if (value > alpha)
                        {
                            bestMove = move;

                            if constexpr (PvNode) // Update pv even in fail-high case
                                update_pv(ss->pv, move, (ss + 1)->pv);

                            if (PvNode && value < beta) // Update alpha here!
                                alpha = value;
                            else
                                break; // Fail high
                        }
                    }
                }

                // Step 9. Check for mate
                // All legal moves have been searched. A special case: if we're in check
                // and no legal moves were found, it is checkmate.
                if (ss->inCheck && bestValue == -VALUE_INFINITE)
                {
                    assert(!MoveList<LEGAL>(pos).size());

                    return mated_in(ss->ply); // Plies to mate from the root
                }

                // Save gathered info in transposition table
                tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
                    bestValue >= beta ? BOUND_LOWER : BOUND_UPPER,
                    ttDepth, bestMove, ss->staticEval);

                assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

                return bestValue;
            }
        }


        // value_to_tt() adjusts a mate or TB score from "plies to mate from the root" to
        // "plies to mate from the current position". Standard scores are unchanged.
        // The function is called before storing a value in the transposition table.

        Value value_to_tt(Value v, int ply) {

            assert(v != VALUE_NONE);

            return  v >= VALUE_TB_WIN_IN_MAX_PLY ? v + ply
                : v <= VALUE_TB_LOSS_IN_MAX_PLY ? v - ply : v;
        }


        // value_from_tt() is the inverse of value_to_tt(): it adjusts a mate or TB score
        // from the transposition table (which refers to the plies to mate/be mated from
        // current position) to "plies to mate/be mated (TB win/loss) from the root". However,
        // for mate scores, to avoid potentially false mate scores related to the 50 moves rule
        // and the graph history interaction, we return an optimal TB score instead.

        template <bool SearchMate>
        Value value_from_tt(Value v, int ply, int r50c) {

            if (v == VALUE_NONE)
                return VALUE_NONE;

            if constexpr (!SearchMate)
            {
                if (v >= VALUE_TB_WIN_IN_MAX_PLY) // TB win or better
                {
                    if (v >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - v > 99 - r50c)
                        return VALUE_MATE_IN_MAX_PLY - 1; // do not return a potentially false mate score

                    return v - ply;
                }

                if (v <= VALUE_TB_LOSS_IN_MAX_PLY) // TB loss or worse
                {
                    if (v <= VALUE_MATED_IN_MAX_PLY && VALUE_MATE + v > 99 - r50c)
                        return VALUE_MATED_IN_MAX_PLY + 1; // do not return a potentially false mate score

                    return v + ply;
                }

                return v;
            }
            else
                return  v >= VALUE_TB_WIN_IN_MAX_PLY ? v - ply : v <= VALUE_TB_LOSS_IN_MAX_PLY ? v + ply : v;
        }


        // update_pv() adds current move and appends child pv[]

        void update_pv(Move* pv, Move move, const Move* childPv) {

            for (*pv++ = move; childPv && *childPv; )
                *pv++ = *childPv++;
            *pv = Move::none();
        }


        // update_all_stats() updates stats at the end of search() when a bestMove is found

        template <bool SearchMate>
        void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
            Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth) {

            Color us = pos.side_to_move();
            Thread* thisThread = pos.this_thread();
            CapturePieceToHistory& captureHistory = thisThread->captureHistory;
            Piece moved_piece = pos.moved_piece(bestMove);
            PieceType captured = type_of(pos.piece_on(bestMove.to_sq()));
            int bonus1 = stat_bonus<SearchMate>(depth + 1);

            if (!pos.capture(bestMove))
            {
                int bonus2 = bestValue > beta + 137 ? bonus1 // larger bonus
                    : stat_bonus<SearchMate>(depth); // smaller bonus

                // Increase stats for the best move in case it was a quiet move
                update_quiet_stats(pos, ss, bestMove, bonus2);

                // Decrease stats for all non-best quiet moves
                for (int i = 0; i < quietCount; ++i)
                {
                    thisThread->mainHistory[us][quietsSearched[i].from_to()] << -bonus2;
                    update_continuation_histories(ss, pos.moved_piece(quietsSearched[i]), quietsSearched[i].to_sq(), -bonus2);
                }
            }
            else
                // Increase stats for the best move in case it was a capture move
                captureHistory[moved_piece][bestMove.to_sq()][captured] << bonus1;

            // Extra penalty for a quiet early move that was not a TT move or
            // main killer move in previous ply when it gets refuted.
            if (prevSq != SQ_NONE
                && ((ss - 1)->moveCount == 1 + (ss - 1)->ttHit || ((ss - 1)->currentMove == (ss - 1)->killers[0]))
                && !pos.captured_piece())
                update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -bonus1);

            // Decrease stats for all non-best capture moves
            for (int i = 0; i < captureCount; ++i)
            {
                moved_piece = pos.moved_piece(capturesSearched[i]);
                captured = type_of(pos.piece_on(capturesSearched[i].to_sq()));
                captureHistory[moved_piece][capturesSearched[i].to_sq()][captured] << -bonus1;
            }
        }


        // Updates histories of the move pairs formed by moves at ply -1, -2, -3, -4, and -6 with current move.
        void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {

            for (int i : {1, 2, 3, 4, 6})
            {
                // Only update the first 2 continuation histories if we are in check
                if (ss->inCheck && i > 2)
                    break;
                if ((ss - i)->currentMove.is_ok())
                    (*(ss - i)->continuationHistory)[pc][to] << bonus / (1 + 3 * (i == 3));
            }
        }


        // Updates move sorting heuristics
        void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus) {

            // Update killers
            if (ss->killers[0] != move)
            {
                ss->killers[1] = ss->killers[0];
                ss->killers[0] = move;
            }

            Color us = pos.side_to_move();
            Thread* thisThread = pos.this_thread();
            thisThread->mainHistory[us][move.from_to()] << bonus;
            update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus);

            // Update countermove history
            if ((ss - 1)->currentMove.is_ok())
            {
                Square prevSq = (ss - 1)->currentMove.to_sq();
                thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] = move;
            }
        }

    } // namespace Search::Classic

} // namespace Stockfish
