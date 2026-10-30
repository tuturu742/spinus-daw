"""A local Ollama through its own chat API, which sets the context length (num_ctx) for each request."""

from __future__ import annotations

import json
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import httpx
import pytest

from spinus_ai import gateway
from spinus_ai.connections import Connection, ConnectionStore, MemoryKeyStore
from spinus_ai.gateway import Message, Request, Text, Tool, ToolResult, complete

LOOKUP = Tool("search_manual", "Search the manual", {"type": "object", "properties": {"query": {"type": "string"}}})


@pytest.fixture(autouse=True)
def fresh_caches() -> Iterator[None]:
    for cache in (gateway._ollama_servers, gateway._ollama_limits, gateway._ollama_contexts):
        cache.clear()
    yield


async def no_text(_: str) -> None:
    pass


def lines(*payloads: dict[str, Any]) -> bytes:
    """A stream as Ollama sends it: one JSON object per line."""
    return "".join(json.dumps(p) + "\n" for p in payloads).encode()


def done(**counts: Any) -> dict[str, Any]:
    return {"model": "qwen3:30b-a3b", "message": {"role": "assistant", "content": ""}, "done": True,
            "done_reason": "stop", "prompt_eval_count": 1500, "eval_count": 40, **counts}  # fmt: skip


def server(
    chat: Callable[[dict[str, Any]], bytes], context_length: int = 262144, sent: list[dict[str, Any]] | None = None
) -> Callable[[httpx.Request], httpx.Response]:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path == "/api/version":
            return httpx.Response(200, json={"version": "0.33.2"})
        if request.url.path == "/api/show":
            return httpx.Response(200, json={"model_info": {"qwen3moe.context_length": context_length}})
        if request.url.path == "/api/chat":
            body = json.loads(request.content)
            if sent is not None:
                sent.append(body)
            return httpx.Response(200, content=chat(body))
        return httpx.Response(404)

    return handler


def connection(tmp_path: Path, url: str = "http://localhost:11434/v1") -> Connection:
    return ConnectionStore(tmp_path, MemoryKeyStore()).upsert("Ollama", "local", url, None)


def client_for(handler: Callable[[httpx.Request], httpx.Response]) -> httpx.AsyncClient:
    return httpx.AsyncClient(transport=httpx.MockTransport(handler))


async def test_text_with_the_context_set(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    chunks = [{"model": "qwen3:30b-a3b", "message": {"role": "assistant", "content": t}, "done": False}
              for t in ("Hel", "lo")]  # fmt: skip
    handler = server(lambda _: lines(*chunks, done()), sent=sent)
    request = Request("qwen3:30b-a3b", "You help.", [Message("user", [Text("hi")])])
    result = await complete(client_for(handler), connection(tmp_path), None, request, no_text)
    assert result.text == "Hello" and result.stop_reason == "stop"
    assert (result.usage.input_tokens, result.usage.output_tokens) == (1500, 40)
    body = sent[0]
    assert body["options"]["num_ctx"] == 16384  # the least asked for
    assert body["options"]["num_predict"] == 4096
    assert body["messages"][0] == {"role": "system", "content": "You help."}
    assert "think" not in body  # the model's own default


async def test_tool_calls_and_their_results(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    call = {"function": {"name": "search_manual", "arguments": {"query": "mixer"}}}
    handler = server(
        lambda _: lines({"message": {"role": "assistant", "content": "", "tool_calls": [call]}, "done": False}, done()),
        sent=sent,
    )
    request = Request("qwen3:30b-a3b", "s", [Message("user", [Text("where is the mixer?")])], [LOOKUP])
    result = await complete(client_for(handler), connection(tmp_path), None, request, no_text)
    assert result.stop_reason == "tool_use"
    use = result.tool_uses[0]
    assert (use.name, use.input) == ("search_manual", {"query": "mixer"})
    assert sent[0]["tools"][0]["function"]["name"] == "search_manual"

    request.messages += [Message("assistant", [use]), Message("user", [ToolResult(use.id, "View, Mixer")])]
    await complete(client_for(handler), connection(tmp_path), None, request, no_text)
    messages = sent[1]["messages"]
    assert messages[2] == {"role": "assistant", "content": "", "tool_calls": [call]}
    assert messages[3] == {"role": "tool", "content": "View, Mixer", "tool_name": "search_manual"}


async def test_the_context_grows_with_the_conversation_and_stops_at_the_models_own(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    handler = server(lambda _: lines(done()), context_length=40000, sent=sent)
    long = Request("qwen3:30b-a3b", "s", [Message("user", [Text("x" * 4 * 20000)])], max_tokens=None)
    await complete(client_for(handler), connection(tmp_path), None, long, no_text)
    assert sent[0]["options"]["num_ctx"] == 40000  # 20000 + 16384 rounds up to 65536: capped by the model
    assert "num_predict" not in sent[0]["options"]

    short = Request("qwen3:30b-a3b", "s", [Message("user", [Text("hi")])])
    await complete(client_for(handler), connection(tmp_path), None, short, no_text)
    assert sent[1]["options"]["num_ctx"] == 40000  # never shrinks for a model: a new size reloads it


async def test_reasoning_levels_become_think(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []
    thought = {"message": {"role": "assistant", "content": "", "thinking": "a" * 400}, "done": False}
    handler = server(lambda _: lines(thought, {"message": {"content": "ok"}, "done": False}, done()), sent=sent)
    for model in ("qwen3:30b-a3b", "gpt-oss:120b"):
        request = Request(model, "s", [Message("user", [Text("hi")])], effort="high")
        result = await complete(client_for(handler), connection(tmp_path), None, request, no_text)
        assert result.text == "ok" and result.usage.reasoning_tokens == 40  # estimated from the thinking's text
    assert sent[0]["think"] is True
    assert sent[1]["think"] == "high"  # gpt-oss takes the level itself


async def test_another_local_server_keeps_the_chat_api(tmp_path: Path) -> None:
    paths: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        paths.append(request.url.path)
        if request.url.path == "/api/version":
            return httpx.Response(404)
        chunk = {"model": "m", "choices": [{"delta": {"content": "Hi"}, "finish_reason": "stop"}]}
        return httpx.Response(200, content=f"data: {json.dumps(chunk)}\n\ndata: [DONE]\n\n".encode())

    c = connection(tmp_path, "http://localhost:8080/v1")
    for _ in range(2):
        hi = Request("m", "s", [Message("user", [Text("hi")])])
        result = await complete(client_for(handler), c, None, hi, no_text)
        assert result.text == "Hi"
    assert paths == ["/api/version", "/v1/chat/completions", "/v1/chat/completions"]  # asked once per server


async def test_a_server_without_api_chat_falls_back_to_the_chat_api(tmp_path: Path) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path in ("/api/chat", "/api/show"):
            return httpx.Response(404, text="404 page not found")
        chunk = {"model": "m", "choices": [{"delta": {"content": "Hi"}, "finish_reason": "stop"}]}
        return httpx.Response(200, content=f"data: {json.dumps(chunk)}\n\ndata: [DONE]\n\n".encode())

    result = await complete(client_for(handler), connection(tmp_path), None,
                            Request("m", "s", [Message("user", [Text("hi")])]), no_text)  # fmt: skip
    assert result.text == "Hi"


async def test_a_missing_model_is_an_error_not_a_fallback(tmp_path: Path) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path == "/api/chat":
            return httpx.Response(404, json={"error": "model 'nope' not found"})
        return server(lambda _: b"")(request)

    with pytest.raises(gateway.ProviderError) as caught:
        await complete(client_for(handler), connection(tmp_path), None,
                       Request("nope", "s", [Message("user", [Text("hi")])]), no_text)  # fmt: skip
    assert caught.value.kind == "bad_request" and "not found" in str(caught.value)
