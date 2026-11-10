# Arch Linux / CachyOS — local packaging

This `PKGBUILD` packages the **current local working tree** of `xfce-net-plugin`, including uncommitted source changes. It does not download a GitHub release, and it does not verify the source with a checksum. It is intended for building from a **trusted local checkout**, not for submission to the AUR as-is.

Run from the root of a cloned repository:

```sh
cd packaging/arch
env PACKAGER='Tantin1 <juanperez33840@gmail.com>' makepkg -Cfs
sudo pacman -U ./xfce-net-plugin-1.0.1_dev-1-x86_64.pkg.tar.zst
xfce4-panel -r
```

Building requires the normal Arch `base-devel` tools and the packages listed in `makedepends`; `makepkg -s` can install missing dependencies. Do not run `makepkg` as root.

When changing `src/VERSION`, also update `pkgver` here (`-` becomes `_` in Arch package versions). Increment `pkgrel` when changing only package metadata or packaging instructions without changing upstream version.

For a public AUR package or a verifiable release build, replace this local-checkout approach with an immutable, versioned source archive or a pinned Git commit and appropriate integrity verification.
