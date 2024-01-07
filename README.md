<div align="center">

  [![Stockfish][stockfish128-logo]][website-link]

  <h3>Stockfish</h3>

  A free and strong UCI chess engine.
  <br>
  <strong>[Explore Stockfish docs »][wiki-link]</strong>
  <br>
  <br>
  [Report bug][issue-link]
  ·
  [Open a discussion][discussions-link]
  ·
  [Discord][discord-link]
  ·
  [Blog][website-blog-link]

  [![Build][build-badge]][build-link]
  [![License][license-badge]][license-link]
  <br>
  [![Release][release-badge]][release-link]
  [![Commits][commits-badge]][commits-link]
  <br>
  [![Website][website-badge]][website-link]
  [![Fishtest][fishtest-badge]][fishtest-link]
  [![Discord][discord-badge]][discord-link]

</div>

## About this modification

I named this Stockfish derivate "Fluorine" - the most agressive element in chemistry.
Due to my interest in chess engine programming I use Stockfish as a playground and see
what I can do to it. And this is my current result.<br>
I have created this mod out of pure hobby and enjoyment. So you should do too...

In case you find my additions/modifications interesting, let me know it. If you like to
contribute to it, you'd make me a happy man.

So.. let's get started. The features of Fluorine are ...


* Latest Stockfish 17 dev engine as base.

  Full functionalty is guaranteed.

* Reintroducing the 'old' Stockfish 15.1 classic evaluation.

	For fun and didactic reasons, I reimplemented the 'classic' evaluation
	function, while adapting the code to some new code features of SF 17.<br>
	Use 'setoption name Use Classic value true' to activate the classic evaluation.<br>
	As a bonus, searching for mates is done with Joerg Oster's "Huntsman-2023" specialized
	evaluation function. Thanks for Joerg's efforts! It can solve many positions way faster than
	traditional evaluation!
	

* Switching from C++17 to C++20

  The use of the compiler flag USE_POPCNT has been cancelled. Instead std::popcount is used.<br>
  Also some other minor parts of the code can benefit from a better readability and simplifications.<br>
  Project files for MS Visual Studio 2022 are included. It can compile for AVX512 and AVX2 processors.
  Only 64-bit mode is supported.


* Implementing some chess theory from Alexander Shashin

	Thanks for [ShashChess][shashchess-link] by amchess for this interesting idea.<br>
	I have taken relevant parts of the Shashin code and added them to the latest
	Stockfish 17 Dev engine.<br>
	To activate the Shashin modification, use 'setoption name Use Shashin value true'.<br>
	In a nutshell, this modification alters the playing style depending on the position.
	

* Distinguishing between UCI and non-UCI mode.

  Whenever a GUI calls "uci", the engine behaves like the normal Stockfish engine and follows
  the UCI protocol.
  Without "uci", we enter a non-UCI mode. This enables the so called SAN (short algebraic notation)
  as the engine ouput, as well as other non-UCI commands. These are new commands/features are:

  -- *fen* 'fen string'

     This features a shortcut of "position fen ..." and also resets all hash tables.<br>
	 FEN string are now analysed using std::regex. This also prevents the engine from crashes
	 caused by some faulty FEN strings. Of course, not every bugged string can be catched.

	 
  -- *new*

     Shortcut for the commands "position startpos" and "ucinewgame". Alas it resets the engine.
	 This also sets the engine back to non-UCI mode.

  
  -- *test perft*

     I have re-written the perft code in such a way, that it uses a thread pool to utilize all
	 CPU cores for parallel performance test.<br>
	 Depending on having Chess960 enabled or not, two files (fisher.epd and standard.epd)
	 that contain test positions and depth outcome will be used for testing the speed and
	 reliability of the move generator. Place these files in the same directory as the engine exe.<br>
	 If you use the usual "go perft 'depth'" command, you will still get the default Stockfish
	 output.


  -- *test mate [movetime n]*

     This is one of the main features of my modification.<br>
	 This will open the file "matetrack.epd" that contains several thousand positions for
	 mate searching. The engine will try to solve the puzzle and writes the results in a CSV
	 file. Place the file in the same directory as the engine exe.

	 For best results to even crack very hard positions, I recommend to increase the hash size
	 and the number of threads to be used!
	 
	 You can supply a time limit with the parameter movetime to cancel the mate search after n seconds.


  -- *accepting move to play*

	 When you enter any valid move in long or short algebraic notation (SAN) instead of an engine command,
     the engine will immediatly play this move and displays the new position.<br>
     E.g. when you start the engine and type "b1c3" or "Nc3" in the console, the move will be executed.<br>
	 The code for the SAN is available in the files san.h and san.cpp


  -- *Use book* as a boolean UCI option (WIP!)

     With this option enabled, the engine will use the file "eco.txt" that contains
	 all ECO openings. Place it in the same directory as the engine exe.<br>
	 To activate book moves, use 'setoption name Use Book value true'. This feature can be turned on
	 and off any time.<br>
	 Note: Currently random book moves will be played.

	
  -- *moves*
  
	This new command shows all legal moves of a position and gives you additional information about
	each move like castling, en passant, good or bad capture, pin and promotion.<br>
	If the opening book is enabled (see above), it will also display the opening of a move if it
	belongs to an opening.


Code dependency: This modification uses a [thread-pool by Barak Shoshany (bshoshany)][bshoshany-link].*
Thank you for this wonderful easy-to-use thread pool!

Please note, that I program in MS Visual-Studio C++ exclusively! No Linux, Apple, Android or MinGW etc.
If you encounter any errors of my mod when using the code on different platforms, let me know.

Constructive comments on my modification are welcomed!


## Overview

[Stockfish][website-link] is a **free and strong UCI chess engine** derived from
Glaurung 2.1 that analyzes chess positions and computes the optimal moves.

Stockfish **does not include a graphical user interface** (GUI) that is required
to display a chessboard and to make it easy to input moves. These GUIs are
developed independently from Stockfish and are available online. **Read the
documentation for your GUI** of choice for information about how to use
Stockfish with it.

See also the Stockfish [documentation][wiki-usage-link] for further usage help.

## Files

This distribution of Stockfish consists of the following files:

  * [README.md][readme-link], the file you are currently reading.

  * [Copying.txt][license-link], a text file containing the GNU General Public
    License version 3.

  * [AUTHORS][authors-link], a text file with the list of authors for the project.

  * [src][src-link], a subdirectory containing the full source code, including a
    Makefile that can be used to compile Stockfish on Unix-like systems.

  * a file with the .nnue extension, storing the neural network for the NNUE
    evaluation. Binary distributions will have this file embedded.

## Contributing

__See [Contributing Guide](CONTRIBUTING.md).__

### Donating hardware

Improving Stockfish requires a massive amount of testing. You can donate your
hardware resources by installing the [Fishtest Worker][worker-link] and viewing
the current tests on [Fishtest][fishtest-link].

### Improving the code

In the [chessprogramming wiki][programming-link], many techniques used in
Stockfish are explained with a lot of background information.
The [section on Stockfish][programmingsf-link] describes many features
and techniques used by Stockfish. However, it is generic rather than
focused on Stockfish's precise implementation.

The engine testing is done on [Fishtest][fishtest-link].
If you want to help improve Stockfish, please read this [guideline][guideline-link]
first, where the basics of Stockfish development are explained.

Discussions about Stockfish take place these days mainly in the Stockfish
[Discord server][discord-link]. This is also the best place to ask questions
about the codebase and how to improve it.

## Compiling Stockfish

Stockfish has support for 32 or 64-bit CPUs, certain hardware instructions,
big-endian machines such as Power PC, and other platforms.

On Unix-like systems, it should be easy to compile Stockfish directly from the
source code with the included Makefile in the folder `src`. In general, it is
recommended to run `make help` to see a list of make targets with corresponding
descriptions. An example suitable for most Intel and AMD chips:

```
cd src
make -j profile-build ARCH=x86-64-avx2
```

Detailed compilation instructions for all platforms can be found in our
[documentation][wiki-compile-link]. Our wiki also has information about
the [UCI commands][wiki-uci-link] supported by Stockfish.

## Terms of use

Stockfish is free and distributed under the
[**GNU General Public License version 3**][license-link] (GPL v3). Essentially,
this means you are free to do almost exactly what you want with the program,
including distributing it among your friends, making it available for download
from your website, selling it (either by itself or as part of some bigger
software package), or using it as the starting point for a software project of
your own.

The only real limitation is that whenever you distribute Stockfish in some way,
you MUST always include the license and the full source code (or a pointer to
where the source code can be found) to generate the exact binary you are
distributing. If you make any changes to the source code, these changes must
also be made available under GPL v3.


[authors-link]:       https://github.com/official-stockfish/Stockfish/blob/master/AUTHORS
[build-link]:         https://github.com/official-stockfish/Stockfish/actions/workflows/stockfish.yml
[commits-link]:       https://github.com/official-stockfish/Stockfish/commits/master
[discord-link]:       https://discord.gg/GWDRS3kU6R
[issue-link]:         https://github.com/official-stockfish/Stockfish/issues/new?assignees=&labels=&template=BUG-REPORT.yml
[discussions-link]:   https://github.com/official-stockfish/Stockfish/discussions/new
[fishtest-link]:      https://tests.stockfishchess.org/tests
[guideline-link]:     https://github.com/official-stockfish/fishtest/wiki/Creating-my-first-test
[license-link]:       https://github.com/official-stockfish/Stockfish/blob/master/Copying.txt
[programming-link]:   https://www.chessprogramming.org/Main_Page
[programmingsf-link]: https://www.chessprogramming.org/Stockfish
[readme-link]:        https://github.com/official-stockfish/Stockfish/blob/master/README.md
[release-link]:       https://github.com/official-stockfish/Stockfish/releases/latest
[src-link]:           https://github.com/official-stockfish/Stockfish/tree/master/src
[stockfish128-logo]:  https://stockfishchess.org/images/logo/icon_128x128.png
[uci-link]:           https://backscattering.de/chess/uci/
[website-link]:       https://stockfishchess.org
[website-blog-link]:  https://stockfishchess.org/blog/
[wiki-link]:          https://github.com/official-stockfish/Stockfish/wiki
[wiki-compile-link]:  https://github.com/official-stockfish/Stockfish/wiki/Compiling-from-source
[wiki-uci-link]:      https://github.com/official-stockfish/Stockfish/wiki/UCI-&-Commands
[wiki-usage-link]:    https://github.com/official-stockfish/Stockfish/wiki/Download-and-usage
[worker-link]:        https://github.com/official-stockfish/fishtest/wiki/Running-the-worker

[build-badge]:        https://img.shields.io/github/actions/workflow/status/official-stockfish/Stockfish/stockfish.yml?branch=master&style=for-the-badge&label=stockfish&logo=github
[commits-badge]:      https://img.shields.io/github/commits-since/official-stockfish/Stockfish/latest?style=for-the-badge
[discord-badge]:      https://img.shields.io/discord/435943710472011776?style=for-the-badge&label=discord&logo=Discord
[fishtest-badge]:     https://img.shields.io/website?style=for-the-badge&down_color=red&down_message=Offline&label=Fishtest&up_color=success&up_message=Online&url=https%3A%2F%2Ftests.stockfishchess.org%2Ftests%2Ffinished
[license-badge]:      https://img.shields.io/github/license/official-stockfish/Stockfish?style=for-the-badge&label=license&color=success
[release-badge]:      https://img.shields.io/github/v/release/official-stockfish/Stockfish?style=for-the-badge&label=official%20release
[website-badge]:      https://img.shields.io/website?style=for-the-badge&down_color=red&down_message=Offline&label=website&up_color=success&up_message=Online&url=https%3A%2F%2Fstockfishchess.org

[shashchess-link]:    https://github.com/amchess/ShashChess
[bshoshany-link]:     https://github.com/bshoshany/thread-pool
