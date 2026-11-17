# Changelog

## 0.9.0 — first public release
- Two engines with identical behaviour: C99 core (`c/`) and JavaScript (`web/`, powers the playground).
- Language: streams and `::`, `is`/`match`, `where`/`select`, UFCS, `_` holes, exact decimals, big integers,
  objects `[k: v]`, `type`, errors as values, `file`/`http`/JSON modules.
- `next` / `stop` in loops, early `return` inside `is` branches, multi-assignment, string `.remove(i)`.
- Methods can be passed as functions: `.map(split)`.
- Standard library written in Prismflow itself (`c/lib/prelude.pr`), shadowable by user code.
- "Did you mean …?" hints for unknown names.
- Bilingual (EN/RU) playground and tutorial.
