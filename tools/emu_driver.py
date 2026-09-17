#!/usr/bin/env python3
"""Emulator stand-in for the phone.

    python tools/emu_driver.py --port <pypkjs port> --mode syllable --at 31700 --shot-prefix /tmp/rl
"""
import argparse
import json
import queue
import struct
import time
import urllib.parse
import urllib.request
import uuid
import zlib

import png
from libpebble2.communication import PebbleConnection
from libpebble2.communication.transports.websocket import WebsocketTransport
from libpebble2.services.appmessage import AppMessageService, ByteArray, CString, Int32, Uint8, Uint16, Uint32
from libpebble2.services.screenshot import Screenshot

APP_UUID = uuid.UUID("79d7fe6a-b970-483a-b096-16da10af0433")

# Same order as package.json messageKeys
KEY = {name: 10000 + i for i, name in enumerate([
    "Cmd", "TrackId", "Title", "Artist", "Duration", "Status", "LineCount", "ChunkIndex", "Chunk",
    "Playing", "Position", "InboxSize", "PhoneTime", "WatchTime", "PhoneTime2",
    "SetMode", "SetTextSize", "SetBgVocals", "SetUpcoming", "SetProgress", "SetBacklight", "SetAutoOpen",
    "SetSyncOffset", "SetAccent", "SetStyle", "SetBgColor",
])}

CMD_TRACK, CMD_LINES, CMD_STATE, CMD_SYNC = 1, 2, 3, 5
CMD_SYNC_REQ = 24
STATUS_READY, STATUS_NO_LYRICS = 2, 3
SEG_BG, SEG_SPACE = 1, 2
LINE_RIGHT = 1

API = "https://api.atomix.one/rl-api"
HEADERS = {
    "P-Access-Token-Id": "58hy4s86",
    "P-Access-Token": "xjehy2lfg5h5mjwotoxrcqugam",
    "User-Agent": "rl-pebble-emu/1.0",
}


def fetch(title, artist, isrc=None, romanize=False):
    """Same request TIDAL's lyrics worker makes."""
    q = {"title": title, "artist": artist, "platform": "rl-mobile"}
    if romanize:
        q["romanize"] = "true"
    if isrc:
        q["isrc"] = isrc
    url = API + "?" + urllib.parse.urlencode(q, quote_via=urllib.parse.quote)
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=30) as r:
        return json.load(r)


def phone_now():
    """Phone elapsedRealtime stand-in."""
    return int(time.monotonic() * 1000)


def text_of(obj):
    return obj.get("romanized") or obj.get("text") or ""


def utf8_trunc(text, limit=255):
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return raw
    raw = raw[:limit]
    while raw and (raw[-1] & 0xC0) == 0x80:
        raw = raw[:-1]
    return raw[:-1] if raw and raw[-1] >= 0xC0 else raw


def singer_sides(body):
    """Duet sides, mirrors WatchProtocol.singerSides."""
    data = body.get("data") or []
    agents = (body.get("metadata") or {}).get("agents")

    def singer_of(item):
        return ((item or {}).get("element") or {}).get("singer") or None

    def type_of(singer):
        if isinstance(agents, dict) and isinstance(agents.get(singer), dict):
            return agents[singer].get("type", "person")
        return {"v1000": "group", "v2000": "other"}.get(singer, "person")

    side_of, persons = {}, 0
    for item in data:
        singer = singer_of(item)
        if not singer or singer in side_of:
            continue
        if type_of(singer) != "person":
            side_of[singer] = 0
        else:
            persons += 1
            side_of[singer] = 1 if persons == 2 else 0
    sides = [side_of.get(singer_of(item), 0) if singer_of(item) else -1 for item in data]
    sided = sum(1 for x in sides if x >= 0)
    right = sum(1 for x in sides if x == 1)
    if sided and right * 100 // sided >= 85:
        sides = [1 - x if x >= 0 else x for x in sides]
    return sides if (0 in sides and 1 in sides) else None


def shape(body):
    """Mirrors WatchProtocol.shape."""
    raws = []
    sides = singer_sides(body)
    for index, item in enumerate(body.get("data") or []):
        syllabus = item.get("syllabus") or []
        segs = []
        if syllabus:
            start = int(syllabus[0].get("time") or 0)
            end = int(syllabus[-1].get("time") or 0) + int(syllabus[-1].get("duration") or 0)
            all_bg = all(x.get("isBackground") for x in syllabus)
            for x in syllabus:
                raw = text_of(x)
                text = raw.strip()
                if not text:
                    continue
                flags = SEG_BG if x.get("isBackground") else 0
                if raw != raw.rstrip():
                    flags |= SEG_SPACE
                segs.append((int(x.get("time") or 0), int(x.get("duration") or 0), flags, text))
        else:
            start = int(round(float(item.get("startTime") or 0) * 1000))
            end = start + 800
            all_bg = False
            words = text_of(item).split()
            for i, word in enumerate(words):
                segs.append((start, 0, SEG_SPACE if i < len(words) - 1 else 0, word))
        if segs:
            raws.append((start, end, all_bg, segs[:120], bool(sides and sides[index] == 1)))

    lines = []
    for i, (start, end, _, segs, right) in enumerate(raws):
        next_lead = next((r[0] for r in raws[i + 1:] if not r[2]), None)
        hold = next_lead if next_lead is not None else 0x7FFFFFFF
        lines.append((start, max(hold, end), segs, right))
    return lines


def encode_line(line):
    start, end, segs, right = line
    out = bytearray(struct.pack("<IIBB", start, end, len(segs), LINE_RIGHT if right else 0))
    for t, d, flags, text in segs:
        raw = utf8_trunc(text)
        out += struct.pack("<HHBB", min(max(t - start, 0), 65535), min(max(d, 0), 65535), flags, len(raw))
        out += raw
    return bytes(out)


def chunks(lines, budget):
    buf, first = bytearray(), 0
    for i, line in enumerate(lines):
        enc = encode_line(line)
        if buf and len(buf) + len(enc) > budget:
            yield first, bytes(buf)
            buf, first = bytearray(), i
        buf += enc
    if buf:
        yield first, bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--title", default="Blinding Lights")
    ap.add_argument("--artist", default="The Weeknd")
    ap.add_argument("--isrc")
    ap.add_argument("--romanize", action="store_true")
    ap.add_argument("--mode", choices=["line", "word", "syllable"], default="syllable")
    ap.add_argument("--size", type=int, default=2)
    ap.add_argument("--bg", action="store_true", help="background vocals")
    ap.add_argument("--backlight-mode", type=int, choices=[0, 1, 2], default=0)
    ap.add_argument("--offset", type=int, default=0, help="sync offset in ms")
    ap.add_argument("--centered", action="store_true")
    ap.add_argument("--bg-color", type=lambda v: int(v, 0), default=0x000000)
    ap.add_argument("--accent", type=lambda v: int(v, 0), default=0xFFFFFF)
    ap.add_argument("--at", type=int, action="append", default=[])
    ap.add_argument("--play", action="store_true", help="keep playing from the last --at")
    ap.add_argument("--shot-prefix")
    ap.add_argument("--age", type=int, default=0, help="stamp positions this many ms in the past")
    args = ap.parse_args()

    body = fetch(args.title, args.artist, args.isrc, args.romanize)
    lines = shape(body)
    print(f"type={body.get('type')} lines={len(lines)}")

    pebble = PebbleConnection(WebsocketTransport(f"ws://localhost:{args.port}/"))
    pebble.connect()
    pebble.run_async()
    svc = AppMessageService(pebble)
    sync_requests = queue.Queue()

    def on_message(tid, app, data):
        if data.get(KEY["Cmd"]) == CMD_SYNC_REQ:
            sync_requests.put((data[KEY["WatchTime"]], phone_now()))
        else:
            print("watch ->", data)

    svc.register_handler("appmessage", on_message)

    def answer_sync(wait=0.0):
        end = time.time() + wait
        while True:
            try:
                watch_time, received = sync_requests.get(timeout=max(0.0, end - time.time()))
            except queue.Empty:
                return
            svc.send_message(APP_UUID, {
                KEY["Cmd"]: Uint8(CMD_SYNC),
                KEY["WatchTime"]: Uint32(watch_time),
                KEY["PhoneTime"]: Uint32(received & 0xFFFFFFFF),
                KEY["PhoneTime2"]: Uint32(phone_now() & 0xFFFFFFFF),
            })
            time.sleep(0.15)

    # One message in flight at a time
    def send(message):
        svc.send_message(APP_UUID, message)
        time.sleep(0.3)
        answer_sync()

    answer_sync(3.0)

    track_id = zlib.crc32(f"{args.title}/{args.artist}".encode()) or 1
    duration = max((seg[0] + seg[1] for line in lines for seg in line[2]), default=0) + 15000

    send({
        KEY["SetMode"]: CString({"line": "0", "word": "1", "syllable": "2"}[args.mode]),
        KEY["SetTextSize"]: CString(str(args.size)),
        KEY["SetBgVocals"]: Uint8(1 if args.bg else 0),
        KEY["SetBacklight"]: CString(str(args.backlight_mode)),
        KEY["SetSyncOffset"]: Int32(args.offset),
        KEY["SetStyle"]: CString("1" if args.centered else "0"),
        KEY["SetBgColor"]: Int32(args.bg_color),
        KEY["SetAccent"]: Int32(args.accent),
    })
    send({
        KEY["Cmd"]: Uint8(CMD_TRACK),
        KEY["TrackId"]: Uint32(track_id),
        KEY["Title"]: CString(args.title),
        KEY["Artist"]: CString(args.artist),
        KEY["Duration"]: Uint32(duration),
        KEY["Status"]: Uint8(STATUS_READY if lines else STATUS_NO_LYRICS),
        KEY["LineCount"]: Uint16(len(lines)),
    })
    for first, blob in chunks(lines, 1800):
        send({
            KEY["Cmd"]: Uint8(CMD_LINES),
            KEY["TrackId"]: Uint32(track_id),
            KEY["ChunkIndex"]: Uint16(first),
            KEY["Chunk"]: ByteArray(blob),
        })

    for i, at in enumerate(args.at):
        playing = args.play and i == len(args.at) - 1
        send({
            KEY["Cmd"]: Uint8(CMD_STATE),
            KEY["TrackId"]: Uint32(track_id),
            KEY["Playing"]: Uint8(1 if playing else 0),
            KEY["Position"]: Int32(at),
            KEY["PhoneTime"]: Uint32((phone_now() - args.age) & 0xFFFFFFFF),
        })
        answer_sync(0.6)
        if args.shot_prefix:
            path = f"{args.shot_prefix}_{at}.png"
            png.from_array(Screenshot(pebble).grab_image(), mode="RGB;8").save(path)
            print("saved", path)


if __name__ == "__main__":
    main()
