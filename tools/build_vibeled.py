#!/usr/bin/env python3
"""Build/test/package the Qt tool on Windows or macOS (no Python runtime in the app)."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def run(args, env):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=ROOT, env=env, check=True)


def msvc_environment(env):
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    if not vs:
        raise RuntimeError("Install Visual Studio C++ build tools")
    vcvars = Path(vs) / "VC/Auxiliary/Build/vcvars64.bat"
    # cmd.exe has its own quote rules; list2cmdline would backslash-escape these.
    output = subprocess.check_output(f'cmd.exe /u /d /s /c ""{vcvars}" >nul && set"', env=env)
    for line in output.decode("utf-16le").splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            # Windows environment names are case-insensitive.
            for old in list(env):
                if old.lower() == key.lower():
                    del env[old]
            env[key] = value
    return env


def find_qt(requested, env):
    if requested:
        candidates = [requested]
    else:
        candidates = [Path(env[key]) for key in ("VIBELED_QT", "QTDIR", "QT_ROOT") if env.get(key)]
        if os.name == "nt":
            candidates.append(Path(r"D:\dev.down\Qt5.15.14"))
        else:
            candidates += [Path("/opt/homebrew/opt/qt"), Path("/usr/local/opt/qt"),
                           Path("/opt/homebrew/opt/qt@5"), Path("/usr/local/opt/qt@5")]
            candidates += sorted((Path.home() / "Qt").glob("*/macos"), reverse=True)
            candidates += sorted((Path.home() / "Qt").glob("*/clang_64"), reverse=True)
        qmake = shutil.which("qmake", path=env.get("PATH"))
        if qmake:
            prefix = subprocess.check_output([qmake, "-query", "QT_INSTALL_PREFIX"], env=env, text=True).strip()
            candidates.append(Path(prefix))
    deploy = "windeployqt.exe" if os.name == "nt" else "macdeployqt"
    for candidate in candidates:
        candidate = candidate.expanduser().resolve()
        if (candidate / "bin" / deploy).is_file():
            return candidate
    raise RuntimeError("Qt was not found. Pass --qt <Qt prefix> or set VIBELED_QT.")


def copy_extras(destination):
    destination.mkdir(parents=True, exist_ok=True)
    hooks = destination / "hooks"
    hooks.mkdir()
    for name in ("agent_hook.py", "logled_agent_hook.py", "vibeled_agent_hook.py"):
        shutil.copy2(ROOT / "tools" / name, hooks / name)
    for source in (ROOT / "LICENSE", ROOT / "THIRD_PARTY_NOTICES.md", ROOT / "docs/vibeled.md"):
        shutil.copy2(source, destination / source.name)


def copy_runtime(target, env):
    # Use the compiler's redistributable files, never DLLs from System32.
    redist = next((value for key, value in env.items() if key.lower() == "vctoolsredistdir"), None)
    folders = sorted(Path(redist).glob("x64/Microsoft.VC*.CRT")) if redist else []
    if not folders:
        raise RuntimeError("MSVC x64 redistributable DLLs were not found in VCToolsRedistDir")
    for dll in folders[-1].glob("*.dll"):
        shutil.copy2(dll, target / dll.name)
    for name in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
        if not (target / name).is_file():
            raise RuntimeError(f"Missing packaged runtime: {name}")


def smoke_check(executable, env):
    clean = {key: value for key, value in env.items()
             if not key.upper().startswith(("QT_", "QML", "DYLD_")) and key.lower() != "path"}
    clean["PATH"] = (str(Path(os.environ["SystemRoot"]) / "System32") + os.pathsep + os.environ["SystemRoot"]
                     if os.name == "nt" else "/usr/bin:/bin:/usr/sbin:/sbin")
    for argument in ("--version", "--help"):
        result = subprocess.run([str(executable), argument], cwd=executable.parent,
                                env=clean, capture_output=True, text=True, timeout=15)
        if result.returncode or "vibeled" not in result.stdout:
            raise RuntimeError(f"Packaged app failed {argument}: {result.returncode}\n{result.stderr}")
    print("Packaged CLI passed with the Qt/build directories removed from PATH.", flush=True)


def publish_tree(source, target):
    # A running EXE/DLL may be renamed on Windows, but must not be overwritten.
    # Build the archive from clean staging so retired binaries never get shipped.
    target.mkdir(parents=True, exist_ok=True)
    for item in source.rglob("*"):
        output = target / item.relative_to(source)
        if item.is_dir():
            output.mkdir(exist_ok=True)
        else:
            if output.exists() and output.suffix.lower() == ".exe":
                output.rename(output.with_name(f"{output.stem}.obsolete-{time.time_ns()}{output.suffix}"))
            try:
                shutil.copy2(item, output)
            except PermissionError:
                if not output.is_file():
                    raise
                output.rename(output.with_name(f"{output.stem}.obsolete-{time.time_ns()}{output.suffix}"))
                shutil.copy2(item, output)


def package_windows(build, stage, dist, qt, env, version):
    target = stage / "vibeled"
    target.mkdir()
    executable = target / "vibeled.exe"
    shutil.copy2(build / "vibeled.exe", executable)
    run([qt / "bin/windeployqt.exe", "--release", "--no-translations", "--no-opengl-sw",
         "--no-compiler-runtime", executable], env)
    copy_runtime(target, env)
    (target / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\n", encoding="utf-8")
    if not (target / "platforms/qwindows.dll").is_file():
        raise RuntimeError("Missing Windows Qt platform plugin")
    copy_extras(target)
    (target / "README.txt").write_text(
        "vibeled - USB 状态灯\n\n"
        "1. 将 ZIP 完整解压到一个目录。不要在压缩包内直接打开程序。\n"
        "2. 双击 vibeled.exe。请保留同目录的 DLL 和子目录。\n"
        "3. 插入接收器，通过 USB CDC 设置灯光，无需 Wi-Fi 或蓝牙配网。\n\n"
        "支持 Windows 10/11 x64；已包含 Qt 和 MSVC 运行库，不需要安装 Qt 或 Python。\n"
        "命令行：vibeled.exe --help\n"
        "可选 Agent Hook 需要 Python 3.10+：python hooks/agent_hook.py --agent codex Stop\n"
        "完整说明见 vibeled.md。\n", encoding="utf-8-sig")
    archive = stage / f"vibeled-{version}-windows-x64.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for item in sorted(target.rglob("*")):
            if item.is_file():
                bundle.write(item, item.relative_to(stage))
    # Check the actual archive, including a path with spaces and non-ASCII characters.
    extracted = stage / "zip check 解压"
    with zipfile.ZipFile(archive) as bundle:
        bundle.extractall(extracted)
    smoke_check(extracted / "vibeled/vibeled.exe", env)
    publish_tree(target, dist / "vibeled")
    return archive


def package_macos(build, stage, dist, qt, env, version):
    volume = stage / "volume"
    volume.mkdir()
    app = volume / "vibeled.app"
    shutil.copytree(build / "vibeled.app", app, symlinks=True)
    run([qt / "bin/macdeployqt", app, "-always-overwrite"], env)
    copy_extras(app / "Contents/Resources")
    run(["codesign", "--force", "--deep", "--sign", "-", app], env)
    run(["codesign", "--verify", "--deep", "--strict", app], env)
    executable = app / "Contents/MacOS/vibeled"
    smoke_check(executable, env)
    archs = subprocess.check_output(["lipo", "-archs", str(executable)], env=env, text=True).split()
    architecture = "universal" if len(archs) > 1 else archs[0]
    (volume / "Applications").symlink_to("/Applications", target_is_directory=True)
    (volume / "README.txt").write_text(
        "vibeled - USB 状态灯\n\n将 vibeled.app 拖到 Applications，然后从应用程序目录启动。\n"
        "Qt 已包含在应用中。灯光通过 USB CDC 控制，无需配网。\n"
        "命令行：/Applications/vibeled.app/Contents/MacOS/vibeled --help\n"
        "可选 Agent Hook（Python 3.10+）：\n"
        "python3 /Applications/vibeled.app/Contents/Resources/hooks/agent_hook.py --agent codex Stop\n"
        "此本地构建使用临时签名，未做 Apple 公证。\n", encoding="utf-8")
    archive = stage / f"vibeled-{version}-macos-{architecture}.dmg"
    run(["hdiutil", "create", "-volname", "vibeled", "-srcfolder", volume,
         "-format", "UDZO", "-fs", "HFS+", archive], env)
    run(["hdiutil", "verify", archive], env)
    target = dist / "vibeled.app"
    if target.exists():
        target.rename(dist / f"vibeled.obsolete-{time.time_ns()}.app")
    shutil.copytree(app, target, symlinks=True)
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt", type=Path,
                        help="Qt prefix containing bin/qmake and lib/cmake")
    parser.add_argument("--no-package", action="store_true")
    parser.add_argument("--refresh-icons", action="store_true",
                        help="regenerate PNG/ICO/ICNS sizes from the checked-in icon artwork using Qt")
    parser.add_argument("--no-pause", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if os.name != "nt" and sys.platform != "darwin":
        parser.error("Packaging is supported on Windows (ZIP) and macOS (DMG)")
    env = dict(os.environ)
    if os.name == "nt":
        env = msvc_environment(env)
    args.qt = find_qt(args.qt, env)
    path_key = next((k for k in env if k.lower() == "path"), "PATH")
    env[path_key] = str(args.qt / "bin") + os.pathsep + env.get(path_key, "")
    print(f"Qt: {args.qt}", flush=True)
    if args.refresh_icons:
        icons_build = ROOT / "build/iconpack"
        configure_icons = ["cmake", "-S", ROOT / "tools/iconpack", "-B", icons_build,
                           "-DCMAKE_BUILD_TYPE=Release"]
        if os.name == "nt":
            configure_icons += ["-G", "Ninja"]
        if args.qt:
            configure_icons += [f"-DCMAKE_PREFIX_PATH={args.qt}"]
        run(configure_icons, env)
        run(["cmake", "--build", icons_build, "--config", "Release"], env)
        packer = icons_build / ("vibeled_iconpack.exe" if os.name == "nt" else "vibeled_iconpack")
        if not packer.is_file():
            packer = icons_build / "Release" / packer.name
        run([packer, ROOT / "tool/assets/vibeled-source.png", ROOT / "tool/assets"], env)
    build = ROOT / "build/vibeled"
    configure = ["cmake", "-S", ROOT / "tool", "-B", build, "-DCMAKE_BUILD_TYPE=Release"]
    if os.name == "nt":
        configure += ["-G", "Ninja"]
    if args.qt:
        configure += [f"-DCMAKE_PREFIX_PATH={args.qt}"]
    run(configure, env)
    run(["cmake", "--build", build, "--config", "Release", "--parallel"], env)
    run(["ctest", "--test-dir", build, "-C", "Release", "--output-on-failure"], env)
    run([sys.executable, ROOT / "tests/test_vibeled_hook.py"], env)
    if args.no_package:
        return
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    version = re.search(r"project\(vibeled VERSION ([\d.]+)", (ROOT / "tool/CMakeLists.txt").read_text()).group(1)
    with tempfile.TemporaryDirectory(prefix="vibeled-package-", dir=ROOT / "build") as temporary:
        stage = Path(temporary).resolve()
        assert stage.is_relative_to((ROOT / "build").resolve())
        package = package_windows if os.name == "nt" else package_macos
        archive = package(build, stage, dist, args.qt, env, version)
        target = dist / archive.name
        os.replace(archive, target)
    checksum = hashlib.sha256(target.read_bytes()).hexdigest()
    target.with_suffix(target.suffix + ".sha256").write_text(f"{checksum}  {target.name}\n", encoding="ascii")
    print(f"\nREADY: {target}\nSHA256: {checksum}\nUse the package in dist; build contains intermediate files only.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"\nBUILD/PACKAGE FAILED: {error}", file=sys.stderr)
        sys.exit(1)
