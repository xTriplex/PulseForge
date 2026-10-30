#!/usr/bin/env python3
"""Bounded package-only desktop smoke for Release/Dist cooked Players."""

from __future__ import annotations

import argparse
import csv
from contextlib import redirect_stderr, redirect_stdout
import ctypes
import ctypes.util
from dataclasses import dataclass
import hashlib
from io import BytesIO, TextIOWrapper
import json
import math
import os
import plistlib
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from ctypes import wintypes
from pathlib import Path, PurePath, PurePosixPath
from unittest.mock import patch

from AcceptanceDiagnostics import configure_utf8_diagnostics


DRAW_MARKER = re.compile(r"Submitted\s+(\d+)\s+indexed mesh instance\(s\) from the player scene")
PRESENTED_GEOMETRY_MARKER = re.compile(
    r"First Vulkan/NVRHI frame submitted and presented successfully \(geometry draw recorded:\s*(?:true|1)\)")
MAX_CAPTURE_PIXELS = 40_000_000
MAX_LOG_BYTES = 4 * 1024 * 1024
MAX_CDB_DIAGNOSTIC_TAIL_BYTES = 64 * 1024


class SmokeFailure(RuntimeError):
    pass


class UnsupportedCapability(SmokeFailure):
    pass


@dataclass(frozen=True)
class Window:
    Handle: int | None
    Width: int
    Height: int
    X: int = 0
    Y: int = 0


def parse_draw_count(Text: str) -> int | None:
    for Match in DRAW_MARKER.finditer(Text):
        Count = int(Match.group(1))
        if Count > 0:
            return Count
    return None


def has_presented_geometry(Text: str) -> bool:
    return PRESENTED_GEOMETRY_MARKER.search(Text) is not None


def build_launch_command(Executable: PurePath) -> list[str]:
    """The package must resolve its bundled Content without command-line arguments."""
    return [str(Executable)]


def _paeth(Left: int, Above: int, UpperLeft: int) -> int:
    Estimate = Left + Above - UpperLeft
    LeftDistance = abs(Estimate - Left)
    AboveDistance = abs(Estimate - Above)
    UpperLeftDistance = abs(Estimate - UpperLeft)
    if LeftDistance <= AboveDistance and LeftDistance <= UpperLeftDistance:
        return Left
    if AboveDistance <= UpperLeftDistance:
        return Above
    return UpperLeft


def decode_png(Data: bytes) -> tuple[int, int, bytearray]:
    if not Data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise SmokeFailure("window capture is not a PNG image")
    Offset = 8
    Width = Height = BitDepth = ColorType = Interlace = -1
    Palette = b""
    Transparency = b""
    ImageData = bytearray()
    SawHeader = False
    SawEnd = False
    while Offset < len(Data):
        if len(Data) - Offset < 12:
            raise SmokeFailure("window PNG has a truncated chunk header")
        Length = struct.unpack_from(">I", Data, Offset)[0]
        ChunkType = Data[Offset + 4:Offset + 8]
        ChunkEnd = Offset + 12 + Length
        if ChunkEnd > len(Data):
            raise SmokeFailure("window PNG has a truncated chunk")
        ChunkData = Data[Offset + 8:Offset + 8 + Length]
        ExpectedCRC = struct.unpack_from(">I", Data, Offset + 8 + Length)[0]
        ActualCRC = zlib.crc32(ChunkType + ChunkData) & 0xFFFFFFFF
        if ActualCRC != ExpectedCRC:
            raise SmokeFailure("window PNG contains a chunk with an invalid CRC")
        if ChunkType == b"IHDR":
            if SawHeader or Length != 13:
                raise SmokeFailure("window PNG has an invalid IHDR chunk")
            Width, Height, BitDepth, ColorType, Compression, Filter, Interlace = struct.unpack(">IIBBBBB", ChunkData)
            SawHeader = True
            if Width <= 0 or Height <= 0 or Width * Height > MAX_CAPTURE_PIXELS:
                raise SmokeFailure(f"window PNG dimensions are invalid or too large: {Width}x{Height}")
            if BitDepth != 8 or Compression != 0 or Filter != 0 or Interlace != 0:
                raise UnsupportedCapability("window PNG uses an unsupported bit depth, filter method, or interlace mode")
        elif ChunkType == b"PLTE":
            Palette = ChunkData
        elif ChunkType == b"tRNS":
            Transparency = ChunkData
        elif ChunkType == b"IDAT":
            if not SawHeader:
                raise SmokeFailure("window PNG has image data before its IHDR chunk")
            ImageData.extend(ChunkData)
            if len(ImageData) > MAX_CAPTURE_PIXELS * 5:
                raise SmokeFailure("window PNG compressed data exceeds the capture limit")
        elif ChunkType == b"IEND":
            SawEnd = True
            break
        Offset = ChunkEnd
    if not SawHeader or not SawEnd or not ImageData:
        raise SmokeFailure("window PNG is missing required image chunks")

    ChannelsByType = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    Channels = ChannelsByType.get(ColorType)
    if Channels is None or (ColorType == 3 and (not Palette or len(Palette) % 3 != 0)):
        raise UnsupportedCapability(f"window PNG color type {ColorType} is unsupported")
    BytesPerPixel = Channels
    RowBytes = Width * Channels
    ExpectedSize = Height * (RowBytes + 1)
    try:
        Decoder = zlib.decompressobj()
        Raw = Decoder.decompress(bytes(ImageData), ExpectedSize + 1)
        if Decoder.unconsumed_tail or len(Raw) > ExpectedSize:
            raise SmokeFailure("window PNG expands beyond its declared dimensions")
        Raw += Decoder.flush()
    except zlib.error as Error:
        raise SmokeFailure(f"window PNG image data could not be decompressed: {Error}") from Error
    if not Decoder.eof or len(Raw) != ExpectedSize:
        raise SmokeFailure(f"window PNG decoded to {len(Raw)} bytes; expected {ExpectedSize}")

    Decoded = bytearray(Width * Height * 4)
    Previous = bytearray(RowBytes)
    SourceOffset = 0
    DestinationOffset = 0
    for _ in range(Height):
        FilterType = Raw[SourceOffset]
        SourceOffset += 1
        Encoded = Raw[SourceOffset:SourceOffset + RowBytes]
        SourceOffset += RowBytes
        Row = bytearray(RowBytes)
        if FilterType > 4:
            raise SmokeFailure(f"window PNG has unsupported scanline filter {FilterType}")
        for Index, Value in enumerate(Encoded):
            Left = Row[Index - BytesPerPixel] if Index >= BytesPerPixel else 0
            Above = Previous[Index]
            UpperLeft = Previous[Index - BytesPerPixel] if Index >= BytesPerPixel else 0
            if FilterType == 1:
                Value += Left
            elif FilterType == 2:
                Value += Above
            elif FilterType == 3:
                Value += (Left + Above) // 2
            elif FilterType == 4:
                Value += _paeth(Left, Above, UpperLeft)
            Row[Index] = Value & 0xFF

        for Pixel in range(Width):
            Source = Pixel * Channels
            if ColorType == 0:
                Red = Green = Blue = Row[Source]
                Alpha = 255
            elif ColorType == 2:
                Red, Green, Blue = Row[Source:Source + 3]
                Alpha = 255
            elif ColorType == 3:
                PaletteIndex = Row[Source]
                PaletteOffset = PaletteIndex * 3
                if PaletteOffset + 2 >= len(Palette):
                    raise SmokeFailure("window PNG palette index is outside PLTE")
                Red, Green, Blue = Palette[PaletteOffset:PaletteOffset + 3]
                Alpha = Transparency[PaletteIndex] if PaletteIndex < len(Transparency) else 255
            elif ColorType == 4:
                Red = Green = Blue = Row[Source]
                Alpha = Row[Source + 1]
            else:
                Red, Green, Blue, Alpha = Row[Source:Source + 4]
            Decoded[DestinationOffset:DestinationOffset + 4] = bytes((Red, Green, Blue, Alpha))
            DestinationOffset += 4
        Previous = Row
    return Width, Height, Decoded


def _png_chunk(Kind: bytes, Payload: bytes) -> bytes:
    return (struct.pack(">I", len(Payload)) + Kind + Payload +
            struct.pack(">I", zlib.crc32(Kind + Payload) & 0xFFFFFFFF))


def encode_png(Width: int, Height: int, RGBA: bytes) -> bytes:
    if Width <= 0 or Height <= 0 or Width * Height > MAX_CAPTURE_PIXELS or len(RGBA) != Width * Height * 4:
        raise SmokeFailure("native capture returned invalid RGBA dimensions")
    Header = struct.pack(">IIBBBBB", Width, Height, 8, 6, 0, 0, 0)
    Scanlines = bytearray()
    RowSize = Width * 4
    for Y in range(Height):
        Scanlines.append(0)
        Start = Y * RowSize
        Scanlines.extend(RGBA[Start:Start + RowSize])
    return (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", Header) +
            _png_chunk(b"IDAT", zlib.compress(Scanlines, level=6)) + _png_chunk(b"IEND", b""))


def check_nonuniform_png(PathValue: Path) -> tuple[int, int, int, int]:
    Width, Height, RGBA = decode_png(PathValue.read_bytes())
    Left = Width // 8
    Right = Width - Left
    Top = Height // 8
    Bottom = Height - Top
    RegionPixels = (Right - Left) * (Bottom - Top)
    Step = max(1, RegionPixels // 250_000)
    Colors: set[tuple[int, int, int]] = set()
    Minimum = 255
    Maximum = 0
    for Y in range(Top, Bottom, Step):
        for X in range(Left, Right, Step):
            Offset = (Y * Width + X) * 4
            Red, Green, Blue = RGBA[Offset:Offset + 3]
            Colors.add((Red, Green, Blue))
            Luminance = (Red * 2126 + Green * 7152 + Blue * 722) // 10_000
            Minimum = min(Minimum, Luminance)
            Maximum = max(Maximum, Luminance)
            if len(Colors) >= 256 and Maximum - Minimum >= 32:
                break
        if len(Colors) >= 256 and Maximum - Minimum >= 32:
            break
    if len(Colors) < 16 or Maximum - Minimum < 20:
        raise SmokeFailure(
            f"captured player client area is visually uniform: distinct_colors={len(Colors)} "
            f"luminance_range={Maximum - Minimum}; expected >=16 colors and >=20 luminance levels")
    return Width, Height, len(Colors), Maximum - Minimum


def validate_third_party_notices(PackageRoot: Path, Platform: str) -> Path:
    RelativePath = (Path("Contents") / "Resources" / "ThirdPartyNotices.txt"
                    if Platform == "macOS" else Path("ThirdPartyNotices.txt"))
    NoticePath = PackageRoot / RelativePath
    if NoticePath.is_symlink() or not NoticePath.is_file():
        raise SmokeFailure(f"the package is missing its owned third-party notices file: {NoticePath}")
    try:
        if NoticePath.stat().st_size > 2 * 1024 * 1024:
            raise SmokeFailure(f"the package third-party notices file exceeds the 2 MiB bound: {NoticePath}")
        NoticeText = NoticePath.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as Error:
        raise SmokeFailure(f"could not read the package third-party notices file: {Error}") from Error

    if Platform == "macOS":
        if "Apache License" not in NoticeText:
            raise SmokeFailure("the macOS package notices omit the Apache 2.0 license text")
        RequiredNotices = (
            "Khronos Vulkan Loader 1.4.350.0",
            "Some source files use more permissive licenses",
            "https://github.com/KhronosGroup/Vulkan-Loader/blob/vulkan-sdk-1.4.350.0/LICENSE.txt",
            "MoltenVK 1.4.1",
            "https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/LICENSE",
        )
        Missing = [Text for Text in RequiredNotices if Text not in NoticeText]
        if Missing:
            raise SmokeFailure("macOS package notices omit component-specific license material: " + ", ".join(Missing))
    return NoticePath


class OutputCollector:
    def __init__(self, Stream, LogPath: Path):
        self.Stream = Stream
        self.LogPath = LogPath
        self.Lock = threading.Lock()
        self.Changed = threading.Event()
        self.Tail = ""
        self.CapturedOutput = bytearray()
        self.DrawCount: int | None = None
        self.PresentedGeometry = False
        self.ReadError: str | None = None
        self.Reader = threading.Thread(target=self._read, name="player-output-reader", daemon=True)
        self.Reader.start()

    def _read(self) -> None:
        Written = 0
        try:
            with self.LogPath.open("xb") as Log:
                while True:
                    Block = self.Stream.read(8192)
                    if not Block:
                        break
                    if Written < MAX_LOG_BYTES:
                        Kept = Block[:MAX_LOG_BYTES - Written]
                        Log.write(Kept)
                        Written += len(Kept)
                        with self.Lock:
                            self.CapturedOutput.extend(Kept)
                        if len(Kept) < len(Block):
                            Log.write(b"\n[log capture truncated at 4 MiB]\n")
                            Written = MAX_LOG_BYTES
                        Log.flush()
                    Text = Block.decode("utf-8", errors="replace")
                    with self.Lock:
                        self.Tail = (self.Tail + Text)[-128_000:]
                        self.DrawCount = parse_draw_count(self.Tail)
                        self.PresentedGeometry = has_presented_geometry(self.Tail)
                        self.Changed.set()
        except OSError as Error:
            with self.Lock:
                self.ReadError = str(Error)
                self.Changed.set()

    def wait_for_presented_geometry(self, Process: subprocess.Popen[bytes], Deadline: float) -> int:
        while time.monotonic() < Deadline:
            with self.Lock:
                Count = self.DrawCount
                PresentedGeometry = self.PresentedGeometry
                Tail = self.Tail
                ReadError = self.ReadError
            if Count is not None and PresentedGeometry:
                return Count
            Status = Process.poll()
            if Status is not None:
                self.Reader.join(timeout=1)
                raise SmokeFailure(
                    f"Player exited with status {Status} before a geometry frame was successfully presented; "
                    f"output tail={Tail[-1200:]!r}")
            if ReadError:
                raise SmokeFailure(f"could not capture Player output: {ReadError}")
            self.Changed.wait(timeout=0.1)
            self.Changed.clear()
        with self.Lock:
            Tail = self.Tail
            Count = self.DrawCount
            PresentedGeometry = self.PresentedGeometry
        raise SmokeFailure(
            "timed out waiting for a successfully presented geometry frame; "
            f"draw_count={Count}; presented_geometry={PresentedGeometry}; output tail={Tail[-1200:]!r}")

    def require_macos_library_provenance(self, PackageRoot: Path) -> tuple[str, str]:
        with self.Lock:
            Output = bytes(self.CapturedOutput).decode("utf-8", errors="replace")

        Lines = Output.splitlines()
        DyldRecord = re.compile(r"(?:\bdyld(?:\[\d+\])?:)|(?:^\s*/.*\.dylib(?:$|[\s\"'<>),]))", re.IGNORECASE)
        LibraryLines = [Line for Line in Lines if ".dylib" in Line.casefold() and DyldRecord.search(Line)]
        if not LibraryLines:
            raise UnsupportedCapability(
                "macOS DYLD_PRINT_LIBRARIES produced no dynamic-library records in captured Player output; "
                "loaded-library provenance is unavailable in this runtime mode")

        Frameworks = PackageRoot / "Contents" / "Frameworks"
        VulkanPath = str(Frameworks / "libvulkan.1.dylib")
        VulkanPattern = re.compile(re.escape(VulkanPath) + r"(?=$|[\s\"'<>),])")
        PackagePrefix = str(PackageRoot).rstrip(os.sep) + os.sep
        MoltenPattern = re.compile(
            re.escape(PackagePrefix) + r"[^\r\n\"'<>]*?[/\\]libMoltenVK\.dylib(?=$|[\s\"'<>),])")

        SawVulkan = any(VulkanPattern.search(Line) for Line in LibraryLines)
        SawMoltenVK = any(MoltenPattern.search(Line) for Line in LibraryLines)
        Missing = []
        if not SawVulkan:
            Missing.append(VulkanPath)
        if not SawMoltenVK:
            Missing.append(f"{PackageRoot}/…/libMoltenVK.dylib")
        if Missing:
            raise SmokeFailure(
                "macOS DYLD_PRINT_LIBRARIES output did not prove these packaged libraries were loaded: "
                + ", ".join(Missing))
        return VulkanPath, "libMoltenVK.dylib under package"


class LinuxX11Desktop:
    def __init__(self):
        if not os.environ.get("DISPLAY"):
            raise UnsupportedCapability("Linux capture requires an X11 display; run under Xvfb or a real X11 session")
        LibraryName = ctypes.util.find_library("X11")
        if not LibraryName:
            raise UnsupportedCapability("Linux X11 capture requires libX11 (the current CI package-only job uses Xvfb)")
        self.X = ctypes.CDLL(LibraryName)
        self._configure()
        self.Display = self.X.XOpenDisplay(None)
        if not self.Display:
            raise UnsupportedCapability("could not connect to DISPLAY for native X11 window capture")
        self.Root = self.X.XDefaultRootWindow(self.Display)
        self.WMProtocols = self.X.XInternAtom(self.Display, b"WM_PROTOCOLS", 0)
        self.WMDelete = self.X.XInternAtom(self.Display, b"WM_DELETE_WINDOW", 0)

    def _configure(self) -> None:
        X = self.X
        X.XOpenDisplay.argtypes = [ctypes.c_char_p]
        X.XOpenDisplay.restype = ctypes.c_void_p
        X.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
        X.XDefaultRootWindow.restype = ctypes.c_ulong
        X.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        X.XInternAtom.restype = ctypes.c_ulong
        X.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                                 ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                                 ctypes.POINTER(ctypes.c_uint)]
        X.XQueryTree.restype = ctypes.c_int
        X.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long,
                                         ctypes.c_long, ctypes.c_int, ctypes.c_ulong,
                                         ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
                                         ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                                         ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
        X.XGetWindowProperty.restype = ctypes.c_int
        X.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_void_p)]
        X.XFetchName.restype = ctypes.c_int
        X.XGetGeometry.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                                   ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                   ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
                                   ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
        X.XGetGeometry.restype = ctypes.c_int
        class XWindowAttributes(ctypes.Structure):
            _fields_ = [
                ("x", ctypes.c_int), ("y", ctypes.c_int),
                ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("border_width", ctypes.c_int), ("depth", ctypes.c_int),
                ("visual", ctypes.c_void_p), ("root", ctypes.c_ulong),
                ("window_class", ctypes.c_int), ("bit_gravity", ctypes.c_int),
                ("win_gravity", ctypes.c_int), ("backing_store", ctypes.c_int),
                ("backing_planes", ctypes.c_ulong), ("backing_pixel", ctypes.c_ulong),
                ("save_under", ctypes.c_int), ("colormap", ctypes.c_ulong),
                ("map_installed", ctypes.c_int), ("map_state", ctypes.c_int),
                ("all_event_masks", ctypes.c_long), ("your_event_mask", ctypes.c_long),
                ("do_not_propagate_mask", ctypes.c_long), ("override_redirect", ctypes.c_int),
                ("screen", ctypes.c_void_p),
            ]

        self.XWindowAttributes = XWindowAttributes
        X.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                           ctypes.POINTER(XWindowAttributes)]
        X.XGetWindowAttributes.restype = ctypes.c_int
        X.XGetWMProtocols.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                      ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_int)]
        X.XGetWMProtocols.restype = ctypes.c_int
        X.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
        X.XGetImage.restype = ctypes.c_void_p
        X.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long,
                                 ctypes.c_void_p]
        X.XSendEvent.restype = ctypes.c_int
        X.XFlush.argtypes = [ctypes.c_void_p]
        X.XCloseDisplay.argtypes = [ctypes.c_void_p]
        X.XFree.argtypes = [ctypes.c_void_p]

    def _pid(self, Handle: int) -> int | None:
        Atom = self.X.XInternAtom(self.Display, b"_NET_WM_PID", 0)
        ActualType = ctypes.c_ulong()
        Format = ctypes.c_int()
        ItemCount = ctypes.c_ulong()
        BytesAfter = ctypes.c_ulong()
        Property = ctypes.POINTER(ctypes.c_ubyte)()
        Status = self.X.XGetWindowProperty(self.Display, Handle, Atom, 0, 1, 0, 0,
                                           ctypes.byref(ActualType), ctypes.byref(Format),
                                           ctypes.byref(ItemCount), ctypes.byref(BytesAfter),
                                           ctypes.byref(Property))
        if Status != 0 or not Property:
            return None
        try:
            if Format.value != 32 or ItemCount.value != 1:
                return None
            return int(ctypes.cast(Property, ctypes.POINTER(ctypes.c_ulong))[0])
        finally:
            self.X.XFree(Property)

    def _title(self, Handle: int) -> str:
        Name = ctypes.c_void_p()
        if not self.X.XFetchName(self.Display, Handle, ctypes.byref(Name)) or not Name.value:
            return ""
        try:
            return ctypes.string_at(Name.value).decode("utf-8", errors="replace")
        finally:
            self.X.XFree(Name)

    def _children(self, Parent: int) -> list[int]:
        Root = ctypes.c_ulong()
        ParentReturn = ctypes.c_ulong()
        Children = ctypes.POINTER(ctypes.c_ulong)()
        Count = ctypes.c_uint()
        if not self.X.XQueryTree(self.Display, Parent, ctypes.byref(Root), ctypes.byref(ParentReturn),
                                 ctypes.byref(Children), ctypes.byref(Count)):
            return []
        try:
            return [int(Children[Index]) for Index in range(Count.value)] if Children else []
        finally:
            if Children:
                self.X.XFree(Children)

    def find_window(self, PID: int, Deadline: float) -> Window:
        while time.monotonic() < Deadline:
            Stack = [(int(self.Root), 0)]
            Candidates: list[int] = []
            Visited = 0
            while Stack and Visited < 10_000:
                Handle, Depth = Stack.pop()
                Visited += 1
                if self._pid(Handle) == PID and "pulseforge player" in self._title(Handle).casefold():
                    Attributes = self.XWindowAttributes()
                    if (self.X.XGetWindowAttributes(self.Display, Handle, ctypes.byref(Attributes)) and
                            Attributes.map_state == 2):  # IsViewable
                        Candidates.append(Handle)
                if Depth < 8:
                    Stack.extend((Child, Depth + 1) for Child in self._children(Handle))
            if len(Candidates) == 1:
                Handle = Candidates[0]
                Root = ctypes.c_ulong()
                X = ctypes.c_int()
                Y = ctypes.c_int()
                Width = ctypes.c_uint()
                Height = ctypes.c_uint()
                Border = ctypes.c_uint()
                Depth = ctypes.c_uint()
                if self.X.XGetGeometry(self.Display, Handle, ctypes.byref(Root), ctypes.byref(X),
                                       ctypes.byref(Y), ctypes.byref(Width), ctypes.byref(Height),
                                       ctypes.byref(Border), ctypes.byref(Depth)):
                    if Width.value >= 100 and Height.value >= 100:
                        if Width.value * Height.value > MAX_CAPTURE_PIXELS:
                            raise UnsupportedCapability("PulseForge X11 window exceeds the capture pixel safety limit")
                        return Window(Handle, int(Width.value), int(Height.value), X.value, Y.value)
            elif len(Candidates) > 1:
                raise UnsupportedCapability(f"X11 found {len(Candidates)} PulseForge Player windows for PID {PID}")
            time.sleep(0.1)
        raise UnsupportedCapability(f"X11 could not identify one mapped PulseForge Player window for PID {PID} via _NET_WM_PID")

    def capture(self, Target: Window, Destination: Path) -> None:
        ImagePointer = self.X.XGetImage(self.Display, Target.Handle, 0, 0, Target.Width, Target.Height,
                                        ctypes.c_ulong(-1).value, 2)
        if not ImagePointer:
            raise UnsupportedCapability("XGetImage could not capture the PulseForge client window")

        class ImageFunctions(ctypes.Structure):
            _fields_ = [("create_image", ctypes.c_void_p), ("destroy_image", ctypes.c_void_p),
                        ("get_pixel", ctypes.c_void_p), ("put_pixel", ctypes.c_void_p),
                        ("sub_image", ctypes.c_void_p), ("add_pixel", ctypes.c_void_p)]

        class XImage(ctypes.Structure):
            _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int),
                        ("format", ctypes.c_int), ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int),
                        ("bitmap_unit", ctypes.c_int), ("bitmap_bit_order", ctypes.c_int),
                        ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
                        ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int),
                        ("red_mask", ctypes.c_ulong), ("green_mask", ctypes.c_ulong),
                        ("blue_mask", ctypes.c_ulong), ("obdata", ctypes.c_void_p),
                        ("functions", ImageFunctions)]

        Image = ctypes.cast(ImagePointer, ctypes.POINTER(XImage))
        try:
            if Image.contents.width * Image.contents.height > MAX_CAPTURE_PIXELS:
                raise UnsupportedCapability("X11 client capture exceeds the pixel safety limit")
            GetPixel = ctypes.CFUNCTYPE(ctypes.c_ulong, ctypes.POINTER(XImage), ctypes.c_int, ctypes.c_int)(
                Image.contents.functions.get_pixel)

            def Component(Value: int, Mask: int) -> int:
                if not Mask:
                    return 0
                Shift = (Mask & -Mask).bit_length() - 1
                Maximum = Mask >> Shift
                return (((Value & Mask) >> Shift) * 255 + Maximum // 2) // Maximum

            RGBA = bytearray(Image.contents.width * Image.contents.height * 4)
            Offset = 0
            for Y in range(Image.contents.height):
                for X in range(Image.contents.width):
                    Pixel = GetPixel(Image, X, Y)
                    RGBA[Offset:Offset + 4] = bytes((
                        Component(Pixel, Image.contents.red_mask),
                        Component(Pixel, Image.contents.green_mask),
                        Component(Pixel, Image.contents.blue_mask), 255))
                    Offset += 4
            Destination.write_bytes(encode_png(Image.contents.width, Image.contents.height, RGBA))
        finally:
            Destroy = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(XImage))(Image.contents.functions.destroy_image)
            Destroy(Image)

    def close_window(self, Target: Window) -> None:
        Protocols = ctypes.POINTER(ctypes.c_ulong)()
        Count = ctypes.c_int()
        if not self.X.XGetWMProtocols(self.Display, Target.Handle, ctypes.byref(Protocols), ctypes.byref(Count)):
            raise UnsupportedCapability("PulseForge X11 window does not expose WM_DELETE_WINDOW")
        try:
            if self.WMDelete not in [int(Protocols[Index]) for Index in range(Count.value)]:
                raise UnsupportedCapability("PulseForge X11 window does not advertise WM_DELETE_WINDOW")
        finally:
            if Protocols:
                self.X.XFree(Protocols)

        class ClientData(ctypes.Union):
            _fields_ = [("bytes", ctypes.c_char * 20), ("shorts", ctypes.c_short * 10),
                        ("longs", ctypes.c_long * 5)]

        class ClientMessage(ctypes.Structure):
            _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                        ("display", ctypes.c_void_p), ("window", ctypes.c_ulong),
                        ("message_type", ctypes.c_ulong), ("format", ctypes.c_int), ("data", ClientData)]

        class XEvent(ctypes.Union):
            _fields_ = [("type", ctypes.c_int), ("client", ClientMessage), ("pad", ctypes.c_long * 24)]

        Event = XEvent()
        Event.client.type = 33
        Event.client.serial = 0
        Event.client.send_event = 1
        Event.client.display = self.Display
        Event.client.window = Target.Handle
        Event.client.message_type = self.WMProtocols
        Event.client.format = 32
        Event.client.data.longs[0] = self.WMDelete
        Event.client.data.longs[1] = 0
        if not self.X.XSendEvent(self.Display, Target.Handle, 0, 0, ctypes.byref(Event)):
            raise UnsupportedCapability("X11 refused to deliver WM_DELETE_WINDOW to PulseForge")
        self.X.XFlush(self.Display)

    def close(self) -> None:
        if self.Display:
            self.X.XCloseDisplay(self.Display)
            self.Display = None


class WindowsDesktop:
    def __init__(self):
        if os.name != "nt":
            raise UnsupportedCapability("Windows desktop capture is only available on Windows")
        self.User32 = ctypes.WinDLL("user32", use_last_error=True)
        self.Gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        self._configure()
        self.User32.OpenInputDesktop.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.User32.OpenInputDesktop.restype = wintypes.HANDLE
        self.User32.CloseDesktop.argtypes = [wintypes.HANDLE]
        self.User32.CloseDesktop.restype = wintypes.BOOL
        Desktop = self.User32.OpenInputDesktop(0, False, 0x0001)
        if not Desktop:
            raise UnsupportedCapability("Windows has no accessible interactive desktop for window capture")
        self.User32.CloseDesktop(Desktop)

    def _configure(self) -> None:
        W = wintypes
        self.User32.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM), W.LPARAM]
        self.User32.EnumWindows.restype = W.BOOL
        self.User32.GetWindowThreadProcessId.argtypes = [W.HWND, ctypes.POINTER(W.DWORD)]
        self.User32.GetWindowThreadProcessId.restype = W.DWORD
        self.User32.IsWindowVisible.argtypes = [W.HWND]
        self.User32.IsWindowVisible.restype = W.BOOL
        self.User32.GetWindowTextLengthW.argtypes = [W.HWND]
        self.User32.GetWindowTextLengthW.restype = ctypes.c_int
        self.User32.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, ctypes.c_int]
        self.User32.GetWindowTextW.restype = ctypes.c_int
        self.User32.GetForegroundWindow.argtypes = []
        self.User32.GetForegroundWindow.restype = W.HWND
        self.User32.GetClientRect.argtypes = [W.HWND, ctypes.POINTER(W.RECT)]
        self.User32.GetClientRect.restype = W.BOOL
        self.User32.ClientToScreen.argtypes = [W.HWND, ctypes.POINTER(W.POINT)]
        self.User32.ClientToScreen.restype = W.BOOL
        self.User32.ShowWindow.argtypes = [W.HWND, ctypes.c_int]
        self.User32.SetForegroundWindow.argtypes = [W.HWND]
        self.User32.BringWindowToTop.argtypes = [W.HWND]
        self.User32.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
        self.User32.PostMessageW.restype = W.BOOL
        self.User32.GetDC.argtypes = [W.HWND]
        self.User32.GetDC.restype = W.HDC
        self.User32.ReleaseDC.argtypes = [W.HWND, W.HDC]
        self.User32.ReleaseDC.restype = ctypes.c_int
        self.Gdi32.CreateCompatibleDC.argtypes = [W.HDC]
        self.Gdi32.CreateCompatibleDC.restype = W.HDC
        self.Gdi32.CreateCompatibleBitmap.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int]
        self.Gdi32.CreateCompatibleBitmap.restype = W.HBITMAP
        self.Gdi32.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
        self.Gdi32.SelectObject.restype = W.HGDIOBJ
        self.Gdi32.BitBlt.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                      W.HDC, ctypes.c_int, ctypes.c_int, W.DWORD]
        self.Gdi32.BitBlt.restype = W.BOOL
        self.Gdi32.GetDIBits.argtypes = [W.HDC, W.HBITMAP, W.UINT, W.UINT, W.LPVOID,
                                         W.LPVOID, W.UINT]
        self.Gdi32.GetDIBits.restype = ctypes.c_int
        self.Gdi32.DeleteObject.argtypes = [W.HGDIOBJ]
        self.Gdi32.DeleteObject.restype = W.BOOL
        self.Gdi32.DeleteDC.argtypes = [W.HDC]
        self.Gdi32.DeleteDC.restype = W.BOOL

    def find_window(self, PID: int, Deadline: float) -> Window:
        W = wintypes
        while time.monotonic() < Deadline:
            Matches: list[tuple[int, str]] = []

            @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
            def Enumerate(Handle, _Parameter):
                WindowPID = W.DWORD()
                self.User32.GetWindowThreadProcessId(Handle, ctypes.byref(WindowPID))
                if WindowPID.value == PID and self.User32.IsWindowVisible(Handle):
                    Length = self.User32.GetWindowTextLengthW(Handle)
                    Buffer = ctypes.create_unicode_buffer(Length + 1)
                    self.User32.GetWindowTextW(Handle, Buffer, len(Buffer))
                    if "pulseforge player" in Buffer.value.casefold():
                        Matches.append((int(Handle), Buffer.value))
                return True

            if not self.User32.EnumWindows(Enumerate, 0):
                Error = ctypes.get_last_error()
                if Error:
                    raise UnsupportedCapability(f"EnumWindows failed with Win32 error {Error}")
            if len(Matches) == 1:
                Handle = Matches[0][0]
                Rect = W.RECT()
                if not self.User32.GetClientRect(Handle, ctypes.byref(Rect)):
                    raise UnsupportedCapability("GetClientRect failed for the PulseForge Player window")
                Width = Rect.right - Rect.left
                Height = Rect.bottom - Rect.top
                if Width >= 100 and Height >= 100:
                    if Width * Height > MAX_CAPTURE_PIXELS:
                        raise UnsupportedCapability("PulseForge Windows client window exceeds the capture pixel safety limit")
                    return Window(Handle, Width, Height)
            if len(Matches) > 1:
                raise UnsupportedCapability(f"Windows found {len(Matches)} PulseForge Player windows for PID {PID}")
            time.sleep(0.1)
        raise UnsupportedCapability(f"Windows could not identify one visible PulseForge Player window for PID {PID}")

    def capture(self, Target: Window, Destination: Path) -> None:
        W = wintypes
        self.User32.ShowWindow(Target.Handle, 9)
        self.User32.BringWindowToTop(Target.Handle)
        self.User32.SetForegroundWindow(Target.Handle)
        time.sleep(0.2)
        if self.User32.GetForegroundWindow() != Target.Handle:
            raise UnsupportedCapability("Windows did not permit the Player window to become the capture target")
        Point = W.POINT(0, 0)
        if not self.User32.ClientToScreen(Target.Handle, ctypes.byref(Point)):
            raise UnsupportedCapability("ClientToScreen failed for the PulseForge Player window")
        ScreenDC = self.User32.GetDC(None)
        if not ScreenDC:
            raise UnsupportedCapability("GetDC could not access the Windows desktop")
        MemoryDC = self.Gdi32.CreateCompatibleDC(ScreenDC)
        Bitmap = None
        PreviousObject = None
        BitmapSelected = False
        try:
            if not MemoryDC:
                raise UnsupportedCapability("CreateCompatibleDC failed for desktop capture")
            Bitmap = self.Gdi32.CreateCompatibleBitmap(ScreenDC, Target.Width, Target.Height)
            if not Bitmap:
                raise UnsupportedCapability("CreateCompatibleBitmap failed for desktop capture")
            PreviousObject = self.Gdi32.SelectObject(MemoryDC, Bitmap)
            if not PreviousObject:
                raise UnsupportedCapability("SelectObject failed for desktop capture")
            BitmapSelected = True
            if not self.Gdi32.BitBlt(MemoryDC, 0, 0, Target.Width, Target.Height, ScreenDC,
                                     Point.x, Point.y, 0x00CC0020 | 0x40000000):
                raise UnsupportedCapability("BitBlt could not capture the PulseForge client window")

            class BitmapInfoHeader(ctypes.Structure):
                _fields_ = [("biSize", W.DWORD), ("biWidth", W.LONG), ("biHeight", W.LONG),
                            ("biPlanes", W.WORD), ("biBitCount", W.WORD), ("biCompression", W.DWORD),
                            ("biSizeImage", W.DWORD), ("biXPelsPerMeter", W.LONG),
                            ("biYPelsPerMeter", W.LONG), ("biClrUsed", W.DWORD), ("biClrImportant", W.DWORD)]

            class BitmapInfo(ctypes.Structure):
                _fields_ = [("bmiHeader", BitmapInfoHeader), ("bmiColors", W.DWORD * 3)]

            Info = BitmapInfo()
            Info.bmiHeader.biSize = ctypes.sizeof(BitmapInfoHeader)
            Info.bmiHeader.biWidth = Target.Width
            Info.bmiHeader.biHeight = -Target.Height
            Info.bmiHeader.biPlanes = 1
            Info.bmiHeader.biBitCount = 32
            Info.bmiHeader.biCompression = 0
            Buffer = (ctypes.c_ubyte * (Target.Width * Target.Height * 4))()
            self.Gdi32.SelectObject(MemoryDC, PreviousObject)
            BitmapSelected = False
            Lines = self.Gdi32.GetDIBits(ScreenDC, Bitmap, 0, Target.Height, Buffer,
                                         ctypes.byref(Info), 0)
            if Lines != Target.Height:
                raise UnsupportedCapability(f"GetDIBits captured {Lines} of {Target.Height} client rows")
            Raw = bytes(Buffer)
            RGBA = bytearray(len(Raw))
            for Offset in range(0, len(Raw), 4):
                Blue, Green, Red = Raw[Offset:Offset + 3]
                RGBA[Offset:Offset + 4] = bytes((Red, Green, Blue, 255))
            Destination.write_bytes(encode_png(Target.Width, Target.Height, RGBA))
        finally:
            if BitmapSelected and PreviousObject:
                self.Gdi32.SelectObject(MemoryDC, PreviousObject)
            if Bitmap:
                self.Gdi32.DeleteObject(Bitmap)
            if MemoryDC:
                self.Gdi32.DeleteDC(MemoryDC)
            self.User32.ReleaseDC(None, ScreenDC)

    def close_window(self, Target: Window) -> None:
        if not self.User32.PostMessageW(Target.Handle, 0x0010, 0, 0):
            raise UnsupportedCapability(f"PostMessage(WM_CLOSE) failed with Win32 error {ctypes.get_last_error()}")


class MacOSDesktop:
    WINDOW_INFO_C = r'''#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int read_number(CFDictionaryRef Dictionary, CFStringRef Key, CFNumberType Type, void* Value)
{
	CFTypeRef Object = CFDictionaryGetValue(Dictionary, Key);
	if (Object == NULL || CFGetTypeID(Object) != CFNumberGetTypeID())
		return 0;
	return CFNumberGetValue((CFNumberRef)Object, Type, Value) ? 1 : 0;
}

int main(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount != 2)
	{
		fputs("expected one target process ID\n", stderr);
		return 2;
	}
	char* End = NULL;
	long TargetPID = strtol(Arguments[1], &End, 10);
	if (End == Arguments[1] || *End != '\0' || TargetPID <= 0 || TargetPID > INT32_MAX)
	{
		fputs("invalid target process ID\n", stderr);
		return 2;
	}
	CFArrayRef Windows = CGWindowListCopyWindowInfo(
		kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
	if (Windows == NULL)
	{
		fputs("CoreGraphics could not enumerate on-screen windows\n", stderr);
		return 3;
	}
	putchar('[');
	int Matches = 0;
	CFIndex Count = CFArrayGetCount(Windows);
	for (CFIndex Index = 0; Index < Count; ++Index)
	{
		CFDictionaryRef Info = (CFDictionaryRef)CFArrayGetValueAtIndex(Windows, Index);
		int32_t OwnerPID = 0;
		int32_t Layer = -1;
		int64_t WindowID = 0;
		if (!read_number(Info, kCGWindowOwnerPID, kCFNumberSInt32Type, &OwnerPID) || OwnerPID != TargetPID ||
			!read_number(Info, kCGWindowLayer, kCFNumberSInt32Type, &Layer) || Layer != 0 ||
			!read_number(Info, kCGWindowNumber, kCFNumberSInt64Type, &WindowID) || WindowID <= 0)
			continue;
		CFTypeRef BoundsObject = CFDictionaryGetValue(Info, kCGWindowBounds);
		if (BoundsObject == NULL || CFGetTypeID(BoundsObject) != CFDictionaryGetTypeID())
			continue;
		CGRect Bounds;
		if (!CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)BoundsObject, &Bounds) ||
			!isfinite(Bounds.origin.x) || !isfinite(Bounds.origin.y) ||
			!isfinite(Bounds.size.width) || !isfinite(Bounds.size.height) ||
			Bounds.size.width <= 0 || Bounds.size.height <= 0)
			continue;
		printf("%s{\"window_id\":%" PRId64 ",\"owner_pid\":%d,\"x\":%.17g,\"y\":%.17g,"
			"\"width\":%.17g,\"height\":%.17g}", Matches == 0 ? "" : ",", WindowID, OwnerPID,
			(double)Bounds.origin.x, (double)Bounds.origin.y, (double)Bounds.size.width, (double)Bounds.size.height);
		++Matches;
	}
	putchar(']');
	putchar('\n');
	CFRelease(Windows);
	return 0;
}
'''

    def __init__(self, PackageRoot: Path):
        if sys.platform != "darwin":
            raise UnsupportedCapability("macOS bundle capture is only available on macOS")
        if not shutil.which("osascript") or not Path("/usr/sbin/screencapture").is_file():
            raise UnsupportedCapability("macOS route requires osascript and /usr/sbin/screencapture")
        InfoPath = PackageRoot / "Contents" / "Info.plist"
        try:
            with InfoPath.open("rb") as Info:
                Bundle = plistlib.load(Info)
            self.ExecutableName = str(Bundle["CFBundleExecutable"])
        except (OSError, KeyError, plistlib.InvalidFileException) as Error:
            raise SmokeFailure(f"macOS package has no readable CFBundleExecutable in Contents/Info.plist: {Error}") from Error

    @staticmethod
    def _osascript(Script: str, Timeout: float = 5.0, Operation: str = "unspecified") -> str:
        try:
            Result = subprocess.run(["osascript", "-e", Script], check=False, capture_output=True,
                                    text=True, encoding="utf-8", errors="strict", timeout=Timeout,
                                    env={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C.UTF-8"})
        except subprocess.TimeoutExpired as Error:
            def OutputTail(Value: str | bytes | None) -> str:
                if isinstance(Value, bytes):
                    Value = Value.decode("utf-8", errors="replace")
                return (Value or "")[-1000:]

            Stdout = Error.stdout if Error.stdout is not None else Error.output
            raise UnsupportedCapability(
                f"bounded AppleScript/Accessibility operation timed out; operation={Operation}; "
                f"timeout={Timeout:.1f}s; "
                f"stdout_tail={OutputTail(Stdout)!r}; stderr_tail={OutputTail(Error.stderr)!r}") from Error
        except OSError as Error:
            raise UnsupportedCapability(
                f"bounded AppleScript/Accessibility operation failed; operation={Operation}; error={Error}") from Error
        if Result.returncode != 0:
            Detail = (Result.stderr or Result.stdout).strip()
            Lower = Detail.casefold()
            if "not authorized" in Lower or "assistive access" in Lower or "not allowed" in Lower:
                raise UnsupportedCapability(f"macOS Accessibility permission is unavailable: {Detail}")
            raise UnsupportedCapability(f"macOS AppleScript operation failed: {Detail or Result.returncode}")
        return Result.stdout.strip()

    def find_window(self, PID: int, Deadline: float) -> Window:
        Compiler = shutil.which("clang")
        if Compiler is None:
            raise UnsupportedCapability("macOS window observation requires the host clang compiler")
        Remaining = Deadline - time.monotonic()
        if Remaining <= 0:
            raise UnsupportedCapability(f"macOS window query deadline expired before inspection; pid={PID}")
        print(f"STANDALONE_PACKAGE_SMOKE_WINDOW_QUERY=begin; pid={PID}; "
              "operation=compile-coregraphics-window-probe", flush=True)
        try:
            with tempfile.TemporaryDirectory(prefix="PulseForge Window Query Ω ") as Temporary:
                Root = Path(Temporary)
                Source = Root / "window_info.c"
                Executable = Root / "window_info"
                Source.write_text(self.WINDOW_INFO_C, encoding="utf-8", newline="\n")
                Build = subprocess.run(
                    [Compiler, "-std=c11", "-O2", str(Source), "-framework", "CoreGraphics",
                     "-framework", "CoreFoundation", "-o", str(Executable)],
                    check=False, capture_output=True, text=True, encoding="utf-8", errors="strict",
                    timeout=Remaining,
                    env={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C.UTF-8"})
                if Build.returncode != 0:
                    Detail = (Build.stderr or Build.stdout).strip()
                    raise UnsupportedCapability(
                        f"CoreGraphics window probe compilation failed; exit_code={Build.returncode}; "
                        f"detail={Detail or 'no diagnostic'}")
                print(f"STANDALONE_PACKAGE_SMOKE_WINDOW_QUERY=complete; pid={PID}; "
                      "operation=compile-coregraphics-window-probe", flush=True)
                Remaining = Deadline - time.monotonic()
                if Remaining <= 0:
                    raise UnsupportedCapability(f"macOS window query deadline expired after probe compilation; pid={PID}")
                print(f"STANDALONE_PACKAGE_SMOKE_WINDOW_QUERY=begin; pid={PID}; "
                      "operation=coregraphics-window-list", flush=True)
                Query = subprocess.run(
                    [str(Executable), str(PID)], check=False, capture_output=True, text=True,
                    encoding="utf-8", errors="strict", timeout=Remaining,
                    env={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C.UTF-8"})
                if Query.returncode != 0:
                    Detail = (Query.stderr or Query.stdout).strip()
                    raise UnsupportedCapability(
                        f"CoreGraphics window enumeration failed; pid={PID}; "
                        f"exit_code={Query.returncode}; detail={Detail or 'no diagnostic'}")
                try:
                    Windows = json.loads(Query.stdout)
                except json.JSONDecodeError as Error:
                    raise UnsupportedCapability(f"CoreGraphics returned malformed window data for pid={PID}") from Error
        except subprocess.TimeoutExpired as Error:
            raise UnsupportedCapability(
                f"bounded CoreGraphics window probe timed out; pid={PID}; "
                f"timeout={Deadline - time.monotonic():.1f}s") from Error
        except (OSError, UnicodeError) as Error:
            raise UnsupportedCapability(f"CoreGraphics window probe failed; pid={PID}; error={Error}") from Error
        if not isinstance(Windows, list):
            raise UnsupportedCapability(f"CoreGraphics returned an invalid window list for pid={PID}")
        for Candidate in Windows:
            if not isinstance(Candidate, dict) or Candidate.get("owner_pid") != PID:
                raise UnsupportedCapability(f"CoreGraphics returned a window with mismatched ownership for pid={PID}")
        if len(Windows) != 1:
            raise UnsupportedCapability(
                f"CoreGraphics expected exactly one on-screen Player window; pid={PID}; count={len(Windows)}")
        Candidate = Windows[0]
        try:
            Handle = int(Candidate["window_id"])
            Left = float(Candidate["x"])
            Top = float(Candidate["y"])
            Width = float(Candidate["width"])
            Height = float(Candidate["height"])
        except (KeyError, TypeError, ValueError) as Error:
            raise UnsupportedCapability(f"CoreGraphics returned incomplete window geometry for pid={PID}") from Error
        if (Handle <= 0 or not all(math.isfinite(Value) for Value in (Left, Top, Width, Height)) or
                Width < 100 or Height < 100):
            raise UnsupportedCapability(f"CoreGraphics returned invalid Player window geometry for pid={PID}")
        if Width * Height > MAX_CAPTURE_PIXELS:
            raise UnsupportedCapability("PulseForge macOS window exceeds the capture pixel safety limit")
        print(f"STANDALONE_PACKAGE_SMOKE_WINDOW_QUERY=complete; pid={PID}; "
              f"operation=coregraphics-window-list; window_id={Handle}; "
              f"bounds={Left:g},{Top:g},{Width:g},{Height:g}", flush=True)
        return Window(Handle, int(Width), int(Height), int(Left), int(Top))

    def capture(self, Target: Window, Destination: Path) -> None:
        if Target.Handle is None:
            CaptureArguments = [
                "/usr/sbin/screencapture", "-x", "-R",
                f"{Target.X},{Target.Y},{Target.Width},{Target.Height}", str(Destination),
            ]
        else:
            CaptureArguments = ["/usr/sbin/screencapture", "-x", "-l", str(Target.Handle), str(Destination)]
        try:
            Result = subprocess.run(CaptureArguments,
                                    check=False, capture_output=True, text=True, timeout=10,
                                    env={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C.UTF-8"})
        except (OSError, subprocess.TimeoutExpired) as Error:
            raise UnsupportedCapability(f"bounded macOS window capture failed: {Error}") from Error
        if Result.returncode != 0 or not Destination.is_file() or Destination.stat().st_size == 0:
            Detail = (Result.stderr or Result.stdout).strip()
            raise UnsupportedCapability(f"screencapture could not capture window {Target.Handle}: {Detail or Result.returncode}")

    def close_window(self, Target: Window, PID: int, Timeout: float = 30.0) -> None:
        Script = f'''tell application "System Events"
    set targetProcess to first process whose unix id is {PID}
    if (count of windows of targetProcess) is not 1 then error "PulseForge process does not have exactly one window to close"
    set targetWindow to first window of targetProcess
    set closeButton to first button of targetWindow whose subrole is "AXCloseButton"
    log "STANDALONE_NATIVE_CLOSE_TARGET_READY; pid={PID}"
    click closeButton
    return "MACOS_NATIVE_WINDOW_CLOSE_REQUESTED; pid={PID}"
end tell'''
        self._osascript(Script, Timeout, "native-window-close")


def _resolve_package(PackageRoot: Path, Platform: str) -> tuple[Path, Path]:
    if PackageRoot.is_symlink() or not PackageRoot.is_dir():
        raise SmokeFailure(f"package root must be an existing, unlinked directory: {PackageRoot}")
    Root = PackageRoot.resolve(strict=True)
    if Platform == "macOS":
        if Root.suffix != ".app":
            raise SmokeFailure(f"macOS package root must be the exported .app bundle: {Root}")
        ResourceRoot = Root / "Contents" / "Resources"
        InfoPath = Root / "Contents" / "Info.plist"
        try:
            with InfoPath.open("rb") as Info:
                ExecutableName = str(plistlib.load(Info)["CFBundleExecutable"])
        except (OSError, KeyError, plistlib.InvalidFileException) as Error:
            raise SmokeFailure(f"macOS package Info.plist could not resolve its executable: {Error}") from Error
        Executable = Root / "Contents" / "MacOS" / ExecutableName
    elif Platform == "Windows":
        ResourceRoot = Root
        Executable = Root / "PulseForgeCookedGame.exe"
    else:
        ResourceRoot = Root
        Executable = Root / "PulseForgeCookedGame"
    if Executable.is_symlink() or not Executable.is_file():
        raise SmokeFailure(f"exported cooked Player executable is missing or linked: {Executable}")
    if Platform != "Windows" and not os.access(Executable, os.X_OK):
        raise SmokeFailure(f"exported cooked Player is not executable: {Executable}")
    ManifestPath = ResourceRoot / "Content" / "manifest.pfmanifest"
    if ManifestPath.is_symlink() or not ManifestPath.is_file():
        raise SmokeFailure(f"package has no bundled cooked-content manifest at {ManifestPath}; source fallback is forbidden")
    try:
        Manifest = json.loads(ManifestPath.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as Error:
        raise SmokeFailure(f"bundled cooked-content manifest is unreadable: {Error}") from Error
    if Manifest.get("targetPlatform") != Platform or Manifest.get("rendererBackend") != "Vulkan":
        raise SmokeFailure(
            f"package manifest target mismatch: platform={Manifest.get('targetPlatform')!r} "
            f"renderer={Manifest.get('rendererBackend')!r}; expected {Platform}/Vulkan")
    if Manifest.get("configuration") not in {"Release", "Dist"}:
        raise SmokeFailure(
            f"package must be Release or Dist; manifest configuration={Manifest.get('configuration')!r}")
    return Root, Executable


def _reject_links(PackageRoot: Path) -> None:
    for Current, DirectoryNames, FileNames in os.walk(PackageRoot, followlinks=False):
        CurrentPath = Path(Current)
        for Name in DirectoryNames + FileNames:
            PathValue = CurrentPath / Name
            if PathValue.is_symlink():
                raise SmokeFailure(f"package contains a linked path; refusing to change permissions: {PathValue}")


def _make_read_only(PackageRoot: Path) -> int:
    _reject_links(PackageRoot)
    Count = 0
    for Current, DirectoryNames, FileNames in os.walk(PackageRoot, topdown=False, followlinks=False):
        CurrentPath = Path(Current)
        for Name in FileNames + DirectoryNames:
            PathValue = CurrentPath / Name
            Mode = PathValue.stat(follow_symlinks=False).st_mode
            if os.name == "nt":
                PathValue.chmod(Mode & ~stat.S_IWRITE)
            else:
                PathValue.chmod(Mode & ~0o222)
            Count += 1
        Mode = CurrentPath.stat(follow_symlinks=False).st_mode
        if os.name == "nt":
            CurrentPath.chmod(Mode & ~stat.S_IWRITE)
        else:
            CurrentPath.chmod(Mode & ~0o222)
    return Count


class WindowsPackageReadOnlyAccess:
    """Applies a restorable deny-write DACL to this owned temporary package only."""

    # Generic W also denies standard rights shared by read/execute handles on Windows.
    # Deny package mutation rights explicitly so image execution remains available.
    # Keep DACL/owner management available so the test can restore the saved ACL after launch.
    WriteDenial = "(OI)(CI)(WD,AD,WEA,WA,DE,DC)"

    def __init__(self, PackageRoot: Path, TemporaryRoot: Path):
        self.PackageRoot = PackageRoot
        self.WorkingDirectory = PackageRoot.parent
        self.Backup = TemporaryRoot / "package-dacl.acl"
        self.Icacls = shutil.which("icacls.exe")
        self.Sid: str | None = None
        self.Saved = False

    @staticmethod
    def _run(Command: list[str], WorkingDirectory: Path) -> subprocess.CompletedProcess[str]:
        try:
            Result = subprocess.run(Command, cwd=WorkingDirectory, text=True, capture_output=True,
                                    timeout=30, check=False)
        except (OSError, subprocess.SubprocessError) as Error:
            raise SmokeFailure(f"could not apply scoped Windows package ACL: {Error}") from Error
        if Result.returncode != 0:
            Detail = (Result.stderr or Result.stdout).strip()
            raise SmokeFailure(f"scoped Windows package ACL command failed ({Result.returncode}): {Detail}")
        return Result

    def apply(self) -> None:
        if os.name != "nt":
            return
        if not self.Icacls:
            raise SmokeFailure("Windows read-only package proof requires the inbox icacls.exe utility")
        try:
            Identity = subprocess.run(["whoami.exe", "/user", "/fo", "csv", "/nh"], text=True,
                                      capture_output=True, timeout=10, check=False)
        except (OSError, subprocess.SubprocessError) as Error:
            raise SmokeFailure(f"could not identify the scoped package test principal: {Error}") from Error
        if Identity.returncode != 0:
            raise SmokeFailure(f"whoami could not identify the package test principal: {Identity.stderr.strip()}")
        Rows = list(csv.reader(Identity.stdout.splitlines()))
        if not Rows or len(Rows[0]) < 2 or not Rows[0][-1].strip().startswith("S-"):
            raise SmokeFailure("whoami returned no usable security identifier for the package test principal")
        self.Sid = Rows[0][-1].strip()
        if self.Backup.exists() or self.Backup.is_symlink():
            raise SmokeFailure(f"refusing to overwrite an existing package ACL backup: {self.Backup}")
        self._run([self.Icacls, self.PackageRoot.name, "/save", str(self.Backup), "/t", "/c", "/q"],
                  self.WorkingDirectory)
        self.Saved = True
        self._run([self.Icacls, self.PackageRoot.name, "/deny", f"*{self.Sid}:{self.WriteDenial}",
                   "/t", "/c", "/q"], self.WorkingDirectory)
        Probe = self.PackageRoot / ".pf-read-only-write-probe"
        try:
            with Probe.open("xb"):
                pass
        except PermissionError:
            return
        except OSError as Error:
            raise SmokeFailure(f"Windows package write-denial probe failed unexpectedly: {Probe}: {Error}") from Error
        try:
            Probe.unlink()
        except OSError:
            pass
        raise SmokeFailure("Windows package DACL still permits creating files in the exported package")

    def restore(self) -> None:
        if not self.Saved:
            return
        if not self.Icacls or not self.Backup.is_file() or self.Backup.is_symlink():
            raise SmokeFailure(f"cannot safely restore the owned package DACL backup: {self.Backup}")
        self._run([self.Icacls, ".", "/restore", str(self.Backup), "/c", "/q"], self.WorkingDirectory)
        self.Saved = False
        try:
            self.Backup.unlink()
        except OSError as Error:
            raise SmokeFailure(f"could not remove the restored package ACL backup: {self.Backup}: {Error}") from Error


def _snapshot_package(PackageRoot: Path) -> dict[str, tuple[object, ...]]:
    _reject_links(PackageRoot)
    Snapshot: dict[str, tuple[object, ...]] = {}

    def AddEntry(PathValue: Path, RelativePath: str) -> None:
        Before = PathValue.stat(follow_symlinks=False)
        CommonMetadata = (Before.st_mode, Before.st_size, Before.st_mtime_ns, Before.st_ctime_ns,
                          Before.st_dev, Before.st_ino, Before.st_nlink)
        if stat.S_ISDIR(Before.st_mode):
            Snapshot[RelativePath] = ("directory",) + CommonMetadata
            return
        if not stat.S_ISREG(Before.st_mode):
            raise SmokeFailure(f"package contains a non-regular entry that cannot be verified: {PathValue}")

        Digest = hashlib.sha256()
        try:
            with PathValue.open("rb") as File:
                while Block := File.read(1024 * 1024):
                    Digest.update(Block)
        except OSError as Error:
            raise SmokeFailure(f"could not hash package file {PathValue}: {Error}") from Error
        After = PathValue.stat(follow_symlinks=False)
        AfterMetadata = (After.st_mode, After.st_size, After.st_mtime_ns, After.st_ctime_ns,
                         After.st_dev, After.st_ino, After.st_nlink)
        if CommonMetadata != AfterMetadata:
            raise SmokeFailure(f"package metadata changed while snapshotting: {PathValue}")
        Snapshot[RelativePath] = ("file",) + CommonMetadata + (Digest.hexdigest(),)

    AddEntry(PackageRoot, ".")
    for Current, DirectoryNames, FileNames in os.walk(PackageRoot, followlinks=False):
        CurrentPath = Path(Current)
        for Name in DirectoryNames:
            PathValue = CurrentPath / Name
            AddEntry(PathValue, PathValue.relative_to(PackageRoot).as_posix())
        for Name in FileNames:
            PathValue = CurrentPath / Name
            AddEntry(PathValue, PathValue.relative_to(PackageRoot).as_posix())
    return Snapshot


def _copy_owned_package(SourceRoot: Path, DestinationParent: Path) -> Path:
    _reject_links(SourceRoot)
    DestinationParent.mkdir()
    DestinationRoot = DestinationParent / SourceRoot.name
    try:
        shutil.copytree(SourceRoot, DestinationRoot, copy_function=shutil.copy2, symlinks=False)
    except (OSError, shutil.Error) as Error:
        if DestinationRoot.exists() and not DestinationRoot.is_symlink() and DestinationRoot.parent.resolve() == DestinationParent.resolve():
            shutil.rmtree(DestinationRoot)
        if DestinationParent.is_dir() and not any(DestinationParent.iterdir()):
            DestinationParent.rmdir()
        raise UnsupportedCapability(f"could not create an owned package copy for a read-only smoke run: {Error}") from Error
    return DestinationRoot.resolve(strict=True)


def _remove_owned_package_copy(PackageRoot: Path, TemporaryRoot: Path) -> None:
    if TemporaryRoot.is_symlink() or not TemporaryRoot.is_dir():
        raise SmokeFailure(f"owned package temporary root is missing or linked: {TemporaryRoot}")
    ResolvedTemporaryRoot = TemporaryRoot.resolve(strict=True)
    SystemTemporaryRoot = Path(tempfile.gettempdir()).resolve(strict=True)
    if ResolvedTemporaryRoot.parent != SystemTemporaryRoot:
        raise SmokeFailure(f"owned package temporary root is outside the system temporary directory: {TemporaryRoot}")
    if PackageRoot.is_symlink() or not PackageRoot.is_dir():
        raise SmokeFailure(f"owned package copy is missing or linked; refusing cleanup: {PackageRoot}")
    ResolvedPackageRoot = PackageRoot.resolve(strict=True)
    if ResolvedPackageRoot.parent.parent != ResolvedTemporaryRoot:
        raise SmokeFailure(f"owned package copy escaped its temporary root; refusing cleanup: {PackageRoot}")
    if {Entry.name for Entry in ResolvedTemporaryRoot.iterdir()} != {"Package"}:
        raise SmokeFailure(f"owned package temporary root contains unexpected entries: {ResolvedTemporaryRoot}")
    _reject_links(ResolvedPackageRoot)

    try:
        for Current, DirectoryNames, FileNames in os.walk(ResolvedPackageRoot, topdown=False, followlinks=False):
            CurrentPath = Path(Current)
            for Name in FileNames + DirectoryNames:
                PathValue = CurrentPath / Name
                Mode = PathValue.stat(follow_symlinks=False).st_mode
                PathValue.chmod(Mode | (stat.S_IWRITE if os.name == "nt" else stat.S_IWUSR))
            Mode = CurrentPath.stat(follow_symlinks=False).st_mode
            CurrentPath.chmod(Mode | (stat.S_IWRITE if os.name == "nt" else stat.S_IWUSR | stat.S_IXUSR))
        shutil.rmtree(ResolvedTemporaryRoot)
    except OSError as Error:
        raise SmokeFailure(f"could not remove the owned read-only package copy: {Error}") from Error


def _build_environment(Platform: str, WorkRoot: Path, SystemVulkanIcd: Path | None = None,
                      WindowsVulkanRuntimeLibrary: Path | None = None) -> dict[str, str]:
    UserData = WorkRoot / "User Data Ω" / "PulseForge"
    Home = WorkRoot / "Home Ω"
    Temp = WorkRoot / "Temp Ω"
    for Directory in (UserData, Home, Temp, Home / ".config", Home / ".local" / "share"):
        Directory.mkdir(parents=True, exist_ok=True)
    Environment = {
        "PULSEFORGE_USER_DATA_DIR": str(UserData),
        "HOME": str(Home),
        "TMPDIR": str(Temp),
        "TEMP": str(Temp),
        "TMP": str(Temp),
        "LANG": "C.UTF-8",
    }
    if Platform == "Linux":
        Environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        Environment["XDG_CONFIG_HOME"] = str(Home / ".config")
        Environment["XDG_DATA_HOME"] = str(Home / ".local" / "share")
        if SystemVulkanIcd is not None:
            if SystemVulkanIcd.is_symlink():
                raise SmokeFailure("--system-vulkan-icd must not be a symbolic link")
            Manifest = SystemVulkanIcd.resolve(strict=True)
            AllowedRoots = (Path("/usr/share/vulkan/icd.d").resolve(), Path("/etc/vulkan/icd.d").resolve())
            if not Manifest.is_file() or not any(
                    Manifest == Directory or Directory in Manifest.parents for Directory in AllowedRoots):
                raise SmokeFailure("--system-vulkan-icd must name a real manifest under /usr/share or /etc/vulkan/icd.d")
            if "lvp" not in Manifest.name.casefold():
                raise SmokeFailure("--system-vulkan-icd must select the system Lavapipe ICD")
            try:
                Document = json.loads(Manifest.read_text(encoding="utf-8"))
                Library = Path(Document["ICD"]["library_path"])
                if "lvp" not in Library.name.casefold():
                    raise ValueError("ICD library_path does not name Lavapipe")
                SystemLibraryRoots = tuple(Path(Value).resolve() for Value in
                                           ("/usr/lib", "/usr/lib64", "/lib", "/lib64"))
                if Library.is_absolute():
                    Library = Library.resolve(strict=True)
                    if not any(Library == Directory or Directory in Library.parents for Directory in SystemLibraryRoots):
                        raise ValueError("ICD library_path is outside system library directories")
                else:
                    if len(Library.parts) != 1 or not Library.name.casefold().startswith("libvulkan_lvp.so"):
                        raise ValueError("relative ICD library_path must name the system Lavapipe loader library")
                    if not ctypes.util.find_library("vulkan_lvp"):
                        raise ValueError("the system linker cannot resolve the Lavapipe library")
            except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as Error:
                raise SmokeFailure(f"system Vulkan ICD manifest is invalid or does not use a system library: {Error}") from Error
            Environment["VK_DRIVER_FILES"] = str(Manifest)
            Environment["VK_ICD_FILENAMES"] = str(Manifest)
        for Name in ("DISPLAY", "XAUTHORITY"):
            Value = os.environ.get(Name)
            if Value:
                Environment[Name] = Value
    elif Platform == "macOS":
        Environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        Environment["LC_ALL"] = "C.UTF-8"
        Environment["DYLD_PRINT_LIBRARIES"] = "1"
    else:
        SystemRoot = os.environ.get("SystemRoot", r"C:\Windows")
        WindowsHome = WorkRoot / "Windows User Ω"
        Roaming = WindowsHome / "AppData" / "Roaming"
        Local = WindowsHome / "AppData" / "Local"
        for Directory in (WindowsHome, Roaming, Local):
            Directory.mkdir(parents=True, exist_ok=True)
        SystemDirectory = Path(SystemRoot) / "System32"
        RuntimeDirectories = [str(SystemDirectory)]
        if WindowsVulkanRuntimeLibrary is not None:
            if WindowsVulkanRuntimeLibrary.is_symlink() or not WindowsVulkanRuntimeLibrary.is_file():
                raise SmokeFailure("--windows-vulkan-runtime-library must name an existing unlinked DLL")
            RuntimeLibrary = WindowsVulkanRuntimeLibrary.resolve(strict=True)
            if RuntimeLibrary.name.casefold() != "vulkan-1.dll":
                raise SmokeFailure("--windows-vulkan-runtime-library must be named vulkan-1.dll")
            RuntimeDirectories.insert(0, str(RuntimeLibrary.parent))
        Environment.update({
            "PATH": os.pathsep.join(RuntimeDirectories),
            "SystemRoot": SystemRoot,
            "WINDIR": SystemRoot,
            "SystemDrive": os.environ.get("SystemDrive", "C:"),
            "USERPROFILE": str(WindowsHome),
            "APPDATA": str(Roaming),
            "LOCALAPPDATA": str(Local),
        })
    return Environment


def _windows_cdb_exception_script() -> str:
    CaptureCommands = (
        ".echo G14_WINDOWS_CDB_SECOND_CHANCE_CAPTURE=BEGIN; "
        ".exr -1; .ecxr; .time; r; u @rip-10 @rip+20; lmv a @rip; kv; "
        ".echo G14_WINDOWS_CDB_SECOND_CHANCE_CAPTURE=END; q"
    )
    return (f'sxd -c2 "{CaptureCommands}" 0xc000001d\n'
            f'sxd -c2 "{CaptureCommands}" *\ng\n')


def _has_cdb_second_chance_capture(Text: str) -> bool:
    return re.search(
        r"(?m)^\s*(?:[0-9a-f]+:[0-9a-f]+>\s*)?"
        r"G14_WINDOWS_CDB_SECOND_CHANCE_CAPTURE=END\s*$",
        Text,
        re.IGNORECASE,
    ) is not None


def read_log_tail(LogPath: Path, MaxBytes: int = MAX_CDB_DIAGNOSTIC_TAIL_BYTES) -> str:
    """Read a bounded diagnostic tail while preserving UTF-8 and exposing other encoded bytes."""
    if MaxBytes < 1:
        raise ValueError("diagnostic log tail bound must be positive")

    with LogPath.open("rb") as Log:
        Log.seek(0, os.SEEK_END)
        LogSize = Log.tell()
        Start = max(0, LogSize - MaxBytes)
        Log.seek(Start)
        Data = Log.read(MaxBytes)

    Truncated = Start > 0
    if Truncated:
        FirstLineBreak = Data.find(b"\n")
        if FirstLineBreak >= 0:
            Data = Data[FirstLineBreak + 1:]

    if Data.startswith((b"\xff\xfe", b"\xfe\xff")):
        Text = Data.decode("utf-16", errors="backslashreplace")
    else:
        try:
            Text = Data.decode("utf-8-sig")
        except UnicodeDecodeError:
            if os.name == "nt":
                Text = None
                for Encoding in ("mbcs", "oem"):
                    try:
                        Text = Data.decode(Encoding)
                        break
                    except UnicodeDecodeError:
                        continue
                if Text is None:
                    Text = Data.decode("utf-8", errors="backslashreplace")
            else:
                Text = Data.decode("utf-8", errors="backslashreplace")

    if Truncated:
        return f"[earlier diagnostic bytes omitted; showing at most {MaxBytes} bytes]\n{Text}"
    return Text


def capture_windows_unhandled_exception(PackageRoot: Path, Cdb: Path, LogDirectory: Path,
                                       WindowsVulkanRuntimeLibrary: Path, Timeout: int) -> int:
    """Rerun an already-failing packaged Player under CDB and retain only bounded text diagnostics."""
    if os.name != "nt":
        raise SmokeFailure("Windows CDB crash capture must run on a Windows host")
    if not 1 <= Timeout <= 30:
        raise SmokeFailure("Windows CDB crash capture timeout must be between 1 and 30 seconds")

    PackageRoot, Executable = _resolve_package(PackageRoot, "Windows")
    if Cdb.is_symlink() or not Cdb.is_file():
        raise SmokeFailure(f"Windows CDB executable is missing or linked: {Cdb}")
    Cdb = Cdb.resolve(strict=True)
    if WindowsVulkanRuntimeLibrary.is_symlink() or not WindowsVulkanRuntimeLibrary.is_file():
        raise SmokeFailure(
            "--windows-vulkan-runtime-library must name an existing unlinked DLL")
    WindowsVulkanRuntimeLibrary = WindowsVulkanRuntimeLibrary.resolve(strict=True)
    if WindowsVulkanRuntimeLibrary.name.casefold() != "vulkan-1.dll":
        raise SmokeFailure("--windows-vulkan-runtime-library must be named vulkan-1.dll")

    LogDirectory.mkdir(parents=True, exist_ok=True)
    LogDirectory = LogDirectory.resolve(strict=True)
    if LogDirectory == PackageRoot or PackageRoot in LogDirectory.parents:
        raise SmokeFailure("CDB diagnostics must be outside the read-only package")
    RunRoot = Path(tempfile.mkdtemp(prefix="PulseForge Windows CDB Ω ", dir=LogDirectory))
    WorkRoot = RunRoot / "Runtime Data Ω"
    WorkingDirectory = RunRoot / "Unrelated Working Directory Ω"
    WorkRoot.mkdir()
    WorkingDirectory.mkdir()

    CommandFile = RunRoot / "cdb-exception-commands.txt"
    CommandFile.write_text(_windows_cdb_exception_script(), encoding="ascii", newline="\n")
    LogPath = LogDirectory / "cdb-live.txt"
    if LogPath.exists() or LogPath.is_symlink():
        raise SmokeFailure(f"CDB diagnostic log already exists or is linked: {LogPath}")
    Environment = _build_environment("Windows", WorkRoot, WindowsVulkanRuntimeLibrary=WindowsVulkanRuntimeLibrary)
    Command = [str(Cdb), "-cf", str(CommandFile), "-G", str(Executable)]
    CreationFlags = subprocess.CREATE_NEW_PROCESS_GROUP
    TimedOut = False
    Process: subprocess.Popen[bytes] | None = None
    print(f"STANDALONE_PACKAGE_CDB_PROBE=START; timeout_seconds={Timeout}; argv={Command!r}")
    with LogPath.open("xb") as Output:
        Process = subprocess.Popen(Command, cwd=WorkingDirectory, env=Environment,
                                   stdin=subprocess.DEVNULL, stdout=Output, stderr=subprocess.STDOUT,
                                   bufsize=0, creationflags=CreationFlags)
        print(f"STANDALONE_PACKAGE_CDB_PROBE=PROCESS; debugger_pid={Process.pid}")
        try:
            ReturnCode = Process.wait(timeout=Timeout)
        except subprocess.TimeoutExpired:
            TimedOut = True
            TaskKill = Path(Environment["SystemRoot"]) / "System32" / "taskkill.exe"
            CleanupFailure: str | None = None
            try:
                Cleanup = subprocess.run([str(TaskKill), "/PID", str(Process.pid), "/T", "/F"],
                                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, timeout=5, check=False)
                Output.write(b"\nSTANDALONE_PACKAGE_CDB_PROBE=CLEANUP; taskkill_exit=")
                Output.write(str(Cleanup.returncode).encode("ascii"))
                Output.write(b"\n")
                Output.write(Cleanup.stdout or b"")
                if Cleanup.returncode != 0:
                    CleanupFailure = f"taskkill exited {Cleanup.returncode} for owned debugger PID {Process.pid}"
            except (OSError, subprocess.SubprocessError) as Error:
                CleanupFailure = f"could not terminate owned debugger process tree {Process.pid}: {Error}"
                Output.write(f"\nSTANDALONE_PACKAGE_CDB_PROBE=CLEANUP_ERROR; {CleanupFailure}\n".encode("utf-8"))
            Output.flush()
            try:
                ReturnCode = Process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                Process.kill()
                try:
                    ReturnCode = Process.wait(timeout=5)
                except subprocess.TimeoutExpired as Error:
                    raise SmokeFailure(
                        f"could not reap owned CDB process PID {Process.pid} after bounded process-tree cleanup") from Error
            if CleanupFailure is not None:
                raise SmokeFailure(CleanupFailure)

    Tail = read_log_tail(LogPath)
    if _has_cdb_second_chance_capture(Tail):
        print(f"STANDALONE_PACKAGE_CDB_PROBE=CAPTURED; cdb_exit={ReturnCode}; log={LogPath}")
    elif TimedOut:
        print(f"STANDALONE_PACKAGE_CDB_PROBE=TIMEOUT; cdb_exit={ReturnCode}; log={LogPath}")
    else:
        print(f"STANDALONE_PACKAGE_CDB_PROBE=NO_UNHANDLED_EXCEPTION; cdb_exit={ReturnCode}; log={LogPath}")
    print(Tail)
    return 0


def _wait_for_exit(Process: subprocess.Popen[bytes], Timeout: float) -> int:
    Deadline = time.monotonic() + Timeout
    while time.monotonic() < Deadline:
        Status = Process.poll()
        if Status is not None:
            return Status
        time.sleep(0.1)
    raise SmokeFailure(f"Player did not exit within {Timeout:.1f}s after native close; PID {Process.pid} remains running")


def _terminate_failed_child(Process: subprocess.Popen[bytes]) -> None:
    if Process.poll() is not None:
        Process.wait()
        return

    print(f"STANDALONE_PACKAGE_SMOKE_FAILURE_CLEANUP=TERMINATE; pid={Process.pid}", file=sys.stderr)
    try:
        Process.terminate()
    except ProcessLookupError:
        pass
    except OSError as Error:
        print(f"STANDALONE_PACKAGE_SMOKE_FAILURE_CLEANUP=TERMINATE_ERROR; {Error}", file=sys.stderr)

    try:
        Process.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        pass

    print(f"STANDALONE_PACKAGE_SMOKE_FAILURE_CLEANUP=KILL; pid={Process.pid}; reason=terminate-timeout",
          file=sys.stderr)
    try:
        Process.kill()
    except ProcessLookupError:
        pass
    except OSError as Error:
        print(f"STANDALONE_PACKAGE_SMOKE_FAILURE_CLEANUP=KILL_ERROR; {Error}", file=sys.stderr)
    try:
        Process.wait(timeout=5)
    except subprocess.TimeoutExpired as Error:
        raise SmokeFailure(f"could not reap failed Player child PID {Process.pid} after bounded terminate and kill") from Error


def _route(Platform: str, PackageRoot: Path):
    if Platform == "Linux":
        if sys.platform != "linux":
            raise UnsupportedCapability("Linux X11 route must run on a Linux host")
        return LinuxX11Desktop()
    if Platform == "Windows":
        if os.name != "nt":
            raise UnsupportedCapability("Windows native-window route must run on Windows")
        return WindowsDesktop()
    if sys.platform != "darwin":
        raise UnsupportedCapability("macOS bundle route must run on macOS")
    return MacOSDesktop(PackageRoot)


def run_smoke(PackageRoot: Path, Platform: str, LogDirectory: Path, Timeout: int,
              SystemVulkanIcd: Path | None = None,
              WindowsVulkanRuntimeLibrary: Path | None = None) -> None:
    if Timeout < 15 or Timeout > 600:
        raise SmokeFailure("timeout-seconds must be between 15 and 600")
    if SystemVulkanIcd is not None and Platform != "Linux":
        raise SmokeFailure("--system-vulkan-icd is only valid for Linux package runs")
    if WindowsVulkanRuntimeLibrary is not None and Platform != "Windows":
        raise SmokeFailure("--windows-vulkan-runtime-library is only valid for Windows package runs")
    if ((Platform == "Windows" and os.name != "nt") or (Platform == "Linux" and sys.platform != "linux") or
            (Platform == "macOS" and sys.platform != "darwin")):
        raise UnsupportedCapability(f"the {Platform} desktop capture route is unavailable on this host")
    PackageSourceRoot, _ = _resolve_package(PackageRoot, Platform)
    if WindowsVulkanRuntimeLibrary is not None:
        if WindowsVulkanRuntimeLibrary.is_symlink() or not WindowsVulkanRuntimeLibrary.is_file():
            raise SmokeFailure("--windows-vulkan-runtime-library must name an existing unlinked DLL")
        ResolvedRuntimeLibrary = WindowsVulkanRuntimeLibrary.resolve(strict=True)
        if ResolvedRuntimeLibrary.name.casefold() != "vulkan-1.dll":
            raise SmokeFailure("--windows-vulkan-runtime-library must be named vulkan-1.dll")
        if ResolvedRuntimeLibrary.parent == PackageSourceRoot or PackageSourceRoot in ResolvedRuntimeLibrary.parents:
            raise SmokeFailure("the test-only Vulkan runtime DLL must be outside the exported package")
        WindowsVulkanRuntimeLibrary = ResolvedRuntimeLibrary
    LogDirectory.mkdir(parents=True, exist_ok=True)
    if LogDirectory.is_symlink():
        raise SmokeFailure(f"log directory must not be a symbolic link: {LogDirectory}")
    LogDirectory = LogDirectory.resolve(strict=True)
    if LogDirectory == PackageSourceRoot or PackageSourceRoot in LogDirectory.parents:
        raise SmokeFailure("log directory must be outside the read-only package")

    RunDirectory = Path(tempfile.mkdtemp(prefix="PulseForge Standalone Smoke Ω ", dir=LogDirectory))
    WorkRoot = RunDirectory / "Isolated Runtime Data Ω"
    WorkRoot.mkdir()
    WorkingDirectory = RunDirectory / "Unrelated Working Directory Ω"
    WorkingDirectory.mkdir()
    Environment = _build_environment(Platform, WorkRoot, SystemVulkanIcd, WindowsVulkanRuntimeLibrary)
    LogPath = RunDirectory / "standalone-player.log"
    ScreenshotPath = RunDirectory / "standalone-player-window.png"
    if LogPath.exists() or ScreenshotPath.exists():
        raise SmokeFailure(f"run output already exists under {RunDirectory}")

    OwnedPackageTemporaryRoot = Path(tempfile.mkdtemp(prefix="PulseForge Read Only Package Ω "))
    OwnedPackageRoot: Path | None = None
    try:
        OwnedPackageRoot = _copy_owned_package(PackageSourceRoot, OwnedPackageTemporaryRoot / "Package")
        PackageRoot, Executable = _resolve_package(OwnedPackageRoot, Platform)
        NoticePath = validate_third_party_notices(PackageRoot, Platform)
        print(f"STANDALONE_PACKAGE_NOTICES=PASS; path={NoticePath}")
    except Exception:
        if OwnedPackageRoot is not None:
            try:
                _remove_owned_package_copy(OwnedPackageRoot, OwnedPackageTemporaryRoot)
            except SmokeFailure as Error:
                print(f"STANDALONE_PACKAGE_SMOKE_PACKAGE_CLEANUP=FAIL; {Error}; retained={OwnedPackageRoot}",
                      file=sys.stderr)
        elif OwnedPackageTemporaryRoot.is_dir():
            try:
                OwnedPackageTemporaryRoot.rmdir()
            except OSError:
                pass
        raise
    if PackageRoot == WorkingDirectory or PackageRoot in WorkingDirectory.parents or WorkingDirectory in PackageRoot.parents:
        raise SmokeFailure("could not establish a working directory unrelated to the package")

    WindowsReadOnly = WindowsPackageReadOnlyAccess(PackageRoot, OwnedPackageTemporaryRoot) if os.name == "nt" else None
    Command = build_launch_command(Executable)
    if len(Command) != 1:
        raise SmokeFailure("internal launch command must contain only the packaged executable")
    Route = None
    try:
        Route = _route(Platform, PackageRoot)
        ChangedEntries = _make_read_only(PackageRoot)
        if WindowsReadOnly is not None:
            WindowsReadOnly.apply()
        PackageSnapshot = _snapshot_package(PackageRoot)
    except Exception:
        if Route is not None and hasattr(Route, "close"):
            Route.close()
        try:
            if WindowsReadOnly is not None:
                WindowsReadOnly.restore()
            _remove_owned_package_copy(PackageRoot, OwnedPackageTemporaryRoot)
        except SmokeFailure as Error:
            print(f"STANDALONE_PACKAGE_SMOKE_PACKAGE_CLEANUP=FAIL; {Error}; retained={PackageRoot}",
                  file=sys.stderr)
        raise
    Process: subprocess.Popen[bytes] | None = None
    Collector: OutputCollector | None = None
    TargetWindow: Window | None = None
    CloseSent = False
    CloseAttempted = False
    RouteClosed = False
    PackageSnapshotVerified = False
    PackageCopyRemoved = False
    try:
        CreationFlags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        try:
            Process = subprocess.Popen(Command, cwd=WorkingDirectory, env=Environment,
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, bufsize=0, creationflags=CreationFlags)
        except OSError as Error:
            raise UnsupportedCapability(f"could not start the staged Player in the host desktop session: {Error}") from Error
        assert Process.stdout is not None
        Collector = OutputCollector(Process.stdout, LogPath)
        print(f"STANDALONE_PACKAGE_SMOKE_PROCESS=STARTED; pid={Process.pid}; argv_count=0")
        print(f"STANDALONE_PACKAGE_SMOKE_ENV=SANITIZED; isolated_user_data={Environment['PULSEFORGE_USER_DATA_DIR']}; "
              f"developer_sdk_overrides=omitted")
        if SystemVulkanIcd is not None:
            print(f"STANDALONE_PACKAGE_SMOKE_SYSTEM_LAVAPIPE={Environment['VK_DRIVER_FILES']}")
        if WindowsVulkanRuntimeLibrary is not None:
            RuntimeDigest = hashlib.sha256(WindowsVulkanRuntimeLibrary.read_bytes()).hexdigest()
            print("STANDALONE_PACKAGE_SMOKE_WINDOWS_VULKAN_RUNTIME=TEST_PREREQUISITE; "
                  f"path={WindowsVulkanRuntimeLibrary}; sha256={RuntimeDigest}; package_copy=external")
        DrawCount = Collector.wait_for_presented_geometry(Process, time.monotonic() + Timeout)
        print(f"STANDALONE_PACKAGE_SMOKE_RENDER_LOG=PASS; indexed_mesh_instances={DrawCount}")
        print("STANDALONE_PACKAGE_SMOKE_PRESENTATION=PASS; geometry_frame=successfully_presented")
        if Platform == "macOS":
            VulkanPath, MoltenVKPath = Collector.require_macos_library_provenance(PackageRoot)
            print("STANDALONE_PACKAGE_SMOKE_MACOS_LIBRARY_PROVENANCE=PASS; "
                  f"libvulkan={VulkanPath}; libMoltenVK={MoltenVKPath}")
        TargetWindow = Route.find_window(Process.pid, time.monotonic() + min(15, Timeout))
        print(f"STANDALONE_PACKAGE_SMOKE_WINDOW=FOUND; handle={TargetWindow.Handle}; size={TargetWindow.Width}x{TargetWindow.Height}")
        Route.capture(TargetWindow, ScreenshotPath)
        try:
            Width, Height, DistinctColors, LuminanceRange = check_nonuniform_png(ScreenshotPath)
        except SmokeFailure as Error:
            raise SmokeFailure(f"{Error}; screenshot={ScreenshotPath}; player_log={LogPath}") from Error
        print(f"STANDALONE_PACKAGE_SMOKE_NONUNIFORM_WINDOW_CAPTURE=PASS; capture={Width}x{Height}; "
              f"distinct_colors={DistinctColors}; luminance_range={LuminanceRange}; path={ScreenshotPath}")
        CloseAttempted = True
        try:
            if Platform == "macOS":
                Route.close_window(TargetWindow, Process.pid, min(30.0, float(Timeout)))
            else:
                Route.close_window(TargetWindow)
        except UnsupportedCapability as Error:
            raise SmokeFailure(
                f"native close request did not complete; player_exit_status={Process.poll()}; "
                f"screenshot={ScreenshotPath}; player_log={LogPath}; detail={Error}") from Error
        CloseSent = True
        print("STANDALONE_PACKAGE_SMOKE_CLOSE=REQUESTED; route=native-window-close")
        Status = _wait_for_exit(Process, min(30, Timeout))
        RouteClosed = True
        if Status != 0:
            raise SmokeFailure(f"Player exited with status {Status} after native window close; see {LogPath}")
        if _snapshot_package(PackageRoot) != PackageSnapshot:
            raise SmokeFailure(f"owned read-only package bytes or metadata changed during the Player run: {PackageRoot}")
        PackageSnapshotVerified = True
        Configuration = _read_configuration(PackageRoot, Platform)
        if WindowsReadOnly is not None:
            WindowsReadOnly.restore()
        _remove_owned_package_copy(PackageRoot, OwnedPackageTemporaryRoot)
        PackageCopyRemoved = True
        print(f"STANDALONE_PACKAGE_SMOKE=PASS; platform={Platform}; configuration={Configuration}; "
              f"package_read_only_entries={ChangedEntries}; package_bytes_metadata=unchanged; zero_argv=true; "
              f"exit_code=0; screenshot={ScreenshotPath}; log={LogPath}")
    finally:
        ActiveFailure = sys.exc_info()[0] is not None
        if Process and Process.poll() is None and not RouteClosed:
            if TargetWindow is None:
                try:
                    TargetWindow = Route.find_window(Process.pid, time.monotonic() + 2)
                except (SmokeFailure, OSError, subprocess.SubprocessError):
                    TargetWindow = None
            if TargetWindow is not None and not CloseAttempted:
                CloseAttempted = True
                try:
                    if Platform == "macOS":
                        Route.close_window(TargetWindow, Process.pid, min(30.0, float(Timeout)))
                    else:
                        Route.close_window(TargetWindow)
                    CloseSent = True
                    try:
                        _wait_for_exit(Process, min(30, Timeout))
                        RouteClosed = True
                    except SmokeFailure:
                        pass
                except (SmokeFailure, OSError, subprocess.SubprocessError):
                    pass
            if not RouteClosed:
                try:
                    _terminate_failed_child(Process)
                except SmokeFailure as Error:
                    print(f"STANDALONE_PACKAGE_SMOKE_FAILURE_CLEANUP=FAIL; {Error}; evidence={RunDirectory}",
                          file=sys.stderr)
                    if not ActiveFailure:
                        raise
        if PackageSnapshot is not None and not PackageSnapshotVerified and (not Process or Process.poll() is not None):
            try:
                if _snapshot_package(PackageRoot) != PackageSnapshot:
                    raise SmokeFailure(f"owned read-only package bytes or metadata changed during the Player run: {PackageRoot}")
                PackageSnapshotVerified = True
                print("STANDALONE_PACKAGE_SMOKE_PACKAGE_INTEGRITY=PASS; bytes_metadata=unchanged")
            except SmokeFailure as Error:
                print(f"STANDALONE_PACKAGE_SMOKE_PACKAGE_INTEGRITY=FAIL; {Error}", file=sys.stderr)
                if not ActiveFailure:
                    raise
        if not PackageCopyRemoved and (not Process or Process.poll() is not None):
            try:
                if WindowsReadOnly is not None:
                    WindowsReadOnly.restore()
                _remove_owned_package_copy(PackageRoot, OwnedPackageTemporaryRoot)
                PackageCopyRemoved = True
            except SmokeFailure as Error:
                print(f"STANDALONE_PACKAGE_SMOKE_PACKAGE_CLEANUP=FAIL; {Error}; retained={PackageRoot}",
                      file=sys.stderr)
        if Route is not None and hasattr(Route, "close"):
            Route.close()
        if Process and Process.poll() is not None:
            Collector.Reader.join(timeout=1) if Collector else None
            if Process.stdout:
                Process.stdout.close()


def _read_configuration(PackageRoot: Path, Platform: str) -> str:
    ResourceRoot = PackageRoot / "Contents" / "Resources" if Platform == "macOS" else PackageRoot
    Manifest = json.loads((ResourceRoot / "Content" / "manifest.pfmanifest").read_text(encoding="utf-8"))
    return str(Manifest["configuration"])


def _self_test_windows_read_only_execution() -> None:
    if os.name != "nt":
        return

    SystemRoot = os.environ.get("SystemRoot")
    SystemExecutable = Path(SystemRoot) / "System32" / "whoami.exe" if SystemRoot else None
    if SystemExecutable is None or not SystemExecutable.is_file() or SystemExecutable.is_symlink():
        raise AssertionError("Windows read-only ACL regression requires the inbox System32/whoami.exe")

    with tempfile.TemporaryDirectory(prefix="PulseForge Read-Only ACL Ω ") as Temporary:
        TemporaryRoot = Path(Temporary)
        PackageParent = TemporaryRoot / "Package"
        PackageParent.mkdir()
        PackageRoot = PackageParent / "Runtime Fixture Ω"
        PackageRoot.mkdir()
        PackagedExecutable = PackageRoot / "whoami.exe"
        shutil.copy2(SystemExecutable, PackagedExecutable)
        _make_read_only(PackageRoot)
        Access = WindowsPackageReadOnlyAccess(PackageRoot, TemporaryRoot)
        try:
            Access.apply()
            Result = subprocess.run([str(PackagedExecutable), "/user"], cwd=TemporaryRoot,
                                    text=True, capture_output=True, timeout=15, check=False)
            if Result.returncode != 0:
                raise AssertionError(f"read-only packaged executable exited {Result.returncode}: {Result.stderr.strip()}")
        finally:
            Access.restore()
            if PackageRoot.exists():
                try:
                    Mode = PackagedExecutable.stat(follow_symlinks=False).st_mode
                    PackagedExecutable.chmod(Mode | stat.S_IWRITE)
                except OSError as Error:
                    raise AssertionError(
                        f"saved package ACL was not restored for owned fixture cleanup: {Error}") from Error
                _remove_owned_package_copy(PackageRoot, TemporaryRoot)

    print("STANDALONE_PACKAGE_SMOKE_WINDOWS_READ_ONLY_EXECUTION=PASS; write denied; packaged process executed")


def self_test() -> None:
    stdout_bytes = BytesIO()
    stderr_bytes = BytesIO()
    stdout = TextIOWrapper(stdout_bytes, encoding="cp1252")
    stderr = TextIOWrapper(stderr_bytes, encoding="cp1252")
    with redirect_stdout(stdout), redirect_stderr(stderr):
        configure_utf8_diagnostics()
        assert stdout.encoding.casefold().replace("-", "") == "utf8"
        assert stderr.encoding.casefold().replace("-", "") == "utf8"
        print("STANDALONE_TEST_PATH=C:/Runner Temp/PulseForge Ω")
        print("STANDALONE_TEST_ERROR=C:/Runner Temp/PulseForge Ω", file=sys.stderr)
        stdout.flush()
        stderr.flush()
    assert "Ω".encode("utf-8") in stdout_bytes.getvalue()
    assert "Ω".encode("utf-8") in stderr_bytes.getvalue()
    CdbScript = _windows_cdb_exception_script()
    assert 'sxd -c2 ' in CdbScript and CdbScript.rstrip().endswith("g")
    assert "G14_WINDOWS_CDB_SECOND_CHANCE_CAPTURE=BEGIN" in CdbScript
    assert "G14_WINDOWS_CDB_SECOND_CHANCE_CAPTURE=END" in CdbScript
    assert ".exr -1; .ecxr; .time; r; u @rip-10 @rip+20; lmv a @rip; kv;" in CdbScript
    assert ".echo G14_WINDOWS_CDB_SECOND_CHANCE_CAPTURE=END; q" in CdbScript
    assert CdbScript.count("sxd -c2 ") == 2 and "0xc000001d" in CdbScript
    assert not _has_cdb_second_chance_capture(
        '0:000> sxd -c2 ".echo G14_WINDOWS_CDB_SECOND_CHANCE_CAPTURE=END; q" 0xc000001d')
    assert _has_cdb_second_chance_capture("G14_WINDOWS_CDB_SECOND_CHANCE_CAPTURE=END\n")
    assert _has_cdb_second_chance_capture("0:000> G14_WINDOWS_CDB_SECOND_CHANCE_CAPTURE=END\n")

    with tempfile.TemporaryDirectory(prefix="PulseForge verifier failure Ω ") as Temporary:
        Root = Path(Temporary)
        CdbLog = Root / "CDB diagnostics Ω.log"
        CdbLog.write_bytes(("obsolete Ω record\n" + "retained Ω diagnostic\n" * 8).encode("utf-8"))
        CdbTail = read_log_tail(CdbLog, MaxBytes=64)
        assert "retained Ω diagnostic" in CdbTail and "obsolete" not in CdbTail
        assert "earlier diagnostic bytes omitted" in CdbTail

        IncompletePackage = Root / "Incomplete Package Ω"
        IncompletePackage.mkdir()
        if os.name == "nt":
            HostPlatform = "Windows"
        elif sys.platform == "darwin":
            HostPlatform = "macOS"
        elif sys.platform == "linux":
            HostPlatform = "Linux"
        else:
            raise AssertionError(f"unsupported acceptance self-test host: {sys.platform}")
        ChildEnvironment = os.environ.copy()
        ChildEnvironment.pop("PYTHONIOENCODING", None)
        Child = subprocess.run(
            [sys.executable, "-X", "utf8=0", str(Path(__file__).resolve()),
             "--package-root", str(IncompletePackage), "--platform", HostPlatform,
             "--log-directory", str(Root / "Logs")],
            cwd=Root,
            env=ChildEnvironment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=15,
            check=False,
        )
        ChildOutput = Child.stdout.decode("utf-8")
        assert Child.returncode == 1, f"acceptance failure status was not preserved: {Child.returncode}; {ChildOutput}"
        assert "STANDALONE_PACKAGE_SMOKE=FAIL" in ChildOutput
        assert "Incomplete Package Ω" in ChildOutput, f"Unicode package path was not retained: {ChildOutput!r}"
        ExpectedFailure = {
            "Windows": "PulseForgeCookedGame.exe",
            "Linux": "PulseForgeCookedGame",
            "macOS": "must be the exported .app bundle",
        }[HostPlatform]
        assert ExpectedFailure in ChildOutput, f"unexpected incomplete-package diagnostic: {ChildOutput!r}"
    assert build_launch_command(PurePosixPath("/tmp/PulseForgeCookedGame")) == ["/tmp/PulseForgeCookedGame"]
    assert parse_draw_count("Submitted 3 indexed mesh instance(s) from the player scene") == 3
    assert parse_draw_count("Submitted 0 indexed mesh instance(s) from the player scene") is None
    assert parse_draw_count("some unrelated line") is None
    assert not has_presented_geometry("Submitted 3 indexed mesh instance(s) from the player scene")
    assert has_presented_geometry(
        "First Vulkan/NVRHI frame submitted and presented successfully (geometry draw recorded: true)")
    assert not has_presented_geometry(
        "First Vulkan/NVRHI frame submitted and presented successfully (geometry draw recorded: false)")
    Pixels = bytes((0, 0, 0, 255, 255, 0, 0, 255,
                    0, 255, 0, 255, 0, 0, 255, 255))
    Width, Height, Decoded = decode_png(encode_png(2, 2, Pixels))
    assert (Width, Height, Decoded) == (2, 2, bytearray(Pixels))
    with tempfile.TemporaryDirectory(prefix="PulseForge Vulkan Runtime Fixture ") as Temporary:
        Root = Path(Temporary)
        RuntimeDirectory = Root / "Runtime"
        RuntimeDirectory.mkdir()
        RuntimeLibrary = RuntimeDirectory / "vulkan-1.dll"
        RuntimeLibrary.write_bytes(b"test-only Vulkan runtime")
        Environment = _build_environment("Windows", Root / "Work",
                                         WindowsVulkanRuntimeLibrary=RuntimeLibrary)
        assert Environment["PATH"].split(os.pathsep)[0] == str(RuntimeDirectory.resolve())
        assert "VULKAN_SDK" not in Environment and "VK_SDK_PATH" not in Environment
        assert "VK_ICD_FILENAMES" not in Environment and "VK_DRIVER_FILES" not in Environment
        WrongNameRuntimeLibrary = RuntimeDirectory / "vk_swiftshader.dll"
        WrongNameRuntimeLibrary.write_bytes(b"test-only Vulkan runtime")
        try:
            _build_environment("Windows", Root / "InvalidWork",
                               WindowsVulkanRuntimeLibrary=WrongNameRuntimeLibrary)
        except SmokeFailure as Error:
            assert "named vulkan-1.dll" in str(Error)
        else:
            raise AssertionError("Windows test Vulkan runtime accepted an unrenamed ICD module")
    MacRoute = MacOSDesktop.__new__(MacOSDesktop)
    MacScripts: list[str] = []
    def FakeOsaScript(Script: str, Timeout: float = 5.0, Operation: str = "unspecified") -> str:
        MacScripts.append(Script)
        return "MACOS_NATIVE_WINDOW_CLOSE_REQUESTED; pid=1234"

    MacRoute._osascript = FakeOsaScript
    assert "CGWindowListCopyWindowInfo" in MacRoute.WINDOW_INFO_C
    assert "kCGWindowOwnerPID" in MacRoute.WINDOW_INFO_C
    assert "kCGWindowBounds" in MacRoute.WINDOW_INFO_C
    assert "CGRectMakeWithDictionaryRepresentation" in MacRoute.WINDOW_INFO_C
    with tempfile.TemporaryDirectory(prefix="PulseForge Notice Fixture ") as Temporary:
        Root = Path(Temporary)
        CapturePath = Root / "capture.png"

        def FakeMacProcess(Arguments, **Options):
            if Arguments[0] == "/usr/bin/clang":
                assert "-framework" in Arguments and "CoreGraphics" in Arguments and "CoreFoundation" in Arguments
                SourcePath = Path(Arguments[Arguments.index("-framework") - 1])
                assert SourcePath.read_text(encoding="utf-8") == MacRoute.WINDOW_INFO_C
                ExecutablePath = Path(Arguments[Arguments.index("-o") + 1])
                ExecutablePath.touch()
                return subprocess.CompletedProcess(Arguments, 0, "", "")
            if Path(Arguments[0]).name == "window_info":
                assert Arguments[1:] == ["1234"]
                return subprocess.CompletedProcess(
                    Arguments, 0,
                    json.dumps([{
                        "window_id": 9876,
                        "owner_pid": 1234,
                        "x": 20.0,
                        "y": 40.0,
                        "width": 800.0,
                        "height": 600.0,
                    }]), "")
            if Arguments[0] == "/usr/sbin/screencapture":
                assert Arguments[1:4] == ["-x", "-l", "9876"]
                CapturePath.write_bytes(b"captured")
                return subprocess.CompletedProcess(Arguments, 0, "", "")
            assert Arguments[0] == "osascript"
            MacScripts.append(Arguments[-1])
            return subprocess.CompletedProcess(Arguments, 0, "MACOS_NATIVE_WINDOW_CLOSE_REQUESTED; pid=1234", "")

        with patch("shutil.which", return_value="/usr/bin/clang"), patch("subprocess.run", side_effect=FakeMacProcess):
            MacWindow = MacRoute.find_window(1234, time.monotonic() + 1)
            assert MacWindow == Window(9876, 800, 600, 20, 40)
            MacRoute.capture(MacWindow, CapturePath)
            MacRoute.close_window(MacWindow, 1234)
        CloseScript = MacScripts[-1]
        assert "set targetProcess to first process whose unix id is 1234" in CloseScript
        assert "unix id is 1234" in CloseScript and "AXCloseButton" in CloseScript
        assert "STANDALONE_NATIVE_CLOSE_TARGET_READY; pid=1234" in CloseScript
        assert "id of targetWindow" not in CloseScript

        (Root / "ThirdPartyNotices.txt").write_text("MIT runtime notices", encoding="utf-8")
        validate_third_party_notices(Root, "Linux")
        MacNoticePath = Root / "Contents" / "Resources" / "ThirdPartyNotices.txt"
        MacNoticePath.parent.mkdir(parents=True)
        MacNoticePath.write_text("MIT runtime notices", encoding="utf-8")
        try:
            validate_third_party_notices(Root, "macOS")
        except SmokeFailure as Error:
            assert "Apache 2.0" in str(Error)
        else:
            raise AssertionError("macOS notices accepted without the bundled Vulkan license")
        MacNotices = "\n".join((
            "Apache License", "Khronos Vulkan Loader 1.4.350.0",
            "Some source files use more permissive licenses",
            "https://github.com/KhronosGroup/Vulkan-Loader/blob/vulkan-sdk-1.4.350.0/LICENSE.txt",
            "MoltenVK 1.4.1", "https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/LICENSE",
        ))
        MacNoticePath.write_text(MacNotices, encoding="utf-8")
        validate_third_party_notices(Root, "macOS")

    _self_test_windows_read_only_execution()

    try:
        with patch("subprocess.run", side_effect=subprocess.TimeoutExpired(
                ["osascript"], 5, output=b"close-target-ready", stderr=b"waiting-for-AX")):
            MacOSDesktop._osascript("return \"probe\"", Timeout=5.0, Operation="window-geometry-query")
    except UnsupportedCapability as Error:
        assert "operation=window-geometry-query" in str(Error) and "timeout=5.0s" in str(Error)
        assert "close-target-ready" in str(Error) and "waiting-for-AX" in str(Error)
    else:
        raise AssertionError("macOS Accessibility timeout discarded its partial operation diagnostics")
    print("STANDALONE_PACKAGE_SMOKE_SELF_TEST=PASS; argv, PNG codec, Vulkan presentation marker, macOS window route, "
          "UTF-8 diagnostics, and platform-specific notices")


def main() -> int:
    configure_utf8_diagnostics()
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--package-root", type=Path)
    Parser.add_argument("--platform", choices=("Windows", "Linux", "macOS"))
    Parser.add_argument("--log-directory", type=Path)
    Parser.add_argument("--timeout-seconds", type=int, default=180)
    Parser.add_argument("--system-vulkan-icd", type=Path,
                        help="Optional system Lavapipe ICD manifest; must live under /usr/share or /etc")
    Parser.add_argument("--windows-vulkan-runtime-library", type=Path,
                        help="Optional external test-only vulkan-1.dll; only its directory is added to the sanitized Windows PATH")
    Parser.add_argument("--capture-windows-unhandled-exception", action="store_true",
                        help="Rerun a failed Windows packaged Player under CDB and record its second-chance context")
    Parser.add_argument("--cdb", type=Path, help="Microsoft CDB executable for the bounded Windows crash diagnostic")
    Parser.add_argument("--debugger-timeout-seconds", type=int, default=20,
                        help="Bound the diagnostic CDB process (1 to 30 seconds)")
    Parser.add_argument("--self-test", action="store_true", help="Run the local argv/PNG helper checks")
    Arguments = Parser.parse_args()
    if Arguments.self_test:
        self_test()
        return 0
    Missing = [Name for Name in ("package_root", "platform", "log_directory") if getattr(Arguments, Name) is None]
    if Missing:
        Parser.error("--package-root, --platform, and --log-directory are required")
    if Arguments.capture_windows_unhandled_exception:
        if Arguments.platform != "Windows":
            Parser.error("--capture-windows-unhandled-exception requires --platform Windows")
        if Arguments.cdb is None or Arguments.windows_vulkan_runtime_library is None:
            Parser.error("CDB capture requires --cdb and --windows-vulkan-runtime-library")
        try:
            return capture_windows_unhandled_exception(
                Arguments.package_root, Arguments.cdb, Arguments.log_directory,
                Arguments.windows_vulkan_runtime_library, Arguments.debugger_timeout_seconds)
        except (OSError, SmokeFailure, subprocess.SubprocessError, TimeoutError, ValueError) as Error:
            print(f"STANDALONE_PACKAGE_CDB_PROBE=FAIL; {Error}", file=sys.stderr)
            return 1
    try:
        run_smoke(Arguments.package_root, Arguments.platform, Arguments.log_directory,
                  Arguments.timeout_seconds, Arguments.system_vulkan_icd,
                  Arguments.windows_vulkan_runtime_library)
        return 0
    except UnsupportedCapability as Error:
        print(f"STANDALONE_PACKAGE_SMOKE=UNSUPPORTED; {Error}", file=sys.stderr)
        return 3
    except (OSError, SmokeFailure, subprocess.SubprocessError, TimeoutError, ValueError) as Error:
        print(f"STANDALONE_PACKAGE_SMOKE=FAIL; {Error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
