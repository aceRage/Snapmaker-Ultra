# Phone stream tiles take their camera's aspect

Branch `fix/phone-stream-tile-aspect`.

## The problem

On the phone (the Android app's WebView, 360dp-wide layout) the Streams tab laid every camera tile
out as a 16:9 row: `render()` computed one `rowH = tileW * 9 / 16` and put it on
`grid.style.gridAutoRows`. The Snapmaker U1's camera is 4:3, so its picture sat letterboxed inside
a 16:9 box with wide black bars left and right. Two consequences:

* the picture looked small and off-centre for its tile;
* `.cell .label` and `.cell .expand` are absolutely positioned at the cell's corners, so with the
  picture inset they appeared *beside* it, on the black bars, instead of over it.

## The fix (phone mode only - the PC layout is byte-identical)

**Per-cell aspect instead of one row height.** In `REMOTE` mode `render()` now sets
`grid-auto-rows: auto` and each `.cell` carries its own shape:

```css
body.remote .cell          { aspect-ratio: var(--cell-ar, 16 / 9); max-height: var(--cell-max, none); }
body.remote .cell.expanded { aspect-ratio: auto; max-height: none; }
```

`body.remote` is added next to the other `REMOTE`-mode setup, so the PC never matches these rules
and keeps its pixel `grid-auto-rows`. `--cell-max` is published on `#grid` by `render()` and
carries the landscape cap the old row height had (a tile is never taller than the visible grid).

**Where a tile's aspect comes from,** best source first:

| tile | source | value |
| --- | --- | --- |
| relay player (our `/stream.html`, same origin) | measured | `video.videoWidth / videoHeight` |
| Bambu MJPEG (`rkind: 'p1'`, an `<img>` in this document) | measured | `naturalWidth / naturalHeight` |
| printer's own page (`rkind: 'url'`), URL contains `/webcam/` | per-kind guess | 4/3 (a U1) |
| anything else | default | 16/9 |

`player.html` watches its `<video>` for `loadedmetadata` and `resize` and posts
`{ snorcaAspect, src }` to `parent` at `location.origin`; `stream_center.html` listens, checks
`e.origin`, finds the cell whose iframe `contentWindow` is `e.source`, and sets `--cell-ar`.
Nothing is persisted - a wrong guess costs one render and is replaced as soon as the player
reports. A cross-origin printer page tells us nothing, hence the guess for those.

`video-rtc.js` / `video-stream.js` are verbatim go2rtc copies and were **not** touched; the
reporting lives in `player.html`, which owns the element.

**The LAN relay trade-off** was deliberately left alone: `relayPreferred()` still returns false for
private origins, so on the LAN a U1 tile keeps embedding the printer's own page. A one-line comment
in `remoteCell()` notes why (printer page = no hub CPU; relay = our player and consistent controls,
at the cost of a go2rtc pipeline per camera).

## Files

* `resources/web/orca/stream_center.html` - the `body.remote .cell` rules, `body.remote`,
  `grid-auto-rows: auto` + `--cell-max` in `render()`, `guessAspect()` / `setCellAspect()` / the
  `message` listener, the `<img>` measurement, the `relayPreferred()` comment.
* `resources/web/orca/player.html` - posts `snorcaAspect` to the parent.

## Gates

No gate expectation changed: `test_phone_ui.py` (the "phone UI gate") asserts on the Devices tab
and the Send sheet, and nothing under `snorca_hubtest` references `gridAutoRows`, `.cell`, `#grid`,
or the player page. `#grid`, `cell`, `cell empty`, `label`, `expand` and the `grid.style.gridAutoRows`
assignment are all kept, so its checks pass by construction.

## Click-test on the emulator

1. **Streams tab, portrait.** A U1 tile is 4:3 - the picture fills the tile edge to edge, no black
   bars left and right, and the name label (top-left) and the expand button (top-right) sit *over*
   the picture. A Bambu tile is 16:9.
2. **Expand.** Tap the expand button on a U1 tile: it fills the screen (the aspect and the cap both
   release), the stream keeps playing, and the label and the close button are inside the safe area.
   Tap again - it goes back to 4:3 in the grid.
3. **Rotate to landscape.** The tile is capped to the screen height instead of overflowing the
   grid; a 4:3 tile is then narrower than its column, which is the cap doing its job.
4. **Over Tailscale/https** the U1 plays through the relay: the tile should reach the same 4:3,
   this time from the player's own measurement rather than the URL guess.
