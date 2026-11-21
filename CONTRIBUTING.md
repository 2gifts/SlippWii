# Contributing to SlippWii

Thanks for wanting to help. Please read this page before you start: it saves you work that might
not be taken, for reasons that have nothing to do with how good it is.

## What SlippWii is

> My goal is to focus solely on online performance and connectivity, and to stay as close as
> possible to the existing Slippi products. SlippWii should not expand beyond the core online
> gameplay. Picture it as "what if there were an official Slippi port to the Wii". I want the
> implementation to be as simple, polished and consistent as possible.
>
> — 2gifts, maintainer

In practice that means two goals, and nothing else:

1. **Online plays exactly like offline.** A SlippWii game feels like the same game on a tournament
   console (Slippi Nintendont, polling drift fix on, CRT): the same input delay, and no frames shown
   twice or skipped.
2. **A PC player can't tell they are playing a Wii.** Against SlippWii they should get the same
   rollbacks, the same smoothness and the same connection as against another Slippi Dolphin.

## What we take

- **Fixes**: crashes, hangs, desyncs, connection problems, anything that breaks a game.
- **Measurable netcode improvements**: fewer or shallower rollbacks for the PC player, fewer
  frames shown twice on the Wii, better connectivity. They need the measurements in
  [docs/TESTING.md](docs/TESTING.md).
- **Closer to Slippi on PC**: something Slippi Dolphin does that SlippWii doesn't yet, for example
  a Gecko code Slippi ships for online play, or a Slippi behaviour that SlippWii does differently.
- **Support**: clearer error messages, better problem reports, docs.

## What we don't take

These aren't judgments of the idea. They're outside what SlippWii is for.

- New game modes, menus, settings or features that Slippi on PC doesn't have.
- Changes to offline play. Offline stays Slippi Nintendont as it is: vanilla Melee plus the
  player's own Slippi Nintendont settings.
- A second netcode mode or a "low latency" option. There is one online behaviour, the same for
  everyone. A faster method replaces the standard one; it doesn't sit beside it.
- Anything that changes how the game feels without the player knowing or choosing it.
- Ranked (it's Slippi's protected competitive mode) and Wi-Fi (far too slow; SlippWii is wired
  only).

## Settled decisions

These were measured, and some were checked by players with their own equipment. Please don't
reopen them without new measurements.

- **1 delay frame online equals a tournament console's input delay.** This was measured inside the
  console and confirmed independently by a player with a light-sensor adapter on a CRT
  ([docs/INPUT_DELAY.md](docs/INPUT_DELAY.md)). Any change that moves it is rejected, whatever
  else it improves.
- **When the game reads the controller doesn't change.** Online timing is built around the
  console's own controller polling.
- **Ideas that have already been tried**, like "use Nintendont's PDF + 1/2 frame code", are in
  [docs/ALREADY_TRIED.md](docs/ALREADY_TRIED.md), with what was found. Please check there first.

## Before you start

For anything bigger than a small fix, ask on the
[SlippWii Discord](https://discord.gg/ytSjxmknHx) first, or open an issue. A two-line description
of the idea and how you'd measure it is enough. It's much nicer to hear "that's out of scope" or
"we tried that" before you've done the work.

## What a pull request needs

1. **What changes for the player**, in a sentence or two: what someone on the Wii or on the PC
   would notice.
2. **Measurements against `main`**, as [docs/TESTING.md](docs/TESTING.md) describes, in the
   table the pull request template gives you. If you don't have the hardware for the Wii runs,
   say so: the maintainer can run them on the reference Wii, but that takes longer.
3. **One change per pull request**, as simple as it can be. A small change with clear numbers is
   much easier to take than a large one.

Pull requests are squash-merged into `main`. Releases are tagged from `main` by the maintainer,
who signs them; only signed releases reach players' Wiis through *Check for Updates*.

Sometimes a well-made change is still declined, because it doesn't fit the scope above or because
it adds complexity for a gain too small to feel. That's about scope, not about your work.

## Using an AI agent

That's welcome. Point it at [AGENTS.md](AGENTS.md) first: it has the rules, the layout and the
commands. You're responsible for what you submit, so please run the tests and read the diff
yourself before opening a pull request.

## Licence

SlippWii is GPL-2.0, as Nintendont is; contributions are accepted under the same licence. Slippi's
online code that SlippWii downloads and runs (slippi-ssbm-asm) is GPL-3.0.
