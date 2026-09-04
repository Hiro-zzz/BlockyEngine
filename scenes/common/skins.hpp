#pragma once
// The people in the scenes, drawn in code.
//
// Same arrangement as `palette.hpp` next door, and for the same reason: the
// engine owns the verbs (`engine/assets/entity/skin_gen.hpp`) and knows about
// nobody, while the cast is content and belongs to whoever is staging it.
//
// They exist because the alternative was worse in two ways at once. These
// scenes used to point at PNG files in one particular Downloads folder, so a
// fresh clone rendered nothing at all from half the showcase -- and the path
// carried a username through into the source. Drawing them means the whole of
// `scenes/` runs from `build.cmd` on any machine with no asset anywhere, which
// is the line the block textures and the game's own fallback skin already hold.
//
// **They are stand-ins, not artwork.** A look here is a handful of colours and
// four structural numbers, which is enough for figures that read as different
// people at the distance these scenes shoot from, and nothing like enough to
// be a character design. Every scene still takes a real skin as an argument --
// `scene_portrait.exe path/to/skin.png` -- and that is the interesting path;
// this is the one that has to work before anybody has downloaded anything.
#include "engine/assets/entity/skin.hpp"
#include "engine/assets/entity/skin_gen.hpp"
#include "engine/core/file.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace skins {

using namespace blocky;
using RGBA = ImageU8::RGBA;

// One person, as far as a sheet of 64x64 texels is concerned.
struct Look {
    const char* name = "";

    RGBA skin{224, 176, 138, 255};
    RGBA hair{74, 52, 38, 255};
    RGBA shirt{78, 116, 158, 255};
    RGBA sleeve{62, 96, 134, 255};
    RGBA trousers{62, 62, 78, 255};
    RGBA shoe{44, 40, 40, 255};

    // A band across the torso, which is the cheapest thing that makes two
    // figures in the same silhouette tell apart at forty blocks.
    RGBA stripe{0, 0, 0, 0};
    int  stripeFromTop = 4;
    int  stripeRows = 0;      // zero draws none

    int  fringeRows = 2;      // hair over the forehead and the temples
    int  cuffRows = 3;        // where the sleeve ends and the hand begins
    int  shoeRows = 3;
    int  noise = 6;
};

// ------------------------------------------------------------------ the cast
//
// The first four keep the names `film.cpp` gave them when they were files, so
// the shot list still reads the way it was written.

inline const Look& crew() {   // the one on the night shift
    static const Look look{"crew",
                           {206, 156, 120, 255}, {38, 32, 30, 255},
                           {46, 62, 84, 255},    {40, 54, 74, 255},
                           {48, 50, 58, 255},    {34, 32, 32, 255},
                           {170, 168, 160, 255}, 3, 2, 3, 3, 3, 7};
    return look;
}

inline const Look& clerk() {  // the one who comes in last
    static const Look look{"clerk",
                           {238, 200, 168, 255}, {150, 88, 46, 255},
                           {198, 198, 190, 255}, {186, 186, 178, 255},
                           {74, 66, 60, 255},    {58, 44, 36, 255},
                           {0, 0, 0, 0},         4, 0, 3, 2, 3, 5};
    return look;
}

inline const Look& stripe() { // the one already sitting there
    static const Look look{"stripe",
                           {198, 148, 112, 255}, {28, 26, 26, 255},
                           {222, 210, 188, 255}, {212, 200, 178, 255},
                           {58, 62, 72, 255},    {40, 38, 38, 255},
                           {168, 62, 54, 255},   4, 3, 4, 4, 3, 5};
    return look;
}

inline const Look& white() {  // the one at the end of the road
    static const Look look{"white",
                           {226, 186, 154, 255}, {216, 212, 204, 255},
                           {228, 228, 224, 255}, {216, 216, 212, 255},
                           {206, 202, 196, 255}, {150, 146, 140, 255},
                           {0, 0, 0, 0},         4, 0, 4, 3, 3, 4};
    return look;
}

inline const Look& rust() {
    static const Look look{"rust",
                           {214, 164, 126, 255}, {62, 40, 30, 255},
                           {168, 88, 48, 255},   {152, 78, 42, 255},
                           {54, 58, 70, 255},    {42, 36, 32, 255},
                           {0, 0, 0, 0},         4, 0, 2, 3, 3, 7};
    return look;
}

inline const Look& moss() {
    static const Look look{"moss",
                           {190, 140, 106, 255}, {24, 22, 24, 255},
                           {86, 102, 62, 255},   {74, 90, 54, 255},
                           {62, 56, 48, 255},    {38, 34, 30, 255},
                           {52, 62, 40, 255},    5, 2, 4, 3, 3, 6};
    return look;
}

inline const Look& slate() {
    static const Look look{"slate",
                           {232, 194, 162, 255}, {198, 164, 96, 255},
                           {92, 108, 128, 255},  {80, 96, 116, 255},
                           {66, 64, 72, 255},    {46, 44, 44, 255},
                           {0, 0, 0, 0},         4, 0, 3, 3, 3, 6};
    return look;
}

inline const Look& ochre() {
    static const Look look{"ochre",
                           {204, 158, 122, 255}, {158, 156, 150, 255},
                           {186, 146, 66, 255},  {172, 134, 58, 255},
                           {70, 62, 58, 255},    {48, 42, 38, 255},
                           {96, 74, 40, 255},    6, 2, 2, 3, 3, 6};
    return look;
}

// Everything above, in one place, so `byName` and any contact sheet stay in
// step with the list rather than carrying a second copy of it.
inline const std::vector<const Look*>& all() {
    static const std::vector<const Look*> list = {&crew(),  &clerk(), &stripe(), &white(),
                                                  &rust(),  &moss(),  &slate(),  &ochre()};
    return list;
}

inline const Look& byName(const std::string& name) {
    for (const Look* look : all())
        if (name == look->name) return *look;
    return crew();
}

// ---------------------------------------------------------------- painting
inline bool draw(Skin& skin, const Look& look, std::string* error = nullptr) {
    using namespace blocky::skingen;

    ImageU8 sheet;
    if (!begin(skin, sheet, error)) return false;

    part(sheet, skin, PartHead, look.skin, look.noise);
    part(sheet, skin, PartBody, look.shirt, look.noise + 1);
    part(sheet, skin, PartRightArm, look.sleeve, look.noise + 1);
    part(sheet, skin, PartLeftArm, look.sleeve, look.noise + 1);
    part(sheet, skin, PartRightLeg, look.trousers, look.noise);
    part(sheet, skin, PartLeftLeg, look.trousers, look.noise);

    // Hair over the crown and down the back, then a fringe as far round the
    // sides as the look asks for. `cap` does the top face too, which is the
    // one nobody remembers and everybody notices from a low angle.
    cap(sheet, skin, PartHead, look.hair, look.fringeRows);
    fill(sheet, skin.faceRect(PartHead, LayerBase, SkinFaceBack), look.hair);

    faceFeatures(sheet, skin.faceRect(PartHead, LayerBase, SkinFaceFront),
                 {236, 236, 236, 255}, {60, 82, 140, 255}, {120, 76, 66, 255});

    if (look.stripeRows > 0)
        band(sheet, skin, PartBody, look.stripe, look.stripeFromTop, look.stripeRows);

    limbEnd(sheet, skin, PartRightArm, look.skin, look.cuffRows);
    limbEnd(sheet, skin, PartLeftArm, look.skin, look.cuffRows);
    limbEnd(sheet, skin, PartRightLeg, look.shoe, look.shoeRows);
    limbEnd(sheet, skin, PartLeftLeg, look.shoe, look.shoeRows);

    return finish(skin, sheet);
}

// What every character scene calls.
//
// A path wins when there is one, so the interesting workflow -- point a scene
// at a real skin and see it -- is untouched and is still the default way to
// use these. Falling back rather than failing is the difference between a
// clone that renders the showcase and a clone that prints nine paths it cannot
// open. `note` says which of the two happened, because a picture of the wrong
// person is not obviously wrong.
inline bool loadOrDraw(Skin& skin, const std::string& path, const std::string& lookName,
                       std::string* note = nullptr) {
    if (!path.empty()) {
        std::vector<uint8_t> bytes;
        std::string error;
        if (readFileBytes(path, bytes, &error) &&
            skin.loadFromPng(bytes.data(), bytes.size(), &error)) {
            if (note) *note = "skin " + path;
            return true;
        }
        if (note) *note = "could not read " + path + " (" + error + "), drawing '" + lookName + "'";
        std::string ignored;
        return draw(skin, byName(lookName), &ignored);
    }

    if (note) *note = "skin '" + lookName + "' drawn in code";
    std::string error;
    if (draw(skin, byName(lookName), &error)) return true;
    if (note) *note = "could not draw '" + lookName + "': " + error;
    return false;
}

} // namespace skins
