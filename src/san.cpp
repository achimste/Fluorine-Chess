#include <cassert>
#include <algorithm>
#include <regex>
#include <sstream>
#include <vector>
#include "san.h"
#include "movegen.h"
#include "uci.h"

// This is all about handling algebraic notation (AN).
// We distinguish between ...
// 1. LAN (long algebraic notation) -> standard in UCI.
// 2. SAN (standard algebraic notation) -> used in PGN files and other displays.

namespace Stockfish::SAN
{

	bool is_ok(const std::string& s)
	{
		std::smatch match;
		std::regex rx("([a-h][1-8])([a-h][1-8])([qrbn]?)");
		if (std::regex_match(s, match, rx))
			return true;

		rx = R"(([NBRQK])?([a-h1-8])?([1-8])?([x])?([a-h][1-8])(\=[NBRQ])?.*)";
		if (std::regex_match(s, match, rx))
			return true;

		std::string castle(s);
		std::for_each(castle.begin(), castle.end(), [](char& c) { c = tolower(c); });
		return castle == "o-o" || s == "o-o-o";
	}

	// Helper: returns square from an AN string.
	static Square get_square_from(const std::string& s) {

		return make_square(File(s[0] - 'a'), Rank(s[1] - '1'));
	}

	// Helper: returns the promotion piece type from a character.
	static PieceType get_pt_from_prom(char c) {

		static std::string_view PieceToChar("  nbrq");
		size_t idx = PieceToChar.find(tolower(c));
		return idx != std::string::npos ? PieceType(idx) : QUEEN;
	}

	// Helper: returns the piece type from a character.
	static PieceType get_pt(char c) {

		static std::string_view PieceToChar(" pnbrqk");
		size_t idx = PieceToChar.find(tolower(c));
		return idx != std::string::npos ? PieceType(idx) : PAWN;
	}

	// Helper: returns a Move from the given parameters.
	using ret = std::pair<std::string, Move>;

	static ret get_move_from(const Position& pos, PieceType pt, Square to, PieceType promotion, File file, Rank rank) {

		MoveList<LEGAL> ml(pos);
		std::vector<ExtMove> list(ml.begin(), ml.end());
			
		std::erase_if(list, [&](const auto& m) {

			return (m.to_sq() != to
				|| type_of(pos.piece_on(m.from_sq())) != pt
				|| (m.type_of() == PROMOTION && (!promotion || m.promotion_type() != promotion)));
			});

		if (list.size() == 0)
			return { "", Move::none() };

		// Disambiguating
		if (list.size() > 1)
		{
			if (file >= FILE_A && file <= FILE_H)
				std::erase_if(list, [&](const auto& m) { return file_of(m.from_sq()) != file; });
			if (rank >= RANK_1 && rank <= RANK_8)
				std::erase_if(list, [&](const auto& m) { return rank_of(m.from_sq()) != rank; });
		}

		if (list.size() == 1)
			return { UCI::move(list[0],pos.is_chess960()), list[0] };

		return { "", Move::none() };
	}

	// Returns the Move from a string in LAN or SAN.
	Move algebraic_to_move(const Position& pos, const std::string& str) {

		Color us = pos.side_to_move();
		Square sq_from, sq_to;
		PieceType p = NO_PIECE_TYPE;
		Move move = Move::none();
		std::smatch match;

		// Test for LAN
		std::regex rx("([a-h][1-8])([a-h][1-8])([qrbn]?)");
		if (std::regex_match(str, match, rx))
		{
			sq_from = get_square_from(match[1]);
			sq_to = get_square_from(match[2]);

			if (type_of(pos.piece_on(sq_from)) == PAWN)
			{
				// Promotion
				if (relative_rank(us, sq_to) == RANK_8)
				{
					PieceType p = match[3].length() ? get_pt_from_prom(match[3].str()[0]) : QUEEN;
					move = Move::make<PROMOTION>(sq_from, sq_to, p);
				}
				else if (pos.state()->epSquare != SQ_NONE && sq_to == pos.state()->epSquare)
				{
					move = Move::make<EN_PASSANT>(sq_from, sq_to);
				}
			}

			// Castling
			else if (type_of(pos.piece_on(sq_from)) == KING)
			{
				if (pos.is_chess960())
				{
					if (pos.piece_on(sq_to) == make_piece(us, ROOK))
						move = Move::make<CASTLING>(sq_from, sq_to);
				}
				else
				{
					if (str == "e1g1" || str == "e8g8")
						move = Move::make<CASTLING>(relative_square(us, SQ_E1), pos.castling_rook_square(us & KING_SIDE));
					if (str == "e1c1" || str == "e8c8")
						move = Move::make<CASTLING>(relative_square(us, SQ_E1), pos.castling_rook_square(us & QUEEN_SIDE));
				}
			}

			// Other moves
			if (!move)
				move = Move(sq_from, sq_to);

			std::cout << UCI::move(move, pos.is_chess960()) << std::endl;

			return MoveList<LEGAL>(pos).contains(move) ? move : Move::none();
		}

		// Test for SAN.
		rx = R"(([NBRQK])?([a-h1-8])?([1-8])?([x])?([a-h][1-8])(\=[NBRQ])?.*)";
		if (std::regex_match(str, match, rx))
		{
			PieceType p = match[1].length() ? get_pt(match[1].str()[0]) : PAWN;
			File amb_file = File(-1);
			Rank amb_rank = Rank(-1);
			if (match[2].length())
			{
				char c = match[2].str()[0];
				if (isdigit(c)) amb_rank = Rank(c - '1');
				else amb_file = File(c - 'a');
			}
			if (match[3].length())
				amb_rank = Rank(match[3].str()[0] - '1');

			sq_to = get_square_from(match[5]);

			// Handle promotion.
			PieceType promotion = NO_PIECE_TYPE;

			if (p == PAWN && relative_rank(us, sq_to) == RANK_8)
				promotion = match[6].length() ? get_pt_from_prom(match[6].str()[1]) : QUEEN;

			return get_move_from(pos, p, sq_to, promotion, amb_file, amb_rank).second;
		}

		// Handle castling.
		if (str == "O-O" || str == "o-o" || str == "0-0")
			return Move::make<CASTLING>(relative_square(us, SQ_E1), pos.castling_rook_square(us & KING_SIDE));
		if (str == "O-O-O" || str == "o-o-o" || str == "0-0-0")
			return Move::make<CASTLING>(relative_square(us, SQ_E1), pos.castling_rook_square(us & QUEEN_SIDE));
		return move;
	}

	// Returns string in LAN from LAN or SAN.
	std::string algebraic_to_string(const Position& pos, const std::string& str)
	{
		std::string smove;
		std::smatch match;

		// Test for LAN.
		std::regex rx("([a-h][1-8])([a-h][1-8])([qrbn]?)");
		if (std::regex_match(str, match, rx))
			return str;

		// Test for SAN.
		Color us = pos.side_to_move();
		Move move = Move::none();
		rx = R"(([NBRQK])?([a-h1-8])?([1-8])?([x])?([a-h][1-8])(\=[NBRQ])?.*)";
		if (std::regex_match(str, match, rx))
		{
			PieceType p = match[1].length() ? get_pt((match[1].str()[0])) : PAWN;
			File amb_file = File(-1);
			Rank amb_rank = Rank(-1);

			if (match[2].length())
			{
				char c = match[2].str()[0];
				if (isdigit(c)) amb_rank = Rank(c - '1');
				else amb_file = File(c - 'a');
			}
			if (match[3].length())
				amb_rank = Rank(match[3].str()[0] - '1');

			Square sq_to = get_square_from(match[5]);

			// Handle promotion.
			PieceType promotion = NO_PIECE_TYPE;
			if (p == PAWN && relative_rank(pos.side_to_move(), sq_to) == RANK_8)
				promotion = get_pt_from_prom(match[6].str()[1]);
			return get_move_from(pos, p, sq_to, promotion, amb_file, amb_rank).first;
		}

		// Handle castling.
		if (str == "O-O" || str == "o-o" || str == "0-0")
		{
			if (pos.is_chess960())
			{
				Move m = Move::make<CASTLING>(pos.square<KING>(us), pos.castling_rook_square(us & KING_SIDE));
				return UCI::move(m, true);
			}
			else
				return pos.side_to_move() == WHITE ? "e1g1" : "e8g8";
		}
		if (str == "O-O-O" || str == "o-o-o" || str == "0-0-0")
		{
			if (pos.is_chess960())
			{
				Move m = Move::make<CASTLING>(pos.square<KING>(us), pos.castling_rook_square(us & QUEEN_SIDE));
				return UCI::move(m, true);
			}
			else
				return pos.side_to_move() == WHITE ? "e1c1" : "e8c8";
		}
		return "";
	}

	// Converts a move to a SAN string.
	std::string to_san(const Position& pos, Move move)
	{
		static const char* piece = "  NBRQK";

		if (!move)
			return "(none)";

		if (move == Move::null())
			return "0000";

		std::ostringstream SAN;

		if (move.type_of() == CASTLING)
			SAN << (move.from_sq() > move.to_sq() ? "O-O-O" : "O-O");
		else
		{
			PieceType pt = type_of(pos.moved_piece(move));

			if (pt != PAWN)
			{
				SAN << piece[pt];

				// Check for ambiguate from-squares
				if (popcount(pos.pieces(pos.side_to_move(), pt) > 1))
				{
					MoveList<LEGAL> ml(pos);
					std::vector<ExtMove> list(ml.begin(), ml.end());
					std::erase_if(list, [&](const auto& m) {

						return m.to_sq() != move.to_sq() || type_of(pos.moved_piece(m)) != pt;
						});

					if (list.size() > 1)
					{
						if (std::ranges::count_if(list, [&](const auto& m) {
							return file_of(move.from_sq()) == file_of(m.from_sq());
							}) == 1)
							SAN << char(file_of(move.from_sq()) + 'a');
						else if (std::ranges::count_if(list, [&](const auto& m) {
							return rank_of(move.from_sq()) == rank_of(m.from_sq());
							}) == 1)
							SAN << char(rank_of(move.from_sq()) + '1');
						else
							SAN << move.from_sq();
					}
				}
			}

			if (pos.capture(move))
			{
				if (pt == PAWN)
					SAN << char(file_of(move.from_sq()) + 'a');

				SAN << 'x';
			}

			SAN << move.to_sq();

			if (move.type_of() == EN_PASSANT)
				SAN << "/e.p.";

			else if (move.type_of() == PROMOTION)
				SAN << '=' << piece[move.promotion_type()];
		}

		if (pos.gives_check(move))
		{
			StateListPtr sp(new std::deque<StateInfo>(1));
			Position copy;
			copy.set(pos.fen(), pos.side_to_move(), &sp->back(), nullptr);
			copy.do_move<false>(move, sp->emplace_back());
			SAN << (MoveList<LEGAL>(copy).size() ? '+' : '#');
		}

		return SAN.str();
	}

	std::string to_san(const Position& pos, const Search::RootMove& rm)
	{
		std::ostringstream SAN;
		StateListPtr sp(new std::deque<StateInfo>(1));
		Position copy;
		copy.set(pos.fen(), pos.is_chess960(), &sp->back(), nullptr);
		for (const auto& move : rm.pv)
		{
			if (!move) break;
			assert(MoveList<LEGAL>(copy).contains(move));

			SAN << " " << to_san(copy, move);
			copy.do_move<false>(move, sp->emplace_back());
		}
		return SAN.str();
	}

}
