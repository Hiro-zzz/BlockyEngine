// Tests for the engine's own scripting language.
//
// A language is unusually pleasant to test, because almost everything it
// claims can be stated as "this source produces this value" or "this source
// fails on this line". So the tests are written that way rather than as a
// handful of programs that run without crashing -- a program that runs is
// evidence of very little.
//
// Three things get more attention than the rest, because each is a promise
// the sandbox depends on rather than a language feature:
//
//   - **errors carry the right line**, or a script author is hunting blind;
//   - **a runaway loop is stopped**, or one bad tool freezes the game;
//   - **a broken reload changes nothing**, or saving a typo costs the world.
//
// Needs no game files.
#include "engine/core/file.hpp"
#include "engine/entity/entity.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/rig/rig.hpp"
#include "engine/script/bindings.hpp"
#include "engine/script/script.hpp"
#include "scenes/common/palette.hpp"

#include "common/props.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace blocky;
using namespace blocky::script;

namespace {

int gFailures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++gFailures;
    }
}

bool nearly(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

// Runs a source that ends by calling `result(x)`, and returns what it passed.
Value gResult;
Value captureResult(Interpreter&, const std::vector<Value>& args) {
    gResult = args.empty() ? Value::nil() : args[0];
    return Value::nil();
}

std::vector<std::string> gPrinted;

// A Script owns the heap its interpreter points into, so it is neither
// copyable nor movable: it gets configured where it is declared.
void setUp(Script& script) {
    script.onBind = [](Interpreter& vm) { vm.defineNative("result", captureResult); };
    script.onPrint = [](const std::string& text) { gPrinted.push_back(text); };
}

// Evaluates an expression and returns its value as text.
std::string evalToString(const std::string& expression) {
    gResult = Value::nil();
    Script script;
    setUp(script);
    if (!script.loadSource("result(" + expression + ")", "<expr>")) {
        return "ERROR: " + script.lastError();
    }
    return toString(gResult);
}

// Runs a whole program and returns the captured result as text.
std::string runToString(const std::string& source) {
    gResult = Value::nil();
    Script script;
    setUp(script);
    if (!script.loadSource(source, "<program>")) return "ERROR: " + script.lastError();
    return toString(gResult);
}

// Runs a program expected to fail, and returns "line: message".
std::string runExpectingError(const std::string& source) {
    Script script;
    setUp(script);
    if (script.loadSource(source, "t")) return "(no error)";

    // Strip the "t:" prefix so the test states the line and the message.
    const std::string& text = script.lastError();
    return text.size() > 2 ? text.substr(2) : text;
}

void expectEval(const std::string& expression, const std::string& expected) {
    std::string actual = evalToString(expression);
    if (actual != expected) {
        std::printf("  FAIL  %s  ->  %s   (expected %s)\n", expression.c_str(), actual.c_str(),
                    expected.c_str());
        ++gFailures;
    }
}

void expectRun(const std::string& label, const std::string& source, const std::string& expected) {
    std::string actual = runToString(source);
    if (actual != expected) {
        std::printf("  FAIL  %s  ->  %s   (expected %s)\n", label.c_str(), actual.c_str(),
                    expected.c_str());
        ++gFailures;
    }
}

// ------------------------------------------------------------------ values
void testExpressions() {
    std::printf("expressions\n");

    expectEval("1 + 2 * 3", "7");
    expectEval("(1 + 2) * 3", "9");
    expectEval("7 % 3", "1");
    expectEval("2 < 3 and 3 < 4", "true");
    expectEval("2 < 3 and 4 < 3", "false");
    expectEval("nil or 5", "5");
    expectEval("0 or 5", "0");            // zero is true, so it wins
    expectEval("false or \"fallback\"", "fallback");
    expectEval("not nil", "true");
    expectEval("-(3 + 4)", "-7");
    expectEval("\"a\" + \"b\"", "ab");
    expectEval("\"n = \" + 4", "n = 4");
    expectEval("1 == 1.0", "true");
    expectEval("\"a\" < \"b\"", "true");

    // Whole numbers print as whole numbers. A count is the commonest thing a
    // script prints and "5.000000 crates" is not what anyone wants.
    expectEval("10 / 2", "5");
    expectEval("10 / 4", "2.5");
}

void testVectors() {
    std::printf("vectors\n");

    expectEval("vec(1, 2, 3)", "(1, 2, 3)");
    expectEval("vec(2)", "(2, 2, 2)");
    expectEval("vec(1, 2, 3) + vec(10, 20, 30)", "(11, 22, 33)");
    expectEval("vec(1, 2, 3) * 2", "(2, 4, 6)");
    expectEval("3 * vec(1, 2, 3)", "(3, 6, 9)");
    expectEval("vec(2, 4, 6) / 2", "(1, 2, 3)");
    expectEval("vec(1, 2, 3) * vec(2, 0, 1)", "(2, 0, 3)");
    expectEval("-vec(1, 2, 3)", "(-1, -2, -3)");
    expectEval("vec(3, 4, 0).x", "3");
    expectEval("length(vec(3, 4, 0))", "5");
    expectEval("dot(vec(1, 2, 3), vec(4, 5, 6))", "32");
    expectEval("cross(vec(1, 0, 0), vec(0, 1, 0))", "(0, 0, 1)");
    expectEval("distance(vec(0, 0, 0), vec(0, 3, 4))", "5");
    expectEval("normalize(vec(0, 5, 0))", "(0, 1, 0)");

    // A vec is a value. Writing through one would have to write back into
    // whatever produced it, and for `f().x = 1` there is nothing to write to.
    check(runExpectingError("let p = vec(1,2,3)\np.x = 9").find("cannot be changed in place") !=
              std::string::npos,
          "a vec cannot be assigned into");
}

void testContainers() {
    std::printf("lists and maps\n");

    expectEval("[1, 2, 3]", "[1, 2, 3]");
    expectEval("[1, 2, 3][1]", "2");
    expectEval("len([1, 2, 3])", "3");
    expectEval("{a: 1, b: 2}", "{a: 1, b: 2}");
    expectEval("{a: 1, b: 2}.b", "2");
    expectEval("{a: 1}.missing", "nil");
    expectEval("has({a: 1}, \"a\")", "true");
    expectEval("keys({a: 1, b: 2})", "[a, b]");
    expectEval("range(3)", "[0, 1, 2]");
    expectEval("range(1, 4)", "[1, 2, 3]");

    expectRun("push", "let xs = []\npush(xs, 1)\npush(xs, 2)\nresult(xs)", "[1, 2]");
    expectRun("index assign", "let xs = [1, 2, 3]\nxs[0] = 9\nresult(xs)", "[9, 2, 3]");
    expectRun("field assign", "let m = {a: 1}\nm.b = 2\nresult(m)", "{a: 1, b: 2}");

    // Insertion order, not hash order: a script that walks a map and spawns
    // things has to do the same thing twice.
    expectRun("map order", "let m = {z: 1, a: 2, m: 3}\nresult(keys(m))", "[z, a, m]");

    check(runExpectingError("result([1,2][5])").find("outside a list of 2") != std::string::npos,
          "an out-of-range index says how long the list was");
}

// ------------------------------------------------------------ control flow
void testControlFlow() {
    std::printf("control flow\n");

    expectRun("if/else", "let x = 5\nif x > 3 { result(\"big\") } else { result(\"small\") }", "big");
    expectRun("else if", "let x = 0\nif x > 0 { result(1) } else if x < 0 { result(-1) } else { result(0) }", "0");

    expectRun("while", "let i = 0\nlet total = 0\nwhile i < 5 { total = total + i; i = i + 1 }\nresult(total)", "10");
    expectRun("for over list", "let total = 0\nfor v in [1, 2, 3, 4] { total = total + v }\nresult(total)", "10");
    expectRun("for over range", "let total = 0\nfor i in range(5) { total = total + i }\nresult(total)", "10");
    expectRun("for over map", "let m = {a: 1, b: 2}\nlet out = []\nfor k in m { push(out, k) }\nresult(out)", "[a, b]");

    // The one ambiguity in the grammar, and the rule it is settled by. After
    // `if`, `while` and `in`, a '{' opens the body; a map there needs
    // parentheses. Go makes the same trade for the same reason.
    check(runExpectingError("for k in {a: 1} { }").find("starts the body, not a map") !=
              std::string::npos,
          "a map literal in a loop header explains itself");
    expectRun("parenthesised map header",
              "let out = []\nfor k in ({a: 1, b: 2}) { push(out, k) }\nresult(out)", "[a, b]");
    expectRun("parenthesised map in an if",
              "if len(({a: 1})) == 1 { result(\"yes\") }", "yes");

    expectRun("break", "let i = 0\nwhile true { i = i + 1; if i == 3 { break } }\nresult(i)", "3");
    expectRun("continue in for",
              "let total = 0\nfor i in range(6) { if i % 2 == 0 { continue }\n total = total + i }\nresult(total)",
              "9");

    // A block is a scope, and an inner `let` shadows rather than overwrites.
    expectRun("shadowing", "let x = 1\n{ let x = 2 }\nresult(x)", "1");
}

void testFunctions() {
    std::printf("functions\n");

    expectRun("call", "fn add(a, b) { return a + b }\nresult(add(2, 3))", "5");
    expectRun("no return", "fn nothing() { }\nresult(nothing())", "nil");
    expectRun("recursion",
              "fn fact(n) { if n <= 1 { return 1 }\n return n * fact(n - 1) }\nresult(fact(6))", "720");

    expectRun("first class",
              "fn twice(f, x) { return f(f(x)) }\nfn inc(n) { return n + 1 }\nresult(twice(inc, 5))",
              "7");

    // A closure keeps the scope it was written in, not the one it is called
    // from. Counters are the shortest way to say so.
    expectRun("closure",
              "fn counter() { let n = 0\n return fn() { n = n + 1\n return n } }\n"
              "let next = counter()\nnext()\nnext()\nresult(next())",
              "3");

    expectRun("closures are independent",
              "fn counter() { let n = 0\n return fn() { n = n + 1\n return n } }\n"
              "let a = counter()\nlet b = counter()\na()\na()\nresult(a() + b())",
              "4");

    check(runExpectingError("fn f(a) { }\nf(1, 2)").find("takes 1 argument(s), got 2") !=
              std::string::npos,
          "calling with the wrong number of arguments is an error");
}

// --------------------------------------------------------- error reporting
void testErrors() {
    std::printf("errors point at the right line\n");

    // The line matters more than the wording: an author reads the number
    // first. Each of these puts the fault on line 3 deliberately.
    check(runExpectingError("let a = 1\nlet b = 2\nlet c = undefinedName\n").substr(0, 2) == "3:",
          "an undefined name is reported on its own line");
    check(runExpectingError("let a = 1\n\nlet b = a + \"x\" * 2\n").substr(0, 2) == "3:",
          "a type error is reported on its own line");
    check(runExpectingError("fn f() {\n  let x = 1\n  return x + nil\n}\nf()").substr(0, 2) == "3:",
          "an error inside a function points inside the function");

    std::string syntax = runExpectingError("let x = \nif {");
    check(syntax.find("expected an expression") != std::string::npos,
          "a syntax error says what it wanted");

    check(runExpectingError("let x = 1\nx = y").find("'y' is not defined") != std::string::npos,
          "reading an undefined name is caught");
    check(runExpectingError("y = 1").find("use 'let'") != std::string::npos,
          "assigning to an undeclared name suggests let");
    check(runExpectingError("result(1 / 0)").find("division by zero") != std::string::npos,
          "division by zero is caught rather than producing infinity");

    // A failed call must leave the script usable. This is the property that
    // keeps one bad tool from ending a session.
    Script script;
    setUp(script);
    check(script.loadSource("on boom { let x = nil + 1 }\non fine { result(42) }", "t"),
          "a script with a failing handler still loads");
    check(!script.call("boom"), "the failing handler reports failure");
    check(script.call("fine"), "and the next handler still runs");
    check(toString(gResult) == "42", "with the right result");
}

void testRunawayLoop() {
    std::printf("a runaway loop is stopped\n");

    Script script;
    setUp(script);
    check(script.loadSource("on spin { while true { } }", "t"), "the script loads");

    script.vm()->stepBudget = 50000;
    bool ok = script.call("spin");
    check(!ok, "an endless loop is reported rather than hung");
    check(script.lastError().find("endless loop") != std::string::npos,
          "and says what it thinks happened");

    // The budget is per call, so a script that survives one tick is not
    // penalised on the next.
    check(script.loadSource("let n = 0\non tick { n = n + 1 }", "t"), "reloads");
    script.vm()->stepBudget = 100;
    for (int i = 0; i < 50; ++i) check(script.call("tick"), "a short handler runs every tick");
}

// -------------------------------------------------------------- host seams
void testHandlersAndBinding() {
    std::printf("handlers and binding\n");

    Script script;
    setUp(script);
    check(script.loadSource("on ready { result(\"up\") }\non tick(dt) { result(dt * 2) }", "t"),
          "a script with handlers loads");

    check(script.hasHandler("ready"), "ready is registered");
    check(script.hasHandler("tick"), "tick is registered");
    check(!script.hasHandler("nope"), "and nothing else is");

    check(script.call("ready"), "ready runs");
    check(toString(gResult) == "up", "and does its work");

    check(script.call("tick", {Value::num(0.5)}), "tick runs with an argument");
    check(toString(gResult) == "1", "and receives it");

    // Calling an event nobody handles is not an error: the host fires `tick`
    // whether or not this particular script cares about it.
    check(script.call("nobodyHandlesThis"), "an unhandled event is quietly ignored");

    // `on` with no parameter list, for events that hand you nothing.
    check(script.loadSource("on ready { result(7) }", "t"), "on without parens parses");
    check(script.call("ready") && toString(gResult) == "7", "and runs");
}

void testReload() {
    std::printf("reload\n");

    // Written to the scratch area rather than into the project.
    const std::string path = "out/test_script_reload.bly";

    auto write = [&](const std::string& text) {
        return writeFileBytes(path, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                              nullptr);
    };

    check(write("on ready { result(1) }"), "wrote the first version");

    Script script;
    setUp(script);
    check(script.loadFile(path), "loads from a file");
    check(script.call("ready") && toString(gResult) == "1", "runs version one");

    check(write("on ready { result(2) }"), "wrote the second version");
    bool changed = false;
    check(script.reloadIfChanged(&changed), "reloads cleanly");
    check(changed, "and noticed the file had changed");
    check(script.call("ready") && toString(gResult) == "2", "now runs version two");

    // Nothing changed: no reload, no work.
    changed = true;
    check(script.reloadIfChanged(&changed), "an unchanged file reloads trivially");
    check(!changed, "and reports that it did nothing");

    // The property the whole layer rests on: a broken save costs a message,
    // not the running script.
    check(write("on ready { result(3"), "wrote a broken version");
    check(!script.reloadIfChanged(&changed), "a syntax error fails the reload");
    check(script.lastError().find("expected") != std::string::npos, "with a syntax message");
    check(script.loaded(), "the previous script is still loaded");
    check(script.call("ready") && toString(gResult) == "2",
          "and still runs -- version two survived the bad save");

    // And recovers when the file is fixed.
    check(write("on ready { result(4) }"), "wrote a fixed version");
    check(script.reloadIfChanged(&changed), "the fixed file loads");
    check(script.call("ready") && toString(gResult) == "4", "and takes over");

    removeFile(path);
}

void testHeapIsFreedOnReload() {
    std::printf("reload frees the heap\n");

    Script script;
    setUp(script);
    // Allocates a few thousand list cells at load time.
    std::string heavy = "let junk = []\nfor i in range(2000) { push(junk, [i, i, i]) }\n";
    check(script.loadSource(heavy, "t"), "the allocating script loads");

    size_t afterFirst = script.heapObjects();
    check(afterFirst > 2000, "and really did allocate");

    check(script.loadSource(heavy, "t"), "loading it again");
    check(script.heapObjects() <= afterFirst + 8,
          "does not stack a second heap on the first -- the arena is freed");

    check(script.loadSource("let nothing = 1", "t"), "and a small script after a big one");
    check(script.heapObjects() < 100, "leaves a small heap");
}

// ------------------------------------------------------------- the bindings
// The verbs a sandbox script actually uses, and the reload policy a host
// builds on top of them.
//
// This is deliberately tested without a window. What `scene_live` adds on top
// is a GL context and a camera; the part that can be wrong -- what a reload
// does to the world -- is all here.
void testSandboxBindings() {
    std::printf("sandbox bindings\n");

    World world(palette::registry());
    PhysicsWorld physics;

    VoxelModel cube = demo::buildCrate();
    script::Sandbox sandbox;
    sandbox.world = &world;
    sandbox.physics = &physics;
    sandbox.addModel("crate", cube, 0.125f);

    Script script;
    script.onPrint = [](const std::string& text) { gPrinted.push_back(text); };
    script.onBind = [&sandbox](Interpreter& vm) {
        vm.defineNative("result", captureResult);
        script::installSandboxLibrary(vm, sandbox);
    };

    // The host's reload policy: the script owns the contents of the world, so
    // rebuilding starts from an empty one. Same code `scene_live` runs.
    auto rebuild = [&]() {
        world.clear();
        physics.clear();
        sandbox.spawned.clear();
        return script.call("ready");
    };

    const std::string path = "out/test_script_sandbox.bly";
    auto write = [&](const std::string& text) {
        return writeFileBytes(path, reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                              nullptr);
    };

    check(write("on ready {\n"
                "  fillbox(vec(-4, -1, -4), vec(4, -1, 4), stone)\n"
                "  let a = spawn(\"crate\", vec(0, 3, 0))\n"
                "  let b = spawn(\"crate\", vec(0, 5, 0))\n"
                "  weld(b, a, vec(0, 4, 0))\n"
                "  result(bodycount())\n"
                "}\n"),
          "wrote a building script");

    check(script.loadFile(path), "it loads");
    check(rebuild(), "and builds");

    check(world.blockCount() == 81, "fillbox placed a 9x9 slab");
    check(physics.bodyCount() == 2, "spawn made two bodies");
    check(physics.constraintCount() == 1, "weld made one joint");
    check(toString(gResult) == "2", "and bodycount agreed");

    // Handles survive a step and still name the right body.
    for (int i = 0; i < 60; ++i) physics.step(world, 1.0f / 120.0f);
    check(physics.body(0).position.y < 3.0f, "the pair fell");

    // Now the edit-and-save that the whole layer exists for.
    check(write("on ready {\n"
                "  fillbox(vec(-2, -1, -2), vec(2, -1, 2), grass_block)\n"
                "  for i in range(5) { spawn(\"crate\", vec(0, 2 + i, 0)) }\n"
                "}\n"),
          "wrote a different script");

    bool changed = false;
    check(script.reloadIfChanged(&changed) && changed, "it reloads");
    check(rebuild(), "and rebuilds");

    check(world.blockCount() == 25, "the world is the new one, not both");
    check(physics.bodyCount() == 5, "with the new bodies");
    check(physics.constraintCount() == 0, "and none of the old joints");

    // A bad save must leave the built world alone -- this is the property the
    // window is there to make visible.
    check(write("on ready { spawn(\"crate\""), "wrote a broken script");
    check(!script.reloadIfChanged(&changed), "the reload fails");
    check(physics.bodyCount() == 5, "and the world it did not touch is intact");
    check(script.call("ready"), "the previous script is still callable");

    // Naming something that does not exist is reported, not crashed on.
    check(write("on ready { spawn(\"unicorn\", vec(0, 0, 0)) }"), "wrote a bad model name");
    check(script.reloadIfChanged(&changed), "which loads fine");
    check(!rebuild(), "and fails at the call");
    check(script.lastError().find("no model called 'unicorn'") != std::string::npos,
          "saying which name was wrong");

    removeFile(path);
}

// ------------------------------------------------------------- characters
// Entities are the half of the sandbox that is not simulated. A prop is
// something the solver owns; a character is something the *script* owns and
// bends, so these tests are about whether a script can say where it stands
// and how it is posed -- and whether it is told clearly when it cannot.
void testEntityBindings() {
    std::printf("entity bindings\n");

    World world(palette::registry());
    PhysicsWorld physics;

    Skin skin = demo::buildBlockySkin({90, 120, 180, 255}, {60, 60, 75, 255}, {215, 175, 140, 255});
    check(skin.valid(), "the built-in skin builds without any game files");

    script::Sandbox sandbox;
    sandbox.world = &world;
    sandbox.physics = &physics;
    sandbox.addSkin("player", skin);

    Script script;
    script.onPrint = [](const std::string& text) { gPrinted.push_back(text); };
    script.onBind = [&sandbox](Interpreter& vm) {
        vm.defineNative("result", captureResult);
        script::installSandboxLibrary(vm, sandbox);
    };

    auto run = [&](const std::string& source) {
        sandbox.entities.clear();
        return script.loadSource(source, "t");
    };

    check(run("let p = spawnentity(\"player\", vec(1, 2, 3), 90)\nresult(entitycount())"),
          "a script can spawn a character");
    check(toString(gResult) == "1", "and count it");
    check(sandbox.entities.size() == 1, "the host sees it too");
    check(sandbox.entities[0].model != nullptr && sandbox.entities[0].skin == &skin,
          "wearing the skin it named");
    check(nearly(sandbox.entities[0].position.y, 2.0f), "standing where it was put");
    check(nearly(sandbox.entities[0].yawDegrees, 90.0f), "facing where it was told");

    check(run("let p = spawnentity(\"player\", vec(0, 0, 0))\nresult(pos(p))"),
          "pos answers for an entity as well as a body");
    check(toString(gResult) == "(0, 0, 0)", "with its position");

    check(run("let p = spawnentity(\"player\", vec(0, 0, 0))\n"
              "place(p, vec(4, 1, -2))\nface(p, 45)\nresult(facing(p))"),
          "place and face move it");
    check(toString(gResult) == "45", "facing reads back");
    check(nearly(sandbox.entities[0].position.x, 4.0f), "and the host sees the move");

    // Posing by joint name. The names are the point: a script naming joint 2
    // is a bug waiting for the skeleton to grow one.
    check(run("let p = spawnentity(\"player\", vec(0, 0, 0))\npose(p, \"head\", 0, 30, 0)"),
          "a joint can be posed by name");
    check(nearly(sandbox.entities[0].pose[joint::Head].rotationDegrees.y, 30.0f),
          "and the rotation lands on that joint");
    check(nearly(sandbox.entities[0].pose[joint::LeftArm].rotationDegrees.y, 0.0f),
          "leaving the others alone");

    check(run("let p = spawnentity(\"player\", vec(0, 0, 0))\nstride(p, 25)"), "stride poses the legs");
    check(std::fabs(sandbox.entities[0].pose[joint::RightLeg].rotationDegrees.x) > 1.0f,
          "and really does bend one");

    // The two ways a script gets this wrong, and what it is told.
    check(!run("spawnentity(\"nobody\", vec(0, 0, 0))"), "an unknown skin fails");
    check(script.lastError().find("no skin called 'nobody'") != std::string::npos,
          "saying which name was wrong");

    check(!run("let p = spawnentity(\"player\", vec(0,0,0))\npose(p, \"elbow\", 0, 0, 0)"),
          "an unknown joint fails");
    check(script.lastError().find("rightarm") != std::string::npos,
          "and lists the joints there are");

    // Handles are typed: a body handle is not an entity handle.
    check(!run("let p = spawnentity(\"player\", vec(0,0,0))\nsetvelocity(p, vec(0,0,0))"),
          "an entity cannot be passed where a body is wanted");
    check(script.lastError().find("wants a body") != std::string::npos, "and is told so");
}

// ------------------------------------------------------- constant strings
void testInternedLiterals() {
    std::printf("string literals are shared\n");

    Script script;
    setUp(script);

    // A per-frame handler passing a constant string to a binding is the
    // commonest shape in a sandbox script, and it used to allocate a string
    // every tick. Strings are immutable here, so one is enough.
    check(script.loadSource("let n = 0\non tick { n = n + len(\"headroom\") }", "t"),
          "the handler loads");

    size_t before = script.heapObjects();
    for (int i = 0; i < 500; ++i) check(script.call("tick"), "it runs");

    std::printf("  %zu objects before 500 ticks, %zu after\n", before, script.heapObjects());
    check(script.heapObjects() <= before + 4,
          "500 ticks using a literal allocate nothing new");

    // Two identical literals in different places are the same object, and a
    // computed string that happens to match is not -- interning is a promise
    // about constants, not about equality.
    check(script.loadSource("result([\"ab\", \"ab\", \"a\" + \"b\"])", "t"), "loads");
    const Value& list = gResult;
    check(list.type == Type::List && list.object && list.object->items.size() == 3, "three strings");
    if (list.type == Type::List && list.object && list.object->items.size() == 3) {
        check(list.object->items[0].object == list.object->items[1].object,
              "two identical literals share one object");
        check(list.object->items[0].object != list.object->items[2].object,
              "a computed string is its own");
        check(valuesEqual(list.object->items[0], list.object->items[2]),
              "though they still compare equal");
    }
}

// ------------------------------------------------------------ scope reuse
// The other half of the heap story, and the half that used to be missing.
//
// `testInternedLiterals` asserts that a handler allocates no new *objects*,
// and that was always true and never the whole answer: scopes are allocated
// whether the script asks or not -- one per call, per block, per branch, per
// loop turn -- so a handler doing arithmetic and nothing else grew the arena
// by tens of megabytes an hour while the object count sat still.
//
// So this asserts the number that actually grew, and then asserts the thing
// that makes reusing a scope legal at all: a closure still owns the one it
// captured.
void testScopesAreRecycled() {
    std::printf("scopes go back to the heap\n");

    {   // Every shape that takes a scope: a call, a block, a branch, a loop.
        Script script;
        setUp(script);
        check(script.loadSource("let n = 0\n"
                                "on tick(dt) {\n"
                                "  if dt > 0 {\n"
                                "    for i in range(4) { n = n + i * dt }\n"
                                "  }\n"
                                "}\n",
                                "t"),
              "the handler loads");

        // A hundred first, so anything the first tick has to allocate once is
        // already allocated when the count is taken.
        for (int i = 0; i < 100; ++i) check(script.call("tick", {Value::num(0.016)}), "it runs");
        size_t before = script.heapScopes();
        for (int i = 0; i < 900; ++i) check(script.call("tick", {Value::num(0.016)}), "it runs");

        std::printf("  %zu scopes after 100 ticks, %zu after 1000\n", before, script.heapScopes());
        check(script.heapScopes() == before, "900 more ticks need no scope that 100 did not");
    }

    {   // A closure keeps the scope it captured, through a thousand calls
        // that each take a scope and give it back. If a recycled scope were
        // ever handed out while this closure still pointed at it, the count
        // would come back wrong rather than crash -- which is the reason to
        // assert on the count.
        Script script;
        setUp(script);
        check(script.loadSource("fn counter() {\n"
                                "  let n = 0\n"
                                "  return fn() { n = n + 1  return n }\n"
                                "}\n"
                                "let c = counter()\n"
                                "on tick(dt) {\n"
                                "  if dt > 0 { for i in range(3) { let junk = i * 2 } }\n"
                                "  result(c())\n"
                                "}\n",
                                "t"),
              "the counter loads");

        for (int i = 0; i < 1000; ++i) check(script.call("tick", {Value::num(0.016)}), "it runs");
        check(gResult.type == Type::Number && gResult.number == 1000.0,
              "a captured variable survives a thousand scopes being reused");
    }

    {   // The sharp case: a closure made *inside a loop turn* captures that
        // turn's scope, so those scopes are exactly the ones that must not be
        // recycled. Three closures, three different values -- one shared or
        // reused scope and they would all answer the same.
        Script script;
        setUp(script);
        check(script.loadSource("let fns = []\n"
                                "for i in range(3) { push(fns, fn() { return i * 10 }) }\n"
                                "result([fns[0](), fns[1](), fns[2]()])\n",
                                "t"),
              "the closures load");

        check(gResult.type == Type::List && gResult.object && gResult.object->items.size() == 3,
              "three closures answered");
        if (gResult.type == Type::List && gResult.object && gResult.object->items.size() == 3) {
            const std::vector<Value>& got = gResult.object->items;
            check(got[0].number == 0.0 && got[1].number == 10.0 && got[2].number == 20.0,
                  "each closure kept its own turn of the loop");
        }
    }
}

}  // namespace

int main() {
    testExpressions();
    testVectors();
    testContainers();
    testControlFlow();
    testFunctions();
    testErrors();
    testRunawayLoop();
    testHandlersAndBinding();
    testReload();
    testSandboxBindings();
    testEntityBindings();
    testInternedLiterals();
    testScopesAreRecycled();
    testHeapIsFreedOnReload();

    if (gFailures == 0) {
        std::printf("\nall script tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED\n", gFailures);
    return 1;
}
