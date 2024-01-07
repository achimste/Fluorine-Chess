#ifndef BOOK_H_INCLUDED
#define BOOK_H_INCLUDED

#include <string>
#include <vector>

#include "position.h"

namespace Stockfish::Book {

	struct BookMove {
		Move move;
		Key hashKey;

		BookMove(Move m, Key key) {
			move = m;
			hashKey = key; // position key AFTER the move has been made
		}
	};

	struct Book {
		std::string opening;
		std::vector<BookMove> list;
	};

	void init();
	Move find_move(const Position& pos);
	const Book* find_opening(const Position& pos);
}

#endif