#pragma once
// The generated block textures, written out for inspection: a contact sheet of
// every tile, and a traced scene built out of them.
//
// This was `scenes/textures.cpp` until the engine and the game were pulled
// apart. The recipes it shows are Konstruct's -- `game::generateTextures` --
// and a scene cannot link against the game, so the tool moved to where the
// pixels are. It sits beside `snapshot`, `physgun`, `swing` and `ragdoll`: a
// mode that writes a file and exits, for the things a hand on a mouse cannot
// check from a build script.
//
// The sheet is the working half. A texture that is wrong is usually wrong in
// one tile, and finding that tile on a rendered wall means flying around;
// finding it on a sheet means looking.
//
//   konstruct textures            the sheet and a traced scene
//   konstruct textures sheet      the sheet only, which is instant
//   konstruct textures size=64    generate at a different resolution

namespace game {

// Runs the mode and returns the process exit code.
int runTextures(int argc, char** argv);

} // namespace game
