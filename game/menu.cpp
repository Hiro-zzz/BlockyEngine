#include "game/menu.hpp"

#include "engine/core/image.hpp"
#include "engine/platform/window.hpp"
#include "engine/world/block.hpp"

#include "game/player.hpp"
#include "game/vitals.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace game {

using namespace blocky;

namespace {

// ------------------------------------------------------------------ palette
//
// Source, around 2004. The look is worth naming precisely because it is made
// of restraint rather than of decoration:
//
//   - the front page has **no panel at all**. Plain text sits on the darkened
//     world in the lower left, and the only ornament is a rule under the
//     wordmark. Half-Life 2 shipped like that and it still reads as confident
//     rather than unfinished;
//   - everything that is a *dialog* -- picking a world, pausing -- gets a VGUI
//     box instead: one flat fill, a one-pixel border, a title strip across the
//     top, and the hint along the bottom;
//   - one warm accent on cool grey, and nothing else coloured.
//
// Written in sRGB, because the overlay draws after the tone curve and what is
// picked here is what appears.
const Rgba kInk = rgb8(206, 210, 212);
const Rgba kInkDim = rgb8(126, 132, 136);
const Rgba kAccent = rgb8(255, 170, 40);
const Rgba kDialog = rgb8(26, 30, 33, 0.93f);
const Rgba kDialogBar = rgb8(46, 52, 56, 0.96f);
const Rgba kDialogEdge = rgb8(88, 96, 100);
const Rgba kSeparator = rgb8(62, 68, 72);
const Rgba kRowHot = rgb8(52, 59, 64, 0.9f);

// One number the whole layout is built from, so the menu is the same shape on
// any window. Rounded to an integer because the font is a bitmap and half a
// texel is a blurry letter.
float uiScale(int height) {
    return std::max(1.0f, std::round(float(height) / 640.0f));
}

struct Row {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(Vec2 p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
};

// The dialog box the Play and Paused screens sit in. Returned whole so the
// rows, the border and the title strip all come from one arithmetic.
struct Dialog {
    float x = 0, y = 0, w = 0, h = 0;
    float bar = 0;   // height of the title strip
    float pad = 0;
};

Dialog dialogRect(const Menu& menu, int width, int height) {
    const float s = uiScale(height);
    Dialog d;
    d.pad = 12.0f * s;
    d.bar = 26.0f * s;
    d.w = std::min(float(width) * 0.66f, 560.0f * s);

    const int rows = menu.screen == Screen::Play ? kMapCount + 1 : 3;
    const float rowH = (menu.screen == Screen::Play ? 54.0f : 34.0f) * s;
    const float hint = 22.0f * s;

    d.h = d.bar + d.pad * 2.0f + float(rows) * rowH + hint;
    d.x = std::round((float(width) - d.w) * 0.5f);
    d.y = std::round((float(height) - d.h) * 0.5f);
    return d;
}

// The rows of the current screen, in order. One function, used by both the
// hit test and the drawing -- two copies of a layout agree until somebody
// moves a button, and then the thing you click is not the thing you see.
int rowCount(const Menu& menu) {
    switch (menu.screen) {
        case Screen::Title:  return 2;             // play, quit
        case Screen::Play:   return kMapCount + 1; // the maps, then back
        case Screen::Paused: return 3;             // resume, title, quit
    }
    return 0;
}

Row rowRect(const Menu& menu, int index, int width, int height) {
    const float s = uiScale(height);
    Row row;

    if (menu.screen == Screen::Title) {
        // Lower left, no box. The hit area runs wider than the word so the
        // pointer does not have to be on the letters -- a list of bare text is
        // only pleasant to use if the target is bigger than the ink.
        const float margin = 44.0f * s;
        row.h = 30.0f * s;
        row.w = std::min(float(width) * 0.42f, 340.0f * s);
        row.x = margin;
        row.y = std::round(float(height) * 0.58f) + float(index) * (row.h + 6.0f * s);
        return row;
    }

    const Dialog d = dialogRect(menu, width, height);
    row.h = (menu.screen == Screen::Play ? 54.0f : 34.0f) * s;
    row.w = d.w - d.pad * 2.0f;
    row.x = d.x + d.pad;
    row.y = d.y + d.bar + d.pad + float(index) * row.h;
    return row;
}

const char* rowLabel(const Menu& menu, int index) {
    switch (menu.screen) {
        case Screen::Title:
            return index == 0 ? "SINGLEPLAYER" : "QUIT";
        case Screen::Play:
            return index < kMapCount ? mapInfo(kMapOrder[index]).name : "BACK";
        case Screen::Paused:
            return index == 0 ? "RESUME" : (index == 1 ? "MAIN MENU" : "QUIT");
    }
    return "";
}

// The wordmark: KONSTRUCT with the K set apart.
//
// A bitmap font gives one weight and one size, so the K cannot be made bolder
// -- it has to be made *bigger*, and then given something to sit in. What it
// sits in is the game: a block, a bracket and a rule, which is the whole
// vocabulary of the thing being named. The rest of the word runs smaller
// beside it, which is what makes the K read as a mark rather than as a letter
// that came out wrong.
//
// Returns the width it drew, so a caller can line something up under it.
float drawWordmark(Overlay& ui, float x, float y, float s) {
    const float kScale = std::max(3.0f, std::round(9.0f * s));
    const float wordScale = std::max(2.0f, std::round(5.0f * s));

    const float kW = ui.measure("K", kScale);
    const float kH = ui.lineHeight(kScale);
    const float pad = 7.0f * s;

    // The block the K stands in: a filled square with a lighter edge, the same
    // shape as the hotbar slots and the dialog borders.
    const float box = kH + pad * 2.0f;
    ui.rect(x, y, box, box, rgba(0.0f, 0.0f, 0.0f, 0.55f));
    ui.frame(x, y, box, box, std::max(1.0f, 2.0f * s), kAccent);
    ui.textShadowed(x + (box - kW) * 0.5f, y + pad, "K", kScale, kAccent);

    // A thicker stub on the lower right of the frame, so the square reads as
    // built rather than drawn. The overlay only ever adds, so this is a
    // heavier stroke laid over the border rather than a notch cut out of it --
    // there is no carving to be had from a painter that cannot subtract.
    ui.rect(x + box - 4.0f * s, y + box - 14.0f * s, 4.0f * s, 14.0f * s, kAccent);
    ui.rect(x + box - 14.0f * s, y + box - 4.0f * s, 14.0f * s, 4.0f * s, kAccent);

    // ONSTRUCT beside it, sitting on the same baseline as the K's feet.
    const float wordX = x + box + 12.0f * s;
    const float wordY = y + box - pad - ui.lineHeight(wordScale);
    ui.textShadowed(wordX, wordY, "ONSTRUCT", wordScale, kInk);

    const float wordW = ui.measure("ONSTRUCT", wordScale);
    const float total = (wordX + wordW) - x;

    // The rule runs under the word only, stopping short of the block: a line
    // through all of it would turn the mark back into one row of text.
    ui.rect(wordX, y + box - pad + 4.0f * s, wordW, 2.0f * s, kAccent);

    const float smallScale = std::max(1.0f, std::round(1.0f * s));
    ui.textShadowed(wordX, y + box + 4.0f * s, "A VOXEL SANDBOX  -  BLOCKYENGINE", smallScale,
                    kInkDim);
    return total;
}

// A bare item on the front page: no fill, no border, and a caret instead of a
// highlight. The caret is the whole selection cue and it is enough, because
// the eye is already on the column.
void drawTitleItem(Overlay& ui, const Row& row, const std::string& label, bool hot, float s) {
    const float scale = std::max(1.0f, std::round(2.0f * s));
    const float y = row.y + (row.h - ui.lineHeight(scale)) * 0.5f;
    const float indent = 16.0f * s;

    if (hot) ui.textShadowed(row.x, y, ">", scale, kAccent);
    ui.textShadowed(row.x + indent, y, label, scale, hot ? kAccent : kInk);
}

// A row inside a dialog: a fill only when the pointer is on it, a hairline
// under it, and the blurb in the dim colour.
void drawDialogRow(Overlay& ui, const Row& row, const std::string& label, const std::string& blurb,
                   bool hot, bool last, float s) {
    if (hot) ui.rect(row.x, row.y, row.w, row.h, kRowHot);
    if (!last) ui.rect(row.x, row.y + row.h - 1.0f, row.w, 1.0f, kSeparator);

    const float textScale = std::max(1.0f, std::round(blurb.empty() ? 1.5f : 2.0f) * s);

    // One left edge for every row, blurb or not, with the caret in the gutter
    // beside it. Indenting only the rows that lack a blurb would put `BACK` a
    // few pixels further in than the map names above it -- the kind of thing
    // nobody can name and everybody can see.
    const float gutter = 10.0f * s;
    const float text = gutter + 14.0f * s;

    const float blurbScale = std::max(1.0f, std::round(1.0f * s));
    const float block = blurb.empty()
                            ? ui.lineHeight(textScale)
                            : ui.lineHeight(textScale) + 5.0f * s + ui.lineHeight(blurbScale);
    const float y = row.y + (row.h - block) * 0.5f;

    if (hot) {
        ui.textShadowed(row.x + gutter, y + (blurb.empty() ? 0.0f : 2.0f * s), ">", textScale,
                        kAccent);
    }
    ui.textShadowed(row.x + text, y, label, textScale, hot ? kAccent : kInk);
    if (!blurb.empty()) {
        ui.textShadowed(row.x + text, y + ui.lineHeight(textScale) + 5.0f * s, blurb, blurbScale,
                        kInkDim);
    }
}

} // namespace

MenuAction updateMenu(Menu& menu, const Window& window, int width, int height, float dt) {
    menu.orbit += dt * 3.4f;
    if (menu.orbit >= 360.0f) menu.orbit -= 360.0f;
    menu.wantsCursor = true;

    const int rows = rowCount(menu);
    if (rows <= 0) return MenuAction::None;
    menu.hovered = std::max(0, std::min(rows - 1, menu.hovered));

    // The pointer moves the selection, and so do the keys. Both, rather than
    // either: a menu that only answers the mouse is annoying to anyone whose
    // hands are already on the keyboard, and one that only answers keys looks
    // broken to everyone else.
    const Vec2 pointer = window.mousePosition();
    for (int i = 0; i < rows; ++i) {
        if (rowRect(menu, i, width, height).contains(pointer)) menu.hovered = i;
    }

    if (window.keyPressed(key::Down) || window.keyPressed(key::S))
        menu.hovered = (menu.hovered + 1) % rows;
    if (window.keyPressed(key::Up) || window.keyPressed(key::W))
        menu.hovered = (menu.hovered + rows - 1) % rows;

    // A new seed, on the row it belongs to. Cheap, and the alternative -- a
    // text field -- is a whole input system for a number nobody types twice.
    if (menu.screen == Screen::Play && window.keyPressed(key::R)) {
        menu.seed = menu.seed * 1664525u + 1013904223u;
        return MenuAction::None;
    }

    const bool clicked = window.mouseLeftPressed() &&
                         rowRect(menu, menu.hovered, width, height).contains(pointer);
    const bool confirmed = clicked || window.keyPressed(key::Enter) || window.keyPressed(key::Space);

    if (window.keyPressed(key::Escape)) {
        switch (menu.screen) {
            case Screen::Title:  return MenuAction::Quit;
            case Screen::Play:   menu.screen = Screen::Title; menu.hovered = 0; return MenuAction::None;
            case Screen::Paused: return MenuAction::Resume;
        }
    }

    if (!confirmed) return MenuAction::None;

    switch (menu.screen) {
        case Screen::Title:
            if (menu.hovered == 0) {
                menu.screen = Screen::Play;
                menu.hovered = 0;
                return MenuAction::None;
            }
            return MenuAction::Quit;

        case Screen::Play:
            if (menu.hovered >= kMapCount) {
                menu.screen = Screen::Title;
                menu.hovered = 0;
                return MenuAction::None;
            }
            menu.chosen = kMapOrder[menu.hovered];
            return MenuAction::StartGame;

        case Screen::Paused:
            if (menu.hovered == 0) return MenuAction::Resume;
            if (menu.hovered == 1) {
                menu.screen = Screen::Title;
                menu.hovered = 0;
                return MenuAction::ToTitle;
            }
            return MenuAction::Quit;
    }
    return MenuAction::None;
}

void drawMenu(const Menu& menu, Overlay& ui) {
    const int width = ui.width(), height = ui.height();
    const float s = uiScale(height);
    const float margin = 44.0f * s;

    if (menu.screen == Screen::Title) {
        // Darkened downwards, where the words are, rather than dimmed flat.
        // A Source title page keeps its background a background: the picture
        // stays legible at the top and the text stays legible at the bottom.
        ui.verticalGradient(0, 0, float(width), float(height), rgba(0.0f, 0.0f, 0.0f, 0.22f),
                            rgba(0.0f, 0.0f, 0.0f, 0.74f));

        drawWordmark(ui, margin, std::round(float(height) * 0.34f), s);

        const int rows = rowCount(menu);
        for (int i = 0; i < rows; ++i) {
            drawTitleItem(ui, rowRect(menu, i, width, height), rowLabel(menu, i),
                          i == menu.hovered, s);
        }

        // Bottom right, where a Source build has always put its version.
        const float smallScale = std::max(1.0f, std::round(1.0f * s));
        const std::string build = "KONSTRUCT  BUILD 0.9  -  BLOCKYENGINE";
        ui.textShadowed(float(width) - ui.measure(build, smallScale) - margin,
                        float(height) - ui.lineHeight(smallScale) - 16.0f * s, build, smallScale,
                        kInkDim);
    } else {
        // Anything that is a dialog gets a box. The world dims behind it, the
        // fill is flat, the border is one pixel, and the title sits in a strip
        // across the top -- VGUI, more or less exactly.
        ui.rect(0, 0, float(width), float(height), rgba(0.0f, 0.0f, 0.0f, 0.58f));

        const Dialog d = dialogRect(menu, width, height);
        ui.rect(d.x, d.y, d.w, d.h, kDialog);
        ui.rect(d.x, d.y, d.w, d.bar, kDialogBar);
        ui.rect(d.x, d.y + d.bar, d.w, std::max(1.0f, s), kDialogEdge);
        ui.frame(d.x, d.y, d.w, d.h, std::max(1.0f, s), kDialogEdge);

        const float barScale = std::max(1.0f, std::round(1.5f * s));
        const char* title = menu.screen == Screen::Play ? "NEW GAME" : "PAUSED";
        ui.textShadowed(d.x + 10.0f * s, d.y + (d.bar - ui.lineHeight(barScale)) * 0.5f, title,
                        barScale, kInk);

        const int rows = rowCount(menu);
        for (int i = 0; i < rows; ++i) {
            std::string blurb;
            if (menu.screen == Screen::Play && i < kMapCount) {
                blurb = mapInfo(kMapOrder[i]).blurb;
                if (kMapOrder[i] == MapKind::Random) {
                    char line[96];
                    std::snprintf(line, sizeof(line), "SEED %u  -  R FOR ANOTHER",
                                  unsigned(menu.seed));
                    blurb = line;
                }
            }
            drawDialogRow(ui, rowRect(menu, i, width, height), rowLabel(menu, i), blurb,
                          i == menu.hovered, i == rows - 1, s);
        }

        const float hintScale = std::max(1.0f, std::round(1.0f * s));
        ui.textShadowed(d.x + 10.0f * s, d.y + d.h - ui.lineHeight(hintScale) - 6.0f * s,
                        "ARROWS OR MOUSE   ENTER TO CHOOSE   ESC TO GO BACK", hintScale, kInkDim);
    }

    // Building a world blocks the frame that does it, so the message has to be
    // put up on the frame before. Without it the window simply stops for a
    // second and a half, which reads as a crash rather than as work.
    if (!menu.busy.empty()) {
        const float scale = std::max(1.0f, std::round(2.0f * s));
        const float w = ui.measure(menu.busy, scale) + 44.0f * s;
        const float h = ui.lineHeight(scale) + 30.0f * s;
        const float x = std::round((float(width) - w) * 0.5f);
        const float y = std::round((float(height) - h) * 0.5f);

        ui.rect(0, 0, float(width), float(height), rgba(0.0f, 0.0f, 0.0f, 0.62f));
        ui.rect(x, y, w, h, kDialog);
        ui.frame(x, y, w, h, std::max(1.0f, s), kDialogEdge);
        ui.rect(x, y, w, std::max(2.0f, 2.0f * s), kAccent);
        ui.textShadowed(x + 22.0f * s, y + 16.0f * s, menu.busy, scale, kInk);
    }
}

Camera menuCamera(const Menu& menu, const World& world, float aspect) {
    Camera camera;
    camera.projection = Camera::Projection::Perspective;
    camera.fovY = radians(58.0f);
    camera.aspect = aspect;

    Vec3 centre{0.0f, 20.0f, 0.0f};
    float radius = 60.0f;
    if (world.hasBlocks()) {
        const Vec3 lo = toVec3(world.minBlock());
        const Vec3 hi = toVec3(world.maxBlock()) + Vec3{1.0f, 1.0f, 1.0f};
        centre = (lo + hi) * 0.5f;

        // Half the horizontal diagonal, which frames the world rather than its
        // height -- a tall thin world should not push the camera into orbit.
        radius = 0.5f * std::sqrt((hi.x - lo.x) * (hi.x - lo.x) + (hi.z - lo.z) * (hi.z - lo.z));
    }

    // Low and close rather than high and far. From above a voxel world reads
    // as a map; from just over the treetops it reads as somewhere to stand,
    // which is what a title screen is trying to say.
    const float angle = radians(menu.orbit);
    const float distance = std::max(24.0f, radius * 0.78f);

    Vec3 eye = centre + Vec3{std::sin(angle) * distance, radius * 0.34f + 14.0f,
                             std::cos(angle) * distance};
    camera.lookAt(eye, centre + Vec3{0.0f, radius * 0.06f, 0.0f});
    return camera;
}

// ------------------------------------------------------------------ the bars
//
// Health, stamina, and -- only when it matters -- breath.
//
// Bars rather than a row of hearts, and not for taste. A row of icons says
// "twenty health" and makes twenty a number the player is meant to count; a
// bar says "most of it left", which is the only thing anybody reads off it
// mid-fall. There is also no icon to draw: this game generates its textures,
// and a heart would be the one asset it had to be given.
//
// The stack sits directly over the hand row and left-aligned with it, because
// three things in one corner read as one panel and three things in three
// corners read as clutter.
void drawBar(Overlay& ui, float x, float y, float w, float h, float fraction, Rgba fill,
             Rgba back, float s) {
    ui.rect(x, y, w, h, back);
    const float filled = std::round(w * saturate(fraction));
    if (filled > 0.0f) ui.rect(x, y, filled, h, fill);
    ui.frame(x, y, w, h, std::max(1.0f, s), rgb8(12, 14, 16, 0.85f));
}

// The tool in a slot: the claw, its core, and the body. Not the voxel model --
// the overlay draws rectangles, and a picture of a thing is a different job
// from the thing.
void drawToolIcon(Overlay& ui, float x, float y, float size, Rgba body, Rgba glow) {
    const float u = size / 8.0f;
    ui.rect(x + u * 2.0f, y + u * 3.0f, u * 5.0f, u * 2.0f, body);   // receiver
    ui.rect(x + u * 3.0f, y + u * 5.0f, u * 2.0f, u * 2.0f, body);   // grip
    ui.rect(x + u * 1.0f, y + u * 1.5f, u * 1.0f, u * 1.5f, body);   // upper prong
    ui.rect(x + u * 1.0f, y + u * 5.0f, u * 1.0f, u * 1.5f, body);   // lower prong
    ui.rect(x + u * 1.0f, y + u * 3.5f, u * 1.5f, u * 1.0f, glow);   // the core
}

void drawHud(const Hud& hud, Overlay& ui, const Player& player, const Vitals& vitals,
             const BlockRegistry& registry) {
    const int width = ui.width(), height = ui.height();
    const float s = uiScale(height);
    const float flash = saturate(vitals.hurtFlash);

    // ------------------------------------------------------- the hurt wash
    // A red band round the edge of the frame when something has just hurt.
    // Four rects rather than a shader, and not as a compromise: the effect is
    // at the edges by definition, and the middle of the screen is where the
    // player is trying to look.
    //
    // **First**, under everything else. Drawn last it covered the bottom of
    // the screen, which is where the health bar is -- so the one moment the
    // bar exists for was the one moment it could not be read.
    if (flash > 0.01f) {
        const Rgba wash = rgba(0.55f, 0.05f, 0.05f, 0.42f * flash);
        const float band = std::round(float(height) * 0.12f);
        ui.rect(0.0f, 0.0f, float(width), band, wash);
        ui.rect(0.0f, float(height) - band, float(width), band, wash);
        ui.rect(0.0f, 0.0f, band, float(height), wash);
        ui.rect(float(width) - band, 0.0f, band, float(height), wash);
    }

    // ---------------------------------------------------------- crosshair
    // Two bars with a hole in the middle. The hole matters: a solid cross
    // hides the one texel you are aiming at.
    const float arm = 9.0f * s, thick = std::max(1.0f, 2.0f * s), gap = 3.0f * s;
    const float cx = std::round(float(width) * 0.5f), cy = std::round(float(height) * 0.5f);
    const Rgba mark = rgba(1.0f, 1.0f, 1.0f, 0.75f);

    ui.rect(cx - gap - arm, cy - thick * 0.5f, arm, thick, mark);
    ui.rect(cx + gap, cy - thick * 0.5f, arm, thick, mark);
    ui.rect(cx - thick * 0.5f, cy - gap - arm, thick, arm, mark);
    ui.rect(cx - thick * 0.5f, cy + gap, thick, arm, mark);

    // ------------------------------------------------------------ the hand
    // One row, and everything that can be in the hand is in it: a block slot
    // shows the block's own colour, the tool slot shows the tool. Which one is
    // chosen is the only state a player has to read off this, so it is the
    // only thing drawn in the accent colour.
    const int count = int(player.hotbar.size());
    const float slot = 44.0f * s;
    const float pad = 4.0f * s;
    const float total = count > 0 ? float(count) * slot + float(count - 1) * pad : 0.0f;
    const float rowX = std::round((float(width) - total) * 0.5f);
    const float rowY = std::round(float(height) - slot - 16.0f * s);

    for (int i = 0; i < count; ++i) {
        const float x = rowX + float(i) * (slot + pad);
        const Player::Slot& entry = player.hotbar[size_t(i)];
        const bool chosen = size_t(i) == player.held % size_t(count);

        ui.rect(x, rowY, slot, slot, rgba(0.05f, 0.06f, 0.09f, 0.72f));

        if (entry.kind == Player::Slot::Kind::Block) {
            // The block's own colour, converted out of the linear light it is
            // stored in. Writing the linear value straight would show every
            // block much darker than it renders.
            const Vec3 linear = registry[entry.block].albedo;
            const Rgba swatch{linearToSrgb(linear.x), linearToSrgb(linear.y),
                              linearToSrgb(linear.z), 1.0f};
            ui.rect(x + 6.0f * s, rowY + 6.0f * s, slot - 12.0f * s, slot - 12.0f * s, swatch);
        } else {
            drawToolIcon(ui, x + 5.0f * s, rowY + 5.0f * s, slot - 10.0f * s,
                         chosen ? rgb8(196, 206, 216) : rgb8(132, 142, 152), rgb8(120, 226, 255));
        }

        ui.frame(x, rowY, slot, slot, std::max(1.0f, chosen ? 2.0f * s : s),
                 chosen ? kAccent : kDialogEdge);

        char digit[2] = {char('1' + i), 0};
        ui.textShadowed(x + 4.0f * s, rowY + 2.0f * s, digit, std::max(1.0f, s), kInkDim);
    }

    // ------------------------------------------------------------ the bars
    const float barW = std::max(total * 0.52f, 140.0f * s);
    const float barH = std::max(3.0f, 8.0f * s);
    const float barGap = std::max(1.0f, 3.0f * s);
    float barY = rowY - barGap - barH;

    // Breath from the bottom up, and only while it means something: a bar
    // that is full every time you can see it is a bar nobody reads.
    if (vitals.breath < 0.999f) {
        drawBar(ui, rowX, barY, barW, barH, vitals.breath, rgb8(120, 206, 255),
                rgb8(18, 26, 34, 0.8f), s);
        barY -= barGap + barH;
    }

    drawBar(ui, rowX, barY, barW, barH, vitals.staminaFraction(), rgb8(196, 168, 72),
            rgb8(30, 26, 16, 0.8f), s);
    barY -= barGap + barH;

    // Health flashes towards white on a hit. A bar falling is easy to miss
    // when whatever made it fall is filling the screen.
    const Rgba healthy = rgb8(206, 66, 58);
    const Rgba flashed = rgb8(255, 236, 226);
    const Rgba health{lerp(healthy.r, flashed.r, flash), lerp(healthy.g, flashed.g, flash),
                      lerp(healthy.b, flashed.b, flash), 1.0f};

    drawBar(ui, rowX, barY, barW, barH, vitals.healthFraction(), health, rgb8(34, 16, 16, 0.8f), s);

    // ------------------------------------------------------------- the hint
    // What the buttons do with what is in the hand, directly above the bars:
    // everything the player has to read is one column in one place, and
    // *under* the row is off the bottom of the screen.
    if (!hud.hint.empty()) {
        const float scale = std::max(1.0f, std::round(s));
        ui.textShadowed(rowX, barY - barGap - ui.lineHeight(scale) - 2.0f * s, hud.hint, scale,
                        kInkDim);
    }

    if (hud.showDebug && !hud.debugLine.empty()) {
        const float scale = std::max(1.0f, std::round(1.5f * s));
        ui.textShadowed(10.0f * s, 10.0f * s, hud.debugLine, scale, kInk);
    }

    if (hud.toastSeconds > 0.0f && !hud.toast.empty()) {
        // Fades over the last half second rather than vanishing, because a
        // word that disappears between two frames reads as a glitch.
        const float fade = std::min(1.0f, hud.toastSeconds / 0.5f);
        const float scale = std::max(1.0f, std::round(2.5f * s));
        const Rgba colour{kAccent.r, kAccent.g, kAccent.b, fade};

        ui.textShadowed((float(width) - ui.measure(hud.toast, scale)) * 0.5f,
                        float(height) * 0.66f, hud.toast, scale, colour);
    }

}
} // namespace game
