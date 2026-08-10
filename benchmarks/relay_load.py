#!/usr/bin/env python3
"""Dependency-free TCP echo/load tool used by the MiniTUN release gate."""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import resource
import signal
import ssl
import statistics
import time
from pathlib import Path

BLOCK_BYTES = 64 * 1024


async def echo_connection(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        while data := await reader.read(BLOCK_BYTES):
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except (ConnectionError, ssl.SSLError):
            pass


async def run_echo(args: argparse.Namespace) -> int:
    context = None
    if args.tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
    server = await asyncio.start_server(
        echo_connection,
        args.host,
        args.port,
        ssl=context,
        backlog=65_535,
        limit=BLOCK_BYTES,
    )
    sockets = server.sockets or []
    if not sockets:
        raise RuntimeError("echo server did not create a listener")
    print(sockets[0].getsockname()[1], flush=True)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for signum in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(signum, stop.set)
    async with server:
        await stop.wait()
    return 0


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(len(ordered) * fraction + 0.999999) - 1))
    return ordered[index]


async def load_connection(
    index: int,
    host: str,
    port: int,
    byte_count: int,
    context: ssl.SSLContext | None,
    start_gate: asyncio.Event,
) -> tuple[int, float, str | None]:
    await start_gate.wait()
    started = time.monotonic()
    try:
        reader, writer = await asyncio.open_connection(
            host,
            port,
            ssl=context,
            server_hostname=host if context is not None else None,
            limit=BLOCK_BYTES,
        )
        seed = hashlib.sha256(f"minitun-load-{index}".encode("ascii")).digest()
        block = (seed * ((BLOCK_BYTES + len(seed) - 1) // len(seed)))[:BLOCK_BYTES]
        remaining = byte_count
        first_byte_ms = 0.0
        transferred = 0
        while remaining > 0:
            size = min(remaining, len(block))
            expected = block[:size]
            writer.write(expected)
            await writer.drain()
            received = await reader.readexactly(size)
            if transferred == 0:
                first_byte_ms = (time.monotonic() - started) * 1000.0
            if received != expected:
                writer.close()
                await writer.wait_closed()
                return transferred, first_byte_ms, "data_corruption"
            transferred += size
            remaining -= size
        writer.write_eof() if context is None and writer.can_write_eof() else None
        writer.close()
        await writer.wait_closed()
        return transferred, first_byte_ms, None
    except Exception as error:  # Error text is bounded and contains no payload.
        return 0, 0.0, f"{type(error).__name__}:{str(error)[:160]}"


async def run_load(args: argparse.Namespace) -> int:
    ports = [int(line) for line in Path(args.ports_file).read_text(encoding="ascii").splitlines()]
    if not ports or any(port < 1 or port > 65_535 for port in ports):
        raise ValueError("ports file is empty or invalid")
    targets = [port for port in ports for _ in range(args.connections_per_port)]
    required_fds = len(targets) + 256
    soft_limit, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft_limit < required_fds:
        raise RuntimeError(f"RLIMIT_NOFILE {soft_limit} is below required {required_fds}")

    context = None
    if args.tls:
        context = ssl.create_default_context(ssl.Purpose.SERVER_AUTH)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        if args.ca:
            context.load_verify_locations(args.ca)
        else:
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE

    gate = asyncio.Event()
    tasks = [
        asyncio.create_task(
            load_connection(index, args.host, port, args.bytes_per_connection, context, gate)
        )
        for index, port in enumerate(targets)
    ]
    await asyncio.sleep(0)
    started = time.monotonic()
    gate.set()
    results = await asyncio.gather(*tasks)
    duration = time.monotonic() - started
    transferred = sum(result[0] for result in results)
    latencies = [result[1] for result in results if result[0] > 0]
    failures = [result[2] for result in results if result[2] is not None]
    corruption = sum(1 for failure in failures if failure == "data_corruption")
    report = {
        "connections_attempted": len(targets),
        "connections_succeeded": len(targets) - len(failures),
        "connections_failed": len(failures),
        "data_corruption": corruption,
        "payload_bytes": transferred,
        "wire_bytes": transferred * 2,
        "duration_seconds": duration,
        "payload_throughput_bits_per_second": transferred * 8.0 / duration if duration else 0.0,
        "first_byte_latency_ms": {
            "p50": percentile(latencies, 0.50),
            "p95": percentile(latencies, 0.95),
            "p99": percentile(latencies, 0.99),
            "mean": statistics.fmean(latencies) if latencies else 0.0,
        },
        "failure_examples": failures[:10],
    }
    rendered = json.dumps(report, indent=2, sort_keys=True)
    print(rendered)
    if args.result:
        Path(args.result).write_text(rendered + "\n", encoding="utf-8")
    return 0 if not failures and transferred == len(targets) * args.bytes_per_connection else 1


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser()
    subcommands = root.add_subparsers(dest="command", required=True)

    echo = subcommands.add_parser("echo")
    echo.add_argument("--host", default="127.0.0.1")
    echo.add_argument("--port", type=int, default=0)
    echo.add_argument("--tls", action="store_true")
    echo.add_argument("--cert")
    echo.add_argument("--key")

    load = subcommands.add_parser("load")
    load.add_argument("--host", default="127.0.0.1")
    load.add_argument("--ports-file", required=True)
    load.add_argument("--connections-per-port", type=int, required=True)
    load.add_argument("--bytes-per-connection", type=int, default=1024 * 1024)
    load.add_argument("--tls", action="store_true")
    load.add_argument("--ca")
    load.add_argument("--result")
    return root


def main() -> int:
    args = parser().parse_args()
    if args.command == "echo":
        if args.tls and (not args.cert or not args.key):
            raise SystemExit("--tls requires --cert and --key")
        return asyncio.run(run_echo(args))
    if args.connections_per_port < 1 or args.bytes_per_connection < 1:
        raise SystemExit("load counts must be positive")
    return asyncio.run(run_load(args))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130) from None
