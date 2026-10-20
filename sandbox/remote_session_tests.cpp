// RemoteEvents between a server and its clients over the transport: who gets
// what, in what order, after which tree changes, and what a hostile client
// cannot do (docs/superpowers/specs/2026-10-20-remote-events-design.md).

#include "replication_support.hpp"

#include "ByteBuffer.hpp"
#include "Connections.hpp"
#include "Folder.hpp"
#include "GameIdentity.hpp"
#include "RemoteCodec.hpp"
#include "RemoteEvent.hpp"
#include "RemoteValues.hpp"
#include "Sessions.hpp"
#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using engine_core::InstanceId;

namespace {

network::GameIdentity identity() {
    network::GameIdentity id;
    network::parse_game_id("0123456789abcdef0123456789abcdef", id.game_id);
    id.resources_hash = 7;
    return id;
}

const network::Address kServer = network::Address::loopback(1);
network::Address client_address(int n) { return network::Address::loopback(static_cast<std::uint16_t>(100 + n)); }

struct Client {
    ScriptRig rig;
    std::unique_ptr<engine_network::ClientSession> session;
};

// A server world and its clients, each world stepped by hand as an engine steps it.
struct Net {
    network::LinkHub hub;
    ScriptRig server;
    std::unique_ptr<engine_network::ServerSession> session;
    std::deque<std::unique_ptr<Client>> clients;
    double now = 0.0;

    explicit Net(engine_network::SessionConfig config = {}) {
        config.endpoint.identity = identity();
        replication::start_server(server);
        session = std::make_unique<engine_network::ServerSession>(server.game, hub.open(kServer), config);
    }
    Client& add_client(engine_network::SessionConfig config = {}) {
        clients.push_back(std::make_unique<Client>());
        Client& client = *clients.back();
        config.endpoint.identity = identity();
        client.session = std::make_unique<engine_network::ClientSession>(
            client.rig.game, hub.open(client_address(static_cast<int>(clients.size()) - 1)), config);
        REQUIRE(client.session->connect(kServer, now));
        return client;
    }
    void tick() {
        now += 1.0 / 60.0;
        session->receive(now);
        for (auto& c : clients) if (c->session) c->session->receive(now);
        server.frames(1);
        for (auto& c : clients) c->rig.frames(1);
        session->send(now);
        for (auto& c : clients) if (c->session) c->session->send(now);
    }
    void run(double seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 60.0); ++i) tick();
    }
    InstanceId workspace() { return server.game.scene_service("Workspace"); }
};

engine_core::RemoteEvent& add_remote(engine_core::DataModel& game, InstanceId parent, const char* name) {
    auto& remote = game.create<engine_core::RemoteEvent>();
    game.set_name(remote.id(), name);
    game.set_parent(remote.id(), parent);
    return remote;
}

engine_core::Folder& add_folder(engine_core::DataModel& game, InstanceId parent, const char* name) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder;
}

// A Server script in the server's Workspace: it runs there and never on a client.
void server_script(Net& net, const char* name, const char* source) {
    engine_core::Script& script = add_script(net.server.game, name, source);
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
}

// A Client script in the server's Workspace: it replicates and runs on each client.
void client_script(Net& net, const char* name, const char* source) {
    engine_core::Script& script = add_script(net.server.game, name, source);
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
}

bool boolean(ScriptRig& rig, const char* name) {
    INFO(name << ": " << rig.runtime.last_error());
    bool value = false;
    return rig.runtime.global_boolean(name, value) && value;
}

double number(ScriptRig& rig, const char* name) {
    INFO(name << ": " << rig.runtime.last_error());
    double value = 0;
    REQUIRE(rig.runtime.global_number(name, value));
    return value;
}

std::string text(ScriptRig& rig, const char* name) {
    INFO(name << ": " << rig.runtime.last_error());
    std::string value;
    REQUIRE(rig.runtime.global_string(name, value));
    return value;
}

std::string local_connection_id(Client& client) {
    engine_core::DataModel& game = client.rig.game;
    const auto* connections = static_cast<engine_core::Connections*>(game.instance(game.connections()));
    const InstanceId local = connections->local_connection();
    REQUIRE(local != 0);
    return static_cast<engine_core::ConnectionInstance*>(game.instance(local))->connection_id();
}

std::string server_connection_id(Net& net, network::PeerId peer) {
    const InstanceId id = net.session->connection_of(peer);
    REQUIRE(id != 0);
    return static_cast<engine_core::ConnectionInstance*>(net.server.game.instance(id))->connection_id();
}

// A raw client: an Endpoint that speaks the transport and nothing else, so it
// can send whatever bytes it likes.
struct RawClient {
    network::Endpoint endpoint;
    bool connected = false;

    RawClient(network::LinkHub& hub, int n, double bytes_per_second = 1048576.0)
        : endpoint(hub.open(client_address(n)), config(bytes_per_second), callbacks()) {}

    static network::EndpointConfig config(double bytes_per_second) {
        network::EndpointConfig config;
        config.role = network::Role::Client;
        config.identity = identity();
        config.bytes_per_second = bytes_per_second;
        return config;
    }
    network::EndpointCallbacks callbacks() {
        network::EndpointCallbacks callbacks;
        callbacks.on_connected = [this](network::PeerId) { connected = true; };
        callbacks.on_disconnected = [this](network::PeerId, network::DisconnectReason, network::DenyReason) {
            connected = false;
        };
        return callbacks;
    }
    void send(const std::vector<std::uint8_t>& bytes) { REQUIRE(endpoint.send_message(0, bytes.data(), bytes.size())); }
};

// Server and raw client stepped together.
void pump(Net& net, RawClient& raw, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        net.now += 1.0 / 60.0;
        net.session->receive(net.now);
        raw.endpoint.receive(net.now);
        net.server.frames(1);
        net.session->send(net.now);
        raw.endpoint.send(net.now);
    }
}

engine_core::RemoteValue number_value(double n) {
    engine_core::RemoteValue value;
    value.slot.kind = engine_core::LuaSlot::Kind::Number;
    value.slot.number = n;
    return value;
}

engine_core::RemoteValue string_value(std::string s) {
    engine_core::RemoteValue value;
    value.slot.kind = engine_core::LuaSlot::Kind::String;
    value.slot.text = std::move(s);
    return value;
}

std::vector<std::uint8_t> remote_message(InstanceId remote, std::vector<engine_core::RemoteValue> values) {
    engine_core::RemoteValues payload;
    payload.values = std::move(values);
    engine_network::ByteWriter out;
    engine_network::encode_remote(remote, payload, [](InstanceId id) { return id; }, out);
    return out.data();
}

}  // namespace

TEST_CASE("RE30 a client's FireServer reaches OnServerEvent with its Connection", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Shout");
    server_script(net, "Hear",
        "workspace.Shout.OnServerEvent:Connect(function(connection, ...)\n"
        "    local a, b, c = ...\n"
        "    _G.id = connection.Id\n"
        "    _G.is_connection = connection:IsA('Connection')\n"
        "    _G.count = select('#', ...)\n"
        "    _G.ok = a == 'hi' and b == Vector3.new(1, 2, 3) and c == nil\n"
        "end)\n");
    client_script(net, "Say", "workspace.Shout:FireServer('hi', Vector3.new(1, 2, 3), nil)\n");
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    REQUIRE(text(net.server, "id") == server_connection_id(net, 0));
    REQUIRE(text(net.server, "id") == local_connection_id(client));
    REQUIRE(boolean(net.server, "is_connection"));
    REQUIRE(number(net.server, "count") == 3);
    REQUIRE(boolean(net.server, "ok"));
    REQUIRE(net.session->dropped_remotes() == 0);
}

TEST_CASE("RE31 FireClient, FireClientExt and FireClientAll reach the right clients", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk");
    // Once both clients say they listen, the server sends each kind once.
    server_script(net, "Answer",
        "local r = workspace.Talk\n"
        "local ready = {}\n"
        "r.OnServerEvent:Connect(function(connection, what)\n"
        "    if what ~= 'ready' then return end\n"
        "    table.insert(ready, connection)\n"
        "    if #ready == 2 then\n"
        "        local a, b = ready[1], ready[2]\n"
        "        _G.a_id = a.Id\n"
        "        r:FireClient(a, 'one', nil)\n"
        "        r:FireClientExt({a, b}, 'ext', nil, nil)\n"
        "        r:FireClientAll('all')\n"
        "    end\n"
        "end)\n");
    client_script(net, "Listen",
        "local r = workspace.Talk\n"
        "_G.log = ''\n"
        "r.OnClientEvent:Connect(function(...)\n"
        "    local what = ...\n"
        "    _G.log = _G.log .. what .. select('#', ...) .. ';'\n"
        "end)\n"
        "r:FireServer('ready')\n");
    Client& first = net.add_client();
    Client& second = net.add_client();
    net.run(1.5);
    const std::string a_id = text(net.server, "a_id");
    Client& a = local_connection_id(first) == a_id ? first : second;
    Client& b = &a == &first ? second : first;
    REQUIRE(local_connection_id(a) == a_id);
    REQUIRE(text(a.rig, "log") == "one2;ext3;all1;");
    REQUIRE(text(b.rig, "log") == "ext3;all1;");
}

TEST_CASE("RE32 the wrong side raises", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk");
    const char* says =
        "local function says(expected, f)\n"
        "    local ok, err = pcall(f)\n"
        "    return not ok and string.find(tostring(err), expected, 1, true) ~= nil\n"
        "end\n";
    server_script(net, "OnServer",
        (std::string(says) +
         "_G.server = says('FireServer is for clients; the server sends with FireClient, FireClientExt or "
         "FireClientAll', function() workspace.Talk:FireServer(1) end)\n").c_str());
    client_script(net, "OnClient",
        (std::string(says) +
         "local connection = game:GetService('Connections').LocalConnection\n"
         "_G.client = says('FireClient is for the server; a client sends with FireServer', "
         "function() workspace.Talk:FireClient(connection, 1) end)\n").c_str());
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(boolean(net.server, "server"));
    REQUIRE(boolean(client.rig, "client"));
}

TEST_CASE("RE33 a reliable message fired in the same tick as a create finds the instance", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk");
    server_script(net, "Make",
        "local r = workspace.Talk\n"
        "r.OnServerEvent:Connect(function()\n"
        "    local ball = Instance.new('Folder')\n"
        "    ball.Name = 'Ball'\n"
        "    ball.Parent = workspace\n"
        "    r:FireClientAll(ball)\n"
        "end)\n");
    client_script(net, "Catch",
        "local r = workspace.Talk\n"
        "r.OnClientEvent:Connect(function(ball)\n"
        "    _G.got = ball ~= nil and ball.Name == 'Ball' and ball.Parent == workspace\n"
        "    _G.ran = true\n"
        "end)\n"
        "r:FireServer()\n");
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(boolean(client.rig, "ran"));
    REQUIRE(boolean(client.rig, "got"));
}

TEST_CASE("RE34 a message to a joining client arrives after its snapshot, once", "[remote][session]") {
    engine_network::SessionConfig config;
    config.replicator.snapshot_part_bytes = 1024;   // a snapshot several ticks long
    config.endpoint.bytes_per_second = 16000.0;     // a low budget for each client
    Net net(config);
    add_remote(net.server.game, net.workspace(), "Talk");
    client_script(net, "Count",
        "_G.count = 0\n"
        "workspace.Talk.OnClientEvent:Connect(function(n)\n"
        "    _G.count += 1\n"
        "    _G.folders = #workspace:GetChildren()\n"
        "    _G.n = n\n"
        "end)\n");
    for (int i = 0; i < 400; ++i) {
        add_folder(net.server.game, net.workspace(), ("Folder number " + std::to_string(i * 7919)).c_str());
    }
    SECTION("fired while its snapshot crosses") {
        Client& client = net.add_client();
        bool fired = false;
        for (int i = 0; i < 600 && !fired; ++i) {
            net.tick();
            // The tick it connected queued its snapshot.
            if (net.session->connection_of(0) != 0 &&
                client.session->state() == engine_network::ClientState::Joining) {
                add_script(net.server.game, "Fire", "workspace.Talk:FireClientAll(1)");
                fired = true;
            }
        }
        REQUIRE(fired);
        net.run(0.1);
        REQUIRE(client.session->state() == engine_network::ClientState::Joining);
    }
    SECTION("fired in the tick it connects, before its snapshot is queued") {
        // Heartbeat runs between the receive that made the Connection and the
        // send that queues its snapshot.
        server_script(net, "Greet",
            "local seen = {}\n"
            "game:GetService('RunService').Heartbeat:Connect(function()\n"
            "    for _, connection in game:GetService('Connections'):GetChildren() do\n"
            "        if not seen[connection] then\n"
            "            seen[connection] = true\n"
            "            workspace.Talk:FireClient(connection, 1)\n"
            "            _G.fired = true\n"
            "        end\n"
            "    end\n"
            "end)\n");
        net.run(0.1);
        net.add_client();
        for (int i = 0; i < 600 && net.session->connection_of(0) == 0; ++i) {
            net.tick();
        }
        REQUIRE(net.session->connection_of(0) != 0);
        REQUIRE(boolean(net.server, "fired"));
    }
    Client& client = *net.clients.back();
    net.run(5.0);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    REQUIRE(number(client.rig, "count") == 1);
    REQUIRE(number(client.rig, "n") == 1);
    // Every folder, the remote, and the script were there when it ran.
    REQUIRE(number(client.rig, "folders") >= 402);
}

TEST_CASE("RE35 instances the other side lacks read nil", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk");
    add_folder(net.server.game, net.server.game.service("ServerStorage"), "Secret");
    server_script(net, "Answer",
        "local r = workspace.Talk\n"
        "r.OnServerEvent:Connect(function(connection, ...)\n"
        "    local f, tag = ...\n"
        "    _G.count = select('#', ...)\n"
        "    _G.local_nil = f == nil and tag == 'mine'\n"
        "    r:FireClient(connection, game:GetService('ServerStorage').Secret, 'secret')\n"
        "end)\n");
    client_script(net, "Ask",
        "local r = workspace.Talk\n"
        "r.OnClientEvent:Connect(function(...)\n"
        "    local f, tag = ...\n"
        "    _G.count = select('#', ...)\n"
        "    _G.secret_nil = f == nil and tag == 'secret'\n"
        "end)\n"
        "local mine = Instance.new('Folder')\n"
        "mine.Parent = workspace\n"
        "r:FireServer(mine, 'mine')\n");
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(boolean(net.server, "local_nil"));
    REQUIRE(number(net.server, "count") == 2);
    REQUIRE(boolean(client.rig, "secret_nil"));
    REQUIRE(number(client.rig, "count") == 2);
}

TEST_CASE("RE36 an oversized reliable message raises on the sender", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk");
    server_script(net, "Hear",
        "workspace.Talk.OnServerEvent:Connect(function() _G.heard = true end)\n"
        "local ok, err = pcall(function() workspace.Talk:FireClientAll(string.rep('x', 300000)) end)\n"
        "_G.server = not ok and string.find(tostring(err), 'FireClientAll message is over 262144', 1, true) ~= nil\n");
    client_script(net, "Big",
        "local ok, err = pcall(function() workspace.Talk:FireServer(string.rep('x', 300000)) end)\n"
        "_G.client = not ok and string.find(tostring(err), 'FireServer message is over 262144', 1, true) ~= nil\n");
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(boolean(client.rig, "client"));
    REQUIRE(boolean(net.server, "server"));
    REQUIRE(net.server.runtime.global_is_nil("heard"));
}

TEST_CASE("RE37 messages on one remote and across remotes keep their order", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "A");
    add_remote(net.server.game, net.workspace(), "B");
    server_script(net, "Log",
        "_G.log = {}\n"
        "local function hear(connection, i) table.insert(_G.log, i) end\n"
        "workspace.A.OnServerEvent:Connect(hear)\n"
        "workspace.B.OnServerEvent:Connect(hear)\n"
        "_G.in_order = function()\n"
        "    if #_G.log ~= 100 then return false end\n"
        "    for i = 1, 100 do if _G.log[i] ~= i then return false end end\n"
        "    return true\n"
        "end\n");
    client_script(net, "Send",
        "for i = 1, 100 do\n"
        "    if i % 3 == 0 then workspace.B:FireServer(i) else workspace.A:FireServer(i) end\n"
        "end\n");
    net.add_client();
    net.run(1.5);
    add_script(net.server.game, "Check", "_G.ordered = _G.in_order()");
    net.run(0.1);
    REQUIRE(boolean(net.server, "ordered"));
}

TEST_CASE("RE38 hostile client messages are dropped and counted", "[remote][session]") {
    Net net;
    const engine_core::RemoteEvent& open = add_remote(net.server.game, net.workspace(), "Open");
    const engine_core::RemoteEvent& hidden =
        add_remote(net.server.game, net.server.game.service("ServerStorage"), "Hidden");
    server_script(net, "Hear",
        "_G.open = 0\n"
        "workspace.Open.OnServerEvent:Connect(function(connection, n) _G.open += 1 _G.last = n end)\n"
        "game:GetService('ServerStorage').Hidden.OnServerEvent:Connect(function() _G.hidden = true end)\n");
    RawClient raw(net.hub, 0);
    REQUIRE(raw.endpoint.connect(kServer, net.now));
    pump(net, raw, 30);
    REQUIRE(raw.connected);
    const std::size_t messages = net.session->dropped_messages();

    // Garbage behind a Remote type byte.
    raw.send({static_cast<std::uint8_t>(engine_network::MessageType::Remote), 0xFF, 0xFF, 0xFF, 0xFF, 0x13, 0x37});
    // A remote clients were never sent.
    raw.send(remote_message(hidden.id(), {number_value(1)}));
    // Tables 33 deep.
    {
        engine_core::RemoteValue deep = number_value(1);
        for (int level = 0; level < 33; ++level) {
            auto table = std::make_shared<engine_core::RemoteTable>();
            table->array.push_back(deep);
            engine_core::RemoteValue outer;
            outer.table = std::move(table);
            deep = outer;
        }
        raw.send(remote_message(open.id(), {deep}));
    }
    // Over the reliable cap.
    raw.send(remote_message(open.id(), {string_value(std::string(300000, 'x'))}));
    pump(net, raw, 120);
    REQUIRE(net.session->dropped_remotes() == 4);
    REQUIRE(raw.connected);
    REQUIRE(net.session->connection_of(0) != 0);
    REQUIRE(number(net.server, "open") == 0);
    REQUIRE(net.server.runtime.global_is_nil("hidden"));

    // And the server still hears it.
    raw.send(remote_message(open.id(), {number_value(5)}));
    pump(net, raw, 30);
    REQUIRE(number(net.server, "open") == 1);
    REQUIRE(number(net.server, "last") == 5);
    REQUIRE(net.session->dropped_remotes() == 4);
    REQUIRE(net.session->dropped_messages() == messages);
}

TEST_CASE("RE39 a flood past the per-receive cap is cut off", "[remote][session]") {
    // The cap is 4 MiB a receive; this transport's in-flight limits keep a
    // burst that size from landing in one receive here, so the test uses a
    // smaller cap through the same path.
    REQUIRE(engine_network::SessionConfig{}.max_remote_bytes_per_pass == 4194304u);
    engine_network::SessionConfig config;
    config.max_remote_bytes_per_pass = 4096;
    Net net(config);
    const engine_core::RemoteEvent& open = add_remote(net.server.game, net.workspace(), "Open");
    server_script(net, "Hear",
        "_G.count = 0\n"
        "workspace.Open.OnServerEvent:Connect(function() _G.count += 1 end)\n");
    RawClient raw(net.hub, 0, 64.0 * 1048576.0);
    REQUIRE(raw.endpoint.connect(kServer, net.now));
    pump(net, raw, 30);
    REQUIRE(raw.connected);

    const std::vector<std::uint8_t> message = remote_message(open.id(), {string_value(std::string(400, 'x'))});
    constexpr int kSent = 24;
    for (int i = 0; i < kSent; ++i) {
        raw.send(message);
    }
    // One send carries them all, and one receive reads them all.
    net.now += 1.0 / 60.0;
    raw.endpoint.send(net.now);
    net.session->receive(net.now);
    const std::size_t dropped = net.session->dropped_remotes();
    const std::size_t accepted_most = config.max_remote_bytes_per_pass / message.size();
    REQUIRE(dropped == kSent - accepted_most);
    pump(net, raw, 30);
    REQUIRE(number(net.server, "count") == static_cast<double>(accepted_most));
    REQUIRE(net.session->dropped_remotes() == dropped);
    REQUIRE(raw.connected);
    REQUIRE(net.session->connection_of(0) != 0);

    // The next receive starts the allowance again.
    raw.send(message);
    pump(net, raw, 30);
    REQUIRE(number(net.server, "count") == static_cast<double>(accepted_most + 1));
}

namespace {

// A Client script that fires once a frame, up to times, while the client's own
// workspace holds a Folder named Go: a test starts it when the link is set up.
std::string fire_each_frame(const char* call, int times) {
    return std::string(
               "local r = workspace.Talk\n"
               "local n = 0\n"
               "game:GetService('RunService').Heartbeat:Connect(function()\n"
               "    if n >= ") + std::to_string(times) + " or not workspace:FindFirstChild('Go') then return end\n"
               "    n += 1\n"
               "    " + call + "\n"
               "end)\n";
}

void go(Client& client) { add_folder(client.rig.game, client.rig.game.scene_service("Workspace"), "Go"); }

}  // namespace

TEST_CASE("RE40 an unreliable FireServer arrives through the payload", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk").set_unreliable(true);
    server_script(net, "Hear",
        "_G.count = 0\n"
        "_G.sum = 0\n"
        "workspace.Talk.OnServerEvent:Connect(function(connection, n) _G.count += 1 _G.sum += n end)\n");
    client_script(net, "Send", fire_each_frame("r:FireServer(n)", 10).c_str());
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    const std::uint64_t acked = client.session->endpoint().stats(0, net.now).reliable_acked;
    go(client);
    net.run(0.5);
    REQUIRE(number(net.server, "count") == 10);
    REQUIRE(number(net.server, "sum") == 55);
    REQUIRE(client.session->dropped_unreliable() == 0);
    REQUIRE(net.session->dropped_remotes() == 0);
    // None went through the reliable channel.
    REQUIRE(client.session->endpoint().stats(0, net.now).reliable_acked == acked);
}

TEST_CASE("RE41 unreliable keeps the argument count and values", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk").set_unreliable(true);
    add_folder(net.server.game, net.workspace(), "Shared");
    // Each side checks the three in firing order, and the server sends each back.
    const char* check =
        "local got = 0\n"
        "local function check(...)\n"
        "    got += 1\n"
        "    local n, a, b = select('#', ...), ...\n"
        "    if got == 1 then _G.args = n == 2 and a == 1 and b == nil\n"
        "    elseif got == 2 then\n"
        "        _G.tab = n == 1 and #a == 3 and a[1] == 1 and a[2] == 2 and a[3][1] == 3 and a.x == 'y'\n"
        "    elseif got == 3 then _G.inst = n == 1 and a == workspace.Shared end\n"
        "    return got\n"
        "end\n";
    server_script(net, "Hear",
        (std::string(check) +
         "local r = workspace.Talk\n"
         "r.OnServerEvent:Connect(function(connection, ...)\n"
         "    local which = check(...)\n"
         "    if which == 1 then r:FireClient(connection, 1, nil)\n"
         "    elseif which == 2 then r:FireClient(connection, {1, 2, {3}, x = 'y'})\n"
         "    else r:FireClient(connection, workspace.Shared) end\n"
         "end)\n").c_str());
    client_script(net, "Send",
        (std::string(check) +
         "local r = workspace.Talk\n"
         "r.OnClientEvent:Connect(check)\n"
         "r:FireServer(1, nil)\n"
         "r:FireServer({1, 2, {3}, x = 'y'})\n"
         "r:FireServer(workspace.Shared)\n").c_str());
    Client& client = net.add_client();
    net.run(1.0);
    for (ScriptRig* rig : {&net.server, &client.rig}) {
        REQUIRE(boolean(*rig, "args"));
        REQUIRE(boolean(*rig, "tab"));
        REQUIRE(boolean(*rig, "inst"));
    }
    REQUIRE(client.session->dropped_unreliable() == 0);
    REQUIRE(net.session->dropped_unreliable() == 0);
}

TEST_CASE("RE42 under 50% loss unreliable messages are lost, never resent, and never crash", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk").set_unreliable(true);
    server_script(net, "Hear",
        "_G.count = 0\n"
        "workspace.Talk.OnServerEvent:Connect(function() _G.count += 1 end)\n");
    client_script(net, "Send", fire_each_frame("r:FireServer(n, string.rep('x', 50))", 100).c_str());
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    net.session->endpoint().conditioner().loss = 0.5f;
    client.session->endpoint().conditioner().loss = 0.5f;
    const std::uint64_t resent = client.session->endpoint().stats(0, net.now).reliable_resent;
    go(client);
    net.run(3.0);
    const double count = number(net.server, "count");
    REQUIRE(count >= 20);
    REQUIRE(count <= 80);
    REQUIRE(client.session->endpoint().stats(0, net.now).reliable_resent == resent);
    REQUIRE(client.session->dropped_unreliable() == 0);
    REQUIRE(net.session->dropped_remotes() == 0);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    REQUIRE(net.session->connection_of(0) != 0);
}

TEST_CASE("RE43 an unreliable message too big for a payload raises", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk").set_unreliable(true);
    const std::string over = "message is over " + std::to_string(engine_network::kMaxUnreliableRemoteBytes);
    const std::string says =
        "local function says(expected, f)\n"
        "    local ok, err = pcall(f)\n"
        "    return not ok and string.find(tostring(err), expected, 1, true) ~= nil\n"
        "end\n";
    server_script(net, "Big",
        (says + "_G.server = says('FireClientAll " + over +
         "', function() workspace.Talk:FireClientAll(string.rep('x', 2000)) end)\n"
         "_G.small = pcall(function() workspace.Talk:FireClientAll(string.rep('x', 1000)) end)\n").c_str());
    client_script(net, "Big",
        (says + "_G.client = says('FireServer " + over +
         "', function() workspace.Talk:FireServer(string.rep('x', 2000)) end)\n").c_str());
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(boolean(net.server, "server"));
    REQUIRE(boolean(net.server, "small"));
    REQUIRE(boolean(client.rig, "client"));
}

TEST_CASE("RE44 many unreliable messages in one tick pack until the budget, the rest dropped and counted",
          "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk").set_unreliable(true);
    server_script(net, "Hear",
        "_G.count = 0\n"
        "workspace.Talk.OnServerEvent:Connect(function(connection, n) _G.count += 1 _G.last = n end)\n");
    client_script(net, "Send",
        "_G.count = 0\n"
        "workspace.Talk.OnClientEvent:Connect(function(n) _G.count += 1 _G.last = n end)\n");
    client_script(net, "Burst", fire_each_frame("for i = 1, 50 do r:FireServer(i, string.rep('x', 100)) end", 1).c_str());
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    go(client);
    net.run(0.5);
    // About ten 107-byte messages fit a payload of 1174.
    const double heard = number(net.server, "count");
    REQUIRE(heard >= 5);
    REQUIRE(heard < 50);
    REQUIRE(heard + static_cast<double>(client.session->dropped_unreliable()) == 50);
    // The first ones went, in order.
    REQUIRE(number(net.server, "last") == heard);
    REQUIRE(net.session->dropped_remotes() == 0);

    // And the server's way.
    add_script(net.server.game, "Burst", "for i = 1, 50 do workspace.Talk:FireClientAll(i, string.rep('x', 100)) end");
    net.run(0.5);
    const double got = number(client.rig, "count");
    REQUIRE(got >= 5);
    REQUIRE(got < 50);
    REQUIRE(got + static_cast<double>(net.session->dropped_unreliable()) == 50);
    REQUIRE(number(client.rig, "last") == got);
}

namespace {

engine_core::RemoteValue table_of_falses(std::size_t count) {
    auto table = std::make_shared<engine_core::RemoteTable>();
    table->array.resize(count);
    for (engine_core::RemoteValue& element : table->array) {
        element.slot.kind = engine_core::LuaSlot::Kind::Bool;
        element.slot.flag = false;
    }
    engine_core::RemoteValue value;
    value.table = std::move(table);
    return value;
}

}  // namespace

TEST_CASE("RE45 a message of more than 65536 values raises on the sender", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk");
    // A table and 65535 values in it is 65536 values: the most a message holds.
    const char* checks =
        "local function says(expected, f)\n"
        "    local ok, err = pcall(f)\n"
        "    return not ok and string.find(tostring(err), expected, 1, true) ~= nil\n"
        "end\n"
        "local function falses(n) local t = table.create(n, false) return t end\n";
    server_script(net, "Big",
        (std::string(checks) +
         "_G.server = says('FireClientAll message has more than 65536 values', "
         "function() workspace.Talk:FireClientAll(falses(70000)) end)\n"
         "_G.server_most = pcall(function() workspace.Talk:FireClientAll(falses(65535)) end)\n"
         "_G.server_over = says('FireClientAll message has more than 65536 values', "
         "function() workspace.Talk:FireClientAll(falses(65535), 1) end)\n"
         "_G.nested = says('FireClientAll message has more than 65536 values', "
         "function() workspace.Talk:FireClientAll({falses(40000), {falses(30000)}}) end)\n").c_str());
    client_script(net, "Big",
        (std::string(checks) +
         "_G.client = says('FireServer message has more than 65536 values', "
         "function() workspace.Talk:FireServer(falses(70000)) end)\n").c_str());
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(boolean(net.server, "server"));
    REQUIRE(boolean(net.server, "server_most"));
    REQUIRE(boolean(net.server, "server_over"));
    REQUIRE(boolean(net.server, "nested"));
    REQUIRE(boolean(client.rig, "client"));
}

TEST_CASE("RE46 a raw client's message of more than 65536 values is dropped and counted", "[remote][session]") {
    Net net;
    const engine_core::RemoteEvent& open = add_remote(net.server.game, net.workspace(), "Open");
    server_script(net, "Hear",
        "_G.open = 0\n"
        "workspace.Open.OnServerEvent:Connect(function(connection, t) _G.open += 1 _G.size = #t end)\n");
    RawClient raw(net.hub, 0);
    REQUIRE(raw.endpoint.connect(kServer, net.now));
    pump(net, raw, 30);
    REQUIRE(raw.connected);

    raw.send(remote_message(open.id(), {table_of_falses(70000)}));
    pump(net, raw, 120);
    REQUIRE(net.session->dropped_remotes() == 1);
    REQUIRE(number(net.server, "open") == 0);
    REQUIRE(raw.connected);

    // The most a message holds is still heard.
    raw.send(remote_message(open.id(), {table_of_falses(65535)}));
    pump(net, raw, 120);
    REQUIRE(number(net.server, "open") == 1);
    REQUIRE(number(net.server, "size") == 65535);
    REQUIRE(net.session->dropped_remotes() == 1);
}

TEST_CASE("RE47 the per-receive value cap cuts off the rest", "[remote][session]") {
    REQUIRE(engine_network::SessionConfig{}.max_remote_values_per_pass == 262144u);
    engine_network::SessionConfig config;
    config.max_remote_values_per_pass = 100;
    Net net(config);
    const engine_core::RemoteEvent& open = add_remote(net.server.game, net.workspace(), "Open");
    server_script(net, "Hear",
        "_G.count = 0\n"
        "workspace.Open.OnServerEvent:Connect(function() _G.count += 1 end)\n");
    RawClient raw(net.hub, 0);
    REQUIRE(raw.endpoint.connect(kServer, net.now));
    pump(net, raw, 30);
    REQUIRE(raw.connected);

    // Ten values a message, small on the wire: the byte cap is nowhere near.
    std::vector<engine_core::RemoteValue> ten;
    for (int i = 0; i < 10; ++i) ten.push_back(number_value(i));
    const std::vector<std::uint8_t> message = remote_message(open.id(), ten);
    constexpr int kSent = 24;
    for (int i = 0; i < kSent; ++i) {
        raw.send(message);
    }
    net.now += 1.0 / 60.0;
    raw.endpoint.send(net.now);
    net.session->receive(net.now);
    REQUIRE(net.session->dropped_remotes() == kSent - 10);
    pump(net, raw, 30);
    REQUIRE(number(net.server, "count") == 10);
    REQUIRE(raw.connected);

    // The next receive starts the allowance again.
    raw.send(message);
    pump(net, raw, 30);
    REQUIRE(number(net.server, "count") == 11);
    REQUIRE(net.session->dropped_remotes() == kSent - 10);
}

TEST_CASE("RE48 FireServer raises once too many messages wait for a server that is not acking", "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Talk");
    client_script(net, "Flood",
        "local r = workspace.Talk\n"
        "local big = string.rep('x', 10000)\n"
        "_G.sent = 0\n"
        "game:GetService('RunService').Heartbeat:Connect(function()\n"
        "    if _G.err or not workspace:FindFirstChild('Go') then return end\n"
        "    for i = 1, 100 do\n"
        "        local ok, err = pcall(function() r:FireServer(big) end)\n"
        "        if not ok then _G.err = tostring(err) return end\n"
        "        _G.sent += 1\n"
        "    end\n"
        "end)\n");
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
    // Nothing the client sends arrives, so nothing it sends is acked.
    client.session->endpoint().conditioner().loss = 1.0f;
    add_folder(client.rig.game, client.rig.game.scene_service("Workspace"), "Go");
    net.run(0.5);
    REQUIRE(text(client.rig, "err").find("FireServer cannot send: too many messages are waiting for the server") !=
            std::string::npos);
    // The reliable window took some first; the outbox then held the cap's worth.
    REQUIRE(number(client.rig, "sent") * 10000.0 >= static_cast<double>(engine_network::kMaxServerOutboxBytes));
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);
}

TEST_CASE("RE49 a raw client's message of more than 1024 arguments is dropped, and server scripts keep running",
          "[remote][session]") {
    Net net;
    const engine_core::RemoteEvent& open = add_remote(net.server.game, net.workspace(), "Open");
    server_script(net, "Hear",
        "_G.open = 0\n"
        "_G.beats = 0\n"
        "game:GetService('RunService').Heartbeat:Connect(function() _G.beats += 1 end)\n"
        "workspace.Open.OnServerEvent:Connect(function(connection, ...) _G.open += 1 _G.count = select('#', ...) end)\n");
    RawClient raw(net.hub, 0);
    REQUIRE(raw.endpoint.connect(kServer, net.now));
    pump(net, raw, 30);
    REQUIRE(raw.connected);

    // 8001 nils: about 8 KB, well under the byte and value caps.
    raw.send(remote_message(open.id(), std::vector<engine_core::RemoteValue>(8001)));
    pump(net, raw, 60);
    REQUIRE(net.session->dropped_remotes() == 1);
    REQUIRE(number(net.server, "open") == 0);
    REQUIRE(net.server.runtime.dropped_deliveries() == 0);
    const double beats = number(net.server, "beats");
    pump(net, raw, 10);
    REQUIRE(number(net.server, "beats") > beats);
    REQUIRE(raw.connected);

    // The most arguments a message holds are heard.
    raw.send(remote_message(open.id(), std::vector<engine_core::RemoteValue>(1024)));
    pump(net, raw, 60);
    REQUIRE(number(net.server, "open") == 1);
    REQUIRE(number(net.server, "count") == 1024);
    REQUIRE(net.session->dropped_remotes() == 1);
}

TEST_CASE("RE56 refused messages are charged, so a stream of almost-valid ones stops being read at the cap",
          "[remote][session]") {
    // At full size these are 65535 values and a bad trailing byte each; this
    // transport's in-flight limits keep enough of those from landing in one
    // receive, so the test scales the cap and the messages down together.
    engine_network::SessionConfig config;
    config.max_remote_values_per_pass = 1000;
    Net net(config);
    const engine_core::RemoteEvent& open = add_remote(net.server.game, net.workspace(), "Open");
    server_script(net, "Hear",
        "_G.count = 0\n"
        "workspace.Open.OnServerEvent:Connect(function() _G.count += 1 end)\n");
    RawClient raw(net.hub, 0);
    REQUIRE(raw.endpoint.connect(kServer, net.now));
    pump(net, raw, 30);
    REQUIRE(raw.connected);

    // A table and 298 values in it, 299 values, then a byte too many.
    std::vector<std::uint8_t> almost = remote_message(open.id(), {table_of_falses(298)});
    almost.push_back(0);
    constexpr int kSent = 8;
    for (int i = 0; i < kSent; ++i) {
        raw.send(almost);
    }
    net.now += 1.0 / 60.0;
    raw.endpoint.send(net.now);
    net.session->receive(net.now);
    // Three read whole, the fourth up to the cap, and the other four not at all.
    REQUIRE(net.session->dropped_remotes() == kSent);
    REQUIRE(net.session->decoded_remote_values() == 1000);
    pump(net, raw, 30);
    REQUIRE(number(net.server, "count") == 0);
    REQUIRE(raw.connected);

    // The next receive starts the allowance again, and a good message is heard.
    raw.send(remote_message(open.id(), {table_of_falses(10)}));
    pump(net, raw, 30);
    REQUIRE(number(net.server, "count") == 1);
    REQUIRE(net.session->decoded_remote_values() == 1011);
}

TEST_CASE("RE57 a client's hold for unheard messages stays bounded, expires, and delivers a late listener's once",
          "[remote][session]") {
    Net net;
    add_remote(net.server.game, net.workspace(), "Quiet");
    add_remote(net.server.game, net.workspace(), "Late");
    server_script(net, "Flood",
        "local ticks = 0\n"
        "game:GetService('RunService').Heartbeat:Connect(function()\n"
        "    if ticks >= 100 or not workspace:FindFirstChild('Start') then return end\n"
        "    if ticks == 0 then for i = 1, 5 do workspace.Late:FireClientAll(i) end end\n"
        "    ticks += 1\n"
        "    for i = 1, 100 do workspace.Quiet:FireClientAll(i) end\n"
        "end)\n");
    // Nobody ever listens to Quiet; Late gets a listener once the client's workspace holds Go.
    client_script(net, "Listen",
        "local seen = {}\n"
        "game:GetService('RunService').Heartbeat:Connect(function()\n"
        "    if _G.listening or not workspace:FindFirstChild('Go') then return end\n"
        "    _G.listening = true\n"
        "    workspace.Late.OnClientEvent:Connect(function(n)\n"
        "        table.insert(seen, n)\n"
        "        _G.late = table.concat(seen, ',')\n"
        "    end)\n"
        "end)\n");
    Client& client = net.add_client();
    net.run(1.0);
    REQUIRE(client.session->state() == engine_network::ClientState::Joined);

    add_folder(net.server.game, net.workspace(), "Start");
    // 10 000 messages for Quiet: at most 256 of them held, with Late's 5.
    std::size_t most_held = 0;
    for (int i = 0; i < 5 * 60; ++i) {
        net.tick();
        most_held = std::max(most_held, client.session->held_unheard());
    }
    REQUIRE(most_held <= engine_network::kMaxUnheardPerRemote + 5);
    REQUIRE(client.session->held_unheard() == engine_network::kMaxUnheardPerRemote + 5);
    REQUIRE(client.session->dropped_unheard() == 10000 - engine_network::kMaxUnheardPerRemote);

    // Within Late's 10 seconds a listener connects: each message once, in order.
    go(client);
    net.run(1.0);
    REQUIRE(text(client.rig, "late") == "1,2,3,4,5");
    REQUIRE(client.session->held_unheard() == engine_network::kMaxUnheardPerRemote);

    // Quiet's are dropped once 10 seconds have passed on the clock.
    net.run(5.0);
    REQUIRE(client.session->held_unheard() == 0);
    REQUIRE(client.session->dropped_unheard() == 10000);
    REQUIRE(text(client.rig, "late") == "1,2,3,4,5");
}
