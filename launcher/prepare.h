// First-run preparation of the user's game dumps (see prepare.cpp).
#pragma once

#include <filesystem>
#include <functional>
#include <string>

// Converts the update dump (and the base dump's modules) into the runtime image under `work`:
//   work/prep/boot-linked.bin, content.bin, patches.bin, eboot.elf, image.meta
// With patches_only the image is reused and only patches.bin is rebuilt.
// Returns an empty string on success, otherwise a message for the user.
// progress(step, text) is called with step 0..6.
std::string PrepareGame(const std::filesystem::path& update, const std::filesystem::path& base,
                        const std::filesystem::path& work, const std::filesystem::path& patch_xml, bool patches_only,
                        const std::function<void(int, const std::string&)>& progress);
