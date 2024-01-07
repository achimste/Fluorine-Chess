#ifndef SAN_H_INCLUDED
#define SAN_H_INCLUDED

#include "position.h"
#include "search.h"

namespace Stockfish::SAN {

	Move algebraic_to_move(const Position& pos, const std::string& str);
	std::string algebraic_to_string(const Position& pos, const std::string& str);
	std::string to_san(const Position& pos, Move move);
	std::string to_san(const Position& pos, const Search::RootMove& rm);
	bool is_ok(const std::string& str);

}

#endif
