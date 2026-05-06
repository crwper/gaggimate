import datetime
import re
import subprocess

Import("env")


def get_build_version():
    """Run `git describe` and return the version string, e.g. 'v1.8.0' or
    'v1.8.0-34-g6059100f' (optionally with a trailing '-dirty')."""
    ret = subprocess.run(
        ["git", "describe", "--tags", "--dirty", "--exclude", "nightly"],
        stdout=subprocess.PIPE, text=True,
    )
    return ret.stdout.strip()


def parse_version_components(build_version):
    """Parse the git-describe output into (major, minor, patch, commits_ahead).

    Accepts:
      - 'v<MAJOR>.<MINOR>.<PATCH>'                  → commits_ahead = 0
      - 'v<MAJOR>.<MINOR>.<PATCH>-<N>-g<HASH>'      → commits_ahead = N
      - either form optionally suffixed with '-dirty'

    Returns (0, 0, 0, 0) if the string doesn't match (e.g. detached HEAD with no
    matching tag). commits_ahead is clamped to 255 so it fits in a uint8_t.
    """
    m = re.match(r"^v(\d+)\.(\d+)\.(\d+)(?:-(\d+)-g[0-9a-f]+)?", build_version)
    if not m:
        print(f"Warning: could not parse version '{build_version}', defaulting to 0.0.0")
        return (0, 0, 0, 0)
    return (
        int(m.group(1)),
        int(m.group(2)),
        int(m.group(3)),
        min(int(m.group(4) or 0), 255),
    )


def get_firmware_version_block(build_version):
    """Return the multi-line `#define` block carrying both the original
    string and the parsed numeric components. Consumed by `slog` writers
    that need a 4-byte version stamp without runtime string parsing."""
    major, minor, patch, commits = parse_version_components(build_version)
    print(f"Build version: {build_version} (major={major} minor={minor} patch={patch} commits_ahead={commits})")
    return (
        f"#define BUILD_GIT_VERSION \"{build_version}\"\n"
        f"#define BUILD_VERSION_MAJOR {major}\n"
        f"#define BUILD_VERSION_MINOR {minor}\n"
        f"#define BUILD_VERSION_PATCH {patch}\n"
        f"#define BUILD_VERSION_COMMITS_AHEAD {commits}"
    )


def get_time_specifier_build_flag():
    build_timestamp = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    print("Build date: " + build_timestamp)
    return "#define BUILD_TIMESTAMP \"" + build_timestamp + "\""


build_version = get_build_version()

with open('src/version.h', 'w') as f:
    f.write(
        '#pragma once\n'
        '#ifndef GIT_VERSION_H\n'
        '#define GIT_VERSION_H\n'
        + get_firmware_version_block(build_version) + '\n'
        + get_time_specifier_build_flag() + '\n'
        '#endif\n'
    )
