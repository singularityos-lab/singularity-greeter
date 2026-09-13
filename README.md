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

## Use of Generative AI

Maintainers may use generative AI tools as assistants while working on singularity-greeter. Non-trivial assisted commits disclose the tool, model, and scope of the work.

AI tools may assist with code comments, documentation, repetitive code, and issue triage. Maintainers make project decisions and review every assisted change before it is merged.

Use these trailers for non-trivial assisted commits:

```plain
Assisted-by: <tool>:<model-version>
AI-Scope: <what the tool generated and the prompt or a short prompt summary>
```

Single-line completions, renames, and formatting changes do not need trailers.

Coding agents must also follow [AGENTS.md](AGENTS.md) before changing files,
creating commits, or opening pull requests.
