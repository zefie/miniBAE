#!/usr/bin/env python3

import argparse
import collections
import copy
import dataclasses
import os
import struct
import sys
from typing import DefaultDict, Dict, Iterable, List, Optional, Sequence, Tuple


SPECIAL_ROLL_CONTROLLERS = {85, 86, 87}
META_ONCE_TYPES = {0x00, 0x02, 0x03, 0x04}


@dataclasses.dataclass
class MidiEvent:
    abs_tick: int
    track_index: int
    sequence_index: int
    kind: str
    status: Optional[int] = None
    data: bytes = b""
    meta_type: Optional[int] = None
    payload: bytes = b""


@dataclasses.dataclass
class MidiTrack:
    events: List[MidiEvent]
    end_tick: int


@dataclasses.dataclass
class MidiFile:
    format_type: int
    division: int
    tracks: List[MidiTrack]


@dataclasses.dataclass
class PassSummary:
    pass_index: int
    audible_tracks: List[Tuple[int, int]]


@dataclasses.dataclass(frozen=True)
class InstrumentKey:
    family: str
    channel: int
    bank_msb: int
    bank_lsb: int
    program: int
    drum_note: int


def read_be16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def read_be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def write_be16(value: int) -> bytes:
    return struct.pack(">H", value)


def write_be32(value: int) -> bytes:
    return struct.pack(">I", value)


def read_vlq(data: bytes, offset: int) -> Tuple[int, int]:
    value = 0
    while True:
        if offset >= len(data):
            raise ValueError("unexpected end of file while reading variable-length quantity")
        byte = data[offset]
        offset += 1
        value = (value << 7) | (byte & 0x7F)
        if (byte & 0x80) == 0:
            return value, offset


def write_vlq(value: int) -> bytes:
    if value < 0:
        raise ValueError("delta times must be non-negative")
    parts = [value & 0x7F]
    value >>= 7
    while value:
        parts.append(0x80 | (value & 0x7F))
        value >>= 7
    parts.reverse()
    return bytes(parts)


def parse_midi(path: str) -> MidiFile:
    with open(path, "rb") as handle:
        data = handle.read()

    if len(data) < 14 or data[:4] != b"MThd":
        raise ValueError("input is not a Standard MIDI file")

    header_length = read_be32(data, 4)
    if header_length < 6:
        raise ValueError("invalid MIDI header length")

    format_type = read_be16(data, 8)
    track_count = read_be16(data, 10)
    division = read_be16(data, 12)
    if division & 0x8000:
        raise ValueError("SMPTE time division is not supported by this tool")

    offset = 8 + header_length
    tracks: List[MidiTrack] = []

    for track_index in range(track_count):
        if offset + 8 > len(data) or data[offset:offset + 4] != b"MTrk":
            raise ValueError(f"missing MTrk chunk for track {track_index}")
        track_length = read_be32(data, offset + 4)
        offset += 8
        track_data = data[offset:offset + track_length]
        offset += track_length
        tracks.append(parse_track(track_data, track_index))

    return MidiFile(format_type=format_type, division=division, tracks=tracks)


def parse_track(track_data: bytes, track_index: int) -> MidiTrack:
    events: List[MidiEvent] = []
    offset = 0
    abs_tick = 0
    running_status: Optional[int] = None
    sequence_index = 0

    while offset < len(track_data):
        delta, offset = read_vlq(track_data, offset)
        abs_tick += delta
        if offset >= len(track_data):
            raise ValueError(f"track {track_index} ends after delta time")

        status = track_data[offset]
        if status < 0x80:
            if running_status is None:
                raise ValueError(f"track {track_index} uses running status before any status byte")
            status = running_status
        else:
            offset += 1
            if status < 0xF0:
                running_status = status

        if status == 0xFF:
            if offset >= len(track_data):
                raise ValueError(f"track {track_index} is truncated in meta event")
            meta_type = track_data[offset]
            offset += 1
            length, offset = read_vlq(track_data, offset)
            payload = track_data[offset:offset + length]
            if len(payload) != length:
                raise ValueError(f"track {track_index} is truncated in meta payload")
            offset += length
            running_status = None
            events.append(
                MidiEvent(
                    abs_tick=abs_tick,
                    track_index=track_index,
                    sequence_index=sequence_index,
                    kind="meta",
                    meta_type=meta_type,
                    payload=payload,
                )
            )
        elif status in (0xF0, 0xF7):
            length, offset = read_vlq(track_data, offset)
            payload = track_data[offset:offset + length]
            if len(payload) != length:
                raise ValueError(f"track {track_index} is truncated in sysex payload")
            offset += length
            running_status = None
            events.append(
                MidiEvent(
                    abs_tick=abs_tick,
                    track_index=track_index,
                    sequence_index=sequence_index,
                    kind="sysex",
                    status=status,
                    payload=payload,
                )
            )
        else:
            message_type = status & 0xF0
            data_length = 1 if message_type in (0xC0, 0xD0) else 2
            payload = track_data[offset:offset + data_length]
            if len(payload) != data_length:
                raise ValueError(f"track {track_index} is truncated in MIDI event payload")
            offset += data_length
            events.append(
                MidiEvent(
                    abs_tick=abs_tick,
                    track_index=track_index,
                    sequence_index=sequence_index,
                    kind="midi",
                    status=status,
                    data=payload,
                )
            )

        sequence_index += 1

    return MidiTrack(events=events, end_tick=abs_tick)


def clone_event(event: MidiEvent, abs_tick: int) -> MidiEvent:
    cloned = copy.copy(event)
    cloned.abs_tick = abs_tick
    return cloned


def event_sort_key(event: MidiEvent) -> Tuple[int, int, int]:
    return (event.abs_tick, event.track_index, event.sequence_index)


def get_note_identity(event: MidiEvent) -> Tuple[int, int]:
    return (event.status & 0x0F, event.data[0])


def is_note_off(event: MidiEvent) -> bool:
    message_type = event.status & 0xF0
    return message_type == 0x80 or (message_type == 0x90 and len(event.data) > 1 and event.data[1] == 0)


def is_note_on(event: MidiEvent) -> bool:
    return (event.status & 0xF0) == 0x90 and len(event.data) > 1 and event.data[1] != 0


def detect_roll_metadata(midi: MidiFile) -> Tuple[bool, int, int]:
    has_roll = False
    max_loop_count = 0
    max_roll_index = 0

    for track in midi.tracks:
        for event in track.events:
            if event.kind != "midi" or (event.status & 0xF0) != 0xB0:
                continue
            controller = event.data[0]
            value = event.data[1]
            if controller == 85:
                max_loop_count = max(max_loop_count, value)
            elif controller in (86, 87):
                has_roll = True
                max_roll_index = max(max_roll_index, value)

    return has_roll, max_loop_count, max_roll_index


def should_repeat_meta(meta_type: int, pass_index: int) -> bool:
    if pass_index == 0:
        return True
    return meta_type not in META_ONCE_TYPES


def unroll_events(midi: MidiFile) -> Tuple[List[MidiEvent], List[PassSummary], bool]:
    has_roll, max_loop_count, max_roll_index = detect_roll_metadata(midi)
    if max_loop_count > 0:
        total_passes = max_loop_count + 1
    elif has_roll:
        total_passes = max_roll_index + 1
    else:
        total_passes = 1

    muted_tracks = [False] * len(midi.tracks)
    all_events = sorted(
        (event for track in midi.tracks for event in track.events if not (event.kind == "meta" and event.meta_type == 0x2F)),
        key=event_sort_key,
    )
    song_length = max((track.end_tick for track in midi.tracks), default=0)
    flattened: List[MidiEvent] = []
    summaries: List[PassSummary] = []

    for pass_index in range(total_passes):
        pass_offset = pass_index * song_length
        track_note_counts: DefaultDict[int, int] = collections.defaultdict(int)

        for event in all_events:
            output_tick = pass_offset + event.abs_tick

            if event.kind == "meta":
                if should_repeat_meta(event.meta_type, pass_index):
                    flattened.append(clone_event(event, output_tick))
                continue

            if event.kind == "sysex":
                flattened.append(clone_event(event, output_tick))
                continue

            message_type = event.status & 0xF0
            if message_type == 0xB0 and event.data[0] in SPECIAL_ROLL_CONTROLLERS:
                controller = event.data[0]
                value = event.data[1]
                if controller == 86 and pass_index == value:
                    muted_tracks[event.track_index] = True
                elif controller == 87 and pass_index == value:
                    muted_tracks[event.track_index] = False
                continue

            if message_type in (0x80, 0x90) and muted_tracks[event.track_index]:
                continue

            flattened.append(clone_event(event, output_tick))
            if is_note_on(event):
                track_note_counts[event.track_index] += 1

        audible_tracks = sorted(track_note_counts.items(), key=lambda item: (-item[1], item[0]))
        summaries.append(PassSummary(pass_index=pass_index, audible_tracks=audible_tracks))

    return flattened, summaries, has_roll


def current_instrument_key(channel: int, bank_msb: int, bank_lsb: int, program: int, note: int) -> InstrumentKey:
    if channel == 9:
        return InstrumentKey("drum", channel, bank_msb, bank_lsb, program, note)
    return InstrumentKey("program", channel, bank_msb, bank_lsb, program, -1)


def instrument_name(key: InstrumentKey) -> str:
    if key.family == "drum":
        return f"Drums ch{key.channel + 1} note {key.drum_note}"
    return f"Program ch{key.channel + 1} bank {key.bank_msb}:{key.bank_lsb} prog {key.program}"


def split_by_instrument(events: Sequence[MidiEvent]) -> List[List[MidiEvent]]:
    conductor: List[MidiEvent] = []
    grouped: Dict[InstrumentKey, List[MidiEvent]] = {}
    active_note_groups: DefaultDict[Tuple[int, int], List[InstrumentKey]] = collections.defaultdict(list)
    active_group_note_counts: DefaultDict[InstrumentKey, int] = collections.defaultdict(int)
    active_groups_by_channel: DefaultDict[int, set] = collections.defaultdict(set)
    current_bank_msb = [0] * 16
    current_bank_lsb = [0] * 16
    current_program = [0] * 16

    def ensure_group(key: InstrumentKey) -> List[MidiEvent]:
        events_for_group = grouped.get(key)
        if events_for_group is None:
            events_for_group = []
            name = instrument_name(key).encode("latin1", errors="replace")
            events_for_group.append(
                MidiEvent(abs_tick=0, track_index=0, sequence_index=-3, kind="meta", meta_type=0x03, payload=name)
            )
            if key.bank_msb:
                events_for_group.append(
                    MidiEvent(
                        abs_tick=0,
                        track_index=0,
                        sequence_index=-2,
                        kind="midi",
                        status=0xB0 | key.channel,
                        data=bytes((0, key.bank_msb)),
                    )
                )
            if key.bank_lsb:
                events_for_group.append(
                    MidiEvent(
                        abs_tick=0,
                        track_index=0,
                        sequence_index=-1,
                        kind="midi",
                        status=0xB0 | key.channel,
                        data=bytes((32, key.bank_lsb)),
                    )
                )
            if key.family == "program":
                events_for_group.append(
                    MidiEvent(
                        abs_tick=0,
                        track_index=0,
                        sequence_index=0,
                        kind="midi",
                        status=0xC0 | key.channel,
                        data=bytes((key.program,)),
                    )
                )
            grouped[key] = events_for_group
        return events_for_group

    for event in sorted(events, key=event_sort_key):
        if event.kind != "midi":
            conductor.append(event)
            continue

        channel = event.status & 0x0F
        message_type = event.status & 0xF0

        if message_type == 0xB0:
            controller = event.data[0]
            if controller == 0:
                current_bank_msb[channel] = event.data[1]
                continue
            if controller == 32:
                current_bank_lsb[channel] = event.data[1]
                continue

        if message_type == 0xC0:
            current_program[channel] = event.data[0]
            continue

        if is_note_on(event):
            key = current_instrument_key(
                channel,
                current_bank_msb[channel],
                current_bank_lsb[channel],
                current_program[channel],
                event.data[0],
            )
            ensure_group(key).append(event)
            active_note_groups[(channel, event.data[0])].append(key)
            active_group_note_counts[key] += 1
            active_groups_by_channel[channel].add(key)
            continue

        if is_note_off(event):
            stack = active_note_groups[(channel, event.data[0])]
            if stack:
                key = stack.pop()
            else:
                key = current_instrument_key(
                    channel,
                    current_bank_msb[channel],
                    current_bank_lsb[channel],
                    current_program[channel],
                    event.data[0],
                )
            ensure_group(key).append(event)
            if active_group_note_counts[key] > 0:
                active_group_note_counts[key] -= 1
            if active_group_note_counts[key] == 0:
                active_groups_by_channel[channel].discard(key)
            continue

        target_groups = set(active_groups_by_channel[channel])
        if not target_groups:
            key = current_instrument_key(
                channel,
                current_bank_msb[channel],
                current_bank_lsb[channel],
                current_program[channel],
                -1,
            )
            target_groups.add(key)

        for key in sorted(target_groups, key=instrument_name):
            ensure_group(key).append(event)

    tracks = [sorted(conductor, key=event_sort_key)]
    for key in sorted(grouped, key=instrument_name):
        tracks.append(sorted(grouped[key], key=event_sort_key))
    return tracks


def encode_event(event: MidiEvent) -> bytes:
    if event.kind == "meta":
        return b"\xFF" + bytes((event.meta_type,)) + write_vlq(len(event.payload)) + event.payload
    if event.kind == "sysex":
        return bytes((event.status,)) + write_vlq(len(event.payload)) + event.payload
    return bytes((event.status,)) + event.data


def encode_track(events: Iterable[MidiEvent]) -> bytes:
    output = bytearray()
    last_tick = 0
    for event in sorted(events, key=event_sort_key):
        delta = event.abs_tick - last_tick
        output.extend(write_vlq(delta))
        output.extend(encode_event(event))
        last_tick = event.abs_tick
    output.extend(b"\x00\xFF\x2F\x00")
    return bytes(output)


def write_midi(path: str, division: int, tracks: Sequence[Sequence[MidiEvent]]) -> None:
    format_type = 0 if len(tracks) == 1 else 1
    with open(path, "wb") as handle:
        handle.write(b"MThd")
        handle.write(write_be32(6))
        handle.write(write_be16(format_type))
        handle.write(write_be16(len(tracks)))
        handle.write(write_be16(division))
        for track_events in tracks:
            track_bytes = encode_track(track_events)
            handle.write(b"MTrk")
            handle.write(write_be32(len(track_bytes)))
            handle.write(track_bytes)


def format_pass_summary(summary: PassSummary) -> str:
    if not summary.audible_tracks:
        return f"pass {summary.pass_index}: no audible note-ons"
    parts = [f"track {track_index} ({note_count} note-ons)" for track_index, note_count in summary.audible_tracks]
    return f"pass {summary.pass_index}: " + ", ".join(parts)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Unroll Beatnik/WebTV rolled MIDI files into a linear MIDI.",
    )
    parser.add_argument("source", help="input MIDI file")
    parser.add_argument("dest", help="output MIDI file")
    parser.add_argument(
        "--split-instruments",
        action="store_true",
        help="write a format-1 MIDI with one track per effective instrument instead of a single flattened track",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="suppress playback-order summary output",
    )
    return parser


def main(argv: Sequence[str]) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)

    try:
        midi = parse_midi(args.source)
        flattened_events, pass_summaries, has_roll = unroll_events(midi)
        output_tracks = split_by_instrument(flattened_events) if args.split_instruments else [flattened_events]
        write_midi(args.dest, midi.division, output_tracks)
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    if not args.quiet:
        print(f"Wrote {args.dest}")
        print(f"Rolled MIDI detected: {'yes' if has_roll else 'no'}")
        for summary in pass_summaries:
            print(format_pass_summary(summary))
        if args.split_instruments:
            print(f"Output tracks: {len(output_tracks)} (including conductor track)")
        else:
            print("Output tracks: 1")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))