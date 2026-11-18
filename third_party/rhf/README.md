# Rhythm Heaven Fever decompilation: symbols and splits

From [conhlee/rhf](https://github.com/conhlee/rhf) at commit `02b140c9e7007e2a4ddd61c80ac3f2eb35251ed7`, a work-in-progress decompilation of the US release (`SOME01`). Licence: **CC0 1.0** (`LICENSE`).

| File | What |
|---|---|
| `SOME01/symbols.txt` | Every symbol of the US build: name, section, address, type, size (22,221 symbols, 8,103 functions) |
| `SOME01/splits.txt` | Which address ranges belong to which source file (translation unit) |

`tools/analysis/port_symbols.py` ports the function names and the translation units to the PAL build (`SOMP01`); `docs/code-analysis.md` has the results. The files are unchanged copies; update them by copying the decomp's `config/SOME01/` again and noting the new commit here.
