# Singularity Greeter

> [!IMPORTANT]
> Report bugs and request features in the
> [Singularity Desktop tracker](https://github.com/singularityos-lab/singularity-desktop/issues/new/choose).

The login greeter for the Singularity Desktop, built on greetd and libsingularity.

## Requirements

- [Meson](https://mesonbuild.com/) >= 0.59
- [Vala](https://vala.dev/) compiler
- GTK4
- libgee-0.8
- json-glib (`json-glib-1.0`)
- gtk4-layer-shell (`gtk4-layer-shell-0`)
- [libsingularity](https://github.com/singularityos-lab/libsingularity)
- [greetd](https://git.sr.ht/~kennylevinsen/greetd) at runtime
- accountsservice at runtime (per-user accent color, wallpaper and avatar)

## Build & Install

```sh
meson setup build
meson compile -C build
meson install -C build
```

## Test mode

Run windowed, without a greetd socket, to preview the interface:

```sh
singularity-greeter -t
```

## License

GPL-3.0-only - see [LICENSE](LICENSE).
