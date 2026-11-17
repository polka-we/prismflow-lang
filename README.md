# Prismflow

**A small, readable scripting language built around streams — no `if`, no `while`, no `for`.**

[Playground](https://polka-we.github.io/prismflow-lang/) · [Tutorial](docs/tutorial.md) · [Русская версия](README.ru.md)

```
users = [[name: "Anna", age: 20], [name: "Boris", age: 15], [name: "Vera", age: 31]]
users where .age > 17 select .name :: print      // Anna, Vera
```

Open the **[online playground](https://polka-we.github.io/prismflow-lang/)** — it runs entirely in your browser
(a single `docs/index.html`, no server) and ships with ~30 examples in English and Russian.

## What makes it different

- **No control-flow statements.** Branching is the `is` expression, repetition is `::` ("for each item of this stream").
  `next`, `stop` and early `return` exist, so real algorithms (BFS, Dijkstra, sieves) stay short.
- **Streams everywhere.** `range(1, 10).map(f).filter(g)::print` — lazy, composable, with infinite streams.
- **Words as pipeline.** `where`, `select`, `_` holes and UFCS (`x.f(a)` = `f(x, a)`) read like a sentence.
  Methods can be passed as functions: `.map(split)`.
- **Exact numbers.** Big integers, and exact decimals with `0.1d + 0.2d == 0.3d`.
- **Errors are values.** `10 / 0 ?? "fallback"`, no exceptions to forget.
- **`=` constant, `:=` mutable.** Dynamic typing, indentation blocks, `[k: v]` objects.
- **Two engines, one behaviour.** A C99 core (embeddable, runs on Android/Termux) and a JavaScript engine
  (the playground), checked against each other by 700+ shared tests.
- Helpful errors: "did you mean …?" for typos.

## Examples

```
// Sieve, then O(1)-ish lookups instead of trial division
sieve := [true] * 100001
sieve[0] := false
sieve[1] := false
range(2, 100001):: i
    is sieve[i] and i * i <= 100000 ->
        range(i * i, 100001, i):: j
            sieve[j] := false
primes = range(100001).filter(i -> sieve[i]).to_list()
print(len(primes))      // 9592
```

More in [`examples/`](examples): FizzBuzz, BFS, Dijkstra, word count, Josephus, types and methods, errors and decimals.

## Install and run

```sh
cd c && make                 # builds ./prism (also copied to ./prismflow)
./prism ../examples/fizzbuzz.pr
bash tests/run.sh            # 700+ tests
```

Needs only a C99 compiler and pthreads. A JS version: `node web/run.js file.pr`
(run `sh web/mk_engine.sh` once).

## Embedding in C

```c
#include "prism.h"
#include <stdio.h>

static void out(const char *t, size_t n, void *u) { fwrite(t, 1, n, stdout); }

int main(void) {
    PrismVM *vm = prism_new();
    prism_set_output(vm, out, NULL);
    PrismResult r = prism_run(vm, "range(5).map(_ * 2)::print");
    (void)r;                       /* see include/prism.h for PrismResult */
    prism_free(vm);
    return 0;
}
```

Compile: `cc demo.c c/src/prism.c -Ic/include -lm -lpthread`. File and HTTP access are disabled unless the host enables
them (see [SECURITY.md](SECURITY.md)).

## Performance

An interpreter, not a JIT: expect speed somewhere between Lua without JIT and CPython. Algorithmic choices matter
more than micro-tuning — see the sieve example in the tutorial.

## Repository layout

| Path | Contents |
|---|---|
| `c/` | C99 core, CLI, tests, Android bits |
| `web/` | JavaScript engine and playground sources |
| `docs/` | Built playground (`index.html`, served by GitHub Pages) and tutorials |
| `examples/` | Sample programs |

Status: 0.9, the syntax may still change before 1.0. License: [MIT](LICENSE). See [CONTRIBUTING.md](CONTRIBUTING.md).
