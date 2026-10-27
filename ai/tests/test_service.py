"""The AI service against fake providers (no keys, no network)."""

from __future__ import annotations

import asyncio
import json
from collections.abc import Callable
from pathlib import Path
from typing import Any

import httpx
import pytest

from spinus_ai.connections import ConnectionStore, MemoryKeyStore
from spinus_ai.gateway import Message, ProviderError, Request, Text, Tool, complete
from spinus_ai.manual import Manual
from spinus_ai.project_tools import Snapshot, note_name
from spinus_ai.routing import Routing
from spinus_ai.server import serve
from spinus_ai.service import RpcError, Service

ROOT = Path(__file__).resolve().parents[2]
EXAMPLE = json.loads((ROOT / "tests/model/files/example-v1.json").read_text())


def sse(events: list[tuple[str, dict[str, Any]]]) -> bytes:
    return "".join(f"event: {e}\ndata: {json.dumps(d)}\n\n" for e, d in events).encode()


def anthropic_reply(text: str = "", tool: tuple[str, dict[str, Any]] | None = None) -> bytes:
    events: list[tuple[str, dict[str, Any]]] = [
        ("message_start", {"type": "message_start", "message": {"model": "claude-test", "usage": {"input_tokens": 11}}})
    ]
    index = 0
    if text:
        events += [
            ("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "text"}}),
            (
                "content_block_delta",
                {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": text[:3]}},
            ),
            (
                "content_block_delta",
                {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": text[3:]}},
            ),
            ("content_block_stop", {"type": "content_block_stop", "index": 0}),
        ]
        index = 1
    if tool:
        arguments = json.dumps(tool[1])
        events += [
            (
                "content_block_start",
                {
                    "type": "content_block_start",
                    "index": index,
                    "content_block": {"type": "tool_use", "id": f"t{index}", "name": tool[0]},
                },
            ),
            (
                "content_block_delta",
                {
                    "type": "content_block_delta",
                    "index": index,
                    "delta": {"type": "input_json_delta", "partial_json": arguments[:5]},
                },
            ),
            (
                "content_block_delta",
                {
                    "type": "content_block_delta",
                    "index": index,
                    "delta": {"type": "input_json_delta", "partial_json": arguments[5:]},
                },
            ),
            ("content_block_stop", {"type": "content_block_stop", "index": index}),
        ]
    events.append(
        (
            "message_delta",
            {
                "type": "message_delta",
                "delta": {"stop_reason": "tool_use" if tool else "end_turn"},
                "usage": {"output_tokens": 7},
            },
        )
    )
    return sse(events)


def openai_reply(text: str = "", tool: tuple[str, dict[str, Any]] | None = None, cost: float | None = None) -> bytes:
    chunks: list[dict[str, Any]] = []
    if text:
        chunks.append({"model": "router/model", "choices": [{"delta": {"content": text}}]})
    if tool:
        arguments = json.dumps(tool[1])
        chunks.append(
            {
                "choices": [
                    {
                        "delta": {
                            "tool_calls": [
                                {"index": 0, "id": "c1", "function": {"name": tool[0], "arguments": arguments[:4]}}
                            ]
                        }
                    }
                ]
            }
        )
        chunks.append(
            {
                "choices": [
                    {
                        "delta": {"tool_calls": [{"index": 0, "function": {"arguments": arguments[4:]}}]},
                        "finish_reason": "tool_calls",
                    }
                ]
            }
        )
    usage: dict[str, Any] = {"prompt_tokens": 20, "completion_tokens": 5}
    if cost is not None:
        usage["cost"] = cost
    chunks.append({"choices": [], "usage": usage})
    return ("".join(f"data: {json.dumps(c)}\n\n" for c in chunks) + "data: [DONE]\n\n").encode()


def client_for(handler: Callable[[httpx.Request], httpx.Response]) -> httpx.AsyncClient:
    return httpx.AsyncClient(transport=httpx.MockTransport(handler))


async def no_text(_: str) -> None:
    pass


# ---------------------------------------------------------------------------------------------------


def test_connections_keep_keys_out_of_the_file(tmp_path: Path) -> None:
    keys = MemoryKeyStore()
    store = ConnectionStore(tmp_path, keys)
    c = store.upsert("Mine", "anthropic", None, "sk-secret")
    assert c.base_url == "https://api.anthropic.com"
    assert keys.get(c.id) == "sk-secret"
    assert "sk-secret" not in (tmp_path / "connections.json").read_text()

    again = ConnectionStore(tmp_path, keys)
    assert [x.name for x in again.connections] == ["Mine"]
    assert again.key_for(again.connections[0]) == "sk-secret"

    with pytest.raises(ValueError):
        store.upsert("x", "nonsense", None, None)
    assert store.remove(c.id)
    assert keys.get(c.id) is None


def test_routing_defaults_and_choices(tmp_path: Path) -> None:
    store = ConnectionStore(tmp_path, MemoryKeyStore())
    routing = Routing(tmp_path, store)
    assert routing.resolve("assistant") == []

    a = store.upsert("A", "anthropic", None, "k")
    local = store.upsert("Ollama", "local", None, None)
    local.models = ["llama3.2", "qwen3"]
    assert [(r.connection.name, r.model) for r in routing.resolve("assistant")] == [
        ("A", "claude-sonnet-5-5"),
        ("Ollama", "llama3.2"),
    ]

    routing.set("assistant", [{"connection": local.id, "model": "qwen3"}, {"connection": a.id}])
    assert [(r.connection.name, r.model) for r in Routing(tmp_path, store).resolve("assistant")] == [
        ("Ollama", "qwen3"),
        ("A", "claude-sonnet-5-5"),
    ]
    with pytest.raises(ValueError):
        routing.set("assistant", [{"connection": "missing"}])


async def test_anthropic_streams_text_and_tool_calls(tmp_path: Path) -> None:
    seen: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request)
        return httpx.Response(200, content=anthropic_reply("Hello there", ("project_overview", {"a": 1})))

    store = ConnectionStore(tmp_path, MemoryKeyStore())
    c = store.upsert("A", "anthropic", None, "k")
    streamed: list[str] = []

    async def on_text(delta: str) -> None:
        streamed.append(delta)

    tools = [Tool("project_overview", "x", {"type": "object", "properties": {}})]
    result = await complete(
        client_for(handler), c, "k", Request("m", "sys", [Message("user", [Text("hi")])], tools), on_text
    )
    assert result.text == "Hello there"
    assert streamed == ["Hel", "lo there"]
    assert result.tool_uses[0].name == "project_overview"
    assert result.tool_uses[0].input == {"a": 1}
    assert (result.usage.input_tokens, result.usage.output_tokens) == (11, 7)
    assert seen[0].headers["x-api-key"] == "k"
    body = json.loads(seen[0].content)
    assert body["system"] == "sys" and body["tools"][0]["name"] == "project_overview"


async def test_openai_compatible_streams_tool_calls_and_cost(tmp_path: Path) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        assert request.url.path.endswith("/chat/completions")
        assert json.loads(request.content)["usage"] == {"include": True}  # OpenRouter reports cost
        return httpx.Response(200, content=openai_reply("Hi", ("search_manual", {"query": "mixer"}), cost=0.0012))

    store = ConnectionStore(tmp_path, MemoryKeyStore())
    c = store.upsert("Router", "openrouter", None, "k")
    result = await complete(client_for(handler), c, "k", Request("m", "s", [Message("user", [Text("hi")])]), no_text)
    assert result.text == "Hi"
    assert result.tool_uses[0].input == {"query": "mixer"}
    assert result.usage.cost == pytest.approx(0.0012)
    assert result.model == "router/model"


@pytest.mark.parametrize(
    ("url", "refused"),
    [
        ("https://api.openai.com/v1", None),  # newer OpenAI models: only max_completion_tokens
        ("https://other.example/v1", None),  # most other services: max_tokens
        ("https://api.openai.com/v1", "max_completion_tokens"),  # an older model asks for max_tokens
        ("https://other.example/v1", "max_tokens"),  # a service that wants max_completion_tokens
    ],
)
async def test_openai_compatible_sends_the_length_cap_the_service_takes(
    tmp_path: Path, url: str, refused: str | None
) -> None:
    sent: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        limit = "max_completion_tokens" if "max_completion_tokens" in body else "max_tokens"
        assert body[limit] == 4096
        sent.append(limit)
        if limit == refused:
            other = "max_tokens" if limit == "max_completion_tokens" else "max_completion_tokens"
            message = f"Unsupported parameter: '{limit}' is not supported with this model. Use '{other}' instead."
            return httpx.Response(400, json={"error": {"message": message, "code": "unsupported_parameter"}})
        return httpx.Response(200, content=openai_reply("Hi"))

    store = ConnectionStore(tmp_path, MemoryKeyStore())
    c = store.upsert("O", "openai", url, "k")
    result = await complete(client_for(handler), c, "k", Request("m", "s", [Message("user", [Text("hi")])]), no_text)
    assert result.text == "Hi"
    first = "max_completion_tokens" if "api.openai.com" in url else "max_tokens"
    assert sent[0] == first and len(sent) == (2 if refused else 1)


async def test_openai_compatible_does_not_retry_other_bad_requests(tmp_path: Path) -> None:
    calls = 0

    def handler(request: httpx.Request) -> httpx.Response:
        nonlocal calls
        calls += 1
        return httpx.Response(400, json={"error": {"message": "The model 'x' does not exist."}})

    store = ConnectionStore(tmp_path, MemoryKeyStore())
    c = store.upsert("O", "openai", None, "k")
    with pytest.raises(ProviderError) as caught:
        await complete(client_for(handler), c, "k", Request("x", "s", [Message("user", [Text("hi")])]), no_text)
    assert caught.value.kind == "bad_request" and calls == 1


@pytest.mark.parametrize(
    ("status", "kind"), [(401, "auth"), (429, "rate_limit"), (503, "outage"), (400, "bad_request")]
)
async def test_provider_errors_are_sorted(tmp_path: Path, status: int, kind: str) -> None:
    store = ConnectionStore(tmp_path, MemoryKeyStore())
    c = store.upsert("A", "anthropic", None, "k")
    with pytest.raises(ProviderError) as caught:
        await complete(
            client_for(lambda r: httpx.Response(status, text="nope")),
            c,
            "k",
            Request("m", "s", [Message("user", [Text("hi")])]),
            no_text,
        )
    assert caught.value.kind == kind


def test_manual_search_and_snapshot_tools() -> None:
    manual = Manual(ROOT / "docs/user")
    found = manual.search("sidechain ducker kick")
    assert found and found[0].page in ("ducker.md", "mixer.md")

    snapshot = Snapshot({"project": EXAMPLE, "deviceNames": {}})
    overview = snapshot.overview()
    assert overview["tempoBpm"] == EXAMPLE["settings"]["tempoBpm"]
    assert len(overview["channels"]) == len(EXAMPLE["channels"])
    pattern = EXAMPLE["patterns"][0]
    channel = pattern["channels"][0]["channel"]
    notes = snapshot.pattern_notes(pattern["id"], channel)
    assert len(notes["notes"]) == len(pattern["channels"][0]["notes"])
    assert note_name(60) == "C5"
    assert "error" in snapshot.pattern_notes(9999, channel)


# ---------------------------------------------------------------------------------------------------


def make_service(tmp_path: Path, handler: Callable[[httpx.Request], httpx.Response]) -> Service:
    return Service(
        tmp_path / "config",
        MemoryKeyStore(),
        ROOT / "docs/user",
        ROOT / "docs/dev/commands.schema.json",
        client=client_for(handler),
        models=tmp_path / "models",
    )


async def collect(service: Service, method: str, params: dict[str, Any]) -> tuple[Any, list[dict[str, Any]]]:
    updates: list[dict[str, Any]] = []
    done = asyncio.Event()

    async def notify(name: str, payload: dict[str, Any]) -> None:
        updates.append(payload)
        if payload.get("kind") in ("done", "error"):
            done.set()

    result = await service.handle(method, params, notify)
    if method == "ai.v1.assistant.ask":
        await asyncio.wait_for(done.wait(), 10)
    return result, updates


async def test_assistant_looks_proposes_and_never_applies(tmp_path: Path) -> None:
    replies = iter(
        [
            anthropic_reply("Let me look.", ("project_overview", {})),
            anthropic_reply(
                "",
                (
                    "propose_changes",
                    {"summary": "Faster", "steps": [{"command": "project.setTempo", "parameters": {"bpm": 140}}]},
                ),
            ),
            anthropic_reply("If you apply it, the song plays at 140 BPM."),
        ]
    )
    requests: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(json.loads(request.content))
        return httpx.Response(200, content=next(replies))

    service = make_service(tmp_path, handler)
    await service.handle("ai.v1.connections.save", {"name": "A", "provider": "anthropic", "key": "k"}, None)  # type: ignore[arg-type]
    job, updates = await collect(
        service,
        "ai.v1.assistant.ask",
        {
            "conversation": [{"role": "user", "text": "Make it faster"}],
            "context": {"project": EXAMPLE, "selection": {"channel": EXAMPLE["channels"][0]["id"]}},
        },
    )
    assert job["job"]
    final = updates[-1]
    assert final["kind"] == "done", final
    result = final["result"]
    assert result["proposals"] == [
        {"summary": "Faster", "steps": [{"command": "project.setTempo", "parameters": {"bpm": 140}}]}
    ]
    assert "140 BPM" in result["text"]
    assert result["usage"]["outputTokens"] == 21
    assert any(u.get("kind") == "status" and u["text"] == "Looking at the project" for u in updates)
    # The overview reached the model as a tool result.
    tool_result = requests[1]["messages"][-1]["content"][0]
    assert tool_result["type"] == "tool_result" and str(EXAMPLE["settings"]["tempoBpm"]) in tool_result["content"]
    usage, _ = await collect(service, "ai.v1.usage.get", {})
    assert usage["usage"]


async def test_unknown_commands_go_back_to_the_model(tmp_path: Path) -> None:
    replies = iter(
        [
            anthropic_reply(
                "", ("propose_changes", {"summary": "x", "steps": [{"command": "rm.everything", "parameters": {}}]})
            ),
            anthropic_reply("I can't do that."),
        ]
    )
    sent: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=next(replies))

    service = make_service(tmp_path, handler)
    await service.handle("ai.v1.connections.save", {"name": "A", "provider": "anthropic", "key": "k"}, None)  # type: ignore[arg-type]
    _, updates = await collect(
        service,
        "ai.v1.assistant.ask",
        {"conversation": [{"role": "user", "text": "go"}], "context": {"project": EXAMPLE}},
    )
    assert updates[-1]["result"]["proposals"] == []
    assert sent[1]["messages"][-1]["content"][0]["is_error"] is True


async def test_falls_back_to_the_next_connection(tmp_path: Path) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.host == "api.anthropic.com":
            return httpx.Response(529, text="overloaded")
        return httpx.Response(200, content=openai_reply("From the router."))

    service = make_service(tmp_path, handler)
    for name, provider in (("A", "anthropic"), ("R", "openrouter")):
        await service.handle("ai.v1.connections.save", {"name": name, "provider": provider, "key": "k"}, None)  # type: ignore[arg-type]
    _, updates = await collect(
        service,
        "ai.v1.assistant.ask",
        {"conversation": [{"role": "user", "text": "hi"}], "context": {"project": EXAMPLE}},
    )
    final = updates[-1]
    assert final["kind"] == "done"
    assert final["result"]["connection"] == "R"
    assert any("trying R" in u.get("text", "") for u in updates)


async def test_no_connection_is_a_plain_message(tmp_path: Path) -> None:
    service = make_service(tmp_path, lambda r: httpx.Response(500))
    _, updates = await collect(
        service, "ai.v1.assistant.ask", {"conversation": [{"role": "user", "text": "hi"}], "context": {}}
    )
    assert updates[-1] == {
        "job": updates[-1]["job"],
        "kind": "error",
        "message": "No AI connection is set up. Add one in Options > AI.",
    }


async def test_connection_test_and_local_servers(tmp_path: Path) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.port == 11434:
            return httpx.Response(200, json={"data": [{"id": "qwen3"}, {"id": "llama3.2"}]})
        if request.url.port == 8080:
            raise httpx.ConnectError("refused")
        if request.headers.get("x-api-key") == "good":
            return httpx.Response(200, json={"data": [{"id": "claude-sonnet-5-5"}]})
        return httpx.Response(401, text="bad key")

    service = make_service(tmp_path, handler)
    found, _ = await collect(service, "ai.v1.connections.findLocal", {})
    assert found["servers"] == [{"baseUrl": "http://localhost:11434/v1", "models": ["llama3.2", "qwen3"]}]

    saved, _ = await collect(service, "ai.v1.connections.save", {"name": "A", "provider": "anthropic", "key": "bad"})
    tested, _ = await collect(service, "ai.v1.connections.test", {"id": saved["id"]})
    assert tested["ok"] is False and "refused the key" in tested["message"]
    await collect(
        service, "ai.v1.connections.save", {"id": saved["id"], "name": "A", "provider": "anthropic", "key": "good"}
    )
    tested, _ = await collect(service, "ai.v1.connections.test", {"id": saved["id"]})
    assert tested == {"ok": True, "message": "Works: 1 models available.", "models": ["claude-sonnet-5-5"]}
    listed, _ = await collect(service, "ai.v1.connections.list", {})
    assert listed["connections"][0]["hasKey"] is True
    assert "good" not in json.dumps(listed)


async def start_server(
    tmp_path: Path, service: Service, token: str = "secret", **options: Any
) -> tuple[asyncio.Task[None], int]:
    """Starts serve() and returns it with the port it listens on, once it does."""
    port_file = tmp_path / "service.port"
    server = asyncio.create_task(serve(service, port_file, token, **options))
    for _ in range(200):
        if port_file.exists():
            return server, int(port_file.read_text())
        await asyncio.sleep(0.01)
    raise AssertionError("the service did not start listening")


async def connect(
    port: int, token: str = "secret", **options: Any
) -> tuple[asyncio.StreamReader, asyncio.StreamWriter]:
    reader, writer = await asyncio.open_connection("127.0.0.1", port, **options)
    writer.write(token.encode() + b"\n")
    await writer.drain()
    return reader, writer


async def test_json_rpc_over_the_connection(tmp_path: Path) -> None:
    service = make_service(tmp_path, lambda r: httpx.Response(500))
    server, port = await start_server(tmp_path, service)
    reader, writer = await connect(port)

    async def call(message: dict[str, Any]) -> dict[str, Any]:
        writer.write(json.dumps(message).encode() + b"\n")
        await writer.drain()
        reply: dict[str, Any] = json.loads(await reader.readline())
        return reply

    hello = await call({"jsonrpc": "2.0", "id": 1, "method": "ai.v1.hello"})
    assert hello["result"]["protocol"] == 1
    assert hello["result"]["manualSections"] > 20
    assert hello["result"]["commands"] > 20
    unknown = await call({"jsonrpc": "2.0", "id": 2, "method": "ai.v1.nope"})
    assert unknown["error"]["code"] == -32601
    assert (await call({"jsonrpc": "2.0", "id": 3, "method": "ai.v1.shutdown"}))["result"] == {}
    writer.close()
    await asyncio.wait_for(server, 5)
    assert not (tmp_path / "service.port").exists()


async def test_only_spinus_can_connect(tmp_path: Path) -> None:
    """A connection that does not start with the token is closed without an answer."""
    service = make_service(tmp_path, lambda r: httpx.Response(500))
    server, port = await start_server(tmp_path, service)
    reader, writer = await connect(port, token="guess")
    writer.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ai.v1.hello"}).encode() + b"\n")
    await writer.drain()
    assert await asyncio.wait_for(reader.read(), 5) == b""
    writer.close()

    reader, writer = await connect(port)
    writer.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ai.v1.shutdown"}).encode() + b"\n")
    await writer.drain()
    assert json.loads(await reader.readline())["result"] == {}
    writer.close()
    await asyncio.wait_for(server, 5)


async def test_long_requests_over_the_connection(tmp_path: Path) -> None:
    """A list of thousands of presets is far longer than asyncio's 64 KB line; a request past the limit is
    answered with an error, and the connection carries on."""
    service = make_service(tmp_path, lambda r: httpx.Response(500))
    server, port = await start_server(tmp_path, service, limit=4 << 20)
    reader, writer = await connect(port, limit=1 << 24)

    async def call(message: dict[str, Any]) -> dict[str, Any]:
        writer.write(json.dumps(message).encode() + b"\n")
        await writer.drain()
        reply: dict[str, Any] = json.loads(await reader.readline())
        return reply

    presets = [{"id": f"/home/someone/.local/share/Surge XT/patches/Category {i}/Preset {i}.fxp", "version": "1"}
               for i in range(20000)]  # fmt: skip
    params = {"library": "surge-xt", "presets": presets}
    missing = await call({"jsonrpc": "2.0", "id": 1, "method": "ai.v1.presets.missing", "params": params})
    assert len(missing["result"]["missing"]) == 20000
    params["presets"] = presets * 4  # over 4 MB
    too_long = await call({"jsonrpc": "2.0", "id": 2, "method": "ai.v1.presets.missing", "params": params})
    assert too_long["error"]["code"] == -32600
    assert (await call({"jsonrpc": "2.0", "id": 3, "method": "ai.v1.hello"}))["result"]["protocol"] == 1
    assert (await call({"jsonrpc": "2.0", "id": 4, "method": "ai.v1.shutdown"}))["result"] == {}
    writer.close()
    await asyncio.wait_for(server, 5)


async def test_wrong_parameters_go_back_to_the_model_to_fix(tmp_path: Path) -> None:
    replies = iter(
        [
            anthropic_reply(
                "",
                (
                    "propose_changes",
                    {"summary": "x", "steps": [{"command": "project.setTempo", "parameters": {"tempo": 128}}]},
                ),
            ),
            anthropic_reply(
                "",
                (
                    "propose_changes",
                    {
                        "summary": "Faster",
                        "steps": [
                            {"command": "insert.add", "parameters": {"name": "Bus"}},
                            {"command": "project.setTempo", "parameters": {"bpm": 128}},
                            {
                                "command": "insert.addEffect",
                                "parameters": {
                                    "insert": "$1.insert",
                                    "device": {"kind": "stock", "stockType": "spinus.filter"},
                                },
                            },
                        ],
                    },
                ),
            ),
            anthropic_reply("Done proposing."),
        ]
    )
    sent: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=next(replies))

    service = make_service(tmp_path, handler)
    await service.handle("ai.v1.connections.save", {"name": "A", "provider": "anthropic", "key": "k"}, None)  # type: ignore[arg-type]
    _, updates = await collect(
        service,
        "ai.v1.assistant.ask",
        {"conversation": [{"role": "user", "text": "go"}], "context": {"project": EXAMPLE}},
    )
    feedback = sent[1]["messages"][-1]["content"][0]
    assert feedback["is_error"] is True
    assert "parameters.bpm is missing" in feedback["content"]
    assert "parameters.tempo is not a parameter" in feedback["content"]
    proposals = updates[-1]["result"]["proposals"]
    assert len(proposals) == 1 and proposals[0]["summary"] == "Faster"


async def test_references_must_name_an_earlier_steps_result(tmp_path: Path) -> None:
    from spinus_ai.manual import Manual as M
    from spinus_ai.project_tools import Tools

    commands = {
        c: {"summary": "", "parameters": {"type": "object"}}
        for c in ("insert.add", "insert.addEffect", "project.setTempo")
    }
    tools = Tools(Snapshot({"project": EXAMPLE}), M(None), commands)

    def propose(*steps: tuple[str, dict[str, Any]]) -> tuple[str, bool]:
        return tools.call(
            "propose_changes", {"summary": "x", "steps": [{"command": c, "parameters": p} for c, p in steps]}
        )

    text, error = propose(("insert.add", {}), ("insert.addEffect", {"insert": "$1.id"}))
    assert error and "$1.id is not a result of insert.add; it gives $1.insert" in text
    text, error = propose(("project.setTempo", {"bpm": 90}), ("insert.addEffect", {"insert": "$1.insert"}))
    assert error and "it gives nothing" in text
    text, error = propose(("insert.addEffect", {"insert": "$2.insert"}), ("insert.add", {}))
    assert error and "must name an earlier step" in text
    assert propose(("insert.add", {}), ("insert.addEffect", {"insert": "$1.insert"}))[1] is False


def test_devices_are_found_and_invented_ones_refused() -> None:
    from spinus_ai.manual import Manual as M
    from spinus_ai.project_tools import Tools

    devices = [
        {
            "name": "Hall Reverb",
            "maker": "Dragonfly Reverb",
            "type": "effect",
            "format": "",
            "available": True,
            "device": {"kind": "stock", "stockType": "bundled.dragonfly.hall"},
        },
        {
            "name": "Surge XT",
            "maker": "Surge Synth Team",
            "type": "instrument",
            "format": "",
            "available": False,
            "device": {"kind": "stock", "stockType": "bundled.surge-xt"},
        },
        {
            "name": "Serum",
            "maker": "Xfer",
            "type": "instrument",
            "format": "WIN",
            "available": True,
            "device": {"kind": "plugin", "plugin": {"format": "VST3", "name": "Serum"}},
        },
    ]
    commands = {
        "insert.addEffect": {"summary": "", "parameters": {"type": "object"}},
        "channel.add": {"summary": "", "parameters": {"type": "object"}},
    }
    tools = Tools(Snapshot({"project": EXAMPLE, "devices": devices}), M(None), commands)

    found, _ = tools.call("find_devices", {"query": "hall", "type": "effect"})
    assert "bundled.dragonfly.hall" in found
    assert tools.call("find_devices", {"query": "hall", "type": "instrument"})[0].startswith("No such device")

    def propose(device: dict[str, Any]) -> tuple[str, bool]:
        return tools.call(
            "propose_changes",
            {"summary": "x", "steps": [{"command": "insert.addEffect", "parameters": {"insert": 2, "device": device}}]},
        )

    text, error = propose({"kind": "stock", "stockType": "Dragonfly Hall Reverb"})
    assert error and "no device with stockType" in text
    text, error = propose({"kind": "stock", "stockType": "bundled.surge-xt"})
    assert error and "not downloaded yet" in text
    text, error = propose({"kind": "plugin", "plugin": {"name": "Massive"}})
    assert error and "no installed plugin named 'Massive'" in text
    assert propose({"kind": "stock", "stockType": "bundled.dragonfly.hall"})[1] is False
    assert propose({"kind": "plugin", "plugin": {"format": "VST3", "name": "Serum"}})[1] is False


# ---------------------------------------------------------------------------------------------------
# Listening


async def no_notify(name: str, payload: dict[str, Any]) -> None:
    pass


async def run_job(service: Service, method: str, params: dict[str, Any]) -> Any:
    """Starts a job and returns its result (or raises its error)."""
    finished: asyncio.Future[Any] = asyncio.get_running_loop().create_future()

    async def notify(name: str, payload: dict[str, Any]) -> None:
        if payload.get("kind") == "done" and not finished.done():
            finished.set_result(payload["result"])
        elif payload.get("kind") == "error" and not finished.done():
            finished.set_exception(RuntimeError(payload["message"]))

    await service.handle(method, params, notify)
    return await asyncio.wait_for(finished, 60)


async def test_listening_jobs_give_notes_and_hits_and_remember_taught_sounds(tmp_path: Path) -> None:
    import numpy as np
    from test_listening import midi_hz, sequence, silence, voice, write_wav

    service = make_service(tmp_path, lambda request: httpx.Response(404))
    melody = write_wav(tmp_path / "melody.wav", np.concatenate([silence(0.2), voice(0.5, midi_hz(62)), silence(0.2)]))

    result = await run_job(service, "ai.v1.listen.melody", {"file": str(melody)})
    assert [round(n["pitch"]) for n in result["notes"]] == [62]
    assert len(list((tmp_path / "config" / "cache" / "listen").iterdir())) == 1
    assert await run_job(service, "ai.v1.listen.melody", {"file": str(melody)}) == result  # from the cache

    rng = np.random.default_rng(2)
    for sound in ("kick", "snare", "hat"):
        take = write_wav(tmp_path / f"{sound}.wav", sequence([sound] * 4, 0.4, rng))
        taught = await run_job(service, "ai.v1.listen.teach", {"file": str(take), "sound": sound})
        assert taught["learned"] == 4
    sounds = await service.handle("ai.v1.listen.sounds", {}, no_notify)
    assert sounds["sounds"] == {"kick": 4, "snare": 4, "hat": 4}

    beat = write_wav(tmp_path / "beat.wav", sequence(["kick", "hat", "snare", "hat"], 0.25, rng))
    hits = await run_job(service, "ai.v1.listen.beats", {"file": str(beat)})
    assert hits["taught"]
    assert [h["sound"] for h in hits["hits"]] == ["kick", "hat", "snare", "hat"]

    await service.handle("ai.v1.listen.forget", {}, no_notify)
    assert (await service.handle("ai.v1.listen.sounds", {}, no_notify))["sounds"]["kick"] == 0

    # Too few hits to learn from, a missing take, an unknown sound.
    single = write_wav(tmp_path / "one.wav", sequence(["kick"], 0.4, rng))
    with pytest.raises(RuntimeError, match="at least 3 times"):
        await run_job(service, "ai.v1.listen.teach", {"file": str(single), "sound": "kick"})
    with pytest.raises(RpcError):
        await service.handle("ai.v1.listen.melody", {"file": str(tmp_path / "missing.wav")}, no_notify)
    with pytest.raises(RpcError):
        await service.handle("ai.v1.listen.teach", {"file": str(beat), "sound": "cowbell"}, no_notify)


def test_anthropic_requests_never_carry_empty_text() -> None:
    """Anthropic refuses empty text ("text content blocks must be non-empty"); seen with a real project."""
    from spinus_ai.gateway import ToolResult, ToolUse, anthropic_body

    body = anthropic_body(
        Request(
            "claude-test",
            "system",
            [
                Message("user", [Text("What is in my song?")]),
                Message("assistant", [Text(""), ToolUse("t1", "get_project", {})]),
                Message("user", [ToolResult("t1", "")]),
                Message("assistant", [Text("  ")]),
                Message("user", [Text("And now?")]),
            ],
        )
    )
    for message in body["messages"]:
        assert message["content"], message
        for block in message["content"]:
            if block["type"] == "text":
                assert block["text"].strip()
            if block["type"] == "tool_result":
                assert block["content"].strip()
    assert body["messages"][1]["content"] == [{"type": "tool_use", "id": "t1", "name": "get_project", "input": {}}]


def test_device_settings_go_as_text_in_the_units_the_device_shows() -> None:
    from spinus_ai.assistant import SYSTEM
    from spinus_ai.manual import Manual as M
    from spinus_ai.project_tools import Tools
    from spinus_ai.service import load_commands

    # The assistant is told the rule Spinus reads settings by (src/dsp/ParameterText.h).
    assert "'text'" in SYSTEM and '"220 ms"' in SYSTEM and "never 0 to 1" in SYSTEM
    commands = load_commands(ROOT / "docs/dev/commands.schema.json")
    assert "text" in commands["device.setParameters"]["parameters"]["properties"]
    tools = Tools(Snapshot({"project": EXAMPLE}), M(None), commands)

    def propose(parameters: dict[str, Any]) -> tuple[str, bool]:
        return tools.call(
            "propose_changes",
            {"summary": "x", "steps": [{"command": "device.setParameters", "parameters": parameters}]},
        )

    assert propose({"device": 3, "text": {"Amount": "35%", "release": "220 ms"}})[1] is False
    assert propose({"device": 3, "values": {"amount": 0.35}})[1] is False
    assert propose({"device": 3, "text": {"amount": 35}})[1] is True  # text is text
    assert propose({"device": 3, "values": {"amount": 35}})[1] is True  # values are 0 to 1
