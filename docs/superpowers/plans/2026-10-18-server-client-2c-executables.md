# Server and Client 2c: AnarchyServer and AnarchyPlayer --connect Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A developer runs `AnarchyServer <project>` and players join with `AnarchyPlayer --connect host:port`. Joins are fast: each client gets a send budget, 1 MB/s by default. A server script can kick a client by destroying its Connection. An exported game ships with its server.

**Architecture:**
- The transport (`src/network`) sends several packets per tick while reliable data is due. It is bounded by:
  - a per-connection byte credit, refilled at the flow-control rate;
  - an in-flight cap, which keeps the 256-entry packet ring from wrapping inside one round trip.
- The receiver acks every tick it hears from its peer.
- A Disconnect packet carries a leave code (left, closed, removed), so a client can tell "server closed" from "removed by the server".
- `src/server/ServerHost` wraps a headless `Engine` and a `ServerSession` behind a testable API. `src/server/main.cpp` adds argument parsing and Ctrl+C.
- The player gains a connect mode. It loads the game's identity, empties the world, and lets a `ClientSession` drive play. A "Connecting…" layer sits over the view.

**Tech Stack:** C++17, Winsock/BSD sockets, JadeFX (player UI), Catch2 v3 sandbox, the `tests/` harness for GamePack, CMake, MSVC.

**Spec:** `docs/superpowers/specs/2026-10-13-server-client-design.md` (section 3, the error table, and the performance table). The transport is described in `docs/superpowers/specs/2026-10-12-network-transport-design.md`, and the work left over from it in `docs/superpowers/networking-parked.md`.

## Global Constraints

- Engine distances are "units"; never write "studs". Names are Anarchy's own, not Roblox's.
- Comments in the engine's voice: a sentence above the thing saying what and why. Match the surrounding code's idiom.
- `src/network` has no engine dependency. `engine_core`, `engine_instances` and `engine_services` never include `engine_network`. `AnarchyServer` must not link the renderer, JadeFX, or `studio_core`.
- Default per-client send budget: `1048576` bytes per second (`--max-send`). The flow control still halves it on loss over 10% or RTT over 250 ms.
- Exact copy:
  - Client reasons: "The server closed" (Ctrl+C or close) and "Removed by the server" (Connection destroyed). Existing reasons are unchanged.
  - Player overlay while connecting: "Connecting…".
  - Server log line: `[HH:MM:SS] text`.
- Defaults `--port 7777` and `--max-connections 64`, range 1-64.
- Build and test (Git Bash, from the worktree `.claude/worktrees/server-client-2c`):
  - Build: `"C:/Program Files/CMake/bin/cmake.exe" --build build --config Release --target sandbox > build/2c.log 2>&1; grep -E " error |error C|LNK" build/2c.log`
  - Run a tag: `./build/Release/sandbox.exe "[tag]"`. Never run sandbox.exe while building.
  - Known failures elsewhere, ignore them: `Q6` in `[project]` (fails on main), `S38` and `A34` (load-sensitive).
- Test names `<prefix><n> sentence`:
  - transport `N73`+ with tag `[network]`;
  - sessions `RP50`+ with tags `[replication][session]`;
  - server `SV1`+ with tag `[server]`;
  - player arguments `PL1`+ with tag `[player]`.
- Commit messages `Area: what changed, as a sentence`, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Explicit paths only; never add `.superpowers/`. Files are LF. Python is not on PATH.

## Review Focus

1. **A host name, not a literal address** (`--connect localhost:7777`, `myserver.lan:7777`): it resolves, preferring IPv4, and an unknown name is a clear error in the window (N80).
2. **Ctrl+C while a client is still joining:** the client ends with "The server closed" and the server exits promptly. No 5-second timeout and no hang (SV4 second section).
3. **A project with no gameId, a zero gameId, or an unreadable resources folder:** the server refuses to start and the player refuses to connect, each with the reason (SV2).
4. **A big join over a lossy link:** a 600 KB message under 10% loss arrives exactly once. It is slower, but never stuck, and the budget halves as flow control says (N75).
5. **A port already in use:** the second server prints why and exits non-zero (SV3).

---

### Task 1: Transport throughput: several packets per tick under a send budget

**Files:**
- Modify: `src/network/Connection.hpp/.cpp`, `src/network/Endpoint.cpp` (`send`), `src/network/ReliableChannel.hpp/.cpp`, `src/network/README.md`
- Modify: `src/engine_network/Sessions.hpp` (the default budget)
- Test: `sandbox/network_endpoint_tests.cpp` (N73-N76), `sandbox/network_reliable_tests.cpp` (N77)

**Interfaces:**
- Produces:
  ```cpp
  // Connection
  // Called once per Endpoint::send for a connected peer: adds
  // bytes_per_second() × (now − last refill) to the credit, capped at two ticks' worth
  // and never below one packet (kMaxPacketBytes).
  void refill(double now);
  // True while this tick may carry one more packet: reliable data is due, the
  // credit holds a full packet, and fewer than max_packets_per_tick() went this tick.
  bool wants_extra_packet(double now) const;
  // At most kRingSize / 2 packets in one round trip, never fewer than 1 or more than 32 a tick:
  // clamp(kRingSize / 2 / ceil(rtt() / send_interval_), 1, 32).
  std::size_t max_packets_per_tick() const;
  // engine_network/Sessions.hpp
  constexpr double kDefaultSendBudget = 1048576.0;   // bytes per second per client
  ```
  `SessionConfig::endpoint.bytes_per_second` defaults to `kDefaultSendBudget`; set it in the `SessionConfig` constructor or member initializer. `EndpointConfig`'s own default stays 32000.
- `ReliableChannel::has_due` returns at the first due entry instead of summing. A block keeps `first_open_fragment` (the lowest fragment not yet acked, advanced in `acked`), and `has_due`, `bytes_due` and `write` start their fragment walks there.

Rules:
- The first packet of a tick is unchanged: payload, keep-alive, and acks go as today, whatever the credit.
- Every packet written subtracts its length from the credit, which may go negative.
- `Endpoint::send` then loops `while (connection->wants_extra_packet(now))`: write the packet and send it. The payload only ever rides the first one.
- `wants_packet` also returns true when a packet arrived since the last send. That gives an ack every tick the peer is heard from, because 33 sequences of ack bits cannot cover several ticks of a 16-packet burst.

- [ ] **Step 1: Write the failing tests**

All four Endpoint tests use LinkHub endpoints stepped on a virtual clock at 60 Hz. `pump(server, client, ticks)` is the file's existing step helper; add it if missing.

- `N73 a 600 KB message reaches a client with a 1 MB/s budget in under a second`:
  - server `bytes_per_second = 1048576`;
  - `send_message` of 600 000 bytes;
  - `REQUIRE(delivered_at_tick <= 60)`;
  - the bytes delivered equal the bytes sent.
- `N74 the budget holds: the same message at 64 KB/s takes at least eight seconds`:
  - `bytes_per_second = 65536`;
  - `REQUIRE(delivered_at_tick >= 8 * 60)`.
- `N75 under 10% loss a 600 KB message arrives exactly once and flow control halves the budget`:
  - conditioner loss 0.10 on both sides;
  - one delivery, bytes equal;
  - at some tick `stats(peer).budget_bytes_per_second == Approx(1048576 * 0.5)`;
  - delivered within 30 s.
- `N76 packets in flight stay inside the ring: at 250 ms RTT a tick carries at most 8 packets`:
  - conditioner latency 0.125 s each way;
  - after RTT has settled (two seconds of pumping), count the server's `packets_sent` per tick while a 2 MB message drains: `REQUIRE(max_per_tick <= 8)`.
  - The client's `packets_sent` grows at least 55 in 60 ticks while it receives, which shows the ack-every-tick rule.

`N77 has_due and write start at the first open fragment` (ReliableChannel):
- send a 50-fragment block;
- ack fragments 0-39 through `acked`;
- `bytes_due` equals the bytes of fragments 40-49 only;
- a write records refs for fragments 40+ first.

- [ ] **Step 2: Run them; N73, N76 and N77 fail**

Run: `./build/Release/sandbox.exe "N73*,N74*,N75*,N76*,N77*"`
Expected: N73 fails (about 600 ticks), N76's ack-count assertion fails, and N77 fails to compile until `first_open_fragment` exists. N74 and N75 may pass already.

- [ ] **Step 3: Implement `refill`, `wants_extra_packet`, `max_packets_per_tick`, the extra-packet loop, the ack rule, the fragment cursor, and `kDefaultSendBudget`**

Keep the steady state allocation-free: N71 must still pass.

- [ ] **Step 4: Run `[network]`, `[replication]` and `[session]`**

Expected: all pass. N71, N72 and the soak tests are unchanged.

- [ ] **Step 5: Commit**

Message: `Network: a peer sends several packets a tick while reliable data is due, under a byte budget and an in-flight cap`

---

### Task 2: Transport leftovers: leave codes, close, IPv4 fallback, resolve, safe reads

**Files:**
- Modify: `src/network/Endpoint.hpp/.cpp`, `src/network/Packet.hpp` (if the code constant lives there), `src/network/Socket.hpp`, `src/network/UdpSocket.cpp`, `src/network/README.md`, `docs/superpowers/networking-parked.md` (strike what this task does)
- Test: `sandbox/network_endpoint_tests.cpp` (N78, N79, N81), `sandbox/network_socket_tests.cpp` (N80, N82)

**Interfaces:**
- Produces:
  ```cpp
  // Endpoint.hpp
  enum class DisconnectReason { None, Requested, Timeout, Denied, Socket, Closed, Removed };
  // Why a side is leaving, carried in its Disconnect packet's one trailing byte.
  enum class LeaveCode : std::uint8_t { Left = 0, Closed = 1, Removed = 2 };
  void disconnect(PeerId peer, double now, LeaveCode code = LeaveCode::Left);
  // Every peer is told Closed and dropped (on_disconnected fires for each, reason Requested on this side).
  void close(double now);
  // Visits each connected peer's id without allocating.
  template <class F> void for_each_peer(F&& visit) const;
  // Socket.hpp
  // "host:port": a literal, as Address::parse, or a name looked up with the
  // system resolver, preferring an IPv4 answer. nullopt with error worded for a player.
  std::optional<Address> resolve(std::string_view host_port, std::string& error);
  // UdpSocket
  static std::unique_ptr<UdpSocket> open_ipv4(std::uint16_t port, std::string& error);
  ```
- Receiving side of a Disconnect packet:
  - Left, or no code byte (an older peer), → `DisconnectReason::Requested`;
  - Closed → `Closed`;
  - Removed → `Removed`;
  - any other byte → `Requested`.
- `UdpSocket::open` falls back to `open_ipv4` when the IPv6 socket cannot be made or cannot clear `IPV6_V6ONLY`. On an IPv4 socket, a send to an IPv6 address returns false without failing the socket.
- `read_connection` clears `reading_` and carries out a pending drop even when a callback throws (a scope guard); the exception still propagates.
- `peers()` stays; `ServerSession` moves to `for_each_peer`.

- [ ] **Step 1: Write the failing tests**

- `N78 a server's close tells each client Closed, and a disconnect with Removed reaches the client as Removed`:
  - two clients join;
  - `server.disconnect(peer_of_a, now, LeaveCode::Removed)`, pump: A's `on_disconnected` reason is `Removed`;
  - `server.close(now)`, pump: B's reason is `Closed`, the server's peer count is 0, and nothing waits for a timeout (it happens within 1 tick).
- `N79 a Disconnect packet with no code byte reads as Requested`:
  - hand-build the old packet with the existing header writer;
  - the client's reason is `Requested`.
- `N80 resolve takes literals and names`:
  - `resolve("127.0.0.1:7777")` is `Address::loopback(7777)`;
  - `resolve("localhost:7777")` has port 7777 and family IPv4 or IPv6;
  - `resolve("no-such-host.invalid:7777")` is nullopt with a non-empty error;
  - `resolve("localhost")` (no port) is nullopt.
- `N81 a callback that throws leaves the endpoint usable`:
  - the client's `on_message` throws on the first message;
  - `REQUIRE_THROWS` around the pump;
  - then `disconnect(0, now)` drops the peer, `state(0) == Disconnected`, and a fresh `connect` succeeds.
- `N82 an IPv4-only socket sends and receives on loopback`:
  - two `open_ipv4(0, error)` sockets exchange a datagram;
  - a send to `Address::parse("[::1]:5")` returns false and `failed()` stays false.

- [ ] **Step 2: Run them; they fail to compile**

Run: `./build/Release/sandbox.exe "N78*,N79*,N80*,N81*,N82*"`
Expected: build errors on `LeaveCode`, `close`, `resolve` and `open_ipv4`.

- [ ] **Step 3: Implement**

Use `getaddrinfo` with `AF_UNSPEC` and `SOCK_DGRAM`. Take the first IPv4 answer, else the first IPv6. Split the host from the port at the last `:` (a `[v6]:p` literal goes through `parse`). Winsock is already started by the socket code; reuse its start-up helper.

- [ ] **Step 4: Run `[network]`, `[replication]` and `[session]`**

Expected: all pass.

- [ ] **Step 5: Commit**

Message: `Network: leave codes say why a peer left, close tells every peer, names resolve, and a socket falls back to IPv4`

---

### Task 3: Sessions: kick, closed, a thread-safe status, an empty client world, and the identity of a project

**Files:**
- Modify: `src/engine_network/Sessions.hpp`, `ServerSession.cpp`, `ClientSession.cpp`, `src/engine_network/README.md`
- Create: `src/engine_network/ClientWorld.hpp/.cpp`, `src/engine_network/ProjectIdentity.hpp/.cpp` (both added to the `engine_network` target)
- Modify: `src/engine_core/Engine.hpp/.cpp` (`network_now` public and static), `sandbox/replication_engine_tests.cpp` (RP48 uses it)
- Test: `sandbox/replication_session_tests.cpp` (RP50-RP53), `sandbox/replication_client_world_tests.cpp` (RP54-RP55, new; add it to the sandbox list)

**Interfaces:**
- Produces:
  ```cpp
  // Engine.hpp: the clock a session's now is on, seconds on the steady clock.
  static double network_now();
  // Sessions.hpp
  struct ClientStatus { ClientState state = ClientState::Idle; std::string reason; };
  // Any thread: the state and reason as of the last receive or send.
  ClientStatus ClientSession::status() const;
  // ClientWorld.hpp: destroys every instance a server would replicate (every
  // non-service descendant of game, outside Core), leaving the services and
  // their properties, so a client starts from what the snapshot binds.
  void clear_for_client(engine_core::DataModel& game);
  // ProjectIdentity.hpp: the identity a server or client of this project
  // presents: game_id parsed from project.gameId, version, and
  // hash_resources(project.resources_root()). False, with error worded for a
  // person, when gameId is missing, malformed, or all zeros, or the hash is 0.
  bool project_identity(const engine_core::Project& project, network::GameIdentity& out, std::string& error);
  ```
- `ServerSession::send` first disconnects, with `LeaveCode::Removed`, every peer whose Connection is no longer alive. Then it runs its existing work.
- `ServerSession::close` uses `endpoint_->close(now)`.
- `ClientSession::describe` maps `Closed` to "The server closed" and `Removed` to "Removed by the server".
- `status()` reads under a mutex that every write of `state_` or `reason_` takes. `state()` and `reason()` remain for the simulation thread.

- [ ] **Step 1: Write the failing tests**

- `RP50 a server script destroying a client's Connection removes that client`:
  - two clients joined;
  - on the server, `destroy` A's Connection (`connection_of(peer)`), then pump;
  - A's `state()` is Disconnected and its `reason()` is "Removed by the server";
  - B is still Joined, and B's world no longer has A's Connection.
- `RP51 closing the server tells each client "The server closed" at once`:
  - `server.close(now)`, then pump one tick;
  - every client's reason is "The server closed" and its state is Disconnected.
- `RP52 status() matches state() and reason() through a whole connection`:
  - sample `status()` at Connecting, Joined and Disconnected (wrong-game deny);
  - each time it equals `{state(), reason()}`.
- `RP53 status() is safe from another thread`:
  - a `std::thread` reads `status()` in a loop while the main thread connects, joins and disconnects 20 times;
  - no crash, and the final read is Disconnected.
- `RP54 clear_for_client leaves only services`:
  - build a game with a Folder in Workspace, a GameObject in ClientStorage, a Folder in ServerStorage, and a Script in Workspace;
  - after `clear_for_client`, every service still exists with its properties (Lighting `Exposure` unchanged);
  - no non-service instance remains outside Core, and the Core children count is unchanged.
- `RP55 project_identity refuses what cannot identify a game`:
  - `Project::create` into a temp folder: true, a 16-byte id, and `resources_hash != 0`;
  - with `gameId` written as 32 zeros in `project.json`, then reloaded: false, and the error mentions "gameId";
  - `resources_root` removed: false, and the error mentions "resources".

- [ ] **Step 2: Run them; they fail to compile**

Run: `./build/Release/sandbox.exe "RP5*"`
Expected: build errors on `status`, `clear_for_client`, `project_identity` and `network_now`.

- [ ] **Step 3: Implement**

`clear_for_client` destroys deepest first with `destroy_tree` on each top-level non-service child, checked with `destroy_error`.

- [ ] **Step 4: Run `[replication]`, `[session]` and `[network]`**

Expected: all pass, including RP48 on `Engine::network_now()`.

- [ ] **Step 5: Commit**

Message: `Network: destroying a client's Connection removes it, a closing server says so, and a client's status can be read from any thread`

---

### Task 4: AnarchyServer

**Files:**
- Create: `src/server/ServerHost.hpp/.cpp`, `src/server/main.cpp`, `src/server/README.md`
- Modify: `CMakeLists.txt`:
  - add the target `AnarchyServer` (console app; links `engine_network`, `engine_core`, `anarchy_network`, and compiles `src/runner/GamePack.cpp` with what it needs, not `studio_core`);
  - add it to the warnings list and to `add_dependencies(AnarchyStudio AnarchyServer)`;
  - add `src/server/ServerHost.cpp` to the sandbox sources.
- Test: `sandbox/server_tests.cpp` (SV1-SV5, new; add it to the sandbox list)

**Interfaces:**
- Consumes: `project_identity`, `ServerSession`, `kDefaultSendBudget`, `UdpSocket::open`, `Endpoint::close`, `Engine::network_now`.
- Produces:
  ```cpp
  namespace server {
  struct ServerOptions {
      std::filesystem::path project;   // a project folder; empty: the game packed into pack_file
      std::filesystem::path pack_file; // the file holding a packed game (main: this program)
      std::uint16_t port = 7777;       // 0: any free port (tests)
      std::uint8_t max_connections = 64;
      double max_send = engine_network::kDefaultSendBudget;
  };
  // args excludes the program name. Flags: --port N, --max-connections N (1-64),
  // --max-send BYTES (> 0); one positional project folder at most. nullopt with
  // error, the usage line included, on anything else.
  std::optional<ServerOptions> parse_server_args(const std::vector<std::string>& args, std::string& error);
  // A loaded project in a headless engine (no renderer, no audio device, no
  // client sync, paced at 60 Hz, role Server) with a ServerSession on a UDP socket.
  class ServerHost {
  public:
      // Loads, checks identity, opens the socket. Null with error when any fails.
      static std::unique_ptr<ServerHost> open(const ServerOptions& options, std::string& error);
      ~ServerHost();                     // stop() if still running
      void start();                      // starts play and the engine thread
      // Closes every client ("The server closed"), stops the engine, unhooks the session. Idempotent.
      void stop();
      std::uint16_t port() const;        // the bound port
      engine_core::Engine& engine();
      engine_network::ServerSession& session();
      // Writes the scripts' pending output as "[HH:MM:SS] text" lines; errors to err.
      void print_output(std::FILE* out, std::FILE* err);
  };
  }  // namespace server
  ```
- `stop()` runs `session.close(Engine::network_now())` inside `engine.on_simulation` and waits for it. Then it calls `engine.stop()`, then `engine.set_network(nullptr)`.
- A packed game is unpacked like the player does: `runner::find_game_pack`, then `runner::unpack_game` into `temp/AnarchyEngine/servers`. `main` sets `pack_file` to its own path (`GetModuleFileNameW`, `/proc/self/exe`, or `_NSGetExecutablePath`) when no folder is given.
- `main`:
  1. Parse the arguments. On an error, print it to stderr and return 2.
  2. `open`. On an error, print it to stderr and return 1.
  3. Print `Serving <project name> on port <p>`.
  4. Install Ctrl+C and SIGTERM handling (`SetConsoleCtrlHandler`, or `signal`) that sets an atomic flag.
  5. `start()`.
  6. Loop: `print_output`, then wait 50 ms on the flag.
  7. `stop()`, a final `print_output`, then return 0.

- [ ] **Step 1: Write the failing tests**

- `SV1 parse_server_args reads defaults and flags, and refuses the rest`:
  - `{}` gives port 7777, 64 connections and budget 1048576;
  - `{"proj", "--port", "9000", "--max-connections", "8", "--max-send", "500000"}` gives each value;
  - each of these is nullopt with the error containing "Usage": `--port 70000`, `--port`, `--max-connections 0`, `--max-connections 65`, `--max-send 0`, `--bogus`, and two folders.
- `SV2 a project that cannot identify its game is refused`:
  - a temp project with a zeroed gameId gives `open` null, with the error containing "gameId";
  - a missing folder gives null, with an error.
- `SV3 a port in use is refused`:
  - `open` A on port 0;
  - `open` B on `A->port()` gives null with a non-empty error.
- `SV4 a server and a real UDP client: a server script's change arrives, and stopping says "The server closed"`:
  - `Project::create` in a temp folder;
  - add a Server-context Script in Workspace with source `local f = Instance.new("Folder") f.Name = "FromServer" f.Parent = workspace`;
  - `project.save()`;
  - `open` on port 0, then `start()`;
  - a client `Engine` (never started) with a `ClientSession` on `UdpSocket::open(0)` and the same `project_identity` connects to `Address::loopback(host->port())` at `Engine::network_now()`;
  - step the client with `run_frame(1/60)` until Joined and Workspace has "FromServer", with a 5 s real-time limit;
  - `host->stop()`;
  - step the client until Disconnected, with a 2 s limit;
  - `reason() == "The server closed"`.
  - Second section: `stop()` right after the client reaches Joining. The client still ends with "The server closed", within 2 s.
- `SV5 a game packed into a file serves as its folder would`:
  - pack the SV4 project with `ide::collect_export_files`-equivalent `runner::PackFile`s, or `runner::write_game_pack` over any small existing file, into `temp/x.bin`;
  - `ServerOptions{.pack_file = temp/x.bin}` with an empty project: `open` succeeds;
  - `session()`'s identity equals the folder's `project_identity`.

- [ ] **Step 2: Run them; they fail to compile**

Run: `./build/Release/sandbox.exe "[server]"`

- [ ] **Step 3: Implement `ServerHost`, `parse_server_args`, `main`, and the CMake target**

- [ ] **Step 4: Run `[server]`, `[replication]` and `[session]`, then build `AnarchyServer`**

Build: `"C:/Program Files/CMake/bin/cmake.exe" --build build --config Release --target AnarchyServer`.
Then:
- `./build/Release/AnarchyServer.exe --bogus; echo $?` prints the usage and `2`.
- `./build/Release/AnarchyServer.exe /nonexistent; echo $?` prints the reason and `1`.

- [ ] **Step 5: Commit**

Message: `Server: AnarchyServer runs a project headless and serves it to clients until Ctrl+C`

---

### Task 5: AnarchyPlayer --connect

**Files:**
- Create: `src/player/PlayerArgs.hpp/.cpp` (added to the AnarchyPlayer sources and to the sandbox sources)
- Modify: `src/player/main.cpp`, `CMakeLists.txt` (AnarchyPlayer links `engine_network`)
- Test: `sandbox/player_args_tests.cpp` (PL1, new; add it to the sandbox list)

**Interfaces:**
- Consumes: `resolve`, `project_identity`, `clear_for_client`, `ClientSession`, `ClientSession::status`, `kDefaultSendBudget`, `Engine::network_now`.
- Produces:
  ```cpp
  namespace player {
  struct PlayerArgs {
      std::string connect;              // "host:port", empty: play standalone
      std::filesystem::path folder;     // empty: the packed game
  };
  // argv as main gets it. Ignores macOS -psn_ arguments. nullopt with error on
  // --connect without a value or an unknown -- flag.
  std::optional<PlayerArgs> parse_player_args(int argc, char** argv, std::string& error);
  }
  ```

Player behaviour with `--connect`:
1. Find the game as today, from the folder or the pack, and `Project::load` it.
2. `project_identity`. On an error, show the message screen.
3. `resolve(connect)`. On an error, show the message screen with "Could not find the server: <error>".
4. `clear_for_client(game)`.
5. Make a `ClientSession` on `UdpSocket::open(0)`, with `kDefaultSendBudget` and that identity, and `engine.set_network(session)`.
6. Do not install the standalone `on_simulation` start-play. Instead `engine.on_simulation([..]{ session->connect(address, Engine::network_now()); })` once, then `engine.resume()`.
7. The scene root is a `StackPane` holding the `GameView` and a centred overlay `VBox`, styled `player-message` and holding one Label. The overlay shows "Connecting…" from the start.
8. The frame tail reads `status()`:
   - Connecting or Joining: the overlay is visible with "Connecting…";
   - Joined: the overlay is hidden;
   - Disconnected: the overlay is visible with the reason.
9. Shutdown, in `~AnarchyPlayer` before members are destroyed: `engine.stop()`, then `engine.set_network(nullptr)`, then reset the session (the Game outlives it).

Without `--connect`, the player behaves exactly as today.

- [ ] **Step 1: Write the failing test**

`PL1 parse_player_args reads a folder and --connect in either order`:
- `{"AnarchyPlayer"}` gives an empty connect and an empty folder;
- `{"AnarchyPlayer", "game"}` gives folder "game";
- `{"AnarchyPlayer", "--connect", "host:7777"}` gives connect "host:7777";
- `{"AnarchyPlayer", "--connect", "h:1", "game"}` and `{"AnarchyPlayer", "game", "--connect", "h:1"}` both give both;
- `{"AnarchyPlayer", "-psn_0_123"}` is ignored;
- `{"AnarchyPlayer", "--connect"}` is nullopt with an error;
- `{"AnarchyPlayer", "--bogus"}` is nullopt with an error.

- [ ] **Step 2: Run it; it fails to compile**

Run: `./build/Release/sandbox.exe "[player]"`

- [ ] **Step 3: Implement `parse_player_args` and the connect mode in `main.cpp`**

`FindGame` takes `PlayerArgs::folder` instead of reading `argv[1]`.

- [ ] **Step 4: Run `[player]`, then build AnarchyPlayer and AnarchyServer**

Build: `"C:/Program Files/CMake/bin/cmake.exe" --build build --config Release --target AnarchyPlayer AnarchyServer`
Expected: no errors. The real connect is checked by hand in Task 7.

- [ ] **Step 5: Commit**

Message: `Player: AnarchyPlayer --connect joins a server, shows "Connecting…" until the world arrives, and says why it ends`

---

### Task 6: Exported games carry their server

**Files:**
- Modify: `src/ide/GameExport.hpp/.cpp`, `CMakeLists.txt` (on a Mac, copy `AnarchyServer` into the studio bundle's Resources beside the player, as the player's post-build does)
- Test: `tests/GamePackTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // The server program export copies, found as find_player finds the player. Empty when it is not there.
  std::filesystem::path find_server();
  // Where export writes the server for a game written to game: beside it, named
  // "<game stem> Server" with .exe on Windows ("Maze.exe" → "Maze Server.exe";
  // on a Mac, "Maze.app" → "Maze Server", a plain program beside the bundle).
  std::filesystem::path server_path(const std::filesystem::path& game);
  ```
- `export_game` packs the same files into a copy of the server with `runner::write_game_pack(server, files, server_path(output), error)` after the game. A missing server fails the export with "AnarchyServer was not found beside the studio. Build it with the studio.", and checks that before writing anything.

- [ ] **Step 1: Write the failing test**

In `tests/GamePackTest.cpp`:
- `server_path` maps `"/out/Maze.exe"` to `"/out/Maze Server.exe"` on Windows, `"/out/Maze.app"` to `"/out/Maze Server"` on a Mac, and `"/out/Maze"` to `"/out/Maze Server"` on Linux. Use `#if` per platform as the file's existing name tests do.
- After `runner::write_game_pack` of a file into a server copy, `runner::find_game_pack` finds the pack in it. The pack is the same as the game's.

- [ ] **Step 2: Run it; it fails to compile**

Run: build and run the `tests` executable that holds `GamePackTest.cpp` (its CMake target; find it beside `tests/GamePackTest.cpp` in `CMakeLists.txt`).

- [ ] **Step 3: Implement `find_server`, `server_path`, and the export step**

- [ ] **Step 4: Run the GamePack tests and `[server]`**

Expected: pass.

- [ ] **Step 5: Commit**

Message: `Export: a game is exported with its server beside it, reading the same pack`

---

### Task 7: Whole-suite check, docs, and a hand test

**Files:**
- Modify:
  - `docs/superpowers/networking-parked.md`: strike what 2c did, and keep the rest;
  - `docs/superpowers/specs/2026-10-12-networking-roadmap-design.md`: mark sub-project 2 done;
  - `src/server/README.md`: usage, flags, exit codes, packed mode.

- [ ] **Step 1: Run the full sandbox**

Run: `./build/Release/sandbox.exe`
Expected: only Q6 fails. S38 and A34 may flake; rerun them alone.

- [ ] **Step 2: Build every Release target**

Run: `"C:/Program Files/CMake/bin/cmake.exe" --build build --config Release > build/2c-all.log 2>&1; grep -E " error |error C|LNK" build/2c-all.log`
Expected: no output.

- [ ] **Step 3: Commit the docs**

Message: `Docs: sub-project 2 is done; the parked list keeps what is left`

- [ ] **Step 4: Hand test, with the user** (the controller does this step, not a subagent)

On a small sample project:
1. Start `AnarchyServer <project>`.
2. Start two `AnarchyPlayer --connect localhost:7777 <project>`. Both show "Connecting…", then the world. A server script's change is visible in both.
3. Ctrl+C the server. Both players show "The server closed".
4. Restart the server. Connect with a player pointed at another project: "This server runs a different game".
