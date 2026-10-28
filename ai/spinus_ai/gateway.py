"""One way to talk to every provider: Anthropic's Messages API, OpenAI's Responses API (OpenAI's own
service), and the OpenAI-compatible chat API used by OpenRouter, other services and local servers (Ollama,
llama.cpp). All stream."""

from __future__ import annotations

import contextlib
import json
from collections.abc import AsyncIterator, Awaitable, Callable
from dataclasses import dataclass, field
from typing import Any, Literal
from urllib.parse import urlparse

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


@dataclass
class ProviderItems:
    """A turn's output items from OpenAI's Responses API (its reasoning, messages and tool calls), passed back
    unchanged on the next request of a tool loop, as OpenAI asks: the reasoning carries on between tool calls.
    The turn's Text and ToolUse blocks hold the same answer for every other provider, which skips these."""

    items: list[dict[str, Any]]


Block = Text | ToolUse | ToolResult | ProviderItems


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
    output_tokens: int = 0  # with the reasoning tokens (both kinds of provider count them in)
    reasoning_tokens: int = 0  # of output_tokens, how many the model spent thinking (when the provider says)
    cost: float | None = None  # only when the provider reports it
    cached_tokens: int = 0  # of input_tokens' kind, read from the provider's cache (cheaper), when it says


@dataclass
class Completion:
    blocks: list[Block]
    stop_reason: str
    usage: Usage
    model: str
    notice: str = ""  # something the user should know about how this answer was made

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
    effort: str = ""  # "low", "medium", "high" (or "xhigh" where offered): how hard the model reasons; "" leaves
    # the provider's default


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

    def block(b: Text | ToolUse | ToolResult) -> dict[str, Any]:
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
        blocks = [
            block(b)
            for b in m.blocks
            if not isinstance(b, ProviderItems) and not (isinstance(b, Text) and not b.text.strip())
        ]
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


def openai_body(request: Request, connection: Connection, limit: str = "max_tokens") -> dict[str, Any]:
    """`limit` names the answer-length cap: most services know the older "max_tokens", some only take
    "max_completion_tokens" (as OpenAI's newer models do)."""
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
        limit: request.max_tokens,
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
    if request.effort:
        # OpenRouter's own field; OpenAI's (and the services that copy it) otherwise.
        if connection.provider == "openrouter":
            body["reasoning"] = {"effort": request.effort}
        else:
            body["reasoning_effort"] = request.effort
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
    limits = ["max_tokens", "max_completion_tokens"]  # OpenAI's own service is asked through complete_responses
    limit, no_reasoning, notice = limits[0], False, ""
    async with contextlib.AsyncExitStack() as stack:
        for _ in range(3):
            body = openai_body(request, connection, limit)
            if no_reasoning:
                body.pop("reasoning", None)
                body["reasoning_effort"] = "none"
            response = await stack.enter_async_context(
                client.stream("POST", f"{connection.base_url}/chat/completions", headers=headers, json=body)
            )
            if response.status_code == 200:
                break
            error = (await response.aread()).decode(errors="replace")
            other = limits[1] if limit == limits[0] else ""
            if response.status_code == 400 and other and f"'{limit}'" in error and other in error:
                limit = other  # the service named the other length cap: ask again with it
            elif response.status_code == 400 and request.tools and "reasoning_effort" in error and not no_reasoning:
                # Some models take tools only without reasoning on this API (OpenAI's gpt-5.5 and newer, through
                # a service other than OpenAI's own): ask again with reasoning off, and say so.
                no_reasoning = True
                notice = (
                    f"{connection.name} does not let {request.model} use tools while it reasons, so its reasoning "
                    "was turned off for this answer."
                )
            elif response.status_code == 400 and no_reasoning:
                # Asked once with reasoning off, and the model can't turn it off either (gpt-6.1-sol: "does not
                # support 'none'"): no way to use tools with it here. Never asked a third time.
                raise ProviderError(
                    "bad_request",
                    f"{connection.name} does not let {request.model} use tools while it reasons, and the model "
                    "can't turn its reasoning off. Use the model through OpenAI's own address "
                    f"(api.openai.com), or another model or connection. {error.strip()[:300]}",
                )
            else:
                raise error_for_status(response.status_code, error)
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
    return Completion(blocks, "tool_use" if calls else (stop_reason or "end_turn"), usage, model, notice)


# ---------------------------------------------------------------------------------------------------
# OpenAI's Responses API (OpenAI's own service: its newer models take tools only there while they reason)


def uses_responses_api(connection: Connection) -> bool:
    return connection.provider == "openai" and urlparse(connection.base_url).hostname == "api.openai.com"


def responses_body(request: Request) -> dict[str, Any]:
    """Nothing is kept on OpenAI's side (store false): the reasoning comes back encrypted with each answer and is
    sent again with the next request of a tool loop, in the turn's ProviderItems."""
    items: list[dict[str, Any]] = []
    for m in request.messages:
        texts = "".join(b.text for b in m.blocks if isinstance(b, Text))
        if m.role == "assistant":
            kept = [i for b in m.blocks if isinstance(b, ProviderItems) for i in b.items]
            if kept:
                items.extend(kept)
                continue
            if texts:
                items.append({"role": "assistant", "content": texts})
            items.extend(
                {"type": "function_call", "call_id": b.id, "name": b.name, "arguments": json.dumps(b.input)}
                for b in m.blocks
                if isinstance(b, ToolUse)
            )
        else:
            items.extend(
                {"type": "function_call_output", "call_id": b.tool_use_id, "output": b.content or "(nothing)"}
                for b in m.blocks
                if isinstance(b, ToolResult)
            )
            if texts:
                items.append({"role": "user", "content": texts})
    body: dict[str, Any] = {
        "model": request.model,
        "instructions": request.system,
        "input": items,
        "max_output_tokens": request.max_tokens,
        "stream": True,
        "store": False,
        "include": ["reasoning.encrypted_content"],
    }
    if request.tools:
        body["tools"] = [
            {
                "type": "function",
                "name": t.name,
                "description": t.description,
                "parameters": t.input_schema,
                "strict": False,
            }
            for t in request.tools
        ]
    if request.effort:
        body["reasoning"] = {"effort": request.effort}
    return body


def responses_usage(usage: dict[str, Any]) -> Usage:
    return Usage(
        input_tokens=int(usage.get("input_tokens") or 0),
        output_tokens=int(usage.get("output_tokens") or 0),
        reasoning_tokens=int((usage.get("output_tokens_details") or {}).get("reasoning_tokens") or 0),
        cached_tokens=int((usage.get("input_tokens_details") or {}).get("cached_tokens") or 0),
    )


def responses_error(error: dict[str, Any] | None) -> ProviderError:
    error = error or {}
    code = str(error.get("code") or "")
    kind: ErrorKind = "outage" if code == "server_error" else "rate_limit" if "rate_limit" in code else "bad_request"
    return ProviderError(kind, str(error.get("message") or "The provider reported an error."))


async def complete_responses(
    client: httpx.AsyncClient, connection: Connection, key: str | None, request: Request, on_text: OnText
) -> Completion:
    headers = {"content-type": "application/json"}
    if key:
        headers["authorization"] = f"Bearer {key}"
    text = ""
    items: dict[int, dict[str, Any]] = {}
    usage = Usage()
    stop_reason = ""
    model = request.model
    async with client.stream(
        "POST", f"{connection.base_url}/responses", headers=headers, json=responses_body(request)
    ) as response:
        if response.status_code != 200:
            raise error_for_status(response.status_code, (await response.aread()).decode(errors="replace"))
        async for event, data in sse_events(response):
            payload = json.loads(data)
            kind = payload.get("type", event)
            if kind == "response.output_text.delta":
                text += payload.get("delta", "")
                await on_text(payload.get("delta", ""))
            elif kind == "response.output_item.done":
                # The finished item: a reasoning item's encrypted content is complete only here.
                items[int(payload.get("output_index", len(items)))] = payload["item"]
            elif kind in ("response.created", "response.completed", "response.incomplete"):
                done = payload.get("response") or {}
                model = done.get("model") or model
                if done.get("usage"):
                    usage = responses_usage(done["usage"])
                if kind == "response.incomplete":
                    reason = (done.get("incomplete_details") or {}).get("reason")
                    stop_reason = "max_tokens" if reason == "max_output_tokens" else str(reason or "incomplete")
            elif kind == "response.failed":
                raise responses_error((payload.get("response") or {}).get("error"))
            elif kind == "error":
                raise responses_error(payload)

    kept = [items[i] for i in sorted(items)]
    blocks: list[Block] = [Text(text)] if text else []
    for index, item in enumerate(kept):
        if item.get("type") != "function_call":
            continue
        try:
            arguments = json.loads(item.get("arguments") or "{}")
        except json.JSONDecodeError:
            arguments = {"_unparsed": item.get("arguments")}
        blocks.append(ToolUse(str(item.get("call_id") or f"call_{index}"), str(item.get("name") or ""), arguments))
    if kept:
        blocks.append(ProviderItems(kept))
    calls = any(isinstance(b, ToolUse) for b in blocks)
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
        if uses_responses_api(connection):
            return await complete_responses(client, connection, key, request, on_text)
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
