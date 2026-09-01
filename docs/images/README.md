# README images

Raw screenshot crops live here; `make_collages.py` stitches the small ones into the collages the top-level `README.md` embeds.
After replacing or adding a crop, run:

```bash
python docs/images/make_collages.py
```

| Used in README | Built from |
|---|---|
| `hero.jpg` | `menu-stream.jpg`, `supportmatch-normal.png`, `compare-ui.png`, `assembly-menuupdate.png`, `assembly-newtypes.png` |
| `assembly.png` | `assembly-newtypes.png`, `assembly-menuupdate.png` |
| `support-matching.png` | `supportmatch-normal.png`, `supportmatch-tree.png`, `supportmatch-option.png` |
| `materials.png` | `filament-applyall.png`, `features-outerwallfilament.png`, `options-spoolman.png` |
| `print-quality.png` | `features-offsetwall.png`, `features-zoverridexy.png` |
| `compare-slices.png` | `compare-menu.png`, `compare-ui.png` |
| `options-autosave.png`, `menu-stream.jpg` | used directly |

Still worth capturing (not yet in the README): an H2D/X2D slice with filaments split across the two nozzles,
the object-list eye column with a ghosted object, a before/after of right-click → Auto-Fit Assembly, and the full Ultra preferences tab.
Aim for ~1600 px wide, dark theme to match the existing crops, no personal data in object or device names.
