"""One way to talk to every provider: Anthropic's Messages API, and the OpenAI-compatible
chat API used by OpenRouter, other services and local servers (Ollama, llama.cpp). Both stream."""

from __future__ import annotations

import json
from collections.abc import AsyncIterator, Awaitable, Callable
from dataclasses import dataclass, field
from typing import Any, Literal

import httpx

from .connections import Connection

# ---------------------------------------------------------------------------------------------------
# Messages: one shape for every provider


@dataclass
class Text:
    text: str


@dataclass
class ToolUse:
    id: str
    name: str
    input: dict[str, Any]


@dataclass
class ToolResult:
    tool_use_id: str
    content: str
    is_error: bool = False


Block = Text | ToolUse | ToolResult


@dataclass
class Message:
    role: Literal["user", "assistant"]
    blocks: list[Block]


@dataclass
class Tool:
    name: str
    description: str
    input_schema: dict[str, Any]


@dataclass
class Usage:
    input_tokens: int = 0
    output_tokens: int = 0
    cost: float | None = None  # only when the provider reports it


@dataclass
class Completion:
    blocks: list[Block]
    stop_reason: str
    usage: Usage
    model: str

    @property
    def text(self) -> str:
        return "".join(b.text for b in self.blocks if isinstance(b, Text))

    @property
    def tool_uses(self) -> list[ToolUse]:
        return [b for b in self.blocks if isinstance(b, ToolUse)]


ErrorKind = Literal["auth", "rate_limit", "outage", "network", "bad_request", "cancelled"]


class ProviderError(Exception):
    def __init__(self, kind: ErrorKind, message: str) -> None:
        super().__init__(message)
        self.kind = kind

    @property
    def worth_falling_back(self) -> bool:
        """Another connection may work: this one is down, busy, unreachable or not allowed."""
        return self.kind in ("auth", "rate_limit", "outage", "network")


OnText = Callable[[str], Awaitable[None]]


@dataclass
class Request:
    model: str
    system: str
    messages: list[Message]
    tools: list[Tool] = field(default_factory=list)
    max_tokens: int = 4096


def error_for_status(status: int, body: str) -> ProviderError:
    detail = body.strip()[:300]
    if status in (401, 403):
        return ProviderError("auth", f"The provider refused the key ({status}). {detail}")
    if status == 429:
        return ProviderError("rate_limit", f"The provider's rate limit was reached. {detail}")
    if status >= 500 or status == 529:
        return ProviderError("outage", f"The provider is having problems ({status}). {detail}")
    return ProviderError("bad_request", f"The provider rejected the request ({status}). {detail}")


async def sse_events(response: httpx.Response) -> AsyncIterator[tuple[str, str]]:
    """Server-sent events as (event, data)."""
    event, data = "", list[str]()
    async for line in response.aiter_lines():
        if line == "":
            if data:
                yield event, "\n".join(data)
            event, data = "", list[str]()
        elif line.startswith(":"):
            continue
        elif line.startswith("event:"):
            event = line[6:].strip()
        elif line.startswith("data:"):
            data.append(line[5:].lstrip())
    if data:
        yield event, "\n".join(data)


# ---------------------------------------------------------------------------------------------------
# Anthropic


def anthropic_body(request: Request) -> dict[str, Any]:
    """Anthropic refuses empty text anywhere ("text content blocks must be non-empty"): a model's turn
    that was only tool calls may come back with an empty text block, and a tool may return nothing."""

    def block(b: Block) -> dict[str, Any]:
        if isinstance(b, Text):
            return {"type": "text", "text": b.text}
        if isinstance(b, ToolUse):
            return {"type": "tool_use", "id": b.id, "name": b.name, "input": b.input}
        return {
            "type": "tool_result",
            "tool_use_id": b.tool_use_id,
            "content": b.content if b.content.strip() else "(nothing)",
            "is_error": b.is_error,
        }

    def content(m: Message) -> list[dict[str, Any]]:
        blocks = [block(b) for b in m.blocks if not (isinstance(b, Text) and not b.text.strip())]
        return blocks or [{"type": "text", "text": "(no answer)"}]

    body: dict[str, Any] = {
        "model": request.model,
        "max_tokens": request.max_tokens,
        "system": request.system,
        "messages": [{"role": m.role, "content": content(m)} for m in request.messages],
        "stream": True,
    }
    if request.tools:
        body["tools"] = [
            {"name": t.name, "description": t.description, "input_schema": t.input_schema} for t in request.tools
        ]
    return body


async def complete_anthropic(
    client: httpx.AsyncClient, connection: Connection, key: str, request: Request, on_text: OnText
) -> Completion:
    headers = {"x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"}
    blocks: list[Block] = []
    partial_json: dict[int, str] = {}
    usage = Usage()
    stop_reason = ""
    model = request.model
    async with client.stream(
        "POST", f"{connection.base_url}/v1/messages", headers=headers, json=anthropic_body(request)
    ) as response:
        if response.status_code != 200:
            raise error_for_status(response.status_code, (await response.aread()).decode(errors="replace"))
        async for event, data in sse_events(response):
            payload = json.loads(data)
            kind = payload.get("type", event)
            if kind == "message_start":
                message = payload["message"]
                model = message.get("model", model)
                usage.input_tokens = message.get("usage", {}).get("input_tokens", 0)
            elif kind == "content_block_start":
                start = payload["content_block"]
                if start["type"] == "tool_use":
                    blocks.append(ToolUse(start["id"], start["name"], {}))
                    partial_json[len(blocks) - 1] = ""
                else:
                    blocks.append(Text(""))
            elif kind == "content_block_delta":
                delta = payload["delta"]
                index = payload["index"]
                if delta["type"] == "text_delta":
                    last = blocks[index]
                    assert isinstance(last, Text)
                    last.text += delta["text"]
                    await on_text(delta["text"])
                elif delta["type"] == "input_json_delta":
                    partial_json[index] = partial_json.get(index, "") + delta["partial_json"]
            elif kind == "content_block_stop":
                index = payload["index"]
                use = blocks[index]
                if isinstance(use, ToolUse) and partial_json.get(index):
                    use.input = json.loads(partial_json[index])
            elif kind == "message_delta":
                stop_reason = payload.get("delta", {}).get("stop_reason") or stop_reason
                usage.output_tokens = payload.get("usage", {}).get("output_tokens", usage.output_tokens)
            elif kind == "error":
                error = payload.get("error", {})
                raise ProviderError(
                    "outage" if error.get("type") == "overloaded_error" else "bad_request",
                    error.get("message", "The provider reported an error."),
                )
    return Completion(blocks, stop_reason, usage, model)


# ---------------------------------------------------------------------------------------------------
# OpenAI-compatible (OpenRouter, other services, local servers)


def openai_body(request: Request, connection: Connection) -> dict[str, Any]:
    messages: list[dict[str, Any]] = [{"role": "system", "content": request.system}]
    for m in request.messages:
        texts = "".join(b.text for b in m.blocks if isinstance(b, Text))
        uses = [b for b in m.blocks if isinstance(b, ToolUse)]
        results = [b for b in m.blocks if isinstance(b, ToolResult)]
        if m.role == "assistant":
            entry: dict[str, Any] = {"role": "assistant", "content": texts or None}
            if uses:
                entry["tool_calls"] = [
                    {"id": u.id, "type": "function", "function": {"name": u.name, "arguments": json.dumps(u.input)}}
                    for u in uses
                ]
            messages.append(entry)
        else:
            messages.extend({"role": "tool", "tool_call_id": r.tool_use_id, "content": r.content} for r in results)
            if texts:
                messages.append({"role": "user", "content": texts})
    body: dict[str, Any] = {
        "model": request.model,
        "messages": messages,
        "max_tokens": request.max_tokens,
        "stream": True,
        "stream_options": {"include_usage": True},
    }
    if request.tools:
        body["tools"] = [
            {
                "type": "function",
                "function": {"name": t.name, "description": t.description, "parameters": t.input_schema},
            }
            for t in request.tools
        ]
    if connection.provider == "openrouter":
        body["usage"] = {"include": True}  # OpenRouter then reports the cost
    return body


async def complete_openai(
    client: httpx.AsyncClient, connection: Connection, key: str | None, request: Request, on_text: OnText
) -> Completion:
    headers = {"content-type": "application/json"}
    if key:
        headers["authorization"] = f"Bearer {key}"
    if connection.provider == "openrouter":
        headers["x-title"] = "Spinus DAW"
    text = ""
    calls: dict[int, dict[str, str]] = {}
    usage = Usage()
    stop_reason = ""
    model = request.model
    async with client.stream(
        "POST", f"{connection.base_url}/chat/completions", headers=headers, json=openai_body(request, connection)
    ) as response:
        if response.status_code != 200:
            raise error_for_status(response.status_code, (await response.aread()).decode(errors="replace"))
        async for _, data in sse_events(response):
            if data == "[DONE]":
                break
            payload = json.loads(data)
            if "error" in payload:
                message = payload["error"].get("message", "The provider reported an error.")
                raise ProviderError("bad_request", str(message))
            model = payload.get("model") or model
            if payload.get("usage"):
                u = payload["usage"]
                usage.input_tokens = u.get("prompt_tokens", 0)
                usage.output_tokens = u.get("completion_tokens", 0)
                if isinstance(u.get("cost"), int | float):
                    usage.cost = float(u["cost"])
            for choice in payload.get("choices", []):
                delta = choice.get("delta", {})
                if delta.get("content"):
                    text += delta["content"]
                    await on_text(delta["content"])
                for call in delta.get("tool_calls") or []:
                    slot = calls.setdefault(call.get("index", 0), {"id": "", "name": "", "arguments": ""})
                    slot["id"] = call.get("id") or slot["id"]
                    function = call.get("function", {})
                    slot["name"] += function.get("name") or ""
                    slot["arguments"] += function.get("arguments") or ""
                stop_reason = choice.get("finish_reason") or stop_reason

    blocks: list[Block] = [Text(text)] if text else []
    for index in sorted(calls):
        call = calls[index]
        try:
            arguments = json.loads(call["arguments"] or "{}")
        except json.JSONDecodeError:
            arguments = {"_unparsed": call["arguments"]}
        blocks.append(ToolUse(call["id"] or f"call_{index}", call["name"], arguments))
    return Completion(blocks, "tool_use" if calls else (stop_reason or "end_turn"), usage, model)


# ---------------------------------------------------------------------------------------------------


async def complete(
    client: httpx.AsyncClient, connection: Connection, key: str | None, request: Request, on_text: OnText
) -> Completion:
    try:
        if connection.provider == "anthropic":
            if not key:
                raise ProviderError("auth", f"The connection '{connection.name}' has no key.")
            return await complete_anthropic(client, connection, key, request, on_text)
        if connection.provider != "local" and not key:
            raise ProviderError("auth", f"The connection '{connection.name}' has no key.")
        return await complete_openai(client, connection, key, request, on_text)
    except httpx.TransportError as error:
        raise ProviderError("network", f"Could not reach {connection.base_url}: {error}") from error


async def list_models(client: httpx.AsyncClient, connection: Connection, key: str | None) -> list[str]:
    """The provider's model names, for choosing one (a connection test)."""
    try:
        if connection.provider == "anthropic":
            headers = {"x-api-key": key or "", "anthropic-version": "2023-06-01"}
            response = await client.get(f"{connection.base_url}/v1/models", headers=headers, params={"limit": 100})
        else:
            headers = {"authorization": f"Bearer {key}"} if key else {}
            response = await client.get(f"{connection.base_url}/models", headers=headers)
    except httpx.TransportError as error:
        raise ProviderError("network", f"Could not reach {connection.base_url}: {error}") from error
    if response.status_code != 200:
        raise error_for_status(response.status_code, response.text)
    data = response.json()
    items = data.get("data", data.get("models", []))
    return [str(m.get("id") or m.get("name")) for m in items if isinstance(m, dict)]
