#ifndef PS2_PROMPT_ART_H
#define PS2_PROMPT_ART_H

#include <string>

// Runtime-generated prompt art.
//
// The shipped prompts/<set>/ textures are baked by the Python tools with the
// classic bindings. This module regenerates the prompt textures so they
// follow the live bindings instead:
//  - the keyboard set's keycaps are labelled with the bound keys;
//  - the controller sets' glyphs show the host button that currently reaches
//    each PS2 button (Classic / Modern / Custom layouts and the on-foot /
//    in-vehicle contexts included).
// The art is written to <config>/ps2recomp/prompts_rt/<set>/ and picked up
// by the prompt loader, which reads that folder after the shipped
// prompts/<set>/ files (last wins). Rewrites are debounced while bindings
// and the pad context settle.
std::string ps2PromptArtDirectory();

// Regenerates the runtime art when the drawn result would change. Returns
// true when files were (re)written and the replacement index needs a refresh.
bool ps2PromptArtRefreshIfNeeded();

#endif
