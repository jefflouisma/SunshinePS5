"""Administrator-installed relay gate. No installation or network I/O on import."""

import argparse
import hashlib
import importlib
import json
import os
import pwd
import re
import stat
import sys
from pathlib import Path

ADMIN_UID = 0
MODULES = ("remote_relay.py", "supervisor.py", "observer.py", "fenrir_provider.py")
PROVIDER_FIELDS = {
    "kubectl",
    "kubeconfig",
    "context",
    "namespace",
    "agent_port",
    "user",
    "app",
    "pairing",
}
FIELDS = {
    "schema_version",
    "account",
    "uid",
    "module_directory",
    "hashes",
    "python",
    "remote_root",
    "provider",
}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate configuration field")
        result[key] = value
    return result


def ancestor(info, allowed):
    require(
        stat.S_ISDIR(info.st_mode)
        and info.st_uid in allowed
        and not info.st_mode & 0o022,
        "unsafe ancestor",
    )


def identity(info):
    return (
        info.st_dev,
        info.st_ino,
        info.st_mode,
        info.st_uid,
        info.st_nlink,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def inspect(
    path,
    owner,
    mode=None,
    directory=False,
    executable=False,
    limit=1048576,
    source_directory=False,
):
    """Read through held no-follow descriptors; reject writable parent chains."""
    require(
        isinstance(path, str)
        and re.fullmatch(r"/[A-Za-z0-9_./-]+", path)
        and str(Path(path)) == path
        and os.path.normpath(path) == path,
        "noncanonical path",
    )
    parts = Path(path).parts[1:]
    require(parts, "root path forbidden")
    parent = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    fd = None
    try:
        ancestor(os.fstat(parent), {ADMIN_UID, owner})
        for part in parts[:-1]:
            child = os.open(
                part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent
            )
            closing, parent = parent, child
            os.close(
                closing
            )  # Ownership detached first; uncertain close never retried.
            ancestor(os.fstat(parent), {ADMIN_UID, owner})
        flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
        if directory:
            flags |= os.O_DIRECTORY
        fd = os.open(parts[-1], flags, dir_fd=parent)
        before = os.fstat(fd)
        require(before.st_uid == owner, "wrong owner")
        require(not before.st_mode & 0o022, "writable deployment object")
        require(mode is None or stat.S_IMODE(before.st_mode) == mode, "wrong mode")
        require(
            stat.S_ISDIR(before.st_mode) if directory else stat.S_ISREG(before.st_mode),
            "wrong object type",
        )
        if directory:
            if source_directory:
                allowed = set(MODULES) | {"deployment_entrypoint.py"}
                with os.scandir(fd) as entries:
                    for entry in entries:
                        require(
                            entry.name in allowed
                            and entry.is_file(follow_symlinks=False),
                            "unexpected module directory entry/bytecode",
                        )
            require(
                identity(before)
                == identity(os.fstat(fd))
                == identity(os.stat(parts[-1], dir_fd=parent, follow_symlinks=False)),
                "directory changed during inspection",
            )
            return identity(before)
        require(before.st_nlink == 1 and before.st_size <= limit, "file alias/quota")
        require(not executable or before.st_mode & 0o111, "executable bit absent")
        data = bytearray()
        while len(data) <= limit:
            chunk = os.read(fd, min(65536, limit + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
        after = os.fstat(fd)
        entry = os.stat(parts[-1], dir_fd=parent, follow_symlinks=False)

        require(
            identity(before) == identity(after) == identity(entry)
            and len(data) == before.st_size,
            "file changed during inspection",
        )
        return bytes(data)
    finally:
        try:
            if fd is not None:
                closing, fd = fd, None
                os.close(closing)
        finally:
            closing, parent = parent, None
            os.close(closing)


def preflight(config_path):
    raw = inspect(config_path, ADMIN_UID, limit=16384)
    config = json.loads(raw.decode("utf-8"), object_pairs_hook=unique)
    require(
        isinstance(config, dict)
        and set(config) == FIELDS
        and type(config["schema_version"]) is int
        and config["schema_version"] == 1,
        "configuration schema",
    )
    uid, account = config["uid"], config["account"]
    require(
        type(uid) is int
        and uid > 0
        and uid != ADMIN_UID
        and isinstance(account, str)
        and re.fullmatch(r"[a-z_][a-z0-9_-]{0,63}", account),
        "dedicated account required",
    )
    require(
        os.getuid() == os.geteuid() == uid
        and pwd.getpwuid(uid).pw_name == account
        and pwd.getpwnam(account).pw_uid == uid,
        "wrong execution account",
    )
    provider = config["provider"]
    require(
        isinstance(provider, dict)
        and set(provider) == PROVIDER_FIELDS
        and all(
            isinstance(v, str) and 0 < len(v) <= 1024 and "\x00" not in v
            for v in provider.values()
        ),
        "provider schema",
    )
    hashes = config["hashes"]
    require(
        isinstance(hashes, dict)
        and set(hashes) == set(MODULES)
        and all(
            isinstance(v, str) and re.fullmatch(r"[0-9a-f]{64}", v)
            for v in hashes.values()
        ),
        "module pins",
    )
    module_dir = config["module_directory"]
    module_identity = inspect(
        module_dir, ADMIN_UID, directory=True, source_directory=True
    )
    for name in MODULES:
        data = inspect(module_dir + "/" + name, ADMIN_UID)
        require(
            hashlib.sha256(data).hexdigest() == hashes[name], "module hash mismatch"
        )
    require(
        inspect(module_dir, ADMIN_UID, directory=True, source_directory=True)
        == module_identity,
        "module directory replaced",
    )
    inspect(config["remote_root"], uid, mode=0o700, directory=True)
    inspect(provider["kubeconfig"], uid, mode=0o600)
    for executable in (config["python"], provider["kubectl"]):
        inspect(executable, ADMIN_UID, executable=True, limit=64 * 1024 * 1024)
    require(config["python"] == sys.executable, "wrong Python executable")
    return config


def serve(config_path):
    # Only a directly configured Subsystem is supported. ForceCommand semantics
    # differ between sshd versions/configurations, so reject that route entirely.
    require("SSH_ORIGINAL_COMMAND" not in os.environ, "ForceCommand unsupported")
    require(sys.flags.isolated and sys.flags.no_site, "Python -I -S required")
    config = preflight(config_path)
    require(sys.platform == "linux", "Linux deployment required")
    require(not any(name[:-3] in sys.modules for name in MODULES), "preloaded modules")
    sys.dont_write_bytecode = True
    os.environ.clear()
    os.environ.update(
        PATH="/usr/bin:/bin",
        LANG="C.UTF-8",
        PYTHONNOUSERSITE="1",
        PYTHONDONTWRITEBYTECODE="1",
    )
    sys.path.insert(0, config["module_directory"])
    module_identity = inspect(
        config["module_directory"], ADMIN_UID, directory=True, source_directory=True
    )
    loaded = {}
    for filename in MODULES:
        module = importlib.import_module(filename[:-3])
        require(
            getattr(module, "__file__", None)
            == config["module_directory"] + "/" + filename,
            "imported module path mismatch",
        )
        loaded[filename] = module
    relay = loaded["remote_relay.py"]
    require(
        inspect(
            config["module_directory"], ADMIN_UID, directory=True, source_directory=True
        )
        == module_identity,
        "module directory replaced during import",
    )
    require(preflight(config_path) == config, "deployment changed during import")

    def owner(request, relay_id, deadline):
        # The remote peer cannot select another executable, credential or root.
        require(
            isinstance(request, dict)
            and request.get("root") == config["remote_root"]
            and request.get("provider") == config["provider"],
            "deployment binding",
        )
        require(preflight(config_path) == config, "deployment configuration changed")
        return relay.Owner(request, relay_id, deadline)

    relay.serve(owner_factory=owner)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--serve", action="store_true")
    args = parser.parse_args()
    if args.serve:
        serve(args.config)
    else:
        preflight(args.config)
        print('{"deployment_preflight":"passed","acceptance":"Incomplete"}')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
