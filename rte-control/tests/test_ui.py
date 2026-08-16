#!/usr/bin/env python3
"""Check that the packaged web UI matches rte-httpd's same-origin contract."""

from html.parser import HTMLParser
from pathlib import Path
from typing import List, Optional, Set, Tuple
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "www"


class ContractParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.errors = []  # type: List[str]
        self.asset_urls = []  # type: List[str]
        self.ids = set()  # type: Set[str]

    def handle_starttag(
        self, tag: str, attrs: List[Tuple[str, Optional[str]]]
    ) -> None:
        attributes = dict(attrs)
        element_id = attributes.get("id")
        if element_id:
            if element_id in self.ids:
                self.errors.append(f"duplicate HTML id: {element_id}")
            self.ids.add(element_id)

        if attributes.get("style") is not None:
            self.errors.append(f"inline style is blocked by the CSP: <{tag}>")
        if tag == "style":
            self.errors.append("inline <style> is blocked by the CSP")
        if tag == "script":
            source = attributes.get("src")
            if not source:
                self.errors.append("inline <script> is blocked by the CSP")
            else:
                self.asset_urls.append(source)
        if tag == "link" and "stylesheet" in (
            attributes.get("rel") or ""
        ).split():
            href = attributes.get("href")
            if not href:
                self.errors.append("stylesheet link has no href")
            else:
                self.asset_urls.append(href)

    handle_startendtag = handle_starttag


def local_asset_path(url: str) -> Path:
    parsed = urlsplit(url)
    if parsed.scheme or parsed.netloc or url.startswith("//"):
        raise ValueError(f"cross-origin asset is not allowed: {url}")
    relative = Path(unquote(parsed.path).lstrip("/"))
    if not relative.parts or ".." in relative.parts:
        raise ValueError(f"unsafe asset path: {url}")
    return WEB_ROOT / relative


def require_text(source: str, value: str, description: str) -> None:
    if value not in source:
        raise AssertionError(f"app.js is missing {description}: {value}")


def main() -> None:
    index_path = WEB_ROOT / "index.html"
    script_path = WEB_ROOT / "app.js"
    stylesheet_path = WEB_ROOT / "app.css"
    for required in (index_path, script_path, stylesheet_path):
        if not required.is_file() or required.stat().st_size == 0:
            raise AssertionError(f"missing or empty UI asset: {required}")

    parser = ContractParser()
    parser.feed(index_path.read_text(encoding="utf-8"))
    parser.close()
    if parser.errors:
        raise AssertionError("; ".join(parser.errors))

    resolved_assets = {local_asset_path(url) for url in parser.asset_urls}
    for asset in resolved_assets:
        if not asset.is_file() or asset.stat().st_size == 0:
            raise AssertionError(f"referenced UI asset is missing or empty: {asset}")
    if script_path not in resolved_assets or stylesheet_path not in resolved_assets:
        raise AssertionError("index.html must load same-origin app.js and app.css")

    script = script_path.read_text(encoding="utf-8")
    require_text(script, "/healthz", "health endpoint")
    require_text(script, "/api/v1/system", "system snapshot endpoint")
    require_text(
        script,
        "/api/v1/rte/capabilities",
        "RTE capabilities endpoint",
    )
    require_text(script, "/api/v1/config", "combined PATCH endpoint")
    require_text(script, "PATCH", "PATCH request method")
    require_text(script, "If-Match", "optimistic-concurrency header")
    require_text(script, "Content-Type", "JSON content header")
    require_text(script, "application/json", "JSON media type")
    require_text(script, "fetch(", "browser request implementation")

    print("test_ui: PASS")


if __name__ == "__main__":
    main()
