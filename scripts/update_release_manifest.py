#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from urllib import error
from urllib import request


DEFAULT_REPO = "waywallen/open-wallpaper-engine"
PLUGIN_ID = "org.waywallen.open-wallpaper-engine"
DEFAULT_ARCHES = ("x86_64", "aarch64")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def read_plugin_version(cmake_path: Path) -> str:
    text = cmake_path.read_text(encoding="utf-8")
    match = re.search(r"^\s*set\s*\(\s*OWE_PLUGIN_VERSION\s+([^) \t\r\n]+)", text, re.MULTILINE)
    if not match:
        raise SystemExit(f"could not read OWE_PLUGIN_VERSION from {cmake_path}")
    return match.group(1).strip("\"'")


def github_request(url: str):
    req = request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "owe-update-release-manifest",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        return request.urlopen(req)
    except error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        raise SystemExit(f"GitHub request failed: {exc.code} {exc.reason}\n{body}") from exc
    except error.URLError as exc:
        raise SystemExit(f"GitHub request failed: {exc.reason}") from exc


def github_json(url: str):
    with github_request(url) as response:
        return json.load(response)


def fetch_release(repo: str, tag: str, latest: bool):
    releases_url = f"https://api.github.com/repos/{repo}/releases"
    if latest:
        return github_json(f"{releases_url}/latest")
    return github_json(f"{releases_url}/tags/{tag}")


def download_sha256(url: str) -> str:
    digest = hashlib.sha256()
    with github_request(url) as response:
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def asset_sha256(asset: dict, download_missing_digest: bool) -> str:
    digest = asset.get("digest") or ""
    if digest.startswith("sha256:"):
        return digest.split(":", 1)[1].lower()

    name = asset.get("name", "<unknown>")
    if not download_missing_digest:
        raise SystemExit(
            f"release asset {name} has no sha256 digest in the GitHub API; "
            "rerun with --download-missing-digest to compute it from the asset"
        )

    url = asset.get("browser_download_url")
    if not url:
        raise SystemExit(f"release asset {name} has no browser_download_url")
    return download_sha256(url)


def asset_name(version: str, arch: str) -> str:
    return f"{PLUGIN_ID}-{version}-linux-{arch}.zip"


def update_manifest(existing: dict, release: dict, version: str, arches: list[str], download_missing_digest: bool) -> dict:
    assets = {asset.get("name"): asset for asset in release.get("assets", [])}
    available = ", ".join(sorted(name for name in assets if name)) or "<none>"

    manifest = dict(existing)
    manifest["version"] = version
    manifest.setdefault("entry_version", 2)
    manifest.setdefault("spawn_version", 6)

    for arch in arches:
        name = asset_name(version, arch)
        asset = assets.get(name)
        if not asset:
            raise SystemExit(f"release is missing asset {name}; available assets: {available}")

        url = asset.get("browser_download_url")
        if not url:
            raise SystemExit(f"release asset {name} has no browser_download_url")

        manifest[arch] = {
            "zip_url": url,
            "sha256": asset_sha256(asset, download_missing_digest),
        }

    return manifest


def parse_args() -> argparse.Namespace:
    root = repo_root()
    parser = argparse.ArgumentParser(description="Update update.json from GitHub release assets.")
    parser.add_argument("--repo", default=DEFAULT_REPO, help=f"GitHub repo, default: {DEFAULT_REPO}")
    parser.add_argument("--version", help="plugin version; defaults to OWE_PLUGIN_VERSION in CMakeLists.txt")
    parser.add_argument("--tag", help="release tag; defaults to v<version>")
    parser.add_argument("--latest", action="store_true", help="read the latest GitHub release instead of a tag")
    parser.add_argument("--arch", action="append", choices=DEFAULT_ARCHES, help="architecture to update; can be repeated")
    parser.add_argument("--update-json", type=Path, default=root / "update.json", help="manifest path")
    parser.add_argument("--cmake", type=Path, default=root / "CMakeLists.txt", help="CMakeLists.txt path")
    parser.add_argument("--download-missing-digest", action="store_true", help="download assets to compute sha256 if API digest is missing")
    parser.add_argument("--dry-run", action="store_true", help="print the new manifest without writing update.json")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    version = args.version or read_plugin_version(args.cmake)
    tag = args.tag or f"v{version}"

    release = fetch_release(args.repo, tag, args.latest)
    if args.latest and not args.version:
        version = str(release.get("tag_name", "")).removeprefix("v")
        if not version:
            raise SystemExit("latest release response did not include tag_name")

    if args.update_json.exists():
        existing = json.loads(args.update_json.read_text(encoding="utf-8"))
    else:
        existing = {}

    arches = args.arch or list(DEFAULT_ARCHES)
    manifest = update_manifest(existing, release, version, arches, args.download_missing_digest)
    output = json.dumps(manifest, indent=2) + "\n"

    if args.dry_run:
        sys.stdout.write(output)
    else:
        args.update_json.write_text(output, encoding="utf-8")
        print(f"updated {args.update_json} from {args.repo} {release.get('tag_name', tag)}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
