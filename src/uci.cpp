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

#include "uci.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "benchmark.h"
#include "book.h"
#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "nnue/evaluate_nnue.h"
#include "position.h"
#include "san.h"
#include "search.h"
#include "thread.h"
#include "tt.h"

namespace Stockfish {

    bool bUCI;

    namespace {

        // FEN string for the initial position in standard chess
        const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";


        // Called when the engine receives the "position" UCI command.
        // It sets up the position that is described in the given FEN string ("fen") or
        // the initial position ("startpos") and then makes the moves given in the following
        // move list ("moves").
        void position(Position& pos, std::istringstream& is, StateListPtr& states) {

            Move        m;
            std::string token, fen;

            is >> token;

            if (token == "startpos")
            {
                fen = StartFEN;
                is >> token; // Consume the "moves" token, if any
            }
            else if (token == "fen")
                while (is >> token && token != "moves")
                    fen += token + " ";
            else
                return;

            states = StateListPtr(new std::deque<StateInfo>(1)); // Drop the old state and create a new one
            pos.set(fen, Options["UCI_Chess960"], &states->back(), Threads.main());

            // Parse the move list, if any
            while (is >> token && (m = UCI::to_move(pos, token)))
                pos.do_move(m, states->emplace_back());
        }

        // Prints the evaluation of the current position, consistent with the UCI options set so far.
        void trace_eval(Position& pos) {

            StateListPtr states(new std::deque<StateInfo>(1));
            Position     p;
            p.set(pos.fen(), Options["UCI_Chess960"], &states->back(), Threads.main());

            if (!useClassic)
                Eval::NNUE::verify();

            sync_cout << "\n" << (useClassic ? Classic::Eval::trace(p) : Eval::trace(p)) << sync_endl;
        }


        // Called when the engine receives the "setoption" UCI command.
        // The function updates the UCI option ("name") to the given value ("value").

        void setoption(std::istringstream& is) {

            Threads.main()->wait_for_search_finished();

            std::string token, name, value;

            is >> token; // Consume the "name" token

            // Read the option name (can contain spaces)
            while (is >> token && token != "value")
                name += (name.empty() ? "" : " ") + token;

            // Read the option value (can contain spaces)
            while (is >> token)
                value += (value.empty() ? "" : " ") + token;

            if (Options.count(name))
                Options[name] = value;
            else
                sync_cout << "No such option: " << name << sync_endl;
        }


        // Called when the engine receives the "go" UCI command.
        // The function sets the thinking time and other parameters from the input string,
        // then starts with a search.

        void go(Position& pos, std::istringstream& is, StateListPtr& states) {

            Search::LimitsType limits;
            std::string        token;
            bool               ponderMode = false;

            limits.startTime = now(); // The search starts as early as possible

            while (is >> token)
                if (token == "searchmoves") // Needs to be the last command on the line
                    while (is >> token)
                    {
                        limits.searchmoves.push_back(UCI::to_move(pos, token));
                    }

                else if (token == "wtime")
                    is >> limits.time[WHITE];
                else if (token == "btime")
                    is >> limits.time[BLACK];
                else if (token == "winc")
                    is >> limits.inc[WHITE];
                else if (token == "binc")
                    is >> limits.inc[BLACK];
                else if (token == "movestogo")
                    is >> limits.movestogo;
                else if (token == "depth")
                    is >> limits.depth;
                else if (token == "nodes")
                    is >> limits.nodes;
                else if (token == "movetime")
                    is >> limits.movetime;
                else if (token == "mate")
                    is >> limits.mate;
                else if (token == "perft")
                    is >> limits.perft;
                else if (token == "infinite")
                    limits.infinite = 1;
                else if (token == "ponder")
                    ponderMode = true;

            Threads.start_thinking(pos, states, limits, ponderMode);
        }


        // Called when the engine receives the "bench" command.
        // First, a list of UCI commands is set up according to the bench parameters,
        // then it is run one by one, printing a summary at the end.

        void bench(Position& pos, std::istream& args, StateListPtr& states) {

            std::string token;
            uint64_t    num, nodes = 0, cnt = 1;

            std::vector<std::string> list = setup_bench(pos, args);

            num = count_if(list.begin(), list.end(),
                [](const std::string& s) { return s.find("go ") == 0 || s.find("eval") == 0; });

            TimePoint elapsed = now();

            for (const auto& cmd : list)
            {
                std::istringstream is(cmd);
                is >> std::skipws >> token;

                if (token == "go" || token == "eval")
                {
                    std::cerr << "\nPosition: " << cnt++ << '/' << num << " (" << pos.fen() << ")"
                        << std::endl;
                    if (token == "go")
                    {
                        go(pos, is, states);
                        Threads.main()->wait_for_search_finished();
                        nodes += Threads.nodes_searched();
                    }
                    else
                        trace_eval(pos);
                }
                else if (token == "setoption")
                    setoption(is);
                else if (token == "position")
                    position(pos, is, states);
                else if (token == "ucinewgame")
                {
                    Search::clear();
                    elapsed = now();
                } // Search::clear() may take a while
            }

            elapsed = now() - elapsed + 1; // Ensure positivity to avoid a 'divide by zero'

            dbg_print();

            std::cerr << "\n==========================="
                << "\nTotal time (ms) : " << elapsed << "\nNodes searched  : " << nodes
                << "\nNodes/second    : " << 1000 * nodes / elapsed << std::endl;
        }

        void fen(Position& pos, std::istringstream& is, StateListPtr& states) {

            std::string token, fen;

            is >> token;

            if (token == "startpos")
                fen = StartFEN;
            else {
                fen = token + " ";
                while (is >> token)
                    fen += token + " ";
            }

            states = StateListPtr(new std::deque<StateInfo>(1)); // Drop the old state and create a new one
            pos.set(fen, Options["UCI_Chess960"], &states->back(), Threads.main());
            Search::clear();
            std::cout << pos << std::endl;
        }

        void all_moves(Position& pos, std::istringstream& is) {

            if (!pos.pos_is_ok())
                return;

            StateListPtr states(new std::deque<StateInfo>(1));
            Position p;
            p.set(pos.fen(), Options["UCI_Chess960"], &states->back(), Threads.main());

            std::ostringstream ss;
            ExtMove moveList[MAX_MOVES + 1], * endList;

            std::string token;
            is >> token;
            if (token == "pseudo")
                endList = p.checkers() ? generate<EVASIONS>(p, moveList) : generate<NON_EVASIONS>(p, moveList);
            else
                endList = generate<LEGAL>(p, moveList);

            int i = 0;
            for (const ExtMove* pMove = moveList; pMove != endList; ++pMove)
            {
                Move move = *pMove;
                std::cout << ++i << ": " << pos.moved_piece(move) << " "
                    << UCI::move(move, p.is_chess960()) << " -> " << SAN::to_san(p, move);

                if (p.legal(move))
                {
                    // Find opening
                    p.do_move<true>(move, states->emplace_back());
                    const auto* entry = Book::find_opening(p);
                    if (entry)
                        std::cout << " " << entry->opening;
                    p.undo_move(move);
                    states->pop_back();

                    if (move.type_of() == EN_PASSANT)
                        std::cout << " (en passant)";
                    else if (p.capture(move))
                        std::cout << (p.see_ge(move) ? " (good" : " (bad") << " capture)";
                    if (move.type_of() == CASTLING)
                        std::cout << ((move.from_sq() > move.to_sq()) ? " (long" : " (short") << " castle)";
                    if (move.type_of() == PROMOTION)
                        std::cout << " (" << move.promotion_type() << " promotion)";
                    if (p.gives_check(move))
                        std::cout << " (check)";
                }
                else
                    std::cout << " (illegal)";

                std::cout << '\n';
            }
            std::cout << std::endl;
        }

        void new_game(Position& pos, StateListPtr& states) {

            bUCI = false;
            Options["UCI_Chess960"] = false;
            states = StateListPtr(new std::deque<StateInfo>(1));
            pos.set(StartFEN, false, &states->back(), Threads.main());
            Search::clear();
        }

        void test_perft()
        {
            const char* filename = Options["UCI_Chess960"] ? "fischer.epd" : "standard.epd";
            std::ifstream is(filename);
            if (is)
            {
                StateListPtr sp(new std::deque<StateInfo>(1));
                Position pos;
                std::string line;
                while (!is.eof())
                {
                    std::getline(is, line);
                    std::istringstream iss(line);
                    std::string fen;
                    std::getline(iss, fen, ';');
                    if (fen.empty())
                        break;

                    pos.set(fen, Options["UCI_Chess960"], &sp->back(), nullptr);
                    std::cout << pos << std::endl;
                    while (true)
                    {
                        std::string s;
                        std::getline(iss, s, ';');
                        if (s.empty())
                        {
                            std::cout << std::endl;
                            break;
                        }
                        std::istringstream iss1(s);

                        char c1;
                        int depth;
                        uint64_t n;
                        iss1 >> c1 >> depth >> n;
                        std::cout << "Depth: " << depth << std::endl;

                        TimePoint start_time = now();
                        uint64_t nodes = perft<true, false>(pos, depth);
                        TimePoint elapsed_time = now() - start_time;
                        std::cout << "Nodes searched: " << nodes
                            << "\nTime: " << elapsed_time / 1000.0
                            << " s -> " << float(nodes) / float(elapsed_time) * 1000.0f << " nps\n";

                        if (nodes == n) std::cout << "Passed!\n";
                        else
                        {
                            std::cout << "ERROR: Expected number of moves was " << n << std::endl;
                            return;
                        }
                        std::cout << std::endl;
                    }
                }
            }
        }

        static std::string current_date() {

            time_t now = std::time(0);
            tm tstruct;
            char buf[40];

            localtime_s(&tstruct, &now);
            std::strftime(buf, sizeof(buf), "%F_%H-%M-%S", &tstruct);
            return buf;
        }

        void test_mate(std::istringstream& is) {

            std::ifstream epd("matetrack.epd");
            if (epd)
            {
                StateListPtr sp;
                Position pos;
                std::string line;
                unsigned posCount = 0;
                std::ofstream csv("matelog " + current_date() + ".csv");

                if (!csv)
                    return;

                csv << "Hash " << int(Options["Hash"]) << " MB;Threads " << int(Options["Threads"]) << std::endl;
                csv << "Index;FEN;Mate in;Time [ms];PV" << std::endl;

                Search::LimitsType limits;
                limits.movetime = 0;

                try
                {
                    std::string movetime;
                    is >> movetime;
                    if (movetime == "movetime" && !is.eof())
                    {
                        is >> movetime;
                        std::cout << "ARG " << movetime << std::endl;
                        if (!movetime.empty()) limits.movetime = std::stoi(movetime) * 1000;
                    }
                }
                catch (std::exception& e)
                {
                    std::cout << "ERROR: " << e.what() << std::endl;
                }

                std::cout << "Starting test mate session" << std::endl;
                std::cout << "Number of threads: " << Threads.size() << std::endl;
                std::cout << "Hash size: " << int(Options["Hash"]) << " MB" << std::endl;
                std::cout << "Time limit: " << limits.movetime / 1000 << " seconds" << std::endl;
                std::cout << "Method: " << (useShashin ? "Shashin" : "Normal") << std::endl;
                std::cout << std::endl;

                while (!epd.eof() && posCount < 100)
                {
                    std::getline(epd, line);

                    // Get the FEN.
                    auto npos = line.find("bm");
                    if (npos == std::string::npos) continue;

                    posCount++;
                    std::string fen = line.substr(0, npos - 1);
                    std::cout << "Position #" << posCount << ": " << fen << std::endl;
                    csv << posCount << ";" << fen << ";";

                    // Get the "mate in"
                    int mateDepth = std::strtol(line.c_str() + npos + 4, 0, 10);
                    std::cout << "Search for mate in " << mateDepth << std::endl;
                    csv << mateDepth << ";";
                    limits.mate = mateDepth;
                    limits.startTime = now();

                    // Start the search
                    sp = StateListPtr(new std::deque<StateInfo>(1));
                    pos.set(fen, false, &sp->back(), Threads.main());
                    TT.clear();
                    Threads.clear();
                    TimePoint time = now();
                    Threads.start_thinking(pos, sp, limits);
                    Threads.main()->wait_for_search_finished();

                    TimePoint elapsed_time = now() - time;
                    csv << elapsed_time << ";";

                    Thread* bestThread = Threads.get_best_thread();
                    bool mate_found = false;

                    // Have we found a "mate in x"?
                    if (limits.mate > 0
                        && bestThread->bestValue >= VALUE_MATE_IN_MAX_PLY
                        && VALUE_MATE - bestThread->bestValue <= 2 * limits.mate)
                        mate_found = true;

                    // Have we found a "mated in x"?
                    if (limits.mate < 0
                        && bestThread->bestValue <= VALUE_MATED_IN_MAX_PLY
                        && VALUE_MATE + bestThread->bestValue <= -2 * limits.mate)
                        mate_found = true;

                    if (mate_found)
                        csv << SAN::to_san(pos, bestThread->rootMoves[0]);
                    else
                        csv << "mate not found within time limit.";

                    csv << std::endl;
                    std::cout << std::endl;
                }
            }
        }

        void test(std::istringstream& is) {

            std::string token;
            is >> token;
            if (token == "perft")
                test_perft();
            else if (token == "mate")
                test_mate(is);
        }

        // The win rate model returns the probability of winning (in per mille units) given an
        // eval and a game ply. It fits the LTC fishtest statistics rather accurately.
        int win_rate_model(Value v, int ply) {

            // The model only captures up to 240 plies, so limit the input and then rescale
            double m = std::min(240, ply) / 64.0;

            // The coefficients of a third-order polynomial fit is based on the fishtest data
            // for two parameters that need to transform eval to the argument of a logistic
            // function.
            constexpr double as[] = { 0.38036525, -2.82015070, 23.17882135, 307.36768407 };
            constexpr double bs[] = { -2.29434733, 13.27689788, -14.26828904, 63.45318330 };

            // Enforce that NormalizeToPawnValue corresponds to a 50% win rate at ply 64
            static_assert(UCI::NormalizeToPawnValue == int(as[0] + as[1] + as[2] + as[3]));

            double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
            double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

            // Transform the eval to centipawns with limited range
            double x = std::clamp(double(v), -4000.0, 4000.0);

            // Return the win rate in per mille units, rounded to the nearest integer
            return int(0.5 + 1000 / (1 + std::exp((a - x) / b)));
        }

    } // namespace


    // Waits for a command from the stdin, parses it, and then calls the appropriate
    // function. It also intercepts an end-of-file (EOF) indication from the stdin to ensure a
    // graceful exit if the GUI dies unexpectedly. When called with some command-line arguments,
    // like running 'bench', the function returns immediately after the command is executed.
    // In addition to the UCI ones, some additional debug commands are also supported.
    void UCI::loop(int argc, char* argv[]) {

        Position     pos;
        std::string  token, cmd;
        StateListPtr states(new std::deque<StateInfo>(1));

        pos.set(StartFEN, false, &states->back(), Threads.main());

        for (int i = 1; i < argc; ++i)
            cmd += std::string(argv[i]) + " ";

        do
        {
            if (argc == 1 && !getline(std::cin, cmd)) // Wait for an input or an end-of-file (EOF) indication
                cmd = "quit";

            std::istringstream is(cmd);

            token.clear(); // Avoid a stale if getline() returns nothing or a blank line
            is >> std::skipws >> token;

            if (token == "quit" || token == "stop")
                Threads.stop = true;

            // The GUI sends 'ponderhit' to tell that the user has played the expected move.
            // So, 'ponderhit' is sent if pondering was done on the same move that the user has played.
            // The search should continue, but should also switch from pondering to the normal search.
            else if (token == "ponderhit")
                Threads.main()->ponder = false; // Switch to the normal search

            else if (token == "uci")
            {
                sync_cout << "id name " << engine_info(true) << "\n" << Options << "\nuciok" << sync_endl;
                bUCI = true;
            }
            else if (token == "setoption")
                setoption(is);
            else if (token == "go")
                go(pos, is, states);
            else if (token == "position")
                position(pos, is, states);
            else if (token == "ucinewgame")
                Search::clear();
            else if (token == "isready")
                sync_cout << "readyok" << sync_endl;

            // Add custom non-UCI commands, mainly for debugging purposes.
            // These commands must not be used during a search!
            else if (token == "flip")
                pos.flip();
            else if (token == "bench")
                bench(pos, is, states);
            else if (token == "d")
                sync_cout << pos << sync_endl;
            else if (token == "eval")
                trace_eval(pos);
            else if (token == "compiler")
                sync_cout << compiler_info() << sync_endl;
            else if (token == "export_net")
            {
                std::optional<std::string> filename;
                std::string                f;
                if (is >> std::skipws >> f)
                    filename = f;
                Eval::NNUE::save_eval(filename);
            }
            else if (token == "--help" || token == "help" || token == "--license" || token == "license")
                sync_cout
                << "\nStockfish is a powerful chess engine for playing and analyzing."
                "\nIt is released as free software licensed under the GNU GPLv3 License."
                "\nStockfish is normally used with a graphical user interface (GUI) and implements"
                "\nthe Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc."
                "\nFor any further information, visit https://github.com/official-stockfish/Stockfish#readme"
                "\nor read the corresponding README.md and Copying.txt files distributed along with this program.\n"
                << sync_endl;

            // Start of my own commands
            else if (token == "fen")
                fen(pos, is, states);
            else if (token == "moves")
                all_moves(pos, is);
            else if (token == "new")
                new_game(pos, states);
            else if (token == "test")
                test(is);
            else if (SAN::is_ok(token))
            {
                Move move = SAN::algebraic_to_move(pos, token);
                if (move)
                {
                    pos.do_move(move, states->emplace_back());
                    std::cout << pos << std::endl;
                }
                else
                    std::cout << "Illegal move!" << std::endl;
            }
            else if (!token.empty() && token[0] != '#')
                sync_cout << "Unknown command: '" << cmd << "'. Type help for more information." << sync_endl;

        } while (token != "quit" && argc == 1); // The command-line arguments are one-shot
    }


    // Turns a Value to an integer centipawn number,
    // without treatment of mate and similar special scores.
    int UCI::to_cp(Value v) { return 100 * v / UCI::NormalizeToPawnValue; }

    // Converts a Value to a string by adhering to the UCI protocol specification:
    //
    // cp <x>    The score from the engine's point of view in centipawns.
    // mate <y>  Mate in 'y' moves (not plies). If the engine is getting mated,
    //           uses negative values for 'y'.
    std::string UCI::value(Value v) {

        assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

        std::stringstream ss;

        if (std::abs(v) < VALUE_TB_WIN_IN_MAX_PLY)
            ss << "cp " << (useClassic ? UCI::Classic::to_cp(v) : UCI::to_cp(v));
        else if (std::abs(v) <= VALUE_TB)
        {
            int ply = VALUE_TB - 1 - std::abs(v); // recompute ss->ply
            ss << "cp " << (v > 0 ? 20000 - ply : -20000 + ply);
        }
        else
            ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

        return ss.str();
    }


    // Reports the win-draw-loss (WDL) statistics given an evaluation
    // and a game ply based on the data gathered for fishtest LTC games.
    std::string UCI::wdl(Value v, int ply) {

        std::stringstream ss;

        int wdl_w = win_rate_model(v, ply);
        int wdl_l = win_rate_model(-v, ply);
        int wdl_d = 1000 - wdl_w - wdl_l;
        ss << " wdl " << wdl_w << " " << wdl_d << " " << wdl_l;

        return ss.str();
    }


    // Converts a Square to a string in algebraic notation (g1, a7, etc.)
    std::string UCI::square(Square s) {
        return std::string{ char('a' + file_of(s)), char('1' + rank_of(s)) };
    }


    // Converts a Move to a string in coordinate notation (g1f3, a7a8q).
    // The only special case is castling where the e1g1 notation is printed in
    // standard chess mode and in e1h1 notation it is printed in Chess960 mode.
    // Internally, all castling moves are always encoded as 'king captures rook'.
    std::string UCI::move(Move m, bool chess960) {

        if (m == Move::none())
            return "(none)";

        if (m == Move::null())
            return "0000";

        Square from = m.from_sq();
        Square to = m.to_sq();

        if (m.type_of() == CASTLING && !chess960)
            to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

        std::string move = UCI::square(from) + UCI::square(to);

        if (m.type_of() == PROMOTION)
            move += " pnbrqk"[m.promotion_type()];

        return move;
    }


    // Converts a string representing a move in coordinate notation
    // (g1f3, a7a8q) to the corresponding legal Move, if any.
    // NEW: Allows SAN notation as input too.
    Move UCI::to_move(const Position& pos, std::string& str) {

        return SAN::algebraic_to_move(pos, str);

#if 0
        if (str.length() == 5)
            str[4] = char(tolower(str[4])); // The promotion piece character must be lowercased

        for (const auto& m : MoveList<LEGAL>(pos))
            if (str == UCI::move(m, pos.is_chess960()))
                return m;

        return Move::none();
#endif
    }

    // Converts a pv to a string.
    std::string UCI::pv_to_string(const Position& pos, const Move* pv, bool isSAN)
    {
        std::ostringstream ss;
        for (; *pv; ++pv)
        {
            if (isSAN)
                ss << " " << SAN::algebraic_to_string(pos, UCI::move(*pv, pos.is_chess960()));
            else
                ss << " " << UCI::move(*pv, pos.is_chess960());
        }
        return ss.str();
    }

    // for Shashin theory begin
    uint8_t UCI::getWinProbability(Value v, int ply) {

        double correctionFactor = (std::min(240, ply) / 64.0);
        double forExp1 = ((((0.38036525 * correctionFactor - 6.94334517) * correctionFactor + 23.17882135)
            * correctionFactor)
            + 307.36768407);
        double forExp2 = (((-2.29434733 * correctionFactor + 13.27689788) * correctionFactor - 14.26828904)
            * correctionFactor)
            + 63.45318330;
        double winrateToMove = 0.5 + 1000 / (1 + std::exp((forExp1 - (std::clamp(double(v), -4000.0, 4000.0))) / forExp2));
        double winrateOpponent = 0.5 + 1000 / (1 + std::exp((forExp1 - (std::clamp(double(-v), -4000.0, 4000.0))) / forExp2));
        double winrateDraw = 1000 - winrateToMove - winrateOpponent;
        return static_cast<uint8_t>(round((winrateToMove + winrateDraw / 2.0) / 10.0));
    }
    // for Shashin theory end

    namespace UCI::Classic {

        int win_rate_model(Value v, int ply) {

            // The model only captures up to 240 plies, so limit the input and then rescale
            double m = std::min(240, ply) / 64.0;

            // The coefficients of a third-order polynomial fit is based on the fishtest data
            // for two parameters that need to transform eval to the argument of a logistic function.
            constexpr double as[] = { -0.58270499,    2.68512549,   15.24638015,  344.49745382 };
            constexpr double bs[] = { -2.65734562,   15.96509799,  -20.69040836,   73.61029937 };

            // Enforce that NormalizeToPawnValue corresponds to a 50% win rate at ply 64
            static_assert(NormalizeToPawnValue == int(as[0] + as[1] + as[2] + as[3]));

            double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
            double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

            // Transform the eval to centipawns with limited range
            double x = std::clamp(double(v), -4000.0, 4000.0);

            // Return the win rate in per mille units rounded to the nearest value
            return int(0.5 + 1000 / (1 + std::exp((a - x) / b)));
        }

        int to_cp(Value v) { return 100 * v / NormalizeToPawnValue; }
    }

} // namespace Stockfish
