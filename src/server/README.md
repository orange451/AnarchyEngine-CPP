# AnarchyServer

`AnarchyServer` runs one project with no window and serves it to `AnarchyPlayer` clients over UDP until Ctrl+C. It links `engine_network`, `engine_core` and `anarchy_network`, and compiles `src/runner/GamePack.cpp` in; it never links the renderer, JadeFX, or `studio_core`.

```
AnarchyServer [project folder] [--port N] [--max-connections 1-64] [--max-send BYTES]
```

- `--port` defaults to 7777 (0 picks any free port), `--max-connections` to 64, and `--max-send` to 1048576 bytes a second for each client. Flow control still halves the budget on a lossy or slow link.
- Started with no folder, it serves the game packed into its own file, as the player plays one. The game is unpacked under the temporary folder's `AnarchyEngine/servers`.
- Exit codes: 2 for a bad argument (the usage line is printed), 1 when the project cannot be served (no project, no gameId, unreadable resources, port in use), 0 after Ctrl+C or SIGTERM. A second Ctrl+C while it shuts down quits at once with 130.

`ServerHost` does the work, so tests drive it without the program (`sandbox/server_tests.cpp`, tag `[server]`):

- `open` loads the project into a headless engine. That engine has no renderer, no audio device and no client sync, runs at 60 Hz in the Server role, and has a `ServerSession` on a UDP socket that presents `project_identity`.
- `start` starts play and the engine thread.
- `stop` closes every client ("The server closed") on the engine thread, waits for it, then stops the engine and unhooks the session.
- `print_output` writes the scripts' prints as `[HH:MM:SS] text`, with errors going to stderr.

The resources hash skips names that start with '.', as a packed game does. A packed project whose resources folder held only `.gitkeep` files has its empty folder made again, so it presents the same identity as its folder.
