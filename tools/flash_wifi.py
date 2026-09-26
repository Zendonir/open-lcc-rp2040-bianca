#!/usr/bin/env python3
"""
Flash the RP2040 firmware over Wi-Fi, through the ESP32-S3 stream server.

The RP2040 has to be in the serial bootloader. Press "Reboot RP2040 to Serial Boot"
in Home Assistant (or the ESPHome dashboard), then run this script within 30 seconds.

Examples:
    # Newest GitHub release
    python3 flash_wifi.py 192.168.1.10

    # Local build
    python3 flash_wifi.py 192.168.1.10 --file build/smart_lcc_app.bin

Only uses the Python standard library (Python 3.8+).
"""

import argparse
import json
import socket
import struct
import sys
import time
import urllib.request
import zlib

DEFAULT_REPO = "Zendonir/open-lcc-rp2040-bianca"
DEFAULT_PORT = 6638
APP_ADDRESS = 0x10008000
ASSET_NAME = "smart_lcc_app.bin"


def opcode(text):
    return struct.unpack("<I", text.encode("ascii"))[0]


CMD_SYNC = opcode("SYNC")
CMD_INFO = opcode("INFO")
CMD_ERASE = opcode("ERAS")
CMD_WRITE = opcode("WRIT")
CMD_SEAL = opcode("SEAL")
CMD_GO = opcode("GOGO")

RSP_SYNC = opcode("PICO")
RSP_OK = opcode("OKOK")
RSP_ERR = opcode("ERR!")


class FlashError(Exception):
    pass


def crc32(data):
    # Same CRC as the bootloader (IEEE 802.3)
    return zlib.crc32(data) & 0xFFFFFFFF


def download_release(repo, tag):
    if tag == "latest":
        url = f"https://api.github.com/repos/{repo}/releases/latest"
    else:
        url = f"https://api.github.com/repos/{repo}/releases/tags/{tag}"

    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(request, timeout=30) as response:
        release = json.load(response)

    for asset in release.get("assets", []):
        if asset["name"] == ASSET_NAME:
            print(f"Downloading {ASSET_NAME} from release {release['tag_name']}")
            with urllib.request.urlopen(asset["browser_download_url"], timeout=60) as response:
                return response.read()

    raise FlashError(f"Release {release.get('tag_name')} has no {ASSET_NAME}")


class Bootloader:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)

    def close(self):
        self.sock.close()

    def _recv(self, length):
        data = b""
        while len(data) < length:
            chunk = self.sock.recv(length - len(data))
            if not chunk:
                raise FlashError("Connection closed by ESP")
            data += chunk
        return data

    def _drain(self):
        self.sock.settimeout(0.2)
        try:
            while self.sock.recv(1024):
                pass
        except socket.timeout:
            pass
        finally:
            self.sock.settimeout(10)

    def sync(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._drain()
            self.sock.sendall(struct.pack("<I", CMD_SYNC))
            self.sock.settimeout(1)
            try:
                if struct.unpack("<I", self._recv(4))[0] == RSP_SYNC:
                    self.sock.settimeout(10)
                    return
            except socket.timeout:
                pass
            finally:
                self.sock.settimeout(10)
        raise FlashError("No answer from the bootloader. Is the RP2040 in serial boot mode?")

    def command(self, cmd, args=(), data=b"", resp_nargs=0):
        self.sock.sendall(struct.pack("<I", cmd) + struct.pack(f"<{len(args)}I", *args) + data)
        status = struct.unpack("<I", self._recv(4))[0]
        if status != RSP_OK:
            raise FlashError(f"Bootloader returned an error (status 0x{status:08x})")
        if resp_nargs:
            return struct.unpack(f"<{resp_nargs}I", self._recv(4 * resp_nargs))
        return ()


def flash(host, port, image, sync_timeout):
    bl = Bootloader(host, port)
    try:
        print(f"Connecting to bootloader via {host}:{port} ...")
        bl.sync(sync_timeout)

        write_min, flash_size, erase_size, write_size, max_data = bl.command(CMD_INFO, resp_nargs=5)
        if APP_ADDRESS < write_min:
            raise FlashError(f"App address 0x{APP_ADDRESS:08x} is below the writable area 0x{write_min:08x}")

        # Pad to the flash page size with 0xff (erased flash)
        if len(image) % write_size:
            image += b"\xff" * (write_size - len(image) % write_size)

        if APP_ADDRESS + len(image) >= write_min + flash_size:
            raise FlashError("Image does not fit into flash")

        erase_len = -(-len(image) // erase_size) * erase_size
        print(f"Erasing {erase_len // 1024} KiB ...")
        bl.command(CMD_ERASE, (APP_ADDRESS, erase_len))

        chunk_size = max_data - max_data % write_size
        for offset in range(0, len(image), chunk_size):
            chunk = image[offset:offset + chunk_size]
            (remote_crc,) = bl.command(CMD_WRITE, (APP_ADDRESS + offset, len(chunk)), chunk, resp_nargs=1)
            if remote_crc != crc32(chunk):
                raise FlashError(f"CRC mismatch at offset 0x{offset:x}")
            done = offset + len(chunk)
            print(f"\rWriting {done * 100 // len(image):3d}%", end="", flush=True)
        print()

        print("Sealing image ...")
        bl.command(CMD_SEAL, (APP_ADDRESS, len(image), crc32(image)))

        print("Starting new firmware ...")
        bl.sock.sendall(struct.pack("<II", CMD_GO, APP_ADDRESS))
    finally:
        bl.close()


def main():
    parser = argparse.ArgumentParser(description="Flash the Open LCC RP2040 firmware over Wi-Fi")
    parser.add_argument("host", help="IP address or hostname of the ESP32-S3")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"Stream server port (default {DEFAULT_PORT})")
    parser.add_argument("--file", help=f"Local {ASSET_NAME} instead of downloading a release")
    parser.add_argument("--release", default="latest", help="Release tag to download (default: latest)")
    parser.add_argument("--repo", default=DEFAULT_REPO, help=f"GitHub repository (default {DEFAULT_REPO})")
    parser.add_argument("--sync-timeout", type=float, default=30, help="Seconds to wait for the bootloader")
    args = parser.parse_args()

    try:
        if args.file:
            with open(args.file, "rb") as f:
                image = f.read()
        else:
            image = download_release(args.repo, args.release)

        flash(args.host, args.port, image, args.sync_timeout)
    except (FlashError, OSError) as e:
        print(f"\nError: {e}", file=sys.stderr)
        print("Nothing is started until the image is sealed, the RP2040 stays in the bootloader. "
              "Just try again.", file=sys.stderr)
        return 1

    print("Done. The machine starts up with the new firmware.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
