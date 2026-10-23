"""A fake local model server (OpenAI-compatible, like Ollama) for Spinus's UI tests.

    python fake_provider.py PORT [REPLIES.json] [--models=NAME,NAME...]

Answers /v1/models (with "fake-model", or the models given), and /v1/chat/completions with a scripted
conversation: by default, look at the project, propose setting the tempo to 99 BPM, then say so. A
replies file holds another conversation, as a list of {"tool": [name, arguments]} or {"text": "..."}."""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPLIES = [
    {"tool": ("project_overview", {})},
    {"tool": ("propose_changes", {"summary": "Slow the song down to 99 BPM",
                                  "steps": [{"command": "project.setTempo", "parameters": {"bpm": 99}}]})},
    {"text": "If you apply the proposal, the song plays at 99 BPM."},
]


MODELS = ["fake-model"]


class Handler(BaseHTTPRequestHandler):
    turn = 0

    def log_message(self, *args: object) -> None:
        pass

    def do_GET(self) -> None:
        body = json.dumps({"data": [{"id": m} for m in MODELS]}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        self.rfile.read(int(self.headers.get("content-length", "0")))
        reply = REPLIES[min(Handler.turn, len(REPLIES) - 1)]
        Handler.turn += 1
        chunks = []
        if "text" in reply:
            words = reply["text"].split(" ")
            chunks += [{"model": MODELS[0], "choices": [{"delta": {"content": w + " "}}]} for w in words]
        else:
            name, args = reply["tool"]
            chunks.append({"model": MODELS[0], "choices": [{"delta": {"tool_calls": [
                {"index": 0, "id": f"call{Handler.turn}", "function": {"name": name, "arguments": json.dumps(args)}}]},
                "finish_reason": "tool_calls"}]})
        chunks.append({"choices": [], "usage": {"prompt_tokens": 10, "completion_tokens": 3}})
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.end_headers()
        for c in chunks:
            self.wfile.write(f"data: {json.dumps(c)}\n\n".encode())
        self.wfile.write(b"data: [DONE]\n\n")


ARGS = [a for a in sys.argv[1:] if not a.startswith("--models=")]
for a in sys.argv[1:]:
    if a.startswith("--models="):
        MODELS = a.removeprefix("--models=").split(",")

if len(ARGS) > 1:
    with open(ARGS[1], encoding="utf-8") as f:
        REPLIES = [{"tool": tuple(r["tool"])} if "tool" in r else r for r in json.load(f)]

server = ThreadingHTTPServer(("127.0.0.1", int(ARGS[0])), Handler)
print("ready", flush=True)
server.serve_forever()
