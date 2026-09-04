# Contributing

[по-русски](CONTRIBUTING.ru.md)

This is one person's project with strong opinions about how it is built. Pull
requests are welcome, and it is only fair to say up front which ones will not
be merged, so nobody spends an evening on something that was never going to
land.

---

## No third-party libraries. None.

Not a preference — it is what this project *is*. The PNG codec, the ZIP reader,
the OpenGL loader, the window, the maths, the physics solver, the scripting
language and the H.264 encoder are all written here, on purpose, and that is
the reason the repository is interesting rather than an obstacle to it.

So a change that adds a dependency will be declined however good the library
is, and this covers all the shapes it comes in:

- a package manager, a submodule, a `FetchContent`;
- a header-only library dropped into the tree;
- a single file vendored from somewhere else;
- code pasted out of another project.

The only things linked are `opengl32`, `user32` and `gdi32`, all of which ship
with Windows.

If something genuinely needs a library, that is a conversation to have in an
issue *before* writing anything — the answer will usually be "then we write a
smaller version of it that does only what we need", which is how everything
here got written.

---

## AI is welcome. You are still responsible.

Use whatever helps — models, generators, whatever comes next. Much of this
repository was written that way and there is nothing to apologise for.

What does not change is who owns the result. When you open a pull request you
are saying, about every line in it:

- **You understand it.** If a reviewer asks why a loop is ordered that way or
  what happens when that value is zero, "the model wrote it" is not an answer.
  If you cannot defend it, do not submit it.
- **You ran it.** It builds, and the test suites pass. Generated code is
  confidently wrong in ways that read beautifully; the only defence is running
  it.
- **You have the right to contribute it** under the MIT licence, and it is not
  copied out of a codebase whose licence says otherwise. A model repeating
  somebody else's code verbatim is your problem once you submit it.
- **You checked the claims.** This codebase states numbers — timings, ratios,
  sample counts. If a change makes such a claim, it was measured, not guessed.

Mentioning the tool in the commit is welcome and normal here; the existing
history uses a `Co-Authored-By` trailer for it. It is a note about how the work
was done, not a transfer of responsibility.

---

## What a good change looks like

**One thing at a time.** A bug fix, or a feature, or a refactor — not all three.
A reformatting pass mixed into a fix makes the fix impossible to review.

**Comments say why, not what.** The code says what it does. The comments here
exist for the reasoning that is not visible in the code: why this and not the
obvious alternative, what went wrong when it was written the other way, what a
number was measured against. Read a few files before writing any; the tone is
consistent and it is the most valuable thing in the repository.

**Tests assert numbers, not impressions.** "Looks smoother" is satisfied
equally well by a filter that destroyed the image. See
[docs/testing.md](docs/testing.md) — it is as much about how to test this kind
of code as about what is covered.

**Follow the conventions.** [docs/conventions.md](docs/conventions.md): +Y up,
entities facing −Z, linear colour everywhere inside, inclusive box bounds,
degrees at the API and radians inside. Getting a sign wrong here does not look
like a wrong angle, it looks like missing geometry.

**Comments in code are English.** The documents under `docs/` are Russian,
except the front page, the gallery and the install guide, which are both.

**Commit messages explain the change.** Look at `git log`: they are prose, they
say what was wrong and why the fix is shaped the way it is. A one-line "fix bug"
is not the house style.

---

## Before opening a pull request

```
build.cmd release
```

Then the seventeen suites — the snippet is in
[docs/install.md](docs/install.md#running-something). All of them must pass.
`test_zip` and `test_assets` skip themselves without a Minecraft installation,
which is not a failure.

A few smaller things that come up:

- **No new compiler warnings.** The build is `/W4 /permissive-`. A full build
  currently emits six `C4310` in `scenes/common/film/type.hpp`, where Cyrillic
  code points are cast into a legacy byte encoding; do not add to them.
- **No absolute paths.** Nothing may point at a directory on your machine. A
  scene that needs an asset either generates it or degrades without it — see
  `scenes/common/skins.hpp` for how the characters are drawn rather than read.
- **Line endings are handled** by `.gitattributes`; do not fight it, and do not
  send a diff that is mostly line endings.
- **Do not commit `out/` or `build/`.** Both are ignored.

---

## What is likely to be declined

- Anything adding a dependency, as above.
- Reformatting, renaming or restyling existing code without a reason beyond
  taste.
- Implementing something from the README's "what it does not" list without
  discussing it first. Those are not a to-do list — most are decisions with the
  reasoning written down, and a few are arguments against ever doing them.
- A change with no test, in an area that has tests.

---

## Reporting a bug

Say what you did, what happened and what you expected. If it is a rendering
problem, attach the frame and the scene arguments that produced it. If it is a
crash, the scene and the arguments are usually enough — everything here is
deterministic on purpose, including the random number generators, so a bug that
happened once should happen again.
