// Konstruct: the game built on BlockyEngine.
//
// The two names are not decoration. `blocky` is an engine -- a renderer, a
// physics solver, a codec, and no opinions about what anybody builds with
// them. Konstruct is one thing built with them, and it has opinions: these
// maps, this hotbar, that menu. Anything here that turns out to belong to the
// engine gets moved down into it rather than shared sideways.
//
// Not a scene. Every `scenes/<name>.cpp` is one file that renders one thing
// and exits, which is right for a picture and wrong for something you play:
// this has a world that persists, a player who moves through it, and pieces
// that had to be split across files before they could be understood. It is
// built as its own target, `konstruct`, from everything under `game/`.
//
// What it is made of, and where each piece came from:
//
//   - the **world** is generated here and meshed by the viewport's chunk
//     cache, which rebuilds only what an edit touched;
//   - the **textures** are generated too -- no Minecraft install is involved
//     at any point;
//   - the **player** is a kinematic character against the lattice, not a
//     rigid body, for the reasons in `engine/physics/character.hpp`;
//   - the **props and joints** come from the sandbox physics, and a script
//     may spawn them.
//
//   konstruct                 play
//   konstruct seed=42         a different world
//   konstruct extent=64       a smaller one, which generates faster
//   konstruct script=<file>   run a sandbox script alongside
//   konstruct snapshot <png>  one frame to a file, for a build script
#include "engine/assets/blocks/texture_gen.hpp"
#include "engine/entity/attach.hpp"
#include "engine/entity/entity.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/prop/prop_set.hpp"
#include "engine/render/gl/viewport.hpp"
#include "engine/scene/scene.hpp"
#include "engine/script/bindings.hpp"
#include "engine/script/script.hpp"

#include "game/animator.hpp"
#include "game/blocks.hpp"
#include "game/textures.hpp"
#include "game/icon.hpp"
#include "game/ragdoll.hpp"
#include "game/look.hpp"
#include "game/maps.hpp"
#include "game/menu.hpp"
#include "game/avatar.hpp"
#include "game/physgun.hpp"
#include "game/props.hpp"
#include "game/spawn.hpp"
#include "game/spawnmenu.hpp"
#include "game/player.hpp"
#include "game/terrain.hpp"
#include "game/vitals.hpp"

#include <chrono>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>

using namespace blocky;

int main(int argc, char** argv) {
    // A mode that writes files and exits, before any of the world is built.
    // It shares nothing with playing but the palette, which is exactly why it
    // is here: the textures it draws are this game's.
    if (argc > 1 && std::strcmp(argv[1], "textures") == 0) return game::runTextures(argc, argv);

    game::TerrainSettings terrain;
    std::string scriptPath;
    std::string snapshot;
    std::string skinPath = "assets/skins/charlie.png";
    int snapshotFrames = 1;
    float sensitivity = 0.09f;
    game::Player::View startView = game::Player::View::First;

    // Only useful to a snapshot, which has no mouse to look with. A GUI is
    // checked from a build script or it is not checked at all, and a walk
    // cycle photographed from a fixed angle is not a check of anything.
    float startYaw = 0.0f, startPitch = 0.0f;
    bool swingInSnapshot = false;
    int  ragdollsInSnapshot = 0;

    // Start with the tool already in hand. A snapshot has no mouse to hold a
    // trigger with, so it also grabs on its own -- same reason `swing` and
    // `ragdoll` exist, since a picture of an interface nobody pressed is a
    // picture of nothing. It works in a window too, where it is simply a way
    // to start with the physgun out instead of pressing F for it.
    bool startWithPhysgun = false;
    float thirdDistance = 0.0f;   // 0 keeps the animator default

    // A snapshot walks forward by default, because a still photograph of an
    // animation proves nothing. Anything being photographed *in front of* the
    // player wants the opposite -- walk away from a ragdoll for three seconds
    // and it is behind the camera.
    bool walkInSnapshot = true;

    // `map=` picks a world and skips the menu; without it the game comes up on
    // the title screen. Both matter: the menu is how a person starts, and the
    // argument is how a build script does, since a script cannot click.
    game::MapKind startMap = game::MapKind::Random;
    bool skipMenu = false;

    // `menu` or `menu=play` holds the menu open, which is the only way a
    // snapshot can photograph one: with no window there is nobody to click
    // past it. Same argument as `swing` -- an interface checked from a build
    // script has to be reachable from a build script.
    bool holdMenu = false;

    // Opens the shelf. The same argument as `menu`: a snapshot has nobody to
    // press Q, so an interface that can only be reached by hand cannot be
    // checked from a build script at all.
    bool openSpawnMenu = false;

    // Two more of the same kind. `props` puts one of everything on the ground
    // in front of the camera; `lift` takes the tool to a block and picks it
    // up. Both exist because a snapshot has no mouse, and a feature that can
    // only be reached with one is a feature no build script ever sees.
    bool propsInSnapshot = false;
    bool liftInSnapshot = false;
    game::Screen menuScreen = game::Screen::Title;
    game::Look startLook = game::Look::Vivid;

    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "seed=", 5) == 0) terrain.seed = uint32_t(std::atoi(argv[i] + 5));
        else if (std::strncmp(argv[i], "extent=", 7) == 0) terrain.extent = std::atoi(argv[i] + 7);
        else if (std::strncmp(argv[i], "script=", 7) == 0) scriptPath = argv[i] + 7;
        else if (std::strncmp(argv[i], "frames=", 7) == 0) snapshotFrames = std::atoi(argv[i] + 7);
        else if (std::strncmp(argv[i], "skin=", 5) == 0) skinPath = argv[i] + 5;
        else if (std::strcmp(argv[i], "snapshot") == 0 && i + 1 < argc) snapshot = argv[++i];
        else if (std::strcmp(argv[i], "third") == 0) startView = game::Player::View::ThirdBack;
        else if (std::strcmp(argv[i], "front") == 0) startView = game::Player::View::ThirdFront;
        else if (std::strcmp(argv[i], "swing") == 0) swingInSnapshot = true;
        else if (std::strcmp(argv[i], "still") == 0) walkInSnapshot = false;
        else if (std::strcmp(argv[i], "ragdoll") == 0) { ragdollsInSnapshot = 3; walkInSnapshot = false; }
        else if (std::strcmp(argv[i], "physgun") == 0) {
            // With nothing loose in the world the tool has nothing to point
            // at, so this brings its own: one ragdoll, dropped in front.
            startWithPhysgun = true;
            walkInSnapshot = false;
            if (ragdollsInSnapshot == 0) ragdollsInSnapshot = 1;
        }
        else if (std::strncmp(argv[i], "ragdoll=", 8) == 0)
            ragdollsInSnapshot = std::atoi(argv[i] + 8);
        else if (std::strncmp(argv[i], "map=", 4) == 0) {
            startMap = game::mapFromName(argv[i] + 4);
            skipMenu = true;
        }
        else if (std::strcmp(argv[i], "spawn") == 0) openSpawnMenu = true;
        else if (std::strcmp(argv[i], "props") == 0) { propsInSnapshot = true; walkInSnapshot = false; }
        else if (std::strcmp(argv[i], "lift") == 0) {
            liftInSnapshot = true;
            startWithPhysgun = true;
            walkInSnapshot = false;
            startPitch = -38.0f;   // down at the floor, where the blocks are
        }
        else if (std::strcmp(argv[i], "menu") == 0) { skipMenu = false; holdMenu = true; }
        else if (std::strncmp(argv[i], "menu=", 5) == 0) {
            skipMenu = false;
            holdMenu = true;
            const char* which = argv[i] + 5;
            if (std::strcmp(which, "play") == 0) menuScreen = game::Screen::Play;
            else if (std::strcmp(which, "paused") == 0) menuScreen = game::Screen::Paused;
        }
        else if (std::strncmp(argv[i], "yaw=", 4) == 0) startYaw = float(std::atof(argv[i] + 4));
        else if (std::strncmp(argv[i], "pitch=", 6) == 0) startPitch = float(std::atof(argv[i] + 6));
        else if (std::strncmp(argv[i], "dist=", 5) == 0) thirdDistance = float(std::atof(argv[i] + 5));
        else if (std::strncmp(argv[i], "sens=", 5) == 0) sensitivity = float(std::atof(argv[i] + 5));
        else if (std::strncmp(argv[i], "look=", 5) == 0) {
            // Preset names are stored in capitals because that is how the
            // interface draws them; nobody types them that way.
            const std::string want = argv[i] + 5;
            for (int k = 0; k < int(game::Look::Count); ++k) {
                std::string name = game::lookPreset(game::Look(k)).name;
                for (char& c : name) c = char(std::tolower(unsigned char(c)));
                if (name == want) startLook = game::Look(k);
            }
        }
    }

    // ------------------------------------------------------------- the world
    Scene scene(game::palette());
    BlockTextureLibrary textures;

    auto started = std::chrono::steady_clock::now();
    int texturedBlocks = game::generateTextures(textures, scene.world.registry());
    scene.blockTextures = &textures;

    std::printf("[game] %d generated block textures in %.2f s\n", texturedBlocks,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());

    // ------------------------------------------------------------ the player
    game::Player player;
    player.mouseSensitivity = sensitivity;
    player.view = startView;
    // The tool first, then the blocks. First because a sandbox is about the
    // things already in the world more than about adding to it, and because
    // slot one is the one a player finds without being told.
    using Slot = game::Player::Slot;
    player.hotbar = {Slot::tool(Slot::Kind::Physgun),
                     Slot::ofBlock(game::block::Stone),     Slot::ofBlock(game::block::Cobblestone),
                     Slot::ofBlock(game::block::OakPlanks), Slot::ofBlock(game::block::Bricks),
                     Slot::ofBlock(game::block::Glass),     Slot::ofBlock(game::block::Glowstone),
                     Slot::ofBlock(game::block::RedWool),   Slot::ofBlock(game::block::WhiteWool)};
    player.held = startWithPhysgun ? 0 : 1;   // a block unless asked otherwise

    // --------------------------------------------------------- the body
    // The player is a character rather than a floating camera, which means
    // there has to be something to look at. Failing to build it is not fatal:
    // without a body the game is exactly what it was before, which is a
    // playable thing and a better answer than refusing to start.
    game::Avatar avatar;
    std::string skinNote;
    if (game::buildAvatar(avatar, skinPath, &skinNote))
        std::printf("[game] %s, %zu boxes\n", skinNote.c_str(), avatar.world.boxes.size());
    else
        std::printf("[game] no body: %s\n", skinNote.c_str());

    game::Animator animator;
    if (thirdDistance > 0.0f) animator.settings.thirdPersonDistance = thirdDistance;
    game::HeldBlocks heldBlocks;
    EntitySet entities;

    // Ragdolls, in a deque rather than a vector: `RigidBody::collider` points
    // into each `Ragdoll`'s own collider list, so the ragdolls themselves must
    // never move in memory. A vector that grows would relocate every one of
    // them and leave the solver reading freed colliders.
    std::deque<game::Ragdoll> ragdolls;

    // ------------------------------------------------------ physics and script
    PhysicsWorld physics;
    PropSet props;

    // How many ragdolls the world keeps. Six bodies and five joints each, and
    // the broad phase is quadratic in the bodies, so an unbounded pile is a
    // game that gets slower the longer it is played -- the shape of
    // performance bug nobody reports, because no single session feels wrong.
    // Oldest first: whichever has been lying there longest is the one nobody
    // is looking at.
    const size_t kRagdollBudget = 8;

    // What there is to play with, and what has been taken out of it.
    //
    // The catalogue is built once and never resized after that, which is not
    // tidiness: every body spawned from it holds a bare pointer to one of its
    // colliders. Same rule as the ragdolls' deque, arrived at the same way.
    const std::vector<game::PropKind> propCatalogue = game::buildPropCatalogue();
    game::PropYard yard;
    yard.open(&propCatalogue);

    // Props are cheaper than ragdolls -- one body each rather than six -- so
    // the budget is higher, but it exists for the same reason: the broad
    // phase is quadratic, and a pile that only grows is a game that gets
    // slower the longer it is played.
    const size_t kPropBudget = 48;

    game::SpawnMenu spawnMenu;
    spawnMenu.open = openSpawnMenu;
    game::Physgun physgun;

    // The tool as voxels, built once. Held here rather than inside `Physgun`
    // because a prop's model must outlive every frame that placed it, and
    // this is where everything else of that kind lives.
    const game::PhysgunModel physgunModel = game::buildPhysgunModel();

    auto trimRagdolls = [&]() {
        while (ragdolls.size() > kRagdollBudget) {
            // Let go of it first if it is the thing being held. The physgun
            // checks every frame that what it has hold of is still alive, but
            // a slot is handed straight back out -- so between the two checks
            // the grip could land on a limb of whoever was spawned next.
            for (int part = 0; part < PartCount; ++part)
                if (physgun.held >= 0 && ragdolls.front().bodies[part] == physgun.held)
                    game::releasePhysgun(physgun);

            game::despawnRagdoll(ragdolls.front(), physics);
            ragdolls.pop_front();
        }
    };
    physgun.equipped = startWithPhysgun;

    script::Sandbox sandbox;
    sandbox.world = &scene.world;
    sandbox.physics = &physics;

    game::Vitals vitals;

    // Where a map put the player, kept so that dying can put them back there.
    // The map decides this once and nothing else may invent a second answer.
    Vec3 spawnPoint{};
    float spawnYaw = 0.0f;

    // Building a world and putting the player in it. One place, because the
    // menu does it again every time somebody starts a game -- and a second
    // copy of "generate, then find the ground, then face the right way" would
    // be the copy that forgets one of the three.
    auto loadWorld = [&](game::MapKind kind, uint32_t seed) {
        game::TerrainSettings settings = terrain;
        settings.seed = seed;

        const auto begun = std::chrono::steady_clock::now();
        scene.world.clear();

        Vec3 spawn{};
        float yaw = 0.0f;
        const uint64_t placed = game::buildMap(scene.world, kind, settings, spawn, yaw);

        // Nothing physical survives a new world. Bodies carry positions in a
        // world that no longer exists, so without this a ragdoll from the last
        // map is left hanging in the air over this one -- and the script's
        // handles, which are indices, would name whatever took their slots.
        physics.clear();
        ragdolls.clear();
        yard.clear();
        sandbox.spawned.clear();
        game::releasePhysgun(physgun);

        player.body = Character{};   // velocity and the ground flag start over
        player.body.yawDegrees = startYaw != 0.0f ? startYaw : yaw;
        player.body.pitchDegrees = startPitch;
        player.body.position = dropToGround(scene.world, player.settings, spawn);

        spawnPoint = spawn;
        spawnYaw = player.body.yawDegrees;
        game::reviveVitals(vitals);

        std::printf("[game] %s: %llu blocks in %zu chunks, %.2f s, spawn (%.1f, %.1f, %.1f)\n",
                    game::mapInfo(kind).id, static_cast<unsigned long long>(placed),
                    scene.world.chunkCount(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - begun).count(),
                    player.body.position.x, player.body.position.y, player.body.position.z);
    };

    script::Script program;
    program.onPrint = [](const std::string& text) { std::printf("[script] %s\n", text.c_str()); };
    program.onBind = [&sandbox](script::Interpreter& vm) {
        script::installSandboxLibrary(vm, sandbox);
    };

    std::string status;
    if (!scriptPath.empty()) {
        if (program.loadFile(scriptPath) && program.call("ready")) {
            std::printf("[game] script: %s\n", scriptPath.c_str());
        } else {
            status = program.lastError();
            std::printf("[game] %s\n", status.c_str());
        }
    }

    // -------------------------------------------------------------- the menu
    game::Menu menu;
    game::Hud hud;
    menu.seed = terrain.seed;

    // A snapshot cannot press F1, and the numbers are most of what a snapshot
    // is taken *for* when something is being checked rather than admired.
    hud.showDebug = !snapshot.empty();

    // The title screen orbits a real world, so there has to be one before
    // anything is chosen. A small one: it is scenery, and generating the full
    // extent to look at it from four hundred blocks away would be paying for
    // detail nobody can see.
    game::MapKind pending = game::MapKind::Random;
    bool buildPending = false;
    bool playing = !holdMenu && (skipMenu || !snapshot.empty());
    menu.screen = menuScreen;

    if (playing) {
        menu.open = false;
        loadWorld(startMap, terrain.seed);
    } else {
        // Scenery for the title screen. A small one by default -- it is looked
        // at from a hundred blocks away and generating the full extent would
        // be paying for detail nobody can see. `map=` alongside `menu` names
        // one instead, so a snapshot of the pause screen can show the map it
        // claims to be paused in.
        game::TerrainSettings backdrop = terrain;
        backdrop.extent = std::min(terrain.extent, 56);
        Vec3 spawn{};
        float yaw = 0.0f;
        game::buildMap(scene.world, skipMenu ? startMap : game::MapKind::Random, backdrop, spawn,
                       yaw);
    }

    // Read by the viewport every frame -- see `ViewportSettings::freeCursor`.
    bool freeCursor = !playing;

    // The look, likewise read every frame so F3 can change it mid-game.
    game::Look look = startLook;
    ViewportEffects effects = game::lookPreset(look).effects;

    // ------------------------------------------------------------- the loop
    const float kStep = 1.0f / 120.0f;
    double accumulator = 0.0;

    // Everything the renderer is shown, rebuilt from scratch each frame.
    //
    // Rebuilding rather than editing is the same call the viewport makes about
    // entity meshes: a pose is cheaper to redo than to ask whether it changed,
    // and there is no state left over to disagree with the world.
    auto rebuildActor = [&]() {
        props.clear();
        entities.clear();

        for (const script::Sandbox::Spawned& piece : sandbox.spawned) {
            if (piece.body < 0 || piece.body >= physics.bodyCount()) continue;
            const script::Sandbox::Model& model = *sandbox.models[piece.model];
            props.addTransformed(model.model, bodyToWorld(physics.body(piece.body)), piece.tint);
        }

        // What is in the hand: the tool if it is out, otherwise the block that
        // a right click would place. One or the other and never both -- the
        // hand is the one place the game says what it is about to do, and two
        // things in it would say two.
        const bool toolInHand = physgun.equipped && !physgunModel.voxels.empty();
        const VoxelModel* held = !toolInHand && avatar.ready()
                                     ? heldBlocks.modelFor(player.heldBlock(), &textures)
                                     : nullptr;

        physgun.muzzleKnown = false;

        if (avatar.ready() && player.thirdPerson()) {
            // Third person: the whole character, posed, with whatever is in
            // the fist hung off the arm joint by the same `attach` seam a
            // lantern or a pickaxe would use.
            Entity self;
            self.model = &avatar.world;
            self.skin = &avatar.skin;
            self.position = player.body.position;
            self.yawDegrees = animator.bodyYawDegrees;
            self.pose = game::playerPose(animator, player, false);

            // The eyes go on afterwards, because they belong to the avatar and
            // the pose does not know it has any. A character whose skin
            // defeated the scanner simply keeps the eyes its artist painted.
            game::poseEyes(animator, avatar.eyes, self.pose);

            if (held) {
                VoxelAttachment grip;
                grip.model = held;
                grip.joint = joint::RightArm;

                // Past the fist rather than in it: the hand is the end of the
                // arm box, and a block centred exactly there is half inside
                // the sleeve.
                grip.offset = jointTip(avatar.world, joint::RightArm) + Vec3{0.0f, -2.2f, 0.0f};

                // A block is held around its middle, unlike a tool, which is
                // held near the base -- so this is not `gripVoxel`.
                IVec3 dims = held->dims();
                grip.anchor = {float(dims.x) * 0.5f, float(dims.y) * 0.5f, float(dims.z) * 0.5f};
                grip.rotationDegrees = {8.0f, -24.0f, 10.0f};

                // Model pixels per voxel, and here one voxel is one whole
                // block: `fromWorld` captures a cell as a single voxel, not
                // as the sixteen-cube an item texture extrudes into.
                grip.voxelScale = 4.2f;
                addAttachment(props, self, grip);
            }

            if (toolInHand) {
                // The same seam, and the reason it was worth building: a tool
                // is held by its grip and points where the arm points, which
                // is exactly what `anchor` and `rotationDegrees` say.
                VoxelAttachment grip;
                grip.model = &physgunModel.voxels;
                grip.joint = joint::RightArm;
                grip.offset = jointTip(avatar.world, joint::RightArm) + Vec3{0.0f, -1.6f, 0.0f};
                grip.anchor = physgunModel.gripVoxel;

                // A joint's rest space is the entity's own, so the tool's -Z
                // is already forward and needs no turning to point that way.
                // What it needs is a wrist: a hand at the end of a hanging arm
                // holds a long thing at an angle, not level with the floor.
                grip.rotationDegrees = {-17.0f, -10.0f, 0.0f};

                // Model pixels per voxel: a little over half, which puts a
                // twenty-two-voxel gun at about three quarters of a block --
                // the size a two-handed thing looks in a blocky character's
                // fist.
                grip.voxelScale = 0.62f;
                addAttachment(props, self, grip);

                Mat4 toWorld;
                if (attachmentToWorld(self, grip, toWorld)) {
                    physgun.muzzleWorld = transformPoint(toWorld, physgunModel.muzzleVoxel);
                    physgun.muzzleKnown = true;
                }
            }

            entities.add(self);
        } else if (avatar.ready() && !avatar.arm.empty()) {
            // First person: the arm alone, as a prop in front of the camera.
            // A prop rather than an entity because it has to follow the head
            // through its pitch, which an entity's yaw cannot express.
            // A hand holding a long thing sits further out and lower than a
            // hand holding a block, or the back of the gun ends up level with
            // the player's eye. Both move together -- see `fistOffset`.
            const Vec3 fistOffset = toolInHand ? Vec3{-0.035f, -0.045f, 0.075f} : Vec3{};

            const Mat4 arm = game::viewArmTransform(animator, player, avatar.arm.dims(), fistOffset);
            props.addTransformed(&avatar.arm, arm);

            if (held) {
                props.addTransformed(
                    held, game::viewHeldTransform(animator, arm, avatar.arm.dims(), held->dims()));
            }

            if (toolInHand) {
                const Mat4 tool = game::viewToolTransform(animator, arm, avatar.arm.dims(),
                                                          physgunModel.gripVoxel);
                props.addTransformed(&physgunModel.voxels, tool);

                physgun.muzzleWorld = transformPoint(tool, physgunModel.muzzleVoxel);
                physgun.muzzleKnown = true;
            }
        }

        // Everything spawned: crates, barrels, and blocks that were lifted
        // out of the lattice and are furniture now.
        //
        // Asked to forget its dead first, and not as a precaution: a slot goes
        // straight back into circulation, so an entry that outlived its body
        // would draw a crate over whoever was spawned into its place. Same
        // rule the physgun follows about its grip.
        yard.forgetDead(physics);
        yard.draw(props, physics);

        // Ragdolls last, and always: they are props whatever the camera is
        // doing, because they are not the player and the view has no opinion
        // about them.
        for (const game::Ragdoll& doll : ragdolls)
            game::addRagdollProps(props, doll, physics, avatar);

        props.build();
        scene.props = props.empty() ? nullptr : &props;
        scene.entities = entities.empty() ? nullptr : &entities;
    };

    ViewportSettings viewport;
    viewport.title = "Konstruct";
    viewport.icon = game::buildIcon(64);
    viewport.hostCamera = true;
    viewport.statusText = &status;
    viewport.snapshotPath = snapshot;
    viewport.snapshotFrames = snapshotFrames;

    // The menu needs the pointer, and it needs Escape. Both are promises: the
    // cursor comes back the moment a game starts, and there is a way to quit
    // from every screen the key can reach.
    viewport.effects = &effects;
    viewport.freeCursor = &freeCursor;
    viewport.hostEscape = true;
    viewport.snapshotOverlay = true;

    viewport.onFrame = [&](const ViewportFrame& frame) {
        // A snapshot run has no window and no player at the keyboard, so it
        // advances by a fixed amount instead of by the wall clock -- the same
        // reason `scene_live` does, and the same reproducibility.
        float dt = frame.input ? frame.dt : kStep * 2.0f;

        // The world asked for on the previous frame. Generating one blocks for
        // a second or more, so the frame that decides only puts the message
        // up, and the frame after it does the work -- otherwise the window
        // freezes with the menu still on it and no explanation.
        if (buildPending) {
            buildPending = false;
            loadWorld(pending, menu.seed);
            menu.busy.clear();
            menu.open = false;
            playing = true;
            freeCursor = false;
            animator = game::Animator{};
            return true;
        }

        // ------------------------------------------------------------- menu
        // With no window there is nobody to drive it, so a held-open menu just
        // sits there and gets its picture taken.
        if (menu.open && !frame.input) {
            scene.camera = game::menuCamera(menu, scene.world, scene.camera.aspect);
            return true;
        }

        if (menu.open && frame.input) {
            const int w = std::max(1, frame.input->width());
            const int h = std::max(1, frame.input->height());

            switch (game::updateMenu(menu, *frame.input, w, h, dt)) {
                case game::MenuAction::StartGame:
                    pending = menu.chosen;
                    buildPending = true;
                    menu.busy = std::string("GENERATING ") + game::mapInfo(pending).name;
                    break;
                case game::MenuAction::Resume:
                    menu.open = false;
                    freeCursor = false;
                    break;
                case game::MenuAction::ToTitle:
                    // The world underneath stays exactly as it was. Coming
                    // back to the title over the map you just left is free,
                    // and throwing it away to show a fresh one would cost a
                    // second and buy nothing.
                    playing = false;
                    break;
                case game::MenuAction::Quit:
                    return false;
                case game::MenuAction::None:
                    break;
            }

            // Whoever still has the menu open has the pointer. Setting this
            // unconditionally would hand it back on the frame Resume closes
            // the menu, and the game would run with a loose cursor.
            freeCursor = menu.open || spawnMenu.open;
            if (menu.open) {
                scene.camera = playing ? scene.camera
                                       : game::menuCamera(menu, scene.world,
                                                          scene.camera.aspect);
                return true;
            }
        }

        // The shelf. Deliberately *before* the pause menu's Escape and outside
        // the block that stops when it is open, because unlike the pause menu
        // this one does not stop the world -- the fixed steps below keep
        // running, so what you spawn falls while you are still looking at the
        // grid. All it takes is the pointer.
        if (frame.input && !menu.open && spawnMenu.open) {
            const game::SpawnRequest request =
                game::updateSpawnMenu(spawnMenu, *frame.input, propCatalogue,
                                      frame.input->width(), frame.input->height(), dt);

            if (frame.input->keyPressed(key::Escape) || frame.input->keyPressed(key::Q)) {
                spawnMenu.open = false;
                freeCursor = false;
            } else if (request.made) {
                // In front of the eye and a little out, so it lands where the
                // player is looking rather than in their face. Dropped rather
                // than thrown: a thing that arrives moving is a thing you have
                // to chase.
                const Vec3 forward = player.body.forward();
                const Vec3 at = player.body.eye(player.settings) + forward * 3.2f;

                if (request.tile == game::ragdollTile(propCatalogue)) {
                    if (avatar.ready()) {
                        ragdolls.emplace_back();
                        if (game::spawnRagdoll(ragdolls.back(), physics, avatar, at,
                                               player.body.yawDegrees + 180.0f, forward)) {
                            trimRagdolls();
                        } else {
                            ragdolls.pop_back();
                        }
                    }
                } else if (request.tile >= 0) {
                    yard.spawn(physics, size_t(request.tile), at);
                    yard.trim(physics, kPropBudget, physgun.held);
                }
            }
        }

        if (frame.input && !menu.open && !spawnMenu.open) {
            // Escape goes back to the menu rather than closing the window --
            // which is why the viewport was asked to leave the key alone.
            if (frame.input->keyPressed(key::Escape)) {
                menu.screen = playing ? game::Screen::Paused : game::Screen::Title;
                menu.hovered = 0;
                menu.open = true;
                freeCursor = true;

                // Drop what the tool was holding. The menu stops the fixed
                // steps, so the servo stops running while the aim keeps
                // moving, and resuming would fling the held thing to wherever
                // the crosshair had got to.
                game::releasePhysgun(physgun);
                return true;
            }
            if (frame.input->keyPressed(key::F1)) hud.showDebug = !hud.showDebug;

            // The shelf. Ragdolls used to be a key of their own, which was
            // the same mistake the physgun made before it became a hotbar
            // slot: one thing you can spawn is a key, and two are a menu that
            // was never written. Now there is one way in and it has room.
            if (frame.input->keyPressed(key::Q)) {
                spawnMenu.open = !spawnMenu.open;
                spawnMenu.hovered = -1;
                freeCursor = spawnMenu.open;

                // Let go on the way in. The menu takes the pointer, so the aim
                // stops moving while the servo keeps running, and what was
                // held would hang in the air until the menu closed.
                if (spawnMenu.open) game::releasePhysgun(physgun);
                return true;
            }

            // Unfreezing is the physgun's, but it is not about what is in the
            // hand -- it undoes something already done to the world, and
            // making it wait until the tool is out would be a rule with no
            // reason behind it.
            if (frame.input->keyPressed(key::R)) {
                int thawed = game::unfreezeAll(physics);
                char note[48];
                std::snprintf(note, sizeof(note), "UNFROZE %d", thawed);
                hud.toast = note;
                hud.toastSeconds = 1.2f;
            }

            if (frame.input->keyPressed(key::F3)) {
                look = game::nextLook(look);
                hud.toast = game::lookPreset(look).name;
                hud.toastSeconds = 1.6f;
            }
        }
        if (hud.toastSeconds > 0.0f) hud.toastSeconds -= dt;

        if (frame.input && !spawnMenu.open) {
            // The tool is out when the tool is in the hand, and that is the
            // whole binding. A key that toggled it was invisible -- nothing
            // on the screen said whether it was pressed -- and it made a tool
            // a different kind of thing from a block, which it is not.
            physgun.equipped = player.holdingTool();

            // Holding E turns what the physgun has hold of, and the view must
            // then stay put: two things turning at once from one mouse
            // movement is how you lose track of both.
            if (!game::turnHeldObject(physgun, *frame.input, player))
                aimPlayer(player, *frame.input, dt);

            // The tool reads the mouse before anything else can: the eye and
            // the true look direction rather than the animated camera's, for
            // the same reason `updateReach` uses them.
            game::PhysgunReach reach;
            reach.world = &scene.world;
            reach.physics = &physics;
            reach.yard = &yard;
            reach.textures = scene.blockTextures;
            game::aimPhysgun(physgun, reach, *frame.input, player.body.eye(player.settings),
                             player.body.forward());

            // A lifted block is one more thing in the world, and the budget
            // covers it like any other.
            yard.trim(physics, kPropBudget, physgun.held);

            // Number keys and the wheel pick what is in the hand -- tool or
            // block, one row, no separate binding for the tool. The wheel is
            // what a hand already on the mouse reaches for, so the *held*
            // tool borrows it for push and pull and gives it back the moment
            // the trigger is released.
            const int slots = int(player.hotbar.size());
            for (int i = 0; i < slots && i < 9; ++i)
                if (frame.input->keyPressed('1' + i)) player.held = size_t(i);

            float wheel = frame.input->wheelDelta();
            if (physgun.holding()) wheel = 0.0f;
            if (wheel != 0.0f && slots > 0) {
                int next = (int(player.held) - int(wheel)) % slots;
                player.held = size_t((next + slots) % slots);
            }

            // Mouse speed is the one setting nobody can be given a good
            // default for, because it depends on the mouse. Adjustable
            // without a restart, and shown in the title so a value can be
            // written down once it feels right.
            if (frame.input->keyPressed(key::Minus))
                player.mouseSensitivity = std::max(0.01f, player.mouseSensitivity * 0.85f);
            if (frame.input->keyPressed(key::Plus))
                player.mouseSensitivity = std::min(1.0f, player.mouseSensitivity * 1.18f);

            // F5 cycles rather than toggles, in the order the genre uses:
            // eyes, over the shoulder, then facing you.
            if (frame.input->keyPressed(key::F5)) {
                player.view = game::Player::View((int(player.view) + 1) % 3);
            }
        }

        accumulator += double(dt);
        int steps = 0;
        while (accumulator >= double(kStep) && steps < 8) {
            if (frame.input && !spawnMenu.open) {
                // Whether the run key means anything is not a question about
                // input, so it is answered above the binding and passed down.
                const bool mayRun =
                    game::maySprint(vitals, player.body, frame.input->keyDown(key::Shift));
                game::stepPlayer(player, scene.world, *frame.input, mayRun, kStep);
            } else if (frame.input) {
                // The shelf is open. It does not stop the world -- what you
                // spawn has to be able to fall while you are still looking at
                // the grid -- but the keys belong to it, so the character is
                // stepped with no wish at all rather than left reading a W
                // that is not meant for walking.
                stepCharacter(player.body, scene.world, player.settings, Vec3{0.0f}, false, false,
                              kStep);
            } else {
                // A snapshot has nobody at the keyboard, so it walks forward
                // on its own. Without this the only thing a snapshot can
                // photograph is a character standing still, which proves the
                // model loaded and nothing about the animation.
                stepCharacter(player.body, scene.world, player.settings,
                              walkInSnapshot ? player.body.walkForward() : Vec3{0.0f}, false,
                              false, kStep);
            }

            // Before the solver, not after: the hold is a velocity the step
            // is then free to argue with, which is what keeps a carried crate
            // solid against a wall instead of dragging through it.
            game::stepPhysgun(physgun, physics, player.body.eye(player.settings),
                              player.body.forward(), kStep);

            physics.step(scene.world, kStep);

            if (program.loaded() && program.hasHandler("tick") &&
                !program.call("tick", {script::Value::num(double(kStep))})) {
                status = program.lastError();
                std::printf("[game] %s\n", status.c_str());
            }
            accumulator -= double(kStep);
            ++steps;
        }

        if (frame.input && spawnMenu.open) {
            // The click that chose a tile must not also break whatever the
            // crosshair happened to be over. Same rule the tool follows: one
            // thing owns the mouse buttons at a time.
            player.looking = false;
            player.edited = false;
            player.acted = false;
        } else if (frame.input && !physgun.equipped) {
            game::updateReach(player, scene.world, *frame.input, dt);

            // A swing per strike, including the ones that hit nothing. The
            // mining repeat drives this, so holding the button gives a cycle
            // rather than one twitch.
            if (player.acted) game::startSwing(animator);
        } else if (frame.input) {
            // A tool in hand owns the mouse buttons. `updateReach` is skipped
            // rather than called and ignored, because "held down" is one of
            // its inputs: called every frame with the trigger down it would
            // mine a tunnel through whatever the physgun failed to grab.
            player.looking = false;
            player.edited = false;
            player.acted = false;
        } else if (ragdollsInSnapshot > 0 && avatar.ready()) {
            // A snapshot has nobody to press the key, so it drops its own --
            // spaced out in front of the camera so they land in a heap rather
            // than inside each other.
            const Vec3 forward = player.body.forward();
            const Vec3 right = player.body.right();

            for (int i = 0; i < ragdollsInSnapshot; ++i) {
                const Vec3 at = player.body.eye(player.settings) + forward * 3.6f +
                                right * (float(i) - float(ragdollsInSnapshot - 1) * 0.5f) * 1.1f +
                                Vec3{0.0f, 1.4f, 0.0f};

                // Thrown rather than dropped, and not for the look of it: a
                // ragdoll released straight down onto flat ground lands on
                // its own feet and *stays standing*, because two legs under a
                // torso is a stable arrangement and nothing is pushing it
                // over. That is the physics being right and the photograph
                // being useless.
                ragdolls.emplace_back();
                if (!game::spawnRagdoll(ragdolls.back(), physics, avatar, at,
                                        player.body.yawDegrees + 180.0f, forward))
                    ragdolls.pop_back();
            }
            trimRagdolls();   // the budget applies to a snapshot as well
            std::printf("[game] dropped %zu ragdolls, %d bodies in the world\n",
                        ragdolls.size(), physics.liveBodyCount());
            ragdollsInSnapshot = 0;
        } else if (propsInSnapshot) {
            // One of each, spread along the line of sight so they land beside
            // one another instead of inside one another.
            const Vec3 forward = player.body.forward();
            const Vec3 right = normalize(cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
            const Vec3 eye = player.body.eye(player.settings);
            for (size_t i = 0; i < propCatalogue.size(); ++i) {
                const float offset = (float(i) - float(propCatalogue.size() - 1) * 0.5f) * 1.6f;
                yard.spawn(physics, i, eye + forward * 5.0f + right * offset + Vec3{0.0f, 1.2f, 0.0f});
            }
            std::printf("[game] spawned %zu props, %d bodies in the world\n", yard.count(),
                        physics.liveBodyCount());
            propsInSnapshot = false;
        } else if (startWithPhysgun && physgun.equipped && !physgun.holding()) {
            // Retried every frame rather than done once: there may be nothing
            // in front of the camera yet on the frame the tool comes out, and
            // whatever is there is probably still falling.
            game::PhysgunReach reach;
            reach.world = &scene.world;
            reach.physics = &physics;
            reach.yard = &yard;
            reach.textures = scene.blockTextures;
            const uint64_t before = scene.world.blockCount();
            if (game::grabPhysgun(physgun, reach, player.body.eye(player.settings),
                                  player.body.forward()) &&
                liftInSnapshot) {
                std::printf("[game] lifted a block: world went from %llu to %llu, %d bodies\n",
                            (unsigned long long)before, (unsigned long long)scene.world.blockCount(),
                            physics.liveBodyCount());

                // Look back up and pull it in. Held where it was taken from, a
                // lifted block sits in its own hole and photographs exactly
                // like a block nobody touched -- so the shot has to move it
                // before it shows anything. Which is the other half of the
                // demonstration: the servo carries it there, it does not
                // teleport.
                player.body.pitchDegrees = 2.0f;
                physgun.distance = 3.0f;
                liftInSnapshot = false;
            }
        } else if (swingInSnapshot && !animator.swinging) {
            // A snapshot has no mouse to click with, so it swings on its own.
            // Same reason it walks on its own: an animation photographed at
            // rest is a photograph of nothing.
            game::startSwing(animator);
        }

        // What it has cost so far. On the frame rather than the fixed step,
        // like the animation: none of it is a solver, and the fall it judges
        // is sampled on the same edge the animator bends the knees on.
        game::stepVitals(vitals, player.body, player.settings,
                         frame.input && frame.input->keyDown(key::Shift), dt);

        if (vitals.died) {
            // No death screen and no game over: a sandbox is a place to try
            // things, and the price of a mistake is the walk back.
            player.body = Character{};
            player.body.yawDegrees = spawnYaw;
            player.body.position = dropToGround(scene.world, player.settings, spawnPoint);
            game::reviveVitals(vitals);
            game::releasePhysgun(physgun);

            animator = game::Animator{};
            hud.toast = "RESPAWNED";
            hud.toastSeconds = 1.6f;
        }

        // Animation runs on the frame, not on the fixed step: the bob and the
        // swing belong to what is drawn, and stepping them at 120 Hz while
        // drawing at 60 makes them stutter whenever the two disagree.

        game::animate(animator, player, dt);

        // The eyes look at the camera, so they have to be aimed before the
        // camera is built and the camera has to come from where it was last
        // frame. One frame of lag on a glance is not a thing anybody can see;
        // the alternative is building the camera twice.
        game::aimEyes(animator, player, scene.camera.position, dt);

        rebuildActor();

        // The camera is the player's, which is what `hostCamera` is for: the
        // viewport stops flying and draws from here.
        scene.camera =
            game::playerCamera(animator, player, scene.world, 74.0f, scene.camera.aspect);

        // The look, plus the medium the look is seen through. Rebuilt every
        // frame rather than only when F3 is pressed, because being underwater
        // is not a choice the player made and must be able to change without
        // one: put your head under, the frame goes blue; come up, it does not.
        effects = game::lookPreset(look).effects;
        if (player.body.eyeFluid == game::block::Water) {
            effects.tint = {0.46f, 0.72f, 0.90f};
            effects.fogColour = {0.035f, 0.115f, 0.185f};
            effects.fogDensity = 0.085f;   // a few blocks of visibility
            effects.saturation *= 0.86f;
            effects.vignette = std::max(effects.vignette, 0.55f);
        } else if (player.body.eyeFluid == game::block::Lava) {
            // Which fluid, not merely that there is one. A head in lava used
            // to turn the frame the same calm blue as a head in a lake, which
            // is the single most misleading thing an interface can do.
            effects.tint = {1.0f, 0.42f, 0.16f};
            effects.fogColour = {0.55f, 0.10f, 0.02f};
            effects.fogDensity = 3.5f;     // you see the block you are inside
            effects.vignette = std::max(effects.vignette, 0.8f);
        }

        // The numbers used to be the whole interface and lived in the window
        // title, because there was nowhere else to put them. Now they are one
        // line behind F1, where a debug readout belongs.
        char line[288];
        const char* aiming = player.looking ? "BLOCK" : "SKY";
        const char* motion = player.body.swimming  ? "  SWIM"
                             : player.body.onGround ? ""
                                                    : "  FALLING";

        // The physics numbers belong on this line too. Awake against asleep
        // is the one figure that says whether a sandbox is still costing what
        // it should: bodies accumulate, and a pile that never sleeps is the
        // difference between a world you can keep playing in and one that
        // slows down while you watch.
        const StepStats& step = physics.stats();
        std::snprintf(line, sizeof(line),
                      "%.0f %.0f %.0f  AIM %s  %s%s  HP %.1f  ST %.1f  BODIES %d (%d up)  "
                      "CONTACTS %d  STEP %.2f ms",
                      player.body.position.x, player.body.position.y, player.body.position.z,
                      aiming, game::Player::viewName(player.view), motion, double(vitals.health),
                      double(vitals.stamina), physics.liveBodyCount(), step.awakeBodies,
                      step.contacts, step.seconds * 1000.0);
        hud.debugLine = line;

        // The line over the row. Rebuilt every frame rather than on a key
        // press, because what the buttons do changes with what the tool is
        // doing and not only with which slot is chosen.
        if (!physgun.equipped) {
            hud.hint = "LMB BREAK   RMB PLACE   Q SPAWN";
        } else if (physgun.holding()) {
            hud.hint = "WHEEL PUSH/PULL   E+MOUSE TURN   RMB FREEZE   RELEASE TO DROP";
        } else if (physgun.aimingAtBlock) {
            // Named differently on purpose. Lifting takes the block out of the
            // world and does not put it back, and a trigger that quietly
            // dismantles a wall should have said which of the two it was about
            // to do.
            hud.hint = "PHYSGUN   LMB LIFT THIS BLOCK   RMB FREEZE   R UNFREEZE ALL";
        } else {
            hud.hint = "PHYSGUN   LMB HOLD TO GRAB   RMB FREEZE   R UNFREEZE ALL";
        }
        return true;
    };

    viewport.onOverlay = [&](Overlay& ui) {
        if (!menu.open) {
            drawHud(hud, ui, player, vitals, scene.world.registry());
            game::drawPhysgun(physgun, ui, scene.camera);
        }
        // Over the HUD, under the pause menu: the shelf is part of playing,
        // and the pause menu is what playing stops for.
        game::drawSpawnMenu(spawnMenu, ui, propCatalogue);
        if (menu.open) game::drawMenu(menu, ui);
    };

    if (snapshot.empty()) {
        std::printf(
            "\ncontrols\n"
            "  mouse         look\n"
            "  W A S D       walk, Shift to run, Space to jump\n"
            "  left click    break a block, held down to keep mining\n"
            "  right click   place the held block\n"
            "  1 - 8, wheel  choose what to place\n"
            "  Q             spawn menu: props and ragdolls\n"
            "  F             physgun: hold left to carry, wheel to push and\n"
            "                pull, E and the mouse to turn, right click to\n"
            "                freeze, R to unfreeze everything\n"
            "  F5            first person / third person\n"
            "  F3            look, F1 debug line\n"
            "  - and =       mouse speed\n"
            "  Esc           menu\n\n");
    }
    return runViewport(scene, viewport);
}
