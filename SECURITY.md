# Security policy

## Reporting a vulnerability

Please do not open a public issue for a security problem. Use GitHub's private reporting instead: open the [Security tab](../../security), choose "Report a vulnerability" and describe the problem, with steps to reproduce if you can. The report stays private until a fix is out.

Reports are answered as soon as possible, usually within a few days. This is a hobby project, so there is no fixed deadline.

## Supported versions

Only the latest release gets fixes. Please check that the problem is still there in the latest release before reporting.

## Scope

In scope:

- the loader and the host (`dinput8.dll`, `host.dll`, `HorizonHost.dll`): anything that lets another process or a file in the game folder run code it should not, or that weakens the check of the game's exe against the known SHA-256
- the plugin loader and the scripting host (Horizon Scripts and its permissions), including a way around the `solo-only` online gate
- the install script `Horizon-Install.cmd` and the release zips: files written outside the game folder, missing backups
- network use: TMX Browser, TMX Upload and the bridge to TM Horizon, for example a replay or file sent without the Allow press

Out of scope:

- vulnerabilities in the games or in third-party mods that happen to be loaded next to it
- problems that need an attacker who already controls your PC
