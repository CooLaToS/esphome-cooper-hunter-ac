#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["rich"]
# ///

from __future__ import annotations

import argparse
import base64
import os
from pathlib import Path
import secrets
import sys

from rich.console import Console
from rich.panel import Panel
from rich.prompt import Prompt
from rich.table import Table
from rich import box

ROOT = Path(__file__).resolve().parent
TARGET = ROOT / "secrets.yaml"

console = Console()

PLACEHOLDERS = {"YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD"}


def parse_existing(path: Path) -> dict[str, str]:
    """Read key: "value" pairs from an existing secrets.yaml."""
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if ":" in line:
            key, _, raw = line.partition(":")
            result[key.strip()] = raw.strip().strip('"')
    return result


def prompt_wifi(existing: dict[str, str]) -> tuple[str, str]:
    ssid_default = existing.get("wifi_ssid", "")
    pw_default = existing.get("wifi_password", "")

    ssid_placeholder = ssid_default if ssid_default and ssid_default not in PLACEHOLDERS else None
    pw_placeholder = pw_default if pw_default and pw_default not in PLACEHOLDERS else None

    console.print("\n[bold]Wi-Fi credentials[/bold]")

    ssid = Prompt.ask(
        "  SSID",
        default=ssid_placeholder,
        show_default=bool(ssid_placeholder),
    ) or ssid_placeholder or "YOUR_WIFI_SSID"

    pw = Prompt.ask(
        "  Password",
        default=pw_placeholder,
        show_default=bool(pw_placeholder),
        password=not bool(pw_placeholder),
    ) or pw_placeholder or "YOUR_WIFI_PASSWORD"

    return ssid, pw


def generate_crypto_keys() -> dict[str, str]:
    return {
        "cooper_hunter_api_key": base64.b64encode(os.urandom(32)).decode(),
        "cooper_hunter_ota_password": secrets.token_hex(16),
        "web_pass": secrets.token_urlsafe(16),
    }


def render_yaml(values: dict[str, str]) -> str:
    return "\n".join(f'{key}: "{value}"' for key, value in values.items()) + "\n"


def show_result_table(values: dict[str, str], existing: dict[str, str]) -> None:
    table = Table(box=box.ROUNDED, show_header=True, header_style="bold cyan")
    table.add_column("Key", style="dim")
    table.add_column("Value")
    table.add_column("", width=4)

    sensitive = {"cooper_hunter_api_key", "cooper_hunter_ota_password", "web_pass", "wifi_password"}

    for key, val in values.items():
        display = "[dim]****[/dim]" if key in sensitive else val
        changed = existing.get(key) != val
        tag = "[green]new[/green]" if changed else "[dim]kept[/dim]"
        table.add_row(key, display, tag)

    console.print(table)


def check_placeholders(values: dict[str, str]) -> list[str]:
    return [k for k, v in values.items() if v in PLACEHOLDERS]


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate secrets.yaml for ESPHome Cooper Hunter config.")
    parser.add_argument("--force", action="store_true", help="Overwrite existing secrets.yaml (keeps values as defaults)")
    parser.add_argument("--non-interactive", action="store_true", help="Skip prompts, use existing/generated values")
    args = parser.parse_args()

    existing: dict[str, str] = {}

    if TARGET.exists():
        if not args.force:
            console.print(f"[yellow]secrets.yaml already exists.[/yellow] Use [bold]--force[/bold] to update it.")
            existing = parse_existing(TARGET)
            stale = check_placeholders(existing)
            if stale:
                console.print(Panel(
                    "\n".join(f"  [red]•[/red] {k}" for k in stale),
                    title="[bold red]Placeholder values detected[/bold red]",
                    subtitle="Run with [bold]--force[/bold] to update",
                ))
            return 1
        existing = parse_existing(TARGET)
        console.print("[yellow]Updating existing secrets.yaml[/yellow] (existing values are defaults)\n")
    else:
        console.print("[bold]Generating secrets.yaml[/bold]\n")

    # Wi-Fi — interactive unless --non-interactive
    if args.non_interactive:
        ssid = existing.get("wifi_ssid", "YOUR_WIFI_SSID")
        pw = existing.get("wifi_password", "YOUR_WIFI_PASSWORD")
    else:
        ssid, pw = prompt_wifi(existing)

    # Crypto keys — always regenerate (existing kept as default if --force via user re-entry not needed)
    new_keys = generate_crypto_keys()

    # If --force, keep existing crypto unless they were placeholders
    if args.force:
        for k in ("cooper_hunter_api_key", "cooper_hunter_ota_password", "web_pass"):
            if k in existing and existing[k] not in PLACEHOLDERS:
                new_keys[k] = existing[k]

        # Prompt to rotate each crypto key individually
        if not args.non_interactive:
            console.print("\n[bold]Crypto keys[/bold] (press Enter to keep current, type [bold]regen[/bold] to rotate)")
            for k in ("cooper_hunter_api_key", "cooper_hunter_ota_password", "web_pass"):
                answer = Prompt.ask(f"  {k}", default="keep", show_default=False)
                if answer.strip().lower() == "regen":
                    pass  # new_keys[k] already has fresh value
                elif answer.strip().lower() != "keep" and answer.strip() != "":
                    new_keys[k] = answer.strip()
                # else keep existing (already set above)

    values = {
        "wifi_ssid": ssid,
        "wifi_password": pw,
        "cooper_hunter_api_key": new_keys["cooper_hunter_api_key"],
        "cooper_hunter_ota_password": new_keys["cooper_hunter_ota_password"],
        "web_user": existing.get("web_user", "admin"),
        "web_pass": new_keys["web_pass"],
    }

    TARGET.write_text(render_yaml(values), encoding="utf-8")

    console.print()
    show_result_table(values, existing)

    stale = check_placeholders(values)
    if stale:
        console.print(Panel(
            "\n".join(f"  [yellow]•[/yellow] {k}" for k in stale),
            title="[bold yellow]Still using placeholder values[/bold yellow]",
            subtitle="Edit secrets.yaml before flashing",
        ))
    else:
        console.print(Panel(
            "[green]secrets.yaml is ready.[/green]\n\n"
            "Flash with:\n"
            "  [bold]esphome run esp32-c3-cooperhunter.yaml --device /dev/cu.usbmodem101[/bold]",
            title="[bold green]Done[/bold green]",
        ))

    return 0


if __name__ == "__main__":
    sys.exit(main())
