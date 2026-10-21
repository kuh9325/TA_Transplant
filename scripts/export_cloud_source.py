#!/usr/bin/env python3
"""Export reviewed Git-index source and app resources locally; never uploads.

Stage reviewed changes before export. Untracked, ignored, unstaged, proprietary
and private inputs are not exported. Initialize pinned submodules first. This
command requires a Git checkout; an exported archive builds via SOURCE_VERSION.
"""
import argparse
import hashlib
import io
import json
import pathlib
import re
import subprocess
import tarfile

# Source formats retained from the legacy exporter; binary resources have a
# separate path policy below. A matching extension alone never selects a file.
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tpp", ".tcc",
            ".m", ".mm", ".s", ".S", ".asm", ".metal", ".vert", ".frag", ".glsl",
            ".proto", ".cmake", ".in", ".txt", ".md", ".rst", ".py", ".sh", ".pl",
            ".bash", ".bat", ".ps1", ".json", ".yml", ".yaml", ".xml", ".def",
            ".rc", ".pc", ".ac", ".am", ".mak", ".ts", ".tsx", ".js", ".css",
            ".html", ".lock", ".map", ".natvis", ".inc", ".tmpl", ".manifest", ".sym", ".exports", ".plist", ".tbd"}
NAMES = {"CMakeLists.txt", "LICENSE", "COPYING", "COPYRIGHT", "NOTICE", "README",
         "Makefile", "configure", ".gitmodules", ".gitignore", ".clang-format", ".clangd", ".eslintignore"}
DIRECTORIES = {"src", "shaders", "proto", "cmake", "packaging", "libs", "launcher", "scripts", "docs", ".github"}
EXCLUDED = {".git", "node_modules", "dist", ".cache", "_msvc", "_protobuf-install", "__pycache__",
            "build", "build-prof", "build-opt", "references", "quality", "OriginalData", ".rwe"}
BLOCKED_SUFFIXES = {".hpi", ".ufo", ".ccx", ".gpf", ".gp3", ".sav", ".rwesave", ".p12", ".pfx", ".key", ".pem", ".pyc"}
RESOURCE_ROOTS = tuple("docs/macos27/icon-redesign-20261019/" + part + "/"
                       for part in ("composer", "sources", "legacy", "previews", "review")) + ("docs/macos27/phase9b-icons/",)
SECRET_PATTERNS = [rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----",
                   rb"\b(?:AKIA|ASIA)[A-Z0-9]{16}\b", rb"\b(?:ghp_|github_pat_)[A-Za-z0-9_]{25,}",
                   rb"\bsk-(?:proj-)?[A-Za-z0-9_-]{30,}"]


def allowed(rel):
    p = pathlib.PurePosixPath(rel)
    if p.is_absolute() or ".." in p.parts or any(part in EXCLUDED for part in p.parts):
        return False
    if p.suffix.lower() in BLOCKED_SUFFIXES or p.name in {".env", "credentials", "id_rsa", "id_ed25519"}:
        return False
    if p.name.startswith(".env.") or "mission-completion-phase7c" in p.parts:
        return False
    if p.name.startswith("MISSION_COMPLETION_") and p.suffix.lower() in {".json", ".csv"}:
        return False
    if p.name in {"creation-results.json", "render-results.json"}:
        return False
    if len(p.parts) > 1 and p.parts[0] not in DIRECTORIES:
        return False
    if p.suffix.lower() in {".svg", ".png", ".icns"}:
        return rel.startswith(RESOURCE_ROOTS)
    return p.suffix in SUFFIXES or p.name in NAMES


def git(root, *args):
    return subprocess.check_output(["git", *args], cwd=root)


def reviewed_files(root, repo=None):
    repo = root if repo is None else repo
    if not (repo / ".git").exists():
        raise ValueError("initialize the pinned submodule before export: " + str(repo))
    for entry in git(repo, "ls-files", "--stage", "-z").split(b"\0"):
        if not entry:
            continue
        meta, name = entry.split(b"\t", 1)
        mode, oid, stage = meta.split()
        if stage != b"0":
            raise ValueError("unmerged index entry")
        path = repo / name.decode()
        rel = path.relative_to(root).as_posix()
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            # Preserve legacy behavior: no symlinks in source exports. Asio has
            # physical include/src trees and CMake supports the physical path.
            continue
        if mode == b"160000":
            if git(path, "rev-parse", "HEAD").strip() != oid:
                raise ValueError("submodule revision differs from parent index: " + rel)
            yield from reviewed_files(root, path)
        elif mode in {b"100644", b"100755"} and allowed(rel):
            data = path.read_bytes()
            if data != git(repo, "show", ":" + name.decode()):
                # Git attributes can normalize CRLF in pinned dependencies.
                # Accept only bytes that normalize to the exact indexed blob.
                normalized = subprocess.check_output(
                    ["git", "hash-object", "--stdin", "--path=" + name.decode()], cwd=repo, input=data).strip()
                if normalized != oid:
                    raise ValueError("stage the reviewed file before export: " + rel)
            if data.startswith((b"HAPI", b"RWESAVE")) or any(re.search(p, data) for p in SECRET_PATTERNS):
                raise ValueError("prohibited content: " + rel)
            yield rel, data, 0o755 if mode == b"100755" else 0o644


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path, help="new .tar.gz file outside source tree")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    digest_path = output.with_suffix(output.suffix + ".sha256")
    if output.is_relative_to(root) or output.exists() or digest_path.exists():
        parser.error("archive and checksum must be new paths outside the source tree")
    try:
        files = sorted(reviewed_files(root))
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    manifest = {rel: hashlib.sha256(data).hexdigest() for rel, data, mode in files}
    version = git(root, "describe", "--dirty=-d").decode().strip()
    metadata = {"SOURCE_VERSION": version + "\n", "SOURCE_MANIFEST.json": json.dumps(manifest, indent=2) + "\n"}
    with tarfile.open(output, "x:gz") as archive:
        for rel, data, mode in files + [(name, data.encode(), 0o644) for name, data in metadata.items()]:
            info = tarfile.TarInfo("TA-Native/" + rel)
            info.size = len(data)
            info.mode = mode
            archive.addfile(info, io.BytesIO(data))
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    with digest_path.open("x") as stream:
        stream.write(digest + "  " + output.name + "\n")
    print(f"Created locally: {output} ({len(files)} reviewed files); SHA256 {digest}")


if __name__ == "__main__":
    main()
