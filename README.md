# vips-isyntax

A libvips loader plugin for Philips iSyntax whole-slide images, built on
[libisyntax](https://github.com/amspath/libisyntax) (BSD-2-Clause). Adds the operation `isyntaxload`.

```bash
bash build.sh                       # needs gcc, cmake, curl, pkg-config, libvips-dev of the libvips that will load it
vipsheader -a slide.isyntax | grep isyntax.
vips copy slide.isyntax out.v                           # full resolution
vips copy "slide.isyntax[level=2]" out.v                # lower-resolution level
vips isyntaxload slide.isyntax label.png --associated label    # or macro
bash test_isyntax.sh slide.isyntax                       # compare with libisyntax directly, must print PASS
```

Output: 3-band uchar RGB as stored (no colour conversion), xres/yres from the MPP, the embedded ICC
profile as `icc-profile-data`, metadata as `isyntax.*`.

Install location (build.sh chooses it):

| libvips | plugin path |
|---|---|
| 8.13 and later | `$libdir/vips-modules-X.Y/vips-isyntax.so` |
| 8.10 - 8.12 | `$libdir/vips-plugins-X.Y/vips-isyntax.so` |
| 8.9 and older | `$libdir/vips-plugins-X.Y/vips-isyntax.plg` (only `.plg` files are loaded) |

The plugin must be compiled against the same libvips version that loads it. pyvips with `[binary]`
uses its own libvips and never sees system plugins; use plain `pyvips` on the system libvips.

Notes
- libisyntax prints messages with `printf`; build.sh patches them to stderr so they can't corrupt
  image streams in `vips ... | vips ...` pipelines.
- libisyntax locks its cache for each whole tile decode, so one slide decodes on one thread at a time;
  downstream work (colour, resize, save) is still multi-threaded.
- Marked untrusted: `VIPS_BLOCK_UNTRUSTED=1` disables it.
- Tested here: builds and registers with libvips 8.9, 8.12 and 8.15; error handling on invalid files.
  Not yet tested on a real iSyntax slide: run test_isyntax.sh on one, e.g. the public test slide at
  https://zenodo.org/record/5037046/files/testslide.isyntax?download=1

## Docker images

`docker/Dockerfile` builds libvips with the plugin for Ubuntu focal (20.04, libvips 8.9), jammy (22.04, 8.12)
or noble (24.04, 8.15). The image contains the `vips` command-line tools, pyvips on the same libvips (so Python
sees the plugin too), `isyntax-crop` and `test_isyntax.sh`. The build fails if the plugin isn't registered.

```bash
docker build -f docker/Dockerfile --build-arg UBUNTU=noble -t vips-isyntax-noble .
docker run --rm -v "$PWD":/data vips-isyntax-noble vipsheader -a slide.isyntax
docker run --rm -v "$PWD":/data vips-isyntax-noble test_isyntax.sh slide.isyntax
```

Pushing a git tag runs `.github/workflows/build-images.yml`, which builds all three releases in parallel and
publishes them under the tag's exact name:

```
ghcr.io/ruitaodong/vips-isyntax-focal:<tag>
ghcr.io/ruitaodong/vips-isyntax-jammy:<tag>
ghcr.io/ruitaodong/vips-isyntax-noble:<tag>
```

The last build step runs `docker/check-plugin.sh`, which prints the plugin files, libvips's own loading messages
and the pyvips result, so a failure can be diagnosed from the job log alone.

libisyntax itself compiles with `-march=native`; build.sh removes that so the plugin runs on any x86-64 CPU,
not just the build machine's. Set `LIBISYNTAX_MARCH` (e.g. `x86-64-v3`, GCC 11+) to target newer CPUs.

New packages on ghcr.io are private: make each one public once (GitHub → Packages → vips-isyntax-&lt;release&gt;
→ Package settings → Change visibility) so clusters can pull without credentials.
