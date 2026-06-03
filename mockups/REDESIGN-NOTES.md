# maiden redesign — running design notes

Status: **interactive prototype** (visual + behavioral mock, not wired to a real
norns backend). Goal: make maiden responsive + beautiful across iPhone / iPad /
desktop, and open up personal-development + script-discovery workflows — **without
removing anything**.

Prototype lives in `mockups/` and is served on the LAN:
- `https://192.168.68.55:8443/` — main interactive app (`index.html`)
- `https://192.168.68.55:8443/repo.html` — current-vs-redesigned repo-manager comparison
- HTTP fallback on `:8770`

Real Ace editor (maiden's actual editor) is loaded from CDN **for the prototype
only** — production keeps maiden's bundled Ace (no CDN, no SRI concern).

---

## Built in the prototype (verified)

1. **Responsive layout** via CSS container queries — desktop 3-pane, tablet
   (explorer → drawer), phone (single pane + bottom tab bar + drawer). Nothing removed.
2. **41 color themes** (base16 / popular editor schemes incl. Catppuccin ×4,
   Rosé Pine ×3, Tokyo Night, Everforest, Kanagawa, Ayu, Gruvbox Material…) in a
   config picker; re-themes the **whole UI** (chrome, tree, REPL, **and editor
   syntax** — the Ace theme is generated from each palette, so schemes Ace doesn't
   bundle still color the editor). Trivially extensible toward the full base16 set.
2b. **Dynamic preference detection** — `prefers-color-scheme` picks the default
   theme and follows OS changes until you manually pick; `pointer:coarse` bumps the
   default editor font on touch devices; `prefers-reduced-motion` disables transitions.
2c. **Responsive hardening** — no horizontal scroll anywhere (verified
   scrollWidth==clientWidth); config is a full-screen sheet on phone (no float, no
   cut-off rows); repo list scrolls under a fixed, compact header (was sliding under
   an oversized toolbar).
3. **Editable Ace editor** — Lua mode, themed, font-size / tab-size / keymap from config.
4. **Expandable + scrollable file tree**; clicking a `.lua` loads it + updates the tab.
5. **Editable REPL** — type commands, Enter/Send echoes + canned response; clearly
   sized `matron / sc` target selector; **autocomplete** popup from common
   norns/maiden functions (arrow keys + Tab/Enter to accept).
6. **Gear → configuration** modal (was a dead icon) housing themes + editor settings + sources.
7. **Repository manager redesign** — cards, search, sort, real Install/Update/Remove
   buttons, tag pills, doc/discussion links, "installed" badge. Books icon opens this
   (was wrongly opening the file browser).
8. **GitHub integration** —
   - token field in config (hint: token stays on-device, proxied via maiden's Go
     server — never bundled into the web app);
   - **sources** model with color-coded providers: `community` (maiden catalog),
     custom catalogs (e.g. `kalium-list`), `@you` (your public+private repos),
     `github` (cross-GitHub search);
   - source **toggles** + a **source filter bar** in the repo manager;
   - color-coded **source tags** on every card; cross-source matches show multiple
     tags (a script that's both on GitHub and in the community catalog shows both);
   - **`discover` tab** — GitHub-wide search for norns scripts, sorted by stars,
     gated behind connecting a token, with `Clone` actions.
9. **Live search + sort** in the repo manager (search by name/author/tags/desc;
   sort by recent / name / author / stars). Real `<select>`, not a label.
10. **localStorage persistence** — theme, editor font/tab/keymap, GitHub connection,
    and source toggles survive reloads. Visible **build stamp** in the config header
    (`bN · date · summary`) so "am I on the latest build?" is never ambiguous.
11. **Real community catalog (350+ scripts)** — the page fetches the actual
    `monome/norns-community` `community.json` (served locally as `community.json`) and
    renders the full list, exactly as real maiden loads a catalog. No more 16-script mock.
12. **Untrusted-source install gate** — installing anything NOT in the built-in
    community catalog (custom catalogs, your GitHub repos, GitHub search/pasted URLs)
    pops a confirmation: warns scripts run with full device access and maintainers
    don't vouch for them; **Cancel** or **Install anyway**; optional snooze 1–4 weeks
    (capped — can be snoozed, never permanently disabled). Snooze persists in localStorage.
13. **Collapsible repo toolbar** — search/sort/sources bar auto-hides on scroll-down,
    reveals on scroll-up, plus a manual chevron toggle. GitHub Connect button drops to
    its own row on phone.
14. **Live GitHub search** — the discover tab calls the real GitHub API
    (`/search/repositories?q=…+norns&sort=stars`), maps results to cards with stars +
    push date, cross-matches names against the community catalog. Auto-runs on first open.
15. **Expandable cards** — tap a card to reveal last-updated date, code/docs/discuss/
    norns.community links, and an **embedded demo video** (YouTube/Vimeo iframe, else a
    platform link). Demo data comes from the `~/gits/nornslist` scraper, exported to
    `enriched.json` (344 entries, 283 with demos) and served alongside the app.
16. **Install status dock** — installing enqueues a bottom-corner job box with a live
    log (clone → deps → copy → engine scan) ending in **✓ installed successfully** or
    **✗ installed with errors**, auto-dismissing on success. Foundation for B1 bulk install.
17. **Clean install states** — installed / **update-available** / not-installed badges +
    matching actions. **Tag pills are click-to-filter**; **sort by last-updated** added.
18. `kalium-list` was a fabricated placeholder → renamed **example-list (demo placeholder)**.
19. **Desktop/iPad scroll fix** — the device now fills the viewport height so long lists
    scroll naturally instead of inside a short fixed box.
20. **Floating file tree everywhere** — the docked explorer is retired; the tree is now a
    slide-in drawer on iPhone/iPad/desktop (toggled by the files button), and it
    auto-hides when you enter the repository manager.
21. **Editing view modes** — editor-only / split / console-only toggle in the tab bar
    (desktop/iPad; iPhone keeps its bottom-tab switching).
22. **Resizable console** — in split view, drag the handle to resize the console up/down;
    clamped so the editor keeps ≥260px and the console ≥120px; height persists.

---

## Enhancement backlog (requested, not yet built)

### B1 — Filters + multi-select + bulk install/update  ✅ DONE (b13)
Per-card checkboxes + "select all" + a bottom **bulk bar** ("Install/Update N
selected") feeding the install dock; one batched trust-prompt if any are untrusted.
Tag pills are click-to-filter; sort by last-updated. (nb case: search/tag-filter `nb`,
select all, install.) *Remaining nicety:* a dedicated author/tag facet panel.

### Upload + demo fixes  ✅ DONE (b16)
- **One upload path** — single `⬆ upload` button (files). **Folders + multiple folders
  via drag-drop** (entries API). The Files…/Folder… menu is gone. *Constraint:* a native
  file dialog can't select files AND folders at once (why Drive/Dropbox split too), so
  drag is the unified path; the button is the quick files shortcut. (Touch can't drag a
  folder easily — re-add a folder-pick button if that matters.)
- **Demo URL hardening** — root cause of broken previews: some scraped `demo` values are
  **not URLs** (e.g. a YouTube *title* → rendered as a relative `href` → broken local link).
  Now: validate URLs (`isUrl`), classify per platform (`demoKind`). YouTube/Vimeo embed;
  **SoundCloud now embeds** (clean track/player URLs; malformed `soundcloud.com/track/<id>`
  → link out); Instagram + other → working external link; non-URL → "no playable demo".
  All card links guarded by `isUrl`. Net: 222 inline embeds, 62 clean external links, 63
  no-demo, **0 broken links**. (A few embeds may still show "unavailable" if the source
  video/track was deleted — data age, not a bug.)

### Polish round  ✅ DONE (b15)
- **Unified upload** — one `⬆ upload ▾` button (Files… / Folder… menu, since the OS
  picker can't offer both at once) feeding **one handler** that takes files AND folders,
  **multiple of each**. Drag-drop recurses folders via the entries API (verified: 2
  folders + a loose file in one drop → correct nested tree). *(The old split was an
  artifact of the two `<input>` types, not a real need.)*
- **Delete selected** — bulk delete from the file browser with a confirm that counts
  folders/contents.
- **Cards always expanded** — the rich detail (last-updated, links, demo) is always
  shown; no fold toggle. Demo iframes still load on-demand (the "load demo" button) so
  353 open cards stay light. *Watch-item:* `available` renders all 353 — if it gets
  janky, paginate it like discover.
- **Help (?) button** now opens a help modal: keyboard shortcuts + doc links (norns
  docs / studies / norns.community / lines / maiden) + build stamp.

### Discover refinements  ✅ DONE (b14)
- **Search on Enter / button only** (no live filtering) on discover; available/installed
  keep live filtering.
- **Pagination** (24/page, GitHub `page` param, capped at 1000) — prev/next + "page N of M
  · X results". Also fixes scroll jank (smaller DOM; removed card hover transform).
- **Hide already-have** toggle — filters out discover results already in the community
  catalog or installed (cross-matched by name). Verified 24 → 5.

### File ops  ✅ DONE (b14)
- **`dust` is the top limit** (was `~`); up disabled at dust.
- **Rename** (inline edit), **delete** (confirm dialog; warns + item count for folders),
  **folder upload** (`webkitdirectory`) in addition to file upload.

### Files view — first-class file manager  ✅ DONE (b13)
A full pane (peer to editor + repo manager): browse anywhere from `~` home (top limit)
with a clickable breadcrumb, default landing in `dust`; multi-select; **+ folder**;
**drag-drop / button upload with smart batching** (many small files → one archive +
on-device extract, not N sequential transfers); **download** (zipped when multiple).
The slide-in tree drawer stays for quick file-switching (tab-bar tree button / iPhone
top-bar menu). Reachable from the activity bar and the iPhone files tab.

### B2 — Engine-name deconfliction at install time
norns SuperCollider engines register by class name; two scripts shipping an engine
with the **same name** collide (sclang errors / wrong engine loads). Add an
install-time check that:
- detects a name collision against already-installed engines;
- offers **Cancel** / **Proceed anyway** / **Proceed with rename** — the rename
  field **prefilled** with an available proposal (e.g. `PolyPerc` → `PolyPerc2`);
- (stretch) rewrites the `engine.name` reference + the `.sc` class on install so the
  renamed engine actually works.
- *Why:* this class of breakage is invisible until runtime and baffling to users.

### B2 — Engine-name deconfliction  ✅ DONE (b17)
Install-time check: if a script registers a SuperCollider engine class already on the
device, a dialog names the conflict + owner, explains the SC class-name rule, and offers
**Cancel / Install anyway / Install renamed** (rename field prefilled with a free name,
e.g. `Glut` → `Glut_2`). Mocked via `SHIP_ENGINE` / `INSTALLED_ENGINES`; in production it
queries the device through the **same plumbing as the MCP server's `engine_check_conflict`**
(see B3). Currently single-install only; bulk auto-proceeds (future: batch-resolve).

### B3 — Companion MCP server  ✅ DONE (b17)
`mcp/maiden_mcp.py` — real, runnable MCP server exposing the device bridge: `repl`,
`script_list/load`, `file_list/read/write`, `engines_list`, `engine_check_conflict`,
`device_info`. Mock mode + `--selftest` (passing) for hardware-free testing; real mode
talks to maiden's REST API + matron's websocket. `engine_check_conflict` is the exact
surface B2 uses — that's the shared plumbing. README + MCP client config included.
Untested on hardware (verify ports against the PanicOS image).

### B5 — Rich detail from norns.community (READMEs + images)
norns.community renders per-script READMEs and sometimes images. Pull those into the
expanded card:
- a **description** populated from the README (beyond the one-line catalog blurb), as an
  optional expandable section;
- an **image gallery** — hide it entirely if there are no images or only the generic
  default; if multiple, an **album-style left/right carousel**.
- Source: the `~/gits/nornslist` scraper already resolves community URLs and could be
  extended to capture README text + image URLs (it already finds demo videos).

### B6 — Tag filter facet (+ auto "additional voice" tag)
A real **tag filter** in the repo manager (today tags are only click-to-filter on a pill,
which isn't discoverable):
- a facet/chip row built from the **existing tags** in the catalog (multi-select,
  show counts);
- **dynamically generate an "additional voice" tag** for scripts that register **nb
  voices**, so the nb-rig workflow (filter → select all → bulk install) is one obvious flow.
- Pairs with B1 (bulk install) and the source filter.

### B4 — Patches & dependency handling (port-aware installs)
Some scripts don't run unmodified on norns *ports* (hardware that doesn't behave like
real norns), and some need other things installed first. Make install dependency-aware,
like a Linux package manager:
- **Optional patches** per script — port-specific fixes applied at install (e.g.
  **AmenBreak** needs a tweak to run on a port). User can opt in.
- **Dependency resolution** — a script can declare it needs: other scripts, sample
  packs / downloads, and **nb voices**. The installer offers to set all of it up front
  (one confirm → installs the graph), surfacing what will be fetched.
- Tie into the install dock (per-dependency progress) and the trust gate (each external
  dep still warns if untrusted).
- *Why:* scripts like AmenBreak (downloads + samples) or nb rigs are fiddly to set up
  by hand; this makes "it just works on my port" realistic.

### B3 — Companion MCP server (separate deliverable)
An MCP server alongside the webapp that talks to the device to:
- **send commands** to matron/sclang (the REPL, programmatically);
- **run / write / edit scripts** on device (read+write `dust/`, trigger loads, restart);
- expose the same norns/maiden function metadata the REPL autocomplete uses.
- *Why:* lets an agent (or external tools) drive the device for codegen, testing,
  and live-coding — complements the human-facing webapp. Likely a thin client over
  matron's websocket + maiden's REST API.

---

## Integration plan (proposed, not started)

- **A. Where it lives:** fork `monome/maiden` → `seajay/maiden`; point
  `scripts/build-norns.sh:60` at the fork. (Alt: brittle patches in `patches/`.)
- **B. Approach:** retrofit the existing React 16 app — replace JS pixel-math layout
  with CSS responsive layout; add theme engine, sources, discover. No rewrite.
- **C. Visual scope:** layout + theming + the repo/discovery features; keep maiden's
  identity and color tokens as one of the themes ("maiden dark/light").
- Server-side work (Go): GitHub token storage + proxy, catalog aggregation, engine
  deconfliction check, multi-install endpoint.

## Open decisions
- Token storage: maiden Go server keychain/file vs browser localStorage (lean server-side).
- Does GitHub search hit the API directly or via a small maiden proxy (rate limits + token secrecy → proxy).
- B2 rename: cosmetic warning only, or actually rewrite the `.sc` class (higher value, more risk).
