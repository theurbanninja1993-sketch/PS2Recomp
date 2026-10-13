#ifndef PS2_PROMPT_ART_H
#define PS2_PROMPT_ART_H

#include <string>

// Runtime-generated prompt art.
//
// The shipped prompts/<set>/ textures are baked by the Python tools with the
// classic bindings. The keyboard set is the one whose art must follow the
// user's key bindings, so it is regenerated at runtime: keycaps are redrawn
// whenever a bound key changes, written to
// <config>/ps2recomp/prompts_rt/<set>/ and picked up by the prompt loader,
// which reads that folder after the shipped prompts/<set>/ files (last wins).
std::string ps2PromptArtDirectory();

// Regenerates the runtime art when the tracked bindings changed. Returns true
// when files were (re)written and the replacement index needs a refresh.
bool ps2PromptArtRefreshIfNeeded();

#endif
