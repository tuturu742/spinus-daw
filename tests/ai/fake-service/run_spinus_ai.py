"""A stand-in for the AI service (spinus-ai) that misbehaves on purpose, for Spinus's tests of what the DAW does
when AI goes wrong. Started by AiService as the real one is (same arguments); what it does comes from the file
`fake-mode` in its --config folder:

    answer      answers every call with {} (ai.v1.assistant.ask with a job that finishes at once)
    silent      takes every call and never answers (a hung or very slow service)
    crash       ends at once, without answering, when a call arrives (a crash mid-request)
    crash-job   answers ai.v1.assistant.ask with a job, then ends (a crash while the job runs)
    silent-job  answers every call with a job, then never says anything about it (a job that hangs)
    slow-job    answers ai.v1.assistant.ask with a job that reports progress every 0.3 s for 3 s, then is done

Only the standard library: it never talks to any model or the internet."""

import argparse
import json
import os
import socket
import sys
import time


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in (
        "--log",
        "--port-file",
        "--token-file",
        "--config",
        "--docs",
        "--schema",
        "--models",
        "--parent-pid",
        "--session-log",
    ):
        parser.add_argument(name)
    args = parser.parse_args()
    mode_file = os.path.join(args.config, "fake-mode")
    mode = "answer"
    if os.path.exists(mode_file):
        with open(mode_file) as f:
            mode = f.read().strip() or "answer"
    with open(args.token_file) as f:
        token = f.read().strip()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    with open(args.port_file, "w") as f:
        f.write(str(server.getsockname()[1]))
    connection, _ = server.accept()
    reader = connection.makefile("r", encoding="utf-8")
    if reader.readline().strip() != token:
        return

    def send(message: dict[str, object]) -> None:
        connection.sendall((json.dumps(message) + "\n").encode())

    for line in reader:
        request = json.loads(line)
        method = request.get("method")
        if method == "ai.v1.shutdown":
            return
        if mode == "crash":
            with open(args.log, "a") as log:
                log.write("Fake service: crashed on purpose\n")
            os._exit(3)
        if mode == "silent":
            continue
        if mode == "silent-job":
            if method == "ai.v1.job.cancel":
                send({"jsonrpc": "2.0", "id": request["id"], "result": {}})
            else:
                send({"jsonrpc": "2.0", "id": request["id"], "result": {"job": f"job-{request['id']}"}})
            continue
        if method == "ai.v1.assistant.ask":
            send({"jsonrpc": "2.0", "id": request["id"], "result": {"job": "job-1"}})
            if mode == "crash-job":
                with open(args.log, "a") as log:
                    log.write("Fake service: crashed during the job on purpose\n")
                os._exit(3)
            if mode == "slow-job":
                for i in range(10):
                    time.sleep(0.3)
                    send(
                        {
                            "jsonrpc": "2.0",
                            "method": "ai.v1.job.update",
                            "params": {"job": "job-1", "kind": "progress", "done": i, "total": 10},
                        }
                    )
            send(
                {
                    "jsonrpc": "2.0",
                    "method": "ai.v1.job.update",
                    "params": {
                        "job": "job-1",
                        "kind": "done",
                        "result": {"text": "Fine.", "model": "fake", "connection": "fake"},
                    },
                }
            )
            continue
        send({"jsonrpc": "2.0", "id": request.get("id"), "result": {"protocol": 1}})


if __name__ == "__main__":
    main()
    sys.exit(0)
