"""OpenAI's Responses API (OpenAI's own service) and the chat API's retry without reasoning."""

from __future__ import annotations

import json
from collections.abc import Callable
from pathlib import Path
from typing import Any

import httpx
import pytest

from spinus_ai.connections import Connection, ConnectionStore, MemoryKeyStore
from spinus_ai.gateway import (
    Message,
    ProviderError,
    ProviderItems,
    Request,
    Text,
    Tool,
    ToolResult,
    ToolUse,
    anthropic_body,
    complete,
    openai_body,
)

LOOKUP = Tool("search_manual", "Search the manual", {"type": "object", "properties": {"query": {"type": "string"}}})

REASONING = {
    "id": "rs_1",
    "type": "reasoning",
    "summary": [],
    "encrypted_content": "gAAAA-opaque",
}
CALL = {
    "id": "fc_1",
    "type": "function_call",
    "status": "completed",
    "call_id": "call_abc",
    "name": "search_manual",
    "arguments": '{"query": "mixer"}',
}
MESSAGE = {
    "id": "msg_1",
    "type": "message",
    "role": "assistant",
    "status": "completed",
    "content": [{"type": "output_text", "text": "The mixer.", "annotations": []}],
}
USAGE = {
    "input_tokens": 1200,
    "input_tokens_details": {"cached_tokens": 1000, "cache_write_tokens": 0},
    "output_tokens": 300,
    "output_tokens_details": {"reasoning_tokens": 250},
    "total_tokens": 1500,
}


def events(*payloads: dict[str, Any]) -> bytes:
    """A stream as OpenAI sends it: each event's type both as the SSE event name and in its data."""
    return "".join(f"event: {p['type']}\ndata: {json.dumps(p)}\n\n" for p in payloads).encode()


def answer(items: list[dict[str, Any]], text: str = "", end: str = "response.completed", **response: Any) -> bytes:
    stream: list[dict[str, Any]] = [
        {"type": "response.created", "response": {"model": "gpt-5.5-2026-04-23", "status": "in_progress"}}
    ]
    for index, item in enumerate(items):
        stream.append(
            {"type": "response.output_item.added", "output_index": index, "item": {**item, "status": "in_progress"}}
        )
        if item["type"] == "message":
            for piece in (text[: len(text) // 2], text[len(text) // 2 :]):
                stream.append(
                    {
                        "type": "response.output_text.delta",
                        "item_id": item["id"],
                        "output_index": index,
                        "content_index": 0,
                        "delta": piece,
                        "logprobs": [],
                    }
                )
        if item["type"] == "function_call":
            stream.append(
                {
                    "type": "response.function_call_arguments.delta",
                    "item_id": item["id"],
                    "output_index": index,
                    "delta": item["arguments"],
                }
            )
        stream.append({"type": "response.output_item.done", "output_index": index, "item": item})
    stream.append(
        {"type": end, "response": {"model": "gpt-5.5-2026-04-23", "status": "completed", "usage": USAGE, **response}}
    )
    return events(*stream)


def openai_connection(tmp_path: Path, url: str | None = None) -> Connection:
    return ConnectionStore(tmp_path, MemoryKeyStore()).upsert("OpenAI", "openai", url, "k")


def client_for(handler: Callable[[httpx.Request], httpx.Response]) -> httpx.AsyncClient:
    return httpx.AsyncClient(transport=httpx.MockTransport(handler))


async def no_text(_: str) -> None:
    pass


async def test_openai_itself_is_asked_through_the_responses_api(tmp_path: Path) -> None:
    sent: list[tuple[str, dict[str, Any]]] = []
    streamed: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append((request.url.path, json.loads(request.content)))
        return httpx.Response(200, content=answer([REASONING, MESSAGE], "The mixer."))

    async def on_text(delta: str) -> None:
        streamed.append(delta)

    request = Request("gpt-5.5", "Be brief.", [Message("user", [Text("hi")])], [LOOKUP], 2048, effort="low")
    result = await complete(client_for(handler), openai_connection(tmp_path), "k", request, on_text)

    path, body = sent[0]
    assert path == "/v1/responses"
    assert body["instructions"] == "Be brief." and body["input"] == [{"role": "user", "content": "hi"}]
    assert body["max_output_tokens"] == 2048 and body["stream"] is True
    assert body["store"] is False and body["include"] == ["reasoning.encrypted_content"]
    assert body["reasoning"] == {"effort": "low"}
    assert body["tools"] == [
        {
            "type": "function",
            "name": "search_manual",
            "description": "Search the manual",
            "parameters": LOOKUP.input_schema,
            "strict": False,
        }
    ]
    assert "max_tokens" not in body and "messages" not in body and "reasoning_effort" not in body

    assert result.text == "The mixer." and "".join(streamed) == "The mixer."
    assert result.stop_reason == "end_turn" and result.model == "gpt-5.5-2026-04-23"
    assert (result.usage.input_tokens, result.usage.output_tokens) == (1200, 300)
    assert (result.usage.cached_tokens, result.usage.reasoning_tokens) == (1000, 250)


async def test_no_reasoning_level_leaves_the_models_own(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=answer([MESSAGE], "Hi"))

    request = Request("gpt-5.5", "s", [Message("user", [Text("hi")])])
    await complete(client_for(handler), openai_connection(tmp_path), "k", request, no_text)
    assert "reasoning" not in sent[0] and "tools" not in sent[0]


async def test_a_tool_call_comes_back_and_its_reasoning_goes_with_the_result(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        if len(sent) == 1:
            return httpx.Response(200, content=answer([REASONING, CALL]))
        return httpx.Response(200, content=answer([MESSAGE], "The mixer."))

    connection = openai_connection(tmp_path)
    messages = [Message("user", [Text("Where is the mixer?")])]
    first = await complete(client_for(handler), connection, "k", Request("gpt-5.5", "s", messages, [LOOKUP]), no_text)

    assert first.stop_reason == "tool_use"
    assert first.tool_uses == [ToolUse("call_abc", "search_manual", {"query": "mixer"})]
    assert [b for b in first.blocks if isinstance(b, ProviderItems)] == [ProviderItems([REASONING, CALL])]

    # The assistant's loop: the model's turn as it came, then the tool's result.
    messages += [Message("assistant", first.blocks), Message("user", [ToolResult("call_abc", "Page: Mixer")])]
    second = await complete(client_for(handler), connection, "k", Request("gpt-5.5", "s", messages, [LOOKUP]), no_text)

    assert sent[1]["input"] == [
        {"role": "user", "content": "Where is the mixer?"},
        REASONING,  # unchanged, with its encrypted content: the reasoning carries on
        CALL,
        {"type": "function_call_output", "call_id": "call_abc", "output": "Page: Mixer"},
    ]
    assert second.text == "The mixer." and second.stop_reason == "end_turn"


async def test_a_turn_from_another_provider_is_sent_as_messages_and_calls(tmp_path: Path) -> None:
    """A fallback in the middle of a loop: the earlier turn has no OpenAI items, only its text and calls."""
    sent: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=answer([MESSAGE], "Done."))

    messages = [
        Message("user", [Text("Look it up")]),
        Message("assistant", [Text("Looking."), ToolUse("t1", "search_manual", {"query": "eq"})]),
        Message("user", [ToolResult("t1", "")]),
    ]
    await complete(
        client_for(handler), openai_connection(tmp_path), "k", Request("m", "s", messages, [LOOKUP]), no_text
    )
    assert sent[0]["input"] == [
        {"role": "user", "content": "Look it up"},
        {"role": "assistant", "content": "Looking."},
        {"type": "function_call", "call_id": "t1", "name": "search_manual", "arguments": '{"query": "eq"}'},
        {"type": "function_call_output", "call_id": "t1", "output": "(nothing)"},
    ]


def test_other_providers_skip_openais_items() -> None:
    turn = Message("assistant", [Text("Hi"), ToolUse("call_abc", "search_manual", {}), ProviderItems([REASONING])])
    request = Request("m", "s", [Message("user", [Text("hi")]), turn], [LOOKUP])
    anthropic = anthropic_body(request)["messages"][1]["content"]
    assert [b["type"] for b in anthropic] == ["text", "tool_use"]
    chat = openai_body(request, Connection("x", "Router", "openrouter", "https://openrouter.ai/api/v1"))
    assert chat["messages"][2]["tool_calls"][0]["id"] == "call_abc"


async def test_an_answer_cut_at_the_length_limit_says_so(tmp_path: Path) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            200,
            content=answer(
                [MESSAGE], "The mix", end="response.incomplete", incomplete_details={"reason": "max_output_tokens"}
            ),
        )

    result = await complete(
        client_for(handler),
        openai_connection(tmp_path),
        "k",
        Request("m", "s", [Message("user", [Text("hi")])]),
        no_text,
    )
    assert result.stop_reason == "max_tokens" and result.text == "The mix"


@pytest.mark.parametrize(
    ("stream", "kind"),
    [
        (events({"type": "error", "code": "server_error", "message": "Try again", "sequence_number": 1}), "outage"),
        (
            events(
                {
                    "type": "response.failed",
                    "response": {"status": "failed", "error": {"code": "rate_limit_exceeded", "message": "Slow down"}},
                }
            ),
            "rate_limit",
        ),
        (
            events({"type": "response.failed", "response": {"error": {"code": "invalid_prompt", "message": "No"}}}),
            "bad_request",
        ),
    ],
)
async def test_errors_in_the_stream_are_sorted(tmp_path: Path, stream: bytes, kind: str) -> None:
    with pytest.raises(ProviderError) as caught:
        await complete(
            client_for(lambda r: httpx.Response(200, content=stream)),
            openai_connection(tmp_path),
            "k",
            Request("m", "s", [Message("user", [Text("hi")])]),
            no_text,
        )
    assert caught.value.kind == kind


async def test_other_openai_compatible_services_keep_the_chat_api(tmp_path: Path) -> None:
    paths: list[str] = []

    def handler(request: httpx.Request) -> httpx.Response:
        paths.append(request.url.path)
        chunk = {"model": "local", "choices": [{"delta": {"content": "Hi"}, "finish_reason": "stop"}]}
        return httpx.Response(200, content=f"data: {json.dumps(chunk)}\n\ndata: [DONE]\n\n".encode())

    store = ConnectionStore(tmp_path, MemoryKeyStore())
    for name, provider, url in [
        ("Proxy", "openai", "https://proxy.example/v1"),
        ("Router", "openrouter", None),
        ("Ollama", "local", "http://localhost:11434/v1"),
    ]:
        c = store.upsert(name, provider, url, "k" if provider != "local" else None)
        result = await complete(
            client_for(handler), c, store.key_for(c), Request("m", "s", [Message("user", [Text("hi")])]), no_text
        )
        assert result.text == "Hi"
    assert all(p.endswith("/chat/completions") for p in paths) and len(paths) == 3


async def test_chat_api_asks_again_without_reasoning_when_tools_need_it(tmp_path: Path) -> None:
    """OpenAI's message for gpt-5.5 and newer through the chat API, as a service that copies OpenAI may send it."""
    sent: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        if len(sent) == 1:
            message = (
                "Function tools with reasoning_effort are not supported for gpt-5.5 in /v1/chat/completions. "
                "To use function tools, use /v1/responses or set reasoning_effort to 'none'."
            )
            return httpx.Response(400, json={"error": {"message": message, "param": "reasoning_effort"}})
        chunk = {"model": "gpt-5.5", "choices": [{"delta": {"content": "Hi"}, "finish_reason": "stop"}]}
        return httpx.Response(200, content=f"data: {json.dumps(chunk)}\n\ndata: [DONE]\n\n".encode())

    store = ConnectionStore(tmp_path, MemoryKeyStore())
    c = store.upsert("Work proxy", "openai", "https://proxy.example/v1", "k")
    request = Request("gpt-5.5", "s", [Message("user", [Text("hi")])], [LOOKUP], effort="high")
    result = await complete(client_for(handler), c, "k", request, no_text)

    assert sent[0]["reasoning_effort"] == "high" and sent[1]["reasoning_effort"] == "none" and len(sent) == 2
    assert result.text == "Hi"
    assert "Work proxy" in result.notice and "reasoning was turned off" in result.notice


async def test_chat_api_does_not_retry_a_reasoning_error_without_tools(tmp_path: Path) -> None:
    calls = 0

    def handler(request: httpx.Request) -> httpx.Response:
        nonlocal calls
        calls += 1
        return httpx.Response(400, json={"error": {"message": "Unsupported value: 'reasoning_effort' 'high'."}})

    store = ConnectionStore(tmp_path, MemoryKeyStore())
    c = store.upsert("P", "openai", "https://proxy.example/v1", "k")
    with pytest.raises(ProviderError):
        await complete(
            client_for(handler), c, "k", Request("m", "s", [Message("user", [Text("hi")])], effort="high"), no_text
        )
    assert calls == 1


async def test_chat_api_asks_without_reasoning_once_and_explains_a_model_that_cannot(tmp_path: Path) -> None:
    """gpt-6.1-sol through the chat API refuses tools while it reasons, and refuses "none" too."""
    sent: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        if len(sent) == 1:
            message = (
                "Function tools with reasoning_effort are not supported for gpt-6.1-sol in /v1/chat/completions. "
                "To use function tools, use /v1/responses or set reasoning_effort to 'none'."
            )
        else:
            message = (
                "Unsupported value: 'reasoning_effort' does not support 'none' with this model. Supported values "
                "are: 'low', 'medium', 'high', and 'xhigh'."
            )
        return httpx.Response(400, json={"error": {"message": message, "param": "reasoning_effort"}})

    store = ConnectionStore(tmp_path, MemoryKeyStore())
    c = store.upsert("Work proxy", "openai", "https://proxy.example/v1", "k")
    request = Request("gpt-6.1-sol", "s", [Message("user", [Text("hi")])], [LOOKUP], effort="xhigh")
    with pytest.raises(ProviderError) as caught:
        await complete(client_for(handler), c, "k", request, no_text)

    assert [b.get("reasoning_effort") for b in sent] == ["xhigh", "none"]  # asked again once, never a third time
    assert caught.value.kind == "bad_request"
    assert "can't turn its reasoning off" in str(caught.value) and "api.openai.com" in str(caught.value)


async def test_the_responses_api_takes_any_reasoning_level_the_model_offers(tmp_path: Path) -> None:
    sent: list[dict[str, Any]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        sent.append(json.loads(request.content))
        return httpx.Response(200, content=answer([MESSAGE], "Hi"))

    request = Request("gpt-6.1-sol", "s", [Message("user", [Text("hi")])], [LOOKUP], effort="xhigh")
    await complete(client_for(handler), openai_connection(tmp_path), "k", request, no_text)
    assert sent[0]["reasoning"] == {"effort": "xhigh"} and "tools" in sent[0]
