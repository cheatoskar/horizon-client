# Contributing

Bug reports, ideas and pull requests are welcome.

## Reporting a bug

Open an [issue](../../issues) and include:

- the game and its version (and which exe, if you know it)
- the version of Horizon Client and of the modules involved
- what you did, what you expected and what happened
- the log of that start: `horizon\logs\` in the game folder (TMLoader: next to the version folders in `Horizon Host\`). There is one log per start.

A screenshot or a short clip helps. Search the existing issues first.

## Feature ideas

Open an issue and describe the problem you want solved. Check the rules below first: some things are out of scope on purpose.

## Pull requests

Pull requests are welcome. Keep them small and focused: one fix or one feature per pull request. For a larger change, open an issue first so the direction is clear before you spend time on it.

In the description, say what changed and how you tested it in the game (game, version, what you did). The [TESTPLAN](TESTPLAN.md) lists the steps used for a release; run the ones your change touches.

## Build and test

MSVC Build Tools (x86 toolset) and Python 3, as in the [README](README.md#how-it-is-built):

```
build.cmd              everything into dist\legacy\, tests into build\test\
tests\run_tests.cmd    every test without a game
tests\run_perf_tests.cmd
build.cmd package      the release zips and dist\manifest.json
```

New behaviour comes with a test in `tests\` where it can be tested without a game. The plugin interface is described in [docs/plugin-abi.md](docs/plugin-abi.md); changes to it need a note there.

## Rules for in-game mods

These mods run inside the game, also in online play, so a few things are out of scope. A pull request that does any of this will not be merged:

- features that give a competitive advantage online or on leaderboards
- anything that touches physics, inputs or timing of the simulation, or edits runs and replays
- checkpoint or trigger display, ideal or brake line, disabling the camera or MediaTracker behaviour
- anything that gets around the Competition Patch or other anti-cheat and replay validation

The exact rules for this project are in the README ([Safety rules](README.md#safety-rules)) and in [docs/plugin-abi.md](docs/plugin-abi.md) ("Online gate" and "Competition Patch"). Forever is read-only: no game hooks, no memory writes, no input changes. The rules come from what the TrackMania Exchange moderators accept: [TMX rules for mods](https://github.com/cheatoskar/trackmania-internals/blob/main/modding/tmx-rules-for-mods.md).

## AI-assisted contributions

Contributions written with AI tools are fine, as long as the contributor has tested them and can explain what the change does. Untested generated code will be sent back.
