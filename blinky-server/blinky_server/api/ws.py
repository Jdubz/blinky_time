from __future__ import annotations

import asyncio
import logging
from typing import Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from ..device.device import Device
from ..device.manager import FleetManager
from .deps import get_fleet

log = logging.getLogger(__name__)
router = APIRouter()


@router.websocket("/ws/{device_id}")
async def device_stream(ws: WebSocket, device_id: str) -> None:
    """WebSocket endpoint for streaming data from a single device."""
    fleet = get_fleet()
    device = fleet.get_device(device_id)
    if not device:
        await ws.close(code=4004, reason="Device not found")
        return

    await ws.accept()

    queue: asyncio.Queue[dict[str, Any] | None] = device.subscribe_stream()
    send_task = asyncio.create_task(_send_loop(ws, queue))
    recv_task = asyncio.create_task(_recv_loop(ws, device))

    try:
        _done, pending = await asyncio.wait(
            [send_task, recv_task],
            return_when=asyncio.FIRST_COMPLETED,
        )
        for task in pending:
            task.cancel()
    except Exception:
        pass
    finally:
        device.unsubscribe_stream(queue)
        send_task.cancel()
        recv_task.cancel()


@router.websocket("/ws/fleet")
async def fleet_stream(ws: WebSocket) -> None:
    """WebSocket endpoint for multiplexed streaming from all devices."""
    fleet = get_fleet()
    await ws.accept()

    queues: list[tuple[str, asyncio.Queue[dict[str, Any] | None]]] = []
    merged: asyncio.Queue[dict[str, Any] | None] = asyncio.Queue(maxsize=500)

    for device in fleet.get_all_devices():
        q: asyncio.Queue[dict[str, Any] | None] = device.subscribe_stream()
        queues.append((device.id, q))

    async def _fan_in(device_id: str, q: asyncio.Queue[dict[str, Any] | None]) -> None:
        try:
            while True:
                msg = await q.get()
                if msg is None:
                    break
                await merged.put(msg)
        except asyncio.CancelledError:
            pass

    fan_in_tasks = [asyncio.create_task(_fan_in(did, q)) for did, q in queues]
    send_task = asyncio.create_task(_send_loop(ws, merged))
    recv_task = asyncio.create_task(_recv_fleet_loop(ws, fleet))

    try:
        _done, pending = await asyncio.wait(
            [send_task, recv_task, *fan_in_tasks],
            return_when=asyncio.FIRST_COMPLETED,
        )
        for task in pending:
            task.cancel()
    except Exception:
        pass
    finally:
        for device in fleet.get_all_devices():
            for did, q in queues:
                if did == device.id:
                    device.unsubscribe_stream(q)
        for t in fan_in_tasks:
            t.cancel()
        send_task.cancel()
        recv_task.cancel()


async def _send_loop(ws: WebSocket, queue: asyncio.Queue[dict[str, Any] | None]) -> None:
    """Forward messages from queue to WebSocket."""
    try:
        while True:
            msg = await queue.get()
            if msg is None:
                break
            await ws.send_json(msg)
    except (WebSocketDisconnect, asyncio.CancelledError):
        pass
    except Exception as e:
        log.debug("WebSocket send error: %s", e)


async def _recv_loop(ws: WebSocket, device: Device) -> None:
    """Receive commands from WebSocket and send to device."""
    try:
        while True:
            data: dict[str, Any] = await ws.receive_json()
            msg_type = data.get("type")

            if msg_type == "command":
                cmd = str(data.get("command", ""))
                resp = await device.protocol.send_command(cmd)
                await ws.send_json(
                    {
                        "type": "response",
                        "device_id": device.id,
                        "command": cmd,
                        "response": resp,
                    }
                )

            elif msg_type == "stream_control":
                if data.get("enabled"):
                    await device.protocol.start_stream(str(data.get("mode", "on")))
                else:
                    await device.protocol.stop_stream()

    except (WebSocketDisconnect, asyncio.CancelledError):
        pass
    except Exception as e:
        log.debug("WebSocket recv error: %s", e)


async def _recv_fleet_loop(ws: WebSocket, fleet: FleetManager) -> None:
    """Receive commands from WebSocket and route to fleet."""
    try:
        while True:
            data: dict[str, Any] = await ws.receive_json()
            msg_type = data.get("type")
            target_id = data.get("device_id")

            if msg_type == "command":
                cmd = str(data.get("command", ""))
                if target_id:
                    device = fleet.get_device(str(target_id))
                    if device:
                        resp = await device.protocol.send_command(cmd)
                        await ws.send_json(
                            {
                                "type": "response",
                                "device_id": device.id,
                                "command": cmd,
                                "response": resp,
                            }
                        )
                else:
                    results = await fleet.send_to_all(cmd)
                    await ws.send_json(
                        {
                            "type": "fleet_response",
                            "command": cmd,
                            "results": results,
                        }
                    )

    except (WebSocketDisconnect, asyncio.CancelledError):
        pass
    except Exception as e:
        log.debug("WebSocket recv error: %s", e)
