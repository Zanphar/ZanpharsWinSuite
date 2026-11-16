# Zanphar's WinSuite — website

The product site for the suite: seven static pages, no build step, no framework
and no third-party assets. It uses the suite's own palette and proportions
(`src/ui.cpp`, `theme::*` and the 214 px rail) so the window and the site read as
one product.

## Preview it

Any static file server works; there is nothing to compile.

```bat
cd website
python -m http.server 8123 --bind 127.0.0.1
```

Then open <http://127.0.0.1:8123/>.

## Pages

| File | What it is |
|---|---|
| `index.html` | Home: hero, the dashboard as the product shot, six feature cards, a look inside, how it works, and the pitch for a native build |
| `features.html` | Every page of the suite in detail, with a screenshot per section, plus the four engines |
| `screenshots.html` | Gallery of all twelve page captures with a click-to-zoom viewer |
| `download.html` | Version, package contents, checksums, requirements, first run, removal, building from source |
| `changelog.html` | 0.2.4, 0.2.3, 0.2.2, 0.2.1, 0.2.0 and 0.1.0 summaries; links to the full `CHANGES.md` |
| `license.html` | The BSD 3-Clause agreement, word for word |
| `privacy.html` | Every file the suite writes and every packet it sends |

Shared assets live in `assets/css/site.css` and `assets/js/site.js`; both are
plain, dependency-free files.

## Screenshots

`assets/img/pages/*.png` were captured from `bin\ZanpharsWinSuite.exe` at the
1024 × 768 design size with the window moved to a fixed position, then cropped
to the client area. The Startup and Deactivate captures were left out on
purpose: they list whatever the capturing machine happens to have installed.

To refresh them, run the built window, open each page, and save the client area
to the same file names. Nothing in the site depends on their pixel size beyond
the `width`/`height` attributes in the markup.

## Documents

`docs\CHANGES.txt` and `docs\LICENSE.txt` are copies of the repository's
`CHANGES.md` and `LICENSE.md`, served as plain text so the changelog and licence
pages work when this folder is published on its own. Refresh them with the
repository copies whenever either file changes.

## Downloads

`downloads\` holds a copy of the current archive and its `.sha256` file, so the
download page works when this folder is served or published on its own. Copy the
new archive here whenever `release.bat` publishes a version, and update the
version, hash and file name in `download.html` at the same time.

## Publishing

Upload the whole `website/` folder to any static host. All links are relative,
so it works from a subdirectory as well as from a domain root. There is no
server-side code, no cookie, no analytics and no external font or script.

The published copy lives at
<https://site-5b3563511c074235bb40fd74c965651c.freebuff.page/> (Freebuff Sites,
preview environment). It is this folder minus `README.md`, served by a
pass-through Worker over the static files, so republishing means copying
`assets/`, `docs/`, `downloads/` and the seven pages to the host again - the
same files, no build step.
