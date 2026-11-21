# Contributing to SlippWii

Thanks for wanting to help. This page says what the project is for, what stays fixed, and what a
change needs, so that nobody is surprised later.

## What SlippWii is

> My goal is to focus solely on online performance and connectivity, and to stay as close as
> possible to the existing Slippi products. SlippWii should not expand beyond the core online
> gameplay. Picture it as "what if there were an official Slippi port to the Wii". I want the
> implementation to be as simple, polished and consistent as possible.
>
> — 2gifts, maintainer

In practice, two goals:

1. **Online plays exactly like offline.** A SlippWii game feels like the same game on a tournament
   console (Slippi Nintendont, polling drift fix on, a CRT): the same input delay, no frame shown
   twice or skipped.
2. **A PC player can't tell they are playing a Wii.** Against SlippWii they get the same
   rollbacks, the same smoothness and the same connection as against another Slippi Dolphin.

Goal 1 is met at 1 delay frame on a good connection. Goal 2 isn't yet.
[docs/FINDINGS.md](docs/FINDINGS.md) says where things stand and which problems are open.

## What stays fixed

These describe what a player gets. A change that breaks one of them isn't taken, whatever else it
improves.

- **1 delay frame online is a tournament console's input delay.** It was measured inside the
  console, and an independent tester confirmed it on a CRT
  ([docs/INPUT_DELAY.md](docs/INPUT_DELAY.md)). A change may not make it slower, and it may not
  make it faster either: the target is the console.
- **Offline play is untouched**: Slippi Nintendont as it is, with the player's own settings. It is
  the reference that online is measured against.
- **One online behaviour for everyone.** No second netcode mode and no "low latency" option. A
  better method replaces the standard one.
- **Nothing changes the feel behind the player's back.** If one game would play differently from
  the next, the player must be able to know why.
- **Slippi's features, no more.** No new modes, menus or settings that Slippi on PC doesn't have.
  No Ranked (Slippi's protected competitive mode). Wired connections only.

## What is open

Everything else. How the rollback engine works, how packets leave the console, how timing is
kept, how any of it is measured: all of that can be rethought from the ground up.

[docs/FINDINGS.md](docs/FINDINGS.md) lists what has been tried, with the numbers. It is there to
give you a head start, not to say no. Where it says something looked like a limit, read that as
the limit of what was tried. If you think Teams can have rollback, or a rollback can cost half of
what it does, or packets can leave the Wii a millisecond sooner, you may well be right, and that
would be exactly the kind of change this project wants.

## What we take

- **Fixes**: crashes, hangs, desyncs, connection problems.
- **Measured netcode improvements**: fewer or shallower rollbacks for the PC player, fewer slow
  frames on the Wii, better connectivity.
- **Closer to Slippi on PC**: something Slippi Dolphin does that SlippWii doesn't yet, such as a
  Gecko code Slippi ships for online play.
- **Support**: clearer messages, better problem reports, docs, test tools that are easier to run.
- **Findings**: something you tried that didn't work, written into
  [docs/FINDINGS.md](docs/FINDINGS.md) with its numbers.

## What a pull request needs

1. **What changes for the player**, in a sentence or two.
2. **Measurements against `main`**, as [docs/TESTING.md](docs/TESTING.md) describes, in the table
   the pull request template gives you. This is the same standard the maintainer's own changes
   go through.
3. **One change at a time**, as simple as it can be.

The Wii runs need hardware and some setup. If you can't do them, send the change with what you
could test and say what you couldn't: the maintainer can run the rest on the reference Wii.

A quick word on the [SlippWii Discord](https://discord.gg/ytSjxmknHx) before a big piece of work
is a good idea. You don't need permission to try something. It's only so you hear early if an
idea touches one of the fixed points above.

Pull requests are squash-merged into `main`. The maintainer tags and signs releases from `main`;
only a signed release reaches players' consoles through *Check for Updates*.

Sometimes a well-made change is still declined: it adds a feature outside the scope, or its gain
is too small to feel for the complexity it brings. That is a judgment about the project's
direction, never about you or your work, and you will get the reason.

## Using an AI agent

Welcome, and probably the usual way to work here. Point the agent at [AGENTS.md](AGENTS.md)
first. You are responsible for what you send, so read the diff and the measurements yourself
before you open a pull request.

## Licence

SlippWii is GPL-2.0, as Nintendont is; contributions come under the same licence. Slippi's online
code that SlippWii downloads and runs (slippi-ssbm-asm) is GPL-3.0.
