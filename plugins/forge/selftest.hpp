#pragma once
// What can be checked about this program without a window.
//
// The editor core has `scene_test_edit`, which is where the documents, the
// undo stack and the exporter are proved. What that cannot reach is the seam
// this program owns: the claim that **the cursor and the renderer agree about
// where the model is**. One builds a matrix and hands it to `PropSet`; the
// other inverts the same matrix and traces through it. They are one
// derivation on purpose, and a check that says so is what keeps them one.
//
// Run as `forge selftest`. Nonzero on failure, like every other test here.
namespace forge {

int runSelfTest();

} // namespace forge
