#pragma once
// The looks the game can be seen in, as a short list rather than a panel of
// sliders.
//
// `ViewportEffects` has nine numbers in it, and nine numbers is a settings
// screen nobody opens twice. What people actually want is three or four
// answers to "how should this look", each of which is a whole set of those
// numbers chosen together -- bloom that suits the bands, an outline that suits
// the bloom. So the numbers stay in the engine, where they are honest, and the
// game hands out combinations of them under names.
//
// The same reasoning `styles` uses one floor down for the tracer: a style is a
// decision about the picture, not a slider.
#include "engine/render/gl/viewport.hpp"

namespace game {

enum class Look {
    Plain,   // what the viewport always did: shade, expose, tone map
    Vivid,   // the same world, photographed properly
    Cel,     // banded light and a drawn edge
    Ink,     // the edge without the bands
    Count,
};

struct LookPreset {
    const char* name;
    blocky::ViewportEffects effects;
};

inline const LookPreset& lookPreset(Look look) {
    static const LookPreset kPresets[] = {
        // -------------------------------------------------------- Plain
        // Every field at its default, which is what the four shaders used to
        // do for themselves. Kept as a preset rather than as "effects off" so
        // that there is something to compare the others against.
        {"PLAIN", {}},

        // -------------------------------------------------------- Vivid
        // No stylisation, just the frame treated as a photograph: glowstone
        // and lava bloom because they are genuinely brighter than white, a
        // little more colour, and corners that fall off the way a lens does.
        {"VIVID",
         [] {
             blocky::ViewportEffects fx;
             fx.bloomIntensity = 0.55f;
             fx.bloomThreshold = 1.1f;
             fx.vignette = 0.30f;
             fx.saturation = 1.14f;
             fx.contrast = 1.06f;
             return fx;
         }()},

        // ---------------------------------------------------------- Cel
        // Four steps of light and a line round the shapes.
        //
        // Four rather than three: a voxel world already quantises its normals
        // to six directions, so the sun gives at most a handful of distinct
        // values before the bands do anything, and three collapses the sky
        // term into the ground term. The outline is what does most of the
        // work -- bands alone read as a rendering bug.
        {"CEL",
         [] {
             blocky::ViewportEffects fx;
             fx.celBands = 4.0f;
             fx.outline = 0.72f;
             fx.bloomIntensity = 0.30f;
             fx.bloomThreshold = 1.25f;
             fx.saturation = 1.20f;
             fx.contrast = 1.05f;
             return fx;
         }()},

        // ---------------------------------------------------------- Ink
        // The edge alone, over ordinary shading, with the colour pulled down.
        // Proof that the two halves of cel are separate seams and not one
        // switch: this is the outline without the bands, and `Vivid` is the
        // rest without the outline.
        {"INK",
         [] {
             blocky::ViewportEffects fx;
             fx.outline = 0.90f;
             fx.outlineDepthTolerance = 0.045f;
             fx.saturation = 0.72f;
             fx.contrast = 1.12f;
             fx.vignette = 0.35f;
             return fx;
         }()},
    };
    return kPresets[int(look)];
}

inline Look nextLook(Look look) { return Look((int(look) + 1) % int(Look::Count)); }

} // namespace game
