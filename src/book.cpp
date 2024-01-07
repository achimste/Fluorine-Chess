#include <deque>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "book.h"
#include "san.h"
#include "thread.h"
#include "uci.h"

namespace Stockfish::Book {

	const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

	static std::deque<Book> book;

	void init()
	{
		if (!Options["Use Book"]) return;
		if (!book.empty())
		{
			std::cout << "Book is already loaded!" << std::endl;
			return;
		}

		std::ifstream is("eco.txt");
		if (!is)
		{
			Options["Use Book"] = false;
			return;
		}
		bool bLoopGame = false;
		bool bHaveECO = false;
		Book* pBook = 0;

		std::cout << "Init book ...\n";

		Position pos;
		StateListPtr sp(new std::deque<StateInfo>(1));
		{
			pos.set(StartFEN, false, &sp->back(), nullptr);
			auto& b = book.emplace_back();
			b.opening = "Initial position";
			b.list.emplace_back(Move::none(), pos.key());
		}

		while (true) {

			std::string line;
			if (!std::getline(is, line)) break;
			if (line.empty()) continue;

			try
			{
				// parse opening
				std::smatch match;
				std::regex rx(R"(\{(\w*)\s*(.*)\}\s*(.*))");
				if (std::regex_match(line, match, rx))
				{
					sp.reset(new std::deque<StateInfo>(1));
					pos.set(StartFEN, false, &sp->back(), Threads.main());
					Book& b = book.emplace_back();
					b.opening = match[1].str() + " " + match[2].str();
#if _DEBUG
					std::cout << b.opening << " ->";
#endif

					std::string line(match[3]);
					std::smatch match2;
					std::regex rx2(R"((\d*)\.\s*(\S*)\s*(\S*))");

					while (std::regex_search(line, match2, rx2))
					{
						for (int i = 2; i <= 3; ++i)
						{
							if (match2[i].str().empty()) continue;
							Move m = SAN::algebraic_to_move(pos, match2[i]);
							if (m)
							{
								pos.do_move(m, sp->emplace_back());
								b.list.emplace_back(m, pos.key());
#if _DEBUG
								std::cout << " " << match2[i];
#endif
							}
							else
							{
								std::cout << "\nILLEGAL MOVE!! -> " << match2[i] << std::endl;
								exit(0);
							}
						}
						line = match2.suffix();
					}
#if _DEBUG
					std::cout << std::endl;
#endif
				}
			}
			catch (std::exception& e)
			{
				std::cout << "ERROR: " << e.what() << std::endl;
			}
		}
		std::cout << "finished" << std::endl;
	}

	Move find_move(const Position& pos)
	{
		Move bookMove = Move::none();
		std::random_device rnd;

		if (book.empty())
			return Move::none();

		if (pos.key() == 0x8F8F01D4562F59FB) // position key of the initial position
		{
			std::uniform_int_distribution<int> dist(0, int(book.size()) - 1);
			bookMove = book[dist(rnd)].list[0].move;
		}
		else
		{
			std::unordered_set<Move, Move::MoveHash> list;
			for (const auto& b : book)
			{
				if (b.list.size() > pos.game_ply())
				{
					const BookMove* l = &b.list[pos.game_ply() - 1];
					if (l->hashKey == pos.key())
						list.insert((l + 1)->move);
				}
			}

			if (list.empty())
				return Move::none();

			auto m = list.begin();
			std::uniform_int_distribution<int> dist(0, int(list.size()) - 1);
			std::advance(m, dist(rnd));
			bookMove = *m;
		}
		return bookMove;
	}

	const Book* find_opening(const Position& pos) {

		const Book* pBook = nullptr;
		size_t min = 100;
		for (const auto& b : book)
		{
			if (b.list.size() >= pos.game_ply())
			{
				for (const auto& l : b.list)
				{
					if (l.hashKey == pos.key() && b.list.size() < min)
					{
						min = b.list.size();
						pBook = &b;
					}
				}
			}
		}

		return pBook;
	}
}
