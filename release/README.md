# Installing the workbench beta

This folder holds the current beta package. One file, one command.

## Install

Download `workbench_*_amd64.deb` from this folder, then from the
directory you saved it in:

```sh
sudo apt install ./workbench_*_amd64.deb
```

`apt` pulls in GTK3, VTE, SQLite3 and OpenSSL for you — there is nothing
else to install. Launch **Workbench** from the applications menu, or run
`workbench` from a shell.

If you have the repository cloned, `git pull` puts the latest package
right here:

```sh
git pull
sudo apt install ./release/workbench_*_amd64.deb
```

## Requirements

- A 64-bit (`amd64`) Debian or Ubuntu system.
- On Windows, this runs under **WSL2 with WSLg** — install a WSL Ubuntu
  distro, then run the same `apt install` command inside it. The window
  appears on the Windows desktop automatically. See the main README for
  WSL setup.

## Updating

```sh
git pull
sudo apt install ./release/workbench_*_amd64.deb
```

Installing a newer package over an older one upgrades it in place. Your
files, tools and captured terminal history live in
`~/.local/share/workbench/` and are kept across upgrades.

Each build has its own version (`0.1.0~beta+<build time>.g<commit>`), so
an upgrade always registers. If `apt` ever reports the package is already
the newest version but you expected a change, you can force it:

```sh
sudo apt install --reinstall ./release/workbench_*_amd64.deb
```

To check what you are actually running:

```sh
dpkg -s workbench | grep -i version
```

## Uninstalling

```sh
sudo apt remove workbench
```

That leaves `~/.local/share/workbench/` alone. Delete that directory too
if you want a completely clean slate.

## Reporting problems

Please include the package version and your distro:

```sh
dpkg -s workbench | grep -i version
lsb_release -d
```
