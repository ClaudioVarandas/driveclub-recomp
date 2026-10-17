// Reads single files out of the game's own archive (game.ndx + gameNNN.dat).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Finds `path` (for example "newdata/vehicles.csv") in the archive whose index is in `update_dir` (or, when
// missing there, `base_dir`). The volume files are taken from `update_dir` first, then `base_dir`.
// Returns "" on success, otherwise a short description of what went wrong.
std::string DatxExtract(const std::wstring& update_dir, const std::wstring& base_dir, const std::string& path,
                        std::vector<uint8_t>& out);
