"""Collect release notices from the exact app/bootloader link maps and source trees.

Build paths are used only as inputs. Published names are relative component IDs.
Unknown archive origins, missing reviewed notices, or missing source trees fail
closed. No network requests or installation steps run during packaging.
"""

import hashlib
import json
import os
from pathlib import Path
import re


# Only recognized notice documents, never similarly named source/build files.
LICENSE_NAME = re.compile(
    r"^(?:(?:licen[sc]e|notice)(?:\.(?:txt|md|rst)|[-_.](?:mit|bsd|apache(?:-?2(?:\.0)?)?))?"
    r"|copying(?:[23](?:\.lib)?|\.(?:txt|md|rst|newlib|libgloss|runtime|picolibc|lib|lesser|lesserv[23]|gpl[23]))?)$",
    re.I,
)
COMMENT = re.compile(r"/\*.*?\*/|(?:^[ \t]*//[^\n]*(?:\n|$))+", re.S | re.M)
PRIVATE_PATH = re.compile(r"/Users/[^/\s]+/|/home/[^/\s]+/|[A-Za-z]:\\Users\\")
PRIVATE_CONTENT = re.compile(
    r"sk-[A-Za-z0-9_.-]{16,}|-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----|"
    r"[\"']?(?:CONFIG_)?(?:JUFF_(?:WIFI_SSID|WIFI_PASSWORD|DEVICE_TOKEN|BRIDGE_URI)|"
    r"(?:[A-Z0-9_]*_)?(?:API_KEY|ACCESS_TOKEN|AUTH_TOKEN|SECRET_KEY|PASSWORD|PRIVATE_KEY))"
    r"[\"']?\s*[:=]\s*[\"']?[^\s\"'#,}]",
    re.I,
)
REQUIRED_IDF = {
    "bt": ("host/nimble/nimble/LICENSE", "host/nimble/nimble/NOTICE",
           "common/tinycrypt/LICENSE"),
    "freertos": ("FreeRTOS-Kernel/LICENSE.md",),
    "lwip": ("lwip/COPYING",),
    "mbedtls": ("mbedtls/LICENSE",),
    "newlib": ("COPYING.NEWLIB",),
    "wpa_supplicant": ("COPYING",),
}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def source_path(path, allowed_root, *, allow_internal_source_link=False):
    """Validate lexical containment and every path component before any read.

    The caller supplies a canonical source boundary. Resolving the input first
    would hide a symlink to private configuration. License documents reject all
    symlinks. Source-comment extraction may follow an internal header alias,
    provided every link target stays inside the same component boundary.
    """
    path = Path(os.path.abspath(path))
    allowed_root = Path(os.path.abspath(allowed_root))
    if not path.is_relative_to(allowed_root):
        raise ValueError(f"License/source is outside its allowed source tree: {path.name}")
    current = allowed_root
    for part in (None, *path.relative_to(allowed_root).parts):
        if part is not None:
            current = current / part
        if current.is_symlink():
            if not allow_internal_source_link or not current.resolve().is_relative_to(allowed_root.resolve()):
                raise ValueError(f"Symlink is not allowed in a license/source path: {path.name}")
    if not path.resolve().is_relative_to(allowed_root.resolve()):
        raise ValueError(f"License/source escapes its source tree: {path.name}")
    return path.resolve() if allow_internal_source_link else path


def check_public_content(text, name):
    if PRIVATE_PATH.search(text):
        raise ValueError(f"Personal absolute path found in license/source: {name}")
    if PRIVATE_CONTENT.search(text):
        raise ValueError(f"Private configuration or credential found in license/source: {name}")


def public_text(path, allowed_root):
    path = source_path(path, allowed_root)
    if not path.is_file():
        raise ValueError(f"Missing required license/source: {path.name}")
    data = path.read_bytes()
    text = data.decode("utf-8")
    if not text.strip():
        raise ValueError(f"Empty license/source: {path.name}")
    check_public_content(text, path.name)
    return data


def collect_firmware_licenses(project_root, build):
    """Return {package-relative name: bytes}; validation completes before writes."""
    project_root, build = project_root.resolve(), build.resolve()
    project = read_json(build / "project_description.json")
    idf = Path(project["idf_path"]).resolve()
    files = {"LICENSE": public_text(project_root / "LICENSE", project_root)}
    records, component_records, archive_hashes = {}, {}, {}

    def add(path, name, allowed_root):
        data = public_text(path, allowed_root)
        if name in files and files[name] != data:
            raise ValueError(f"Different source trees provide conflicting notice {name}")
        files[name] = data
        return name

    def add_supplement(root, name, public_id):
        supplement = project_root / "licenses" / name
        metadata_bytes = public_text(supplement / "provenance.json", project_root)
        metadata = json.loads(metadata_bytes)
        component_hash = source_path(root / ".component_hash", root).read_text().strip()
        if not component_hash or metadata.get("component_hash") != component_hash:
            raise ValueError(f"Supplemental notices do not match this component version: {name}")
        if not metadata.get("files"):
            raise ValueError(f"Empty supplemental license list: {name}")
        added = []
        for filename in metadata["files"]:
            if Path(filename).name != filename:
                raise ValueError("Invalid supplemental license filename")
            path = supplement / filename
            if digest(public_text(path, project_root)) != metadata.get("sha256", {}).get(filename):
                raise ValueError(f"Supplemental license checksum mismatch: {name}/{filename}")
            added.append(add(path, f"licenses/{public_id}/{filename}", project_root))
        files[f"licenses/{public_id}/provenance.json"] = metadata_bytes
        return added

    add(idf / "LICENSE", "licenses/esp-idf/LICENSE", idf)
    contexts = [(build, project), (build / "bootloader", read_json(build / "bootloader/project_description.json"))]
    archive_rows = []
    for directory, description in contexts:
        if Path(description["idf_path"]).resolve() != idf:
            raise ValueError("Application and bootloader use different ESP-IDF trees")
        map_path = directory / Path(description["app_elf"]).with_suffix(".map").name
        text = map_path.read_text(encoding="utf-8")
        if "Archive member included" not in text or "Discarded input sections" not in text:
            raise ValueError("Link map lacks archive provenance; rebuild before packaging")
        included = re.split(r"Allocating common symbols|Discarded input sections", text, maxsplit=1)[0]
        matches = re.findall(r"^([^\n]+?\.a)\(([^\n)]+)\)", included, re.M)
        if not matches:
            raise ValueError("No linked archives found in link map")
        infos = description["build_component_info"]
        for archive_text, member in matches:
            archive = Path(archive_text.strip())
            if not archive.is_absolute():
                archive = directory / archive
            archive = archive.resolve()
            if not archive.is_file():
                raise ValueError(f"Linked archive is unavailable: {archive.name}")
            origin = None
            # Built component archives and nested libraries such as mbedTLS.
            if archive.is_relative_to(directory / "esp-idf"):
                name = archive.relative_to(directory / "esp-idf").parts[0]
                if name not in infos:
                    raise ValueError(f"Unknown linked component: {name}")
                origin = (name, infos[name])
            else:
                # Prebuilt archives (radio, HAL, ESP-SR) live in component trees.
                candidates = [(name, info) for name, info in infos.items()
                              if info.get("dir") and archive.is_relative_to(Path(info["dir"]).resolve())]
                if candidates:
                    origin = max(candidates, key=lambda item: len(item[1]["dir"]))
            if origin:
                name, info = origin
                root = Path(os.path.abspath(info["dir"]))
                if root.is_relative_to(idf):
                    root = source_path(root, idf)
                    public_id = "esp-idf/" + root.relative_to(idf).as_posix()
                elif root.is_relative_to(project_root / "firmware/managed_components"):
                    root = source_path(root, project_root)
                    public_id = "managed/" + name
                elif root == project_root / "firmware/main":
                    root = source_path(root, project_root)
                    public_id = "juff/main"
                else:
                    raise ValueError(f"Unreviewed component source: {name}")
                entry = component_records.setdefault(public_id, {
                    "root": root, "name": name, "sources": set(), "archives": set(), "members": set()})
                if entry["root"] != root:
                    raise ValueError(f"Conflicting source directory for {name}")
                entry["sources"].update(Path(os.path.abspath(source)) for source in info.get("sources", []))
                entry["archives"].add(archive)
                entry["members"].add(member)
                relative = archive.relative_to(root).as_posix() if archive.is_relative_to(root) else archive.name
                archive_id = public_id + "/" + relative
            else:
                # Only target runtime libraries, never compiler executables, are distributed.
                runtime = {"libc.a": "newlib", "libm.a": "newlib", "libgcc.a": "gcc", "libstdc++.a": "gcc"}.get(archive.name)
                tool_root = next((parent for parent in archive.parents if (parent / "share/licenses").is_dir()), None)
                if not runtime:
                    raise ValueError(f"Unreviewed linked archive: {archive.name}")
                if tool_root is None:
                    raise ValueError(f"Missing target runtime licenses for {archive.name}; install the full Espressif toolchain including share/licenses")
                names = ("COPYING.NEWLIB", "COPYING.LIBGLOSS") if runtime == "newlib" else ("COPYING3", "COPYING.RUNTIME")
                for filename in names:
                    add(tool_root / "share/licenses" / runtime / filename,
                        f"licenses/toolchain/{runtime}/{filename}", tool_root)
                archive_id = "toolchain/" + archive.relative_to(tool_root).as_posix()
            if archive not in archive_hashes:
                archive_hashes[archive] = digest(archive.read_bytes())
            archive_rows.append({"archive": archive_id, "sha256": archive_hashes[archive]})

    for public_id, entry in sorted(component_records.items()):
        root, name = entry["root"], entry["name"]
        if public_id == "juff/main":
            continue
        if not root.is_dir():
            raise ValueError(f"Component source tree is missing: {name}")
        covered = []
        for source in entry["sources"]:
            if not source.is_relative_to(root):
                continue
            source = source_path(source, root, allow_internal_source_link=True)
            if source.is_dir():
                covered.extend(path for path in source.rglob("*") if path.is_file()
                               and path.suffix.lower() in (".c", ".cpp", ".cc", ".s", ".h"))
            else:
                covered.append(source)
        prebuilt = [archive for archive in entry["archives"] if archive.is_relative_to(root)]
        notices = []
        if public_id.startswith("esp-idf/"):
            for relative in REQUIRED_IDF.get(name, ()):
                notices.append(add(root / relative, f"licenses/{public_id}/{relative}", root))
        for path in sorted(root.rglob("*")):
            if not path.is_file() or not LICENSE_NAME.match(path.name):
                continue
            # Exclude unrelated example/test/vendor-device subtrees.
            if path.parent != root and not any(source.is_relative_to(path.parent) for source in covered + prebuilt):
                continue
            relative = path.relative_to(root).as_posix()
            notices.append(add(path, f"licenses/{public_id}/{relative}", root))
        # Every prebuilt library must carry a license in its own ancestor chain.
        for archive in prebuilt:
            parents = [archive.parent] + [p for p in archive.parents if p != archive.parent and p.is_relative_to(root)]
            if not any(any(p.is_file() and LICENSE_NAME.match(p.name) for p in parent.iterdir()) for parent in parents):
                if name != "xtensa":
                    raise ValueError(f"Prebuilt archive has no enclosing license: {name}/{archive.name}")
                hal_header = source_path(root / "include/xtensa/hal.h", root)
                if not hal_header.is_file() or b"Permission is hereby granted" not in hal_header.read_bytes():
                    raise ValueError("Missing Cadence HAL permission notice for libxt_hal.a")
        # Preserve file-level notices (BSD/MIT/TLSF/http-parser/Cadence etc.) as well
        # as SPDX copyrights, instead of assuming all IDF code is Apache-2.0.
        notice_sources = set(covered)
        notice_sources.update(path for path in root.rglob("*.h")
                              if not any(part in ("test", "tests", "test_apps", "examples") for part in path.relative_to(root).parts))
        blocks = {}
        for source in sorted(notice_sources):
            source = source_path(source, root, allow_internal_source_link=True)
            if not source.is_file():
                raise ValueError(f"Configured source is unavailable: {name}/{source.name}")
            text = source.read_text(encoding="utf-8", errors="replace")[:32768]
            for match in COMMENT.finditer(text):
                block = match.group(0).strip()
                if re.search(r"copyright|SPDX-License-Identifier|permission is hereby granted", block, re.I):
                    check_public_content(block, f"{name}/{source.name}")
                    blocks.setdefault(block, set()).add(source.relative_to(root).as_posix())
        if blocks:
            content = "\n\n".join("Sources: " + ", ".join(sorted(paths)) + "\n" + block for block, paths in sorted(blocks.items())) + "\n"
            key = f"licenses/{public_id}/SOURCE_NOTICES.txt"
            files[key] = content.encode()
            notices.append(key)
        if public_id.startswith("managed/") and not any(LICENSE_NAME.match(Path(path).name) for path in notices):
            # This explicit, version-bound supplement is reviewed with its source URL.
            notices.extend(add_supplement(root, name, public_id))
        if name == "espressif__esp-sr":
            # Upstream binary includes WebRTC VAD; retain its separately reviewed notices.
            notices.extend(add_supplement(root, name, public_id))
        records[public_id] = sorted(set(notices))

    # Publish provenance without sdkconfig values, source-machine paths, or map files.
    archive_rows = list({(row["archive"], row["sha256"]): row for row in archive_rows}.values())
    inventory = {"schema_version": 1, "selection": "app and bootloader linked archive members",
                 "archives": sorted(archive_rows, key=lambda row: row["archive"]),
                 "components": records}
    files["licenses/inventory.json"] = json.dumps(inventory, indent=2).encode() + b"\n"
    lines = ["# Third-party notices", "", "JUFF source is licensed under the accompanying LICENSE (Apache-2.0).",
             "This firmware also includes the components and target runtime archives listed below.",
             "Their original license and copyright texts are retained under licenses/.",
             "The GCC runtime license includes its accompanying Runtime Library Exception.",
             "Source notices preserve file-level terms; a component's top-level license is not",
             "a replacement for those terms. Source-tree notices may cover more files than the linker retained.",
             "", "## Linked components", ""]
    if "managed/espressif__esp-sr" in records:
        lines[5:5] = ["ESP-SR's license grants use on Espressif products; this package targets ESP32-S3.",
                      "Its WebRTC supplement retains upstream notices without claiming an unpublished binary source revision."]
    lines.extend(f"- {component}" for component in sorted(records))
    lines.extend(["", "See licenses/inventory.json for archive hashes and the notice index.",
                  "This index records inputs to this build, without exporting private build configuration.", ""])
    files["THIRD_PARTY_NOTICES.md"] = "\n".join(lines).encode()
    for name, data in files.items():
        check_public_content(data.decode("utf-8"), name)
    return files
