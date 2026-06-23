import os
import platform

import nuke


def _normalize_os_name(system_name):
    if system_name == "Darwin":
        return "MacOS"
    return system_name


def _normalize_architecture(machine_name):
    value = machine_name.lower()
    if value in ("x86_64", "amd64", "x64"):
        return "x86_64"
    if value in ("arm64", "aarch64"):
        return "arm64"
    return value


def _get_plugin_path():
    version = "{}.{}".format(nuke.NUKE_VERSION_MAJOR, nuke.NUKE_VERSION_MINOR)
    base_dir = os.path.dirname(os.path.abspath(__file__))
    os_name = _normalize_os_name(platform.system())

    if os_name == "MacOS":
        architecture = _normalize_architecture(platform.machine())
        return os.path.join(base_dir, os_name, version, architecture)

    return os.path.join(base_dir, os_name, version)


def _add_gif_writer_plugin_path():
    plugin_path = _get_plugin_path()

    if os.path.isdir(plugin_path):
        nuke.pluginAddPath(plugin_path)
        nuke.tprint("gifWriter: added plugin path {}".format(plugin_path))
        return

    nuke.tprint("gifWriter: no plugin folder found at {}".format(plugin_path))


_add_gif_writer_plugin_path()
