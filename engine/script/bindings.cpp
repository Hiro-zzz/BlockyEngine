#include "engine/script/bindings.hpp"

#include "engine/physics/constraint.hpp"

namespace blocky::script {
namespace {

Sandbox& hostOf(Interpreter& vm) {
    Sandbox* sandbox = static_cast<Sandbox*>(vm.userData);
    if (!sandbox) vm.fail("this script has no world bound to it");
    return *sandbox;
}

// A body handle turned back into an index, with every way it can be wrong
// reported rather than trusted. A script that stored a handle and then had
// its bodies cleared is a normal thing to happen during a reload.
int bodyIndex(Interpreter& vm, const Value& value, const char* who) {
    if (value.type != Type::Handle || value.handleKind != kHandleBody)
        vm.fail(std::string(who) + " wants a body, got " + typeName(value.type));

    Sandbox& host = hostOf(vm);
    if (!host.physics) vm.fail(std::string(who) + ": there is no physics world");

    int index = int(value.handle);
    if (index < 0 || index >= host.physics->bodyCount())
        vm.fail(std::string(who) + ": this body no longer exists");
    return index;
}

IVec3 asBlockPos(Interpreter& vm, const Value& value, const char* who, size_t index) {
    Vec3 position = vm.asVec(value, who, index);
    return floorToInt(position);
}

// --------------------------------------------------------------- the world
Value nativeSetBlock(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "setblock");
    Sandbox& host = hostOf(vm);
    if (!host.world) vm.fail("setblock: there is no world");

    IVec3 position = asBlockPos(vm, args[0], "setblock", 0);
    double id = vm.asNumber(args[1], "setblock", 1);
    if (id < 0 || id >= double(host.world->registry().size())) vm.fail("setblock: no such block");

    host.world->set(position, BlockId(id));
    return Value::nil();
}

Value nativeGetBlock(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "getblock");
    Sandbox& host = hostOf(vm);
    if (!host.world) vm.fail("getblock: there is no world");

    return Value::num(double(host.world->get(asBlockPos(vm, args[0], "getblock", 0))));
}

Value nativeFillBox(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 3, "fillbox");
    Sandbox& host = hostOf(vm);
    if (!host.world) vm.fail("fillbox: there is no world");

    IVec3 low = asBlockPos(vm, args[0], "fillbox", 0);
    IVec3 high = asBlockPos(vm, args[1], "fillbox", 1);
    double id = vm.asNumber(args[2], "fillbox", 2);
    if (id < 0 || id >= double(host.world->registry().size())) vm.fail("fillbox: no such block");

    // A script that mistypes a coordinate can otherwise ask for a fill of a
    // billion blocks and appear to hang. This is the same protection the step
    // budget gives loops, for the one native that can do unbounded work.
    IVec3 lo = minv(low, high), hi = maxv(low, high);
    double volume = (double(hi.x - lo.x) + 1) * (double(hi.y - lo.y) + 1) * (double(hi.z - lo.z) + 1);
    if (volume > 4000000.0) vm.fail("fillbox: that is " + std::to_string(int64_t(volume)) +
                                    " blocks -- refusing, in case a coordinate is wrong");

    host.world->fillBox(lo, hi, BlockId(id));
    return Value::num(volume);
}

// ------------------------------------------------------------------ bodies
Value nativeSpawn(Interpreter& vm, const std::vector<Value>& args) {
    if (args.size() != 2 && args.size() != 3) vm.fail("spawn takes a name, a position, and an optional tint");

    Sandbox& host = hostOf(vm);
    if (!host.physics) vm.fail("spawn: there is no physics world");

    const std::string& name = vm.asString(args[0], "spawn", 0);
    int model = host.findModel(name);
    if (model < 0) vm.fail("spawn: there is no model called '" + name + "'");

    Vec3 position = vm.asVec(args[1], "spawn", 1);
    Vec3 tint{1.0f, 1.0f, 1.0f};
    if (args.size() == 3) tint = vm.asVec(args[2], "spawn", 2);

    const Sandbox::Model& entry = *host.models[size_t(model)];

    RigidBody body;
    setMassFromCollider(body, entry.collider, entry.density);
    body.voxelSize = entry.voxelSize;
    body.position = position;
    body.friction = 0.65f;
    body.restitution = 0.05f;

    int index = host.physics->add(body);
    host.spawned.push_back({index, size_t(model), tint});
    return Value::handleOf(kHandleBody, index);
}

// -------------------------------------------------------------- characters
Entity& entityAt(Interpreter& vm, const Value& value, const char* who) {
    if (value.type != Type::Handle || value.handleKind != kHandleEntity)
        vm.fail(std::string(who) + " wants an entity, got " + typeName(value.type));

    Sandbox& host = hostOf(vm);
    int index = int(value.handle);
    if (index < 0 || size_t(index) >= host.entities.size())
        vm.fail(std::string(who) + ": this entity no longer exists");
    return host.entities[size_t(index)];
}

// Joints by name rather than by number. A script writing `pose(p, "head", ...)`
// says what it means; one writing `pose(p, 2, ...)` is a bug waiting for the
// skeleton to grow a joint.
int jointByName(Interpreter& vm, const std::string& name) {
    struct Named { const char* name; int index; };
    static const Named kJoints[] = {
        {"root", joint::Root},         {"body", joint::Body},
        {"head", joint::Head},         {"rightarm", joint::RightArm},
        {"leftarm", joint::LeftArm},   {"rightleg", joint::RightLeg},
        {"leftleg", joint::LeftLeg},
    };
    for (const Named& entry : kJoints)
        if (name == entry.name) return entry.index;

    vm.fail("'" + name +
            "' is not a joint -- try root, body, head, rightarm, leftarm, rightleg, leftleg");
}

Value nativeSpawnEntity(Interpreter& vm, const std::vector<Value>& args) {
    if (args.size() < 2 || args.size() > 3)
        vm.fail("spawnentity takes a skin, a position, and an optional yaw");

    Sandbox& host = hostOf(vm);
    const std::string& name = vm.asString(args[0], "spawnentity", 0);
    int skin = host.findSkin(name);
    if (skin < 0) vm.fail("spawnentity: there is no skin called '" + name + "'");

    Entity entity;
    entity.model = &host.skins[size_t(skin)]->model;
    entity.skin = host.skins[size_t(skin)]->skin;
    entity.position = vm.asVec(args[1], "spawnentity", 1);
    if (args.size() == 3) entity.yawDegrees = float(vm.asNumber(args[2], "spawnentity", 2));

    host.entities.push_back(entity);
    return Value::handleOf(kHandleEntity, int64_t(host.entities.size()) - 1);
}

Value nativePlace(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "place");
    entityAt(vm, args[0], "place").position = vm.asVec(args[1], "place", 1);
    return args[0];
}

Value nativeFace(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "face");
    entityAt(vm, args[0], "face").yawDegrees = float(vm.asNumber(args[1], "face", 1));
    return args[0];
}

Value nativeFacing(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "facing");
    return Value::num(double(entityAt(vm, args[0], "facing").yawDegrees));
}

Value nativePose(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 5, "pose");
    Entity& entity = entityAt(vm, args[0], "pose");
    int index = jointByName(vm, vm.asString(args[1], "pose", 1));

    entity.pose[index].rotationDegrees = {float(vm.asNumber(args[2], "pose", 2)),
                                          float(vm.asNumber(args[3], "pose", 3)),
                                          float(vm.asNumber(args[4], "pose", 4))};
    return args[0];
}

Value nativeStride(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "stride");
    Entity& entity = entityAt(vm, args[0], "stride");
    entity.pose = Pose::striding(float(vm.asNumber(args[1], "stride", 1)));
    return args[0];
}

Value nativeWave(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "wave");
    Entity& entity = entityAt(vm, args[0], "wave");
    entity.pose = Pose::waving(float(vm.asNumber(args[1], "wave", 1)));
    return args[0];
}

Value nativeRest(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "rest");
    entityAt(vm, args[0], "rest").pose = Pose::standing();
    return args[0];
}

Value nativeEntityCount(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 0, "entitycount");
    return Value::num(double(hostOf(vm).entities.size()));
}

// `pos` answers for a body or for an entity. One verb for "where is it"
// reads better than two, and the handle already says which is which.
Value nativePos(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "pos");
    if (args[0].type == Type::Handle && args[0].handleKind == kHandleEntity)
        return Value::vector(entityAt(vm, args[0], "pos").position);
    return Value::vector(hostOf(vm).physics->body(bodyIndex(vm, args[0], "pos")).position);
}

Value nativeVelocity(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "velocity");
    return Value::vector(hostOf(vm).physics->body(bodyIndex(vm, args[0], "velocity")).linearVelocity);
}

Value nativeSetVelocity(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "setvelocity");
    RigidBody& body = hostOf(vm).physics->body(bodyIndex(vm, args[0], "setvelocity"));
    body.linearVelocity = vm.asVec(args[1], "setvelocity", 1);
    body.wake();
    return Value::nil();
}

Value nativeSetSpin(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "setspin");
    RigidBody& body = hostOf(vm).physics->body(bodyIndex(vm, args[0], "setspin"));
    body.angularVelocity = vm.asVec(args[1], "setspin", 1);
    body.wake();
    return Value::nil();
}

Value nativeTurn(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 3, "turn");
    RigidBody& body = hostOf(vm).physics->body(bodyIndex(vm, args[0], "turn"));
    Vec3 axis = vm.asVec(args[1], "turn", 1);
    double degrees = vm.asNumber(args[2], "turn", 2);
    if (lengthSq(axis) < 1e-12f) vm.fail("turn: the axis has no direction");

    body.orientation = normalize(Quat::axisAngle(axis, radians(float(degrees))) * body.orientation);
    body.wake();
    return Value::nil();
}

Value nativeAsleep(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "asleep");
    return Value::boolean_(hostOf(vm).physics->body(bodyIndex(vm, args[0], "asleep")).sleeping);
}

Value nativeBodyCount(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 0, "bodycount");
    return Value::num(double(hostOf(vm).physics->bodyCount()));
}

// ------------------------------------------------------------------ joints
// `nil` for the second body means the world, which is how anything gets
// anchored to something that does not move.
int optionalBodyIndex(Interpreter& vm, const Value& value, const char* who) {
    if (value.isNil()) return -1;
    return bodyIndex(vm, value, who);
}

Value nativeWeld(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 3, "weld");
    Sandbox& host = hostOf(vm);

    int a = bodyIndex(vm, args[0], "weld");
    int b = optionalBodyIndex(vm, args[1], "weld");
    Vec3 point = vm.asVec(args[2], "weld", 2);

    const RigidBody& bodyB = host.physics->body(b >= 0 ? b : a);
    int joint = host.physics->addConstraint(weldAt(host.physics->body(a), a, bodyB, b, point));
    return Value::handleOf(kHandleJoint, joint);
}

Value nativeBallSocket(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 3, "ballsocket");
    Sandbox& host = hostOf(vm);

    int a = bodyIndex(vm, args[0], "ballsocket");
    int b = optionalBodyIndex(vm, args[1], "ballsocket");
    Vec3 point = vm.asVec(args[2], "ballsocket", 2);

    const RigidBody& bodyB = host.physics->body(b >= 0 ? b : a);
    int joint = host.physics->addConstraint(ballSocketAt(host.physics->body(a), a, bodyB, b, point));
    return Value::handleOf(kHandleJoint, joint);
}

Value nativeHinge(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 4, "hinge");
    Sandbox& host = hostOf(vm);

    int a = bodyIndex(vm, args[0], "hinge");
    int b = optionalBodyIndex(vm, args[1], "hinge");
    Vec3 point = vm.asVec(args[2], "hinge", 2);
    Vec3 axis = vm.asVec(args[3], "hinge", 3);
    if (lengthSq(axis) < 1e-12f) vm.fail("hinge: the axle has no direction");

    const RigidBody& bodyB = host.physics->body(b >= 0 ? b : a);
    int joint = host.physics->addConstraint(hingeAt(host.physics->body(a), a, bodyB, b, point, axis));
    return Value::handleOf(kHandleJoint, joint);
}

Value nativeRope(Interpreter& vm, const std::vector<Value>& args) {
    if (args.size() != 4 && args.size() != 5) vm.fail("rope takes two bodies, two points, and an optional length");
    Sandbox& host = hostOf(vm);

    int a = bodyIndex(vm, args[0], "rope");
    int b = optionalBodyIndex(vm, args[1], "rope");
    Vec3 pointA = vm.asVec(args[2], "rope", 2);
    Vec3 pointB = vm.asVec(args[3], "rope", 3);
    float length_ = args.size() == 5 ? float(vm.asNumber(args[4], "rope", 4)) : 0.0f;

    const RigidBody& bodyB = host.physics->body(b >= 0 ? b : a);
    int joint = host.physics->addConstraint(
        ropeBetween(host.physics->body(a), a, bodyB, b, pointA, pointB, length_));
    return Value::handleOf(kHandleJoint, joint);
}

int jointIndex(Interpreter& vm, const Value& value, const char* who) {
    if (value.type != Type::Handle || value.handleKind != kHandleJoint)
        vm.fail(std::string(who) + " wants a joint, got " + typeName(value.type));

    Sandbox& host = hostOf(vm);
    if (!host.physics) vm.fail(std::string(who) + ": there is no physics world");

    int index = int(value.handle);
    if (index < 0 || index >= host.physics->constraintCount())
        vm.fail(std::string(who) + ": this joint no longer exists");
    return index;
}

Value nativeRod(Interpreter& vm, const std::vector<Value>& args) {
    if (args.size() != 4 && args.size() != 5) vm.fail("rod takes two bodies, two points, and an optional length");
    Sandbox& host = hostOf(vm);

    int a = bodyIndex(vm, args[0], "rod");
    int b = optionalBodyIndex(vm, args[1], "rod");
    Vec3 pointA = vm.asVec(args[2], "rod", 2);
    Vec3 pointB = vm.asVec(args[3], "rod", 3);
    float length_ = args.size() == 5 ? float(vm.asNumber(args[4], "rod", 4)) : 0.0f;

    const RigidBody& bodyB = host.physics->body(b >= 0 ? b : a);
    int joint = host.physics->addConstraint(
        rodBetween(host.physics->body(a), a, bodyB, b, pointA, pointB, length_));
    return Value::handleOf(kHandleJoint, joint);
}

Value nativeBreakable(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "breakable");
    hostOf(vm).physics->constraint(jointIndex(vm, args[0], "breakable")).breakImpulse =
        float(vm.asNumber(args[1], "breakable", 1));
    return args[0];
}

Value nativeBroken(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "broken");
    return Value::boolean_(hostOf(vm).physics->constraint(jointIndex(vm, args[0], "broken")).broken);
}

Value nativeDamp(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "damp");
    double amount = vm.asNumber(args[1], "damp", 1);
    if (amount < 0.0) vm.fail("damp: friction cannot be negative");

    hostOf(vm).physics->constraint(jointIndex(vm, args[0], "damp")).friction = float(amount);
    return args[0];
}

Value nativeMotor(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 3, "motor");
    double torque = vm.asNumber(args[2], "motor", 2);
    if (torque < 0.0) vm.fail("motor: the torque limit cannot be negative");

    Constraint& joint = hostOf(vm).physics->constraint(jointIndex(vm, args[0], "motor"));
    if (joint.kind != ConstraintKind::Hinge) vm.fail("motor: only a hinge has an axle to drive");

    joint.motor = true;
    joint.motorSpeed = float(vm.asNumber(args[1], "motor", 1));
    joint.motorTorque = float(torque);
    return args[0];
}

Value nativeUnmotor(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "unmotor");
    hostOf(vm).physics->constraint(jointIndex(vm, args[0], "unmotor")).motor = false;
    return args[0];
}

Value nativeSetLength(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 2, "setlength");
    double wanted = vm.asNumber(args[1], "setlength", 1);
    if (wanted < 0.0) vm.fail("setlength: a length cannot be negative");

    Constraint& joint = hostOf(vm).physics->constraint(jointIndex(vm, args[0], "setlength"));
    if (joint.kind != ConstraintKind::Distance) vm.fail("setlength: only a rope or a rod has a length");

    joint.distance = float(wanted);
    return args[0];
}

Value nativeJointLength(Interpreter& vm, const std::vector<Value>& args) {
    vm.expectArgCount(args, 1, "jointlength");
    const Constraint& joint = hostOf(vm).physics->constraint(jointIndex(vm, args[0], "jointlength"));
    if (joint.kind != ConstraintKind::Distance) vm.fail("jointlength: only a rope or a rod has a length");
    return Value::num(double(joint.distance));
}

}  // namespace

void Sandbox::addModel(const std::string& name, const VoxelModel& model, float voxelSize,
                       float density) {
    auto entry = std::make_unique<Model>();
    entry->name = name;
    entry->model = &model;
    entry->voxelSize = voxelSize;
    entry->density = density;
    entry->collider = buildCollider(model, voxelSize);
    models.push_back(std::move(entry));
}

int Sandbox::findModel(const std::string& name) const {
    for (size_t i = 0; i < models.size(); ++i)
        if (models[i]->name == name) return int(i);
    return -1;
}

void Sandbox::addSkin(const std::string& name, const Skin& skin) {
    auto entry = std::make_unique<SkinEntry>();
    entry->name = name;
    entry->skin = &skin;
    // The model is built from the skin because slim and classic arms differ,
    // and the difference is detected from the image rather than declared.
    entry->model = buildPlayerModel(skin);
    skins.push_back(std::move(entry));
}

int Sandbox::findSkin(const std::string& name) const {
    for (size_t i = 0; i < skins.size(); ++i)
        if (skins[i]->name == name) return int(i);
    return -1;
}

void installSandboxLibrary(Interpreter& vm, Sandbox& sandbox) {
    vm.userData = &sandbox;

    vm.defineNative("setblock", nativeSetBlock);
    vm.defineNative("getblock", nativeGetBlock);
    vm.defineNative("fillbox", nativeFillBox);

    vm.defineNative("spawn", nativeSpawn);
    vm.defineNative("pos", nativePos);
    vm.defineNative("velocity", nativeVelocity);
    vm.defineNative("setvelocity", nativeSetVelocity);
    vm.defineNative("setspin", nativeSetSpin);
    vm.defineNative("turn", nativeTurn);
    vm.defineNative("asleep", nativeAsleep);
    vm.defineNative("bodycount", nativeBodyCount);

    vm.defineNative("spawnentity", nativeSpawnEntity);
    vm.defineNative("place", nativePlace);
    vm.defineNative("face", nativeFace);
    vm.defineNative("facing", nativeFacing);
    vm.defineNative("pose", nativePose);
    vm.defineNative("stride", nativeStride);
    vm.defineNative("wave", nativeWave);
    vm.defineNative("rest", nativeRest);
    vm.defineNative("entitycount", nativeEntityCount);

    vm.defineNative("weld", nativeWeld);
    vm.defineNative("ballsocket", nativeBallSocket);
    vm.defineNative("hinge", nativeHinge);
    vm.defineNative("rope", nativeRope);
    vm.defineNative("rod", nativeRod);
    vm.defineNative("breakable", nativeBreakable);
    vm.defineNative("broken", nativeBroken);
    vm.defineNative("damp", nativeDamp);
    vm.defineNative("motor", nativeMotor);
    vm.defineNative("unmotor", nativeUnmotor);
    vm.defineNative("setlength", nativeSetLength);
    vm.defineNative("jointlength", nativeJointLength);

    // Block ids as names. A script writing `setblock(p, stone)` is readable;
    // one writing `setblock(p, 1)` is a bug waiting for the palette to change.
    //
    // Taken from the palette rather than from a table here, which is both the
    // split and a straight improvement: a host that registers its own blocks
    // gets them named in scripts without touching this file, and the two lists
    // cannot disagree because there is one. The name a block answers to is the
    // name it was registered under -- `grass_block`, not `grass`.
    if (sandbox.world) {
        const BlockRegistry& palette = sandbox.world->registry();
        for (BlockId id = 0; id < BlockId(palette.size()); ++id)
            vm.defineGlobal(palette[id].name, Value::num(double(id)));
    }
}

} // namespace blocky::script
