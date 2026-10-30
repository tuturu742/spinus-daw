"""One way to talk to every provider: Anthropic's Messages API, OpenAI's Responses API (OpenAI's own
service), Ollama's own chat API (a local Ollama: it alone lets a request set the context length), and the
OpenAI-compatible chat API used by OpenRouter, other services and other local servers (llama.cpp, LM Studio).
All stream."""

from __future__ import annotations

import asyncio
import contextlib
import json
import re
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
    partial: str = ""  # the arguments as they arrived when they could not be read (an answer cut off)


@dataclass
class ToolResult:
    tool_use_id: str
    content: str
    is_error: bool = False


@dataclass
class Thinking:
    """A model's reasoning block (Anthropic), passed back unchanged in a tool loop as the provider asks:
    `thinking` (often empty: the reasoning is not shown) and its `signature`, or a redacted block's `data`."""

    thinking: str = ""
    signature: str = ""
    data: str = ""  # redacted_thinking


@dataclass
class ProviderItems:
    """A turn's output items from OpenAI's Responses API (its reasoning, messages and tool calls), passed back
    unchanged on the next request of a tool loop, as OpenAI asks: the reasoning carries on between tool calls.
    The turn's Text and ToolUse blocks hold the same answer for every other provider, which skips these."""

    items: list[dict[str, Any]]


Block = Text | ToolUse | ToolResult | Thinking | ProviderItems


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
    cleared_tokens: int = 0  # input the provider cleared from the conversation before reading it (context editing)


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
    def __init__(self, kind: ErrorKind, message: str, passing: bool = False) -> None:
        super().__init__(message)
        self.kind = kind
        # Worth asking the same connection again at once: a dropped connection, or a server's trouble in the middle
        # of an answer. A refused request (a 5xx before the answer) goes to the next connection instead.
        self.passing = passing or kind == "network"

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
    max_tokens: int | None = 4096  # None: the model's whole output allowance (ANSWER_ROOM where one is required)
    effort: str = ""  # "low", "medium", "high" (or "xhigh" where offered): how hard the model reasons; "" leaves
    # the provider's default
    cache: bool = False  # ask the provider to cache the request's start (tools, system, earlier turns) for the next
    # Anthropic's context editing (beta CONTEXT_EDITING): what the provider may clear from the conversation on its side
    # (old tool results), which does not count as an edit of the history.
    context_edits: list[dict[str, Any]] = field(default_factory=list)
    # The provider's tokens per Spinus's estimate (characters over four), from the last answer to this request: notes
    # ("0 40 1 90; ...") count several times more tokens than prose. The clearing done here sizes with it.
    token_ratio: float = 0.0


EFFORTS = ("low", "medium", "high", "xhigh")
CONTEXT_EDITING = "context-management-2025-06-27"
# The answer's room when the provider requires a number and none is known (Anthropic): the most every current
# model writes; a model that allows less says so (400) and is asked again with what it names, or SMALLER_ROOM.
ANSWER_ROOM = 64000
SMALLER_ROOM = (32000, 8192)
# Why an answer stopped when it ran out of room (Anthropic, OpenAI).
CUT_OFF = ("max_tokens", "length")
# A model's answer: long conversations and reasoning can keep a provider quiet for minutes before it streams.
ANSWER_TIMEOUT = httpx.Timeout(600.0, connect=15.0)
# A dropped connection, or a provider's passing trouble in the middle of an answer ("internal error" in its stream),
# is asked again twice: a step of a long tool loop is not lost to one bad answer.
NETWORK_RETRY_WAITS = (2.0, 8.0)


def network_trouble(error: httpx.TransportError) -> str:
    """What went wrong, in words, for errors whose text is empty (a timeout, a closed connection)."""
    if str(error):
        return str(error)
    if isinstance(error, httpx.TimeoutException):
        return "no answer in time"
    if isinstance(error, httpx.RemoteProtocolError):
        return "the connection was closed before the answer was complete"
    if isinstance(error, httpx.ConnectError):
        return "could not connect"
    return type(error).__name__


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

    def block(b: Text | ToolUse | ToolResult | Thinking) -> dict[str, Any]:
        if isinstance(b, Text):
            return {"type": "text", "text": b.text}
        if isinstance(b, Thinking):
            if b.data:
                return {"type": "redacted_thinking", "data": b.data}
            return {"type": "thinking", "thinking": b.thinking, "signature": b.signature}
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
            if not isinstance(b, ProviderItems)
            and not (isinstance(b, Text) and not b.text.strip())
            and not (isinstance(b, Thinking) and not b.signature and not b.data)
        ]
        return blocks or [{"type": "text", "text": "(no answer)"}]

    body: dict[str, Any] = {
        "model": request.model,
        "max_tokens": ANSWER_ROOM if request.max_tokens is None else request.max_tokens,
        "system": request.system,
        "messages": [{"role": m.role, "content": content(m)} for m in request.messages],
        "stream": True,
    }
    if request.tools:
        body["tools"] = [
            {"name": t.name, "description": t.description, "input_schema": t.input_schema} for t in request.tools
        ]
    if request.effort:
        body["output_config"] = {"effort": request.effort}
    if request.context_edits:
        body["context_management"] = {"edits": request.context_edits}
    if request.cache:
        body["cache_control"] = {"type": "ephemeral"}  # the prefix up to the last turn, read back next time
    return body


def anthropic_reasoning(usage: dict[str, Any]) -> int | None:
    """The thinking tokens an Anthropic usage reports (output_tokens_details.thinking_tokens), if it does."""
    details = usage.get("output_tokens_details")
    value = details.get("thinking_tokens") if isinstance(details, dict) else None
    return int(value) if isinstance(value, int) else None


async def complete_anthropic(
    client: httpx.AsyncClient, connection: Connection, key: str, request: Request, on_text: OnText
) -> Completion:
    headers = {"x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"}
    if request.context_edits:
        headers["anthropic-beta"] = CONTEXT_EDITING
    blocks: list[Block] = []
    partial_json: dict[int, str] = {}
    usage = Usage()
    stop_reason = ""
    model = request.model
    body = anthropic_body(request)
    rooms = list(SMALLER_ROOM) if request.max_tokens is None else []
    async with contextlib.AsyncExitStack() as stack:
        while True:
            response = await stack.enter_async_context(
                client.stream(
                    "POST", f"{connection.base_url}/v1/messages", headers=headers, json=body, timeout=ANSWER_TIMEOUT
                )
            )
            if response.status_code == 200:
                break
            text = (await response.aread()).decode(errors="replace")
            if not (response.status_code == 400 and "max_tokens" in text and rooms):
                raise error_for_status(response.status_code, text)
            # The model allows a shorter answer: the most it names, else the next smaller room.
            named = [int(n) for n in re.findall(r"\b(\d{4,6})\b", text) if int(n) < body["max_tokens"]]
            body["max_tokens"] = max(named) if named else rooms.pop(0)
            rooms = [r for r in rooms if r < body["max_tokens"]]
        async for event, data in sse_events(response):
            payload = json.loads(data)
            kind = payload.get("type", event)
            if kind == "message_start":
                message = payload["message"]
                model = message.get("model", model)
                started = message.get("usage", {})
                # All the input, as OpenAI counts it: Anthropic's input_tokens leave out what was read from or
                # written to the cache.
                usage.cached_tokens = int(started.get("cache_read_input_tokens") or 0)
                usage.input_tokens = (
                    int(started.get("input_tokens") or 0)
                    + usage.cached_tokens
                    + int(started.get("cache_creation_input_tokens") or 0)
                )
                usage.reasoning_tokens = anthropic_reasoning(message.get("usage", {})) or 0
            elif kind == "content_block_start":
                start = payload["content_block"]
                if start["type"] == "tool_use":
                    blocks.append(ToolUse(start["id"], start["name"], {}))
                    partial_json[len(blocks) - 1] = ""
                elif start["type"] == "thinking":
                    blocks.append(Thinking(str(start.get("thinking") or ""), str(start.get("signature") or "")))
                elif start["type"] == "redacted_thinking":
                    blocks.append(Thinking(data=str(start.get("data") or "")))
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
                elif delta["type"] in ("thinking_delta", "signature_delta"):
                    thought = blocks[index]
                    if isinstance(thought, Thinking):
                        thought.thinking += str(delta.get("thinking") or "")
                        thought.signature += str(delta.get("signature") or "")
            elif kind == "content_block_stop":
                index = payload["index"]
                use = blocks[index]
                if isinstance(use, ToolUse) and partial_json.get(index):
                    try:
                        use.input = json.loads(partial_json.pop(index))
                    except json.JSONDecodeError as unread:
                        use.partial = str(unread.doc)
            elif kind == "message_delta":
                stop_reason = payload.get("delta", {}).get("stop_reason") or stop_reason
                usage.output_tokens = payload.get("usage", {}).get("output_tokens", usage.output_tokens)
                applied = (payload.get("context_management") or {}).get("applied_edits") or []
                usage.cleared_tokens += sum(
                    int(e.get("cleared_input_tokens") or 0) for e in applied if isinstance(e, dict)
                )
                thought = anthropic_reasoning(payload.get("usage", {}))
                usage.reasoning_tokens = usage.reasoning_tokens if thought is None else thought
            elif kind == "error":
                error = payload.get("error", {})
                raise ProviderError(
                    "outage" if error.get("type") == "overloaded_error" else "bad_request",
                    error.get("message", "The provider reported an error."),
                )
    for index, text in partial_json.items():  # cut off before the block's end
        use = blocks[index]
        if isinstance(use, ToolUse) and text:
            use.partial = text
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
        "stream": True,
        "stream_options": {"include_usage": True},
    }
    if request.max_tokens is not None:  # None: the service's own limit, the model's whole allowance
        body[limit] = request.max_tokens
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


CLEARED = "[cleared]"


def clear_old_tool_calls(request: Request) -> int:
    """Anthropic's clear_tool_uses edit (request.context_edits), done here for the services that have no context
    editing of their own (chat completions): past the edit's trigger, the oldest tool calls' results (and inputs) are
    replaced by a note, all but the last `keep`, at least `clear_at_least` tokens at a time. The request's messages are
    changed in place, so a tool loop's later requests start with the same cleared history, and the provider's prefix
    cache holds between clearings. A plain chat history may be edited; nothing here preserves reasoning. Returns the
    tokens cleared now (characters over four)."""

    ratio = min(4.0, max(0.5, request.token_ratio)) if request.token_ratio > 0 else 1.0

    def size(value: Any) -> int:
        return int(len(json.dumps(value, default=lambda o: o.__dict__)) / 4 * ratio)

    cleared = 0
    for edit in request.context_edits:
        if edit.get("type") != "clear_tool_uses_20250919":
            continue
        if size([request.system, request.messages]) <= int(edit["trigger"]["value"]):
            continue
        excluded = set(edit.get("exclude_tools", []))
        uses = [
            b
            for m in request.messages
            if m.role == "assistant"
            for b in m.blocks
            if isinstance(b, ToolUse) and b.name not in excluded
        ]
        old = {b.id: b for b in uses[: max(0, len(uses) - int(edit.get("keep", {}).get("value", 3)))]}
        results = [
            b
            for m in request.messages
            for b in m.blocks
            if isinstance(b, ToolResult) and b.tool_use_id in old and b.content != CLEARED
        ]
        gain = sum(size(r.content) for r in results)
        if edit.get("clear_tool_inputs"):
            gain += sum(size(old[r.tool_use_id].input) for r in results)
        if not results or gain < int((edit.get("clear_at_least") or {}).get("value", 0)):
            continue
        for r in results:
            r.content = CLEARED
            if edit.get("clear_tool_inputs"):
                old[r.tool_use_id].input = {}
        cleared += gain
    return cleared


async def complete_openai(
    client: httpx.AsyncClient, connection: Connection, key: str | None, request: Request, on_text: OnText
) -> Completion:
    cleared = clear_old_tool_calls(request) if request.context_edits else 0
    headers = {"content-type": "application/json"}
    if key:
        headers["authorization"] = f"Bearer {key}"
    if connection.provider == "openrouter":
        headers["x-title"] = "Spinus DAW"
    text = ""
    calls: dict[int, dict[str, str]] = {}
    usage = Usage(cleared_tokens=cleared)
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
                client.stream(
                    "POST",
                    f"{connection.base_url}/chat/completions",
                    headers=headers,
                    json=body,
                    timeout=ANSWER_TIMEOUT,
                )
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
                raise chat_error(payload["error"])
            model = payload.get("model") or model
            if payload.get("usage"):
                u = payload["usage"]
                usage.input_tokens = u.get("prompt_tokens", 0)
                usage.output_tokens = u.get("completion_tokens", 0)
                prompt_details = u.get("prompt_tokens_details")
                if isinstance(prompt_details, dict) and isinstance(prompt_details.get("cached_tokens"), int):
                    usage.cached_tokens = prompt_details["cached_tokens"]
                details = u.get("completion_tokens_details")
                if isinstance(details, dict) and isinstance(details.get("reasoning_tokens"), int):
                    usage.reasoning_tokens = details["reasoning_tokens"]
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
            blocks.append(ToolUse(call["id"] or f"call_{index}", call["name"], {"_unparsed": call["arguments"]},
                                  call["arguments"]))  # fmt: skip
            continue
        blocks.append(ToolUse(call["id"] or f"call_{index}", call["name"], arguments))
    if usage.input_tokens > 0:  # how the provider counts against the estimate, for the next clearing
        request.token_ratio = usage.input_tokens / max(1.0, len(json.dumps(body)) / 4)
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
        "stream": True,
        "store": False,
        "include": ["reasoning.encrypted_content"],
    }
    if request.max_tokens is not None:  # None: the model's whole allowance
        body["max_output_tokens"] = request.max_tokens
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


TRANSIENT_WORDS = ("internal", "overloaded", "unavailable", "temporar", "try again", "timed out", "timeout")


def chat_error(error: Any) -> ProviderError:
    """An error a chat-completions service sends inside its stream (OpenRouter passes on the model provider's): a
    server's trouble (a code of 500 or more, or words like "internal error") is an outage, asked again; the rest are
    the request's."""
    error = error if isinstance(error, dict) else {"message": str(error)}
    message = str(error.get("message") or "The provider reported an error.")
    code = error.get("code")
    number = int(code) if isinstance(code, int) or (isinstance(code, str) and code.isdigit()) else 0
    if number == 429 or "rate limit" in message.lower():
        return ProviderError("rate_limit", message)
    if number >= 500 or any(word in message.lower() for word in TRANSIENT_WORDS):
        return ProviderError("outage", message, passing=True)
    return ProviderError("bad_request", message)


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
        "POST",
        f"{connection.base_url}/responses",
        headers=headers,
        json=responses_body(request),
        timeout=ANSWER_TIMEOUT,
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
            raw = str(item.get("arguments") or "")
            blocks.append(ToolUse(str(item.get("call_id") or f"call_{index}"), str(item.get("name") or ""),
                                  {"_unparsed": raw}, raw))  # fmt: skip
            continue
        blocks.append(ToolUse(str(item.get("call_id") or f"call_{index}"), str(item.get("name") or ""), arguments))
    if kept:
        blocks.append(ProviderItems(kept))
    calls = any(isinstance(b, ToolUse) for b in blocks)
    return Completion(blocks, "tool_use" if calls else (stop_reason or "end_turn"), usage, model)


# ---------------------------------------------------------------------------------------------------
# Ollama's own chat API: the OpenAI-compatible one can't set the context length, and Ollama's default context
# silently cuts off a long conversation (a music harness's tools and system prompt alone are a few thousand tokens).

OLLAMA_PORT = 11434
# The least context asked for, and the steps it grows in (powers of two: a new size reloads the model, so it changes
# seldom, and never shrinks for a model while Spinus runs).
OLLAMA_MIN_CONTEXT = 16384
# The answer's room when the request leaves it to the model.
OLLAMA_ANSWER_ROOM = 16384
_ollama_servers: dict[str, bool] = {}  # server root -> whether it is Ollama
_ollama_limits: dict[tuple[str, str], int] = {}  # (server root, model) -> the model's own context length
_ollama_contexts: dict[tuple[str, str], int] = {}  # (server root, model) -> the largest context asked for so far


def ollama_root(connection: Connection) -> str:
    """The server's address without the OpenAI-compatible API's /v1."""
    url = connection.base_url.rstrip("/")
    return url[: -len("/v1")] if url.endswith("/v1") else url


async def is_ollama(client: httpx.AsyncClient, connection: Connection) -> bool:
    """A local connection to Ollama: its usual port, or a server that answers Ollama's /api/version (asked once
    per server)."""
    if connection.provider != "local":
        return False
    root = ollama_root(connection)
    if root not in _ollama_servers:
        if urlparse(root).port == OLLAMA_PORT:
            _ollama_servers[root] = True
        else:
            try:
                response = await client.get(f"{root}/api/version", timeout=httpx.Timeout(3.0))
                _ollama_servers[root] = response.status_code == 200 and "version" in response.json()
            except (httpx.TransportError, ValueError):
                _ollama_servers[root] = False
    return _ollama_servers[root]


async def ollama_model_context(client: httpx.AsyncClient, root: str, model: str) -> int:
    """The model's own context length (/api/show's model_info), 0 when Ollama doesn't say."""
    if (root, model) not in _ollama_limits:
        limit = 0
        try:
            response = await client.post(f"{root}/api/show", json={"model": model}, timeout=httpx.Timeout(10.0))
            if response.status_code == 200:
                info = response.json().get("model_info") or {}
                limit = max((int(v) for k, v in info.items() if k.endswith(".context_length")), default=0)
        except (httpx.TransportError, ValueError):
            pass
        _ollama_limits[(root, model)] = limit
    return _ollama_limits[(root, model)]


def ollama_context(request: Request, root: str, limit: int) -> int:
    """num_ctx for this request: the prompt (estimated, scaled by how the model counted before) plus the answer's
    room, rounded up to a power of two, at least OLLAMA_MIN_CONTEXT, never below what this model was given before,
    and no more than the model's own context length."""
    ratio = min(4.0, max(0.5, request.token_ratio)) if request.token_ratio > 0 else 1.0
    prompt = json.dumps([request.system, request.messages, request.tools], default=lambda o: o.__dict__)
    need = int(len(prompt) / 4 * ratio) + (request.max_tokens or OLLAMA_ANSWER_ROOM)
    size = OLLAMA_MIN_CONTEXT
    while size < need:
        size *= 2
    size = max(size, _ollama_contexts.get((root, request.model), 0))
    if limit > 0:
        size = min(size, limit)
    _ollama_contexts[(root, request.model)] = max(size, _ollama_contexts.get((root, request.model), 0))
    return size


def ollama_think(request: Request) -> bool | str | None:
    """Ollama's `think`: the level itself for models that take one (gpt-oss), else on; None leaves the model's
    default."""
    if not request.effort:
        return None
    return request.effort if request.effort in ("low", "medium", "high") and "gpt-oss" in request.model else True


def ollama_body(request: Request, num_ctx: int) -> dict[str, Any]:
    messages: list[dict[str, Any]] = [{"role": "system", "content": request.system}]
    names: dict[str, str] = {}
    for m in request.messages:
        texts = "".join(b.text for b in m.blocks if isinstance(b, Text))
        uses = [b for b in m.blocks if isinstance(b, ToolUse)]
        results = [b for b in m.blocks if isinstance(b, ToolResult)]
        if m.role == "assistant":
            entry: dict[str, Any] = {"role": "assistant", "content": texts}
            if uses:
                entry["tool_calls"] = [{"function": {"name": u.name, "arguments": u.input}} for u in uses]
                names.update({u.id: u.name for u in uses})
            messages.append(entry)
        else:
            messages.extend(
                {"role": "tool", "content": r.content, "tool_name": names.get(r.tool_use_id, "")} for r in results
            )
            if texts:
                messages.append({"role": "user", "content": texts})
    options: dict[str, Any] = {"num_ctx": num_ctx}
    if request.max_tokens is not None:
        options["num_predict"] = request.max_tokens
    body: dict[str, Any] = {"model": request.model, "messages": messages, "stream": True, "options": options}
    if request.tools:
        body["tools"] = [
            {
                "type": "function",
                "function": {"name": t.name, "description": t.description, "parameters": t.input_schema},
            }
            for t in request.tools
        ]
    think = ollama_think(request)
    if think is not None:
        body["think"] = think
    return body


class NotOllama(Exception):
    """The server has no /api/chat after all: the OpenAI-compatible API is used instead."""


async def complete_ollama(
    client: httpx.AsyncClient, connection: Connection, request: Request, on_text: OnText
) -> Completion:
    cleared = clear_old_tool_calls(request) if request.context_edits else 0
    root = ollama_root(connection)
    num_ctx = ollama_context(request, root, await ollama_model_context(client, root, request.model))
    body = ollama_body(request, num_ctx)
    text, thought = "", ""
    calls: list[ToolUse] = []
    usage = Usage(cleared_tokens=cleared)
    stop_reason = ""
    model = request.model
    async with client.stream("POST", f"{root}/api/chat", json=body, timeout=ANSWER_TIMEOUT) as response:
        if response.status_code != 200:
            error = (await response.aread()).decode(errors="replace")
            if response.status_code == 404 and "model" not in error:  # no /api/chat here: not Ollama after all
                raise NotOllama()
            raise error_for_status(response.status_code, error)
        async for line in response.aiter_lines():
            if not line.strip():
                continue
            payload = json.loads(line)
            if payload.get("error"):
                raise chat_error({"message": payload["error"]})
            model = payload.get("model") or model
            message = payload.get("message") or {}
            if message.get("thinking"):
                thought += message["thinking"]
            if message.get("content"):
                text += message["content"]
                await on_text(message["content"])
            for call in message.get("tool_calls") or []:
                function = call.get("function") or {}
                arguments = function.get("arguments")
                if isinstance(arguments, str):
                    try:
                        arguments = json.loads(arguments or "{}")
                    except json.JSONDecodeError:
                        calls.append(ToolUse(call.get("id") or f"call_{len(calls)}", function.get("name", ""),
                                             {"_unparsed": arguments}, arguments))  # fmt: skip
                        continue
                calls.append(ToolUse(call.get("id") or f"call_{len(calls)}", function.get("name", ""), arguments or {}))
            if payload.get("done"):
                stop_reason = payload.get("done_reason") or "stop"
                usage.input_tokens = int(payload.get("prompt_eval_count") or 0)
                usage.output_tokens = int(payload.get("eval_count") or 0)
    # Ollama counts the reasoning in eval_count without saying how much: estimated from its text (characters over four).
    usage.reasoning_tokens = min(usage.output_tokens, len(thought) // 4)
    blocks: list[Block] = [Text(text)] if text else []
    blocks.extend(calls)
    if usage.input_tokens > 0:
        request.token_ratio = usage.input_tokens / max(1.0, len(json.dumps(body)) / 4)
    return Completion(blocks, "tool_use" if calls else (stop_reason or "end_turn"), usage, model)


# ---------------------------------------------------------------------------------------------------


def offered_effort(asked: str, error: str) -> str | None:
    """The level to ask again with when a model refused the reasoning level `asked`: the lowest it says it supports
    ("Supported values are: 'medium' and 'high'"), else medium for low; None when the refusal is about something
    else, or names no level to use."""
    if asked not in EFFORTS or not ("effort" in error.lower() or "reasoning" in error.lower()):
        return None
    if "turn its reasoning off" in error:  # the chat API's own retry already failed: no level would do
        return None
    tail = error[error.lower().find("support") :] if "support" in error.lower() else ""
    named = [level for level in EFFORTS if level != asked and f"'{level}'" in tail]
    if named:
        return named[0]
    # Nothing named: only the default low (a model whose least reasoning is more) steps up, never a level the user
    # chose higher, which is not guessed at.
    return "medium" if asked == "low" else None


async def complete(
    client: httpx.AsyncClient, connection: Connection, key: str | None, request: Request, on_text: OnText
) -> Completion:
    """The model's answer. A model that refuses the reasoning level asked for is asked once more with the lowest
    level it offers (request.effort changes, so a tool loop's later turns use it too), and the answer says so."""
    try:
        return await complete_retrying(client, connection, key, request, on_text)
    except ProviderError as error:
        asked = request.effort
        level = offered_effort(asked, str(error)) if error.kind == "bad_request" else None
        if level is None:
            raise
        request.effort = level
        completion = await complete_retrying(client, connection, key, request, on_text)
        said = f"{request.model} does not reason at the level '{asked}', so '{level}', the lowest it offers, was used."
        completion.notice = f"{completion.notice} {said}".strip()
        return completion


async def complete_retrying(
    client: httpx.AsyncClient, connection: Connection, key: str | None, request: Request, on_text: OnText
) -> Completion:
    for wait in (*NETWORK_RETRY_WAITS, None):
        try:
            return await complete_once(client, connection, key, request, on_text)
        except ProviderError as error:
            if not error.passing or wait is None:
                raise
            await asyncio.sleep(wait)
    raise AssertionError("unreachable")


async def complete_once(
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
        if await is_ollama(client, connection):
            try:
                return await complete_ollama(client, connection, request, on_text)
            except NotOllama:
                _ollama_servers[ollama_root(connection)] = False
        return await complete_openai(client, connection, key, request, on_text)
    except httpx.TransportError as error:
        raise ProviderError("network", f"Could not reach {connection.base_url}: {network_trouble(error)}") from error


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
