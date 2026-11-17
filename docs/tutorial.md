# Prismflow: a language tutorial

## Introduction

Prismflow (formerly Prism) is a small language with no `if`, no `for` and no `while`. Conditions are written with the `is` expression, and repetition is done with streams. This tutorial goes from the first program to parallel code. You can run every example in the Prismflow playground.

The first program:

```python
print("Hello, world!")
```

In the playground, click "Run" and the result appears in the output window. If the program waits for input, an input line appears under the output window. You can also fill in the stdin field beforehand, and the program will read its lines from there.

The language rests on three ideas:

- **Only what is declared with `:=` can be changed.** `=` creates a constant, `:=` creates a mutable variable.
- **There is no `if` and no loops.** Instead, there are `is`, `when`, `match`, and streams with the foreach operator `::`.
- **An error is a value.** You can check it, or replace it with a fallback value using `??`.

A comment starts with `//` and runs to the end of the line.

## Values and variables

A name is bound to a value with `=` (a constant) or `:=` (a mutable variable). Code blocks are indented by 4 spaces. The language has no curly braces.

```
x = 10          // constant
y := 1          // mutable
y += 4          // only what is declared with := can be changed
y := y * 2      // a new value for a mutable variable also uses :=
print(x, y)     // 10 10
```

Trying to change a constant is an error with a hint:

```
x = 1
x += 1          // Error: "x" is a constant (declared with =)
```

### Numbers

There are two types: `int` (an integer of any size) and `float` (a fractional number). The `/` operator always returns a `float`. Integer division is the function `div`.

```
print(7 / 2, div(7, 2), 7 % 3, 2 ** 10)    // 3.5 3 1 1024
print(type(5), type(5.0), type("s"), type(true), type(none))
print(2 ** 100)                            // 1267650600228229401496703205376
print(int(3.9), float(2), "12".to_int(), "1.5".to_float())    // 3 2.0 12 1.5
```

A regular `float` is a double: about 15-17 significant digits, so `0.1 + 0.2 == 0.3` is false. For exact fractions there are arbitrary-precision decimal numbers. They have the same type, `float`.

- `0.1d`, `5d` — a literal with the `d` suffix gives an exact decimal number. `dec(x)` does the same from a number or a string. `dec(x, n)` also rounds to `n` digits after the decimal point.
- `precision(n)` — at the start of the program, it turns on precision for all the code. All fractional literals and division results become decimal, and `/` computes to `n` digits. `precision(0)` returns to the regular double. `precision()` shows the current value (0..1000).

Addition, subtraction and multiplication are exact (the result is rounded to `n` digits, or to 30 without `precision`). Rounding goes away from zero at `.5`. Decimals mix with `int` and with a regular `float`: the result is decimal. `sqrt`, `abs`, `floor`, `ceil`, `round(x, n)`, `**` with an integer exponent, `min`, `max`, `sum` and `avg` work without losing precision. But `sin`, `cos`, `exp`, `log`, `pi`, `random` and a power with a fractional exponent are computed as doubles.

```
print(0.1 + 0.2 == 0.3)                   // false: regular float
print(0.1d + 0.2d == 0.3d, 0.1d + 0.2d)   // true 0.3
precision(30)
print(1 / 3)                              // 0.333333333333333333333333333333
print(sqrt(2), round(2 / 3, 4))           // 1.41421356237309504880168872421 0.6667
precision(0)
```

Exact numbers are slower than doubles: about 5 µs per operation at `precision(30)`. Turn them on where you need the precision.

Indexes and counters are `int` only: `a[1.0]` gives an error, so write `a[int(1.0)]`.

### Strings, bool and none

Strings go in double quotes. Backticks substitute the values in `{...}` right away. A multiline string is written in triple quotes. `true` and `false` are of type `bool`, and `none` means the absence of a value.

```
s = "Hello"
name = "Anna"
print(s + ", world", s.len(), s.upper())             // Hello, world 5 HELLO
print(`Hello, {name}! In a year you will be {20 + 1}`)    // Hello, Anna! In a year you will be 21
print("a" * 3, "banana".replace("a", "o"))         // aaa bonono
text = """first
second"""
print(text.lines())                                // ["first", "second"]
print("a,b,c".split(","), " x ".trim())             // ["a", "b", "c"] x
```

A regular quoted string does not substitute `{name}` by itself. The substitution is done by `.interpret()` or by backticks.

## Arrays and objects

An array is `[1, 2, 3]`. An object is a named array: `[key: value]`. The empty ones are `[]` for an array and `[:]` for an object. You cannot mix keyed and unkeyed elements in one pair of brackets.

```
nums = [1, 2, 3]
user = [name: "Anna", age: 20, tags: ["a", "b"]]
print(nums, user, [], [:])        // [1, 2, 3] [name: "Anna", ...] [] [:]
print(user.name, user.tags[1], nums[0], nums[-1])    // Anna b 1 3
print(user.missing, nums[10])     // none none: a missing field or element is none
```

### Changing values

Only what lives in a `:=` variable can be changed. You can add object fields on the fly.

```python
u := [name: "Bob"]
u.age := 30             // new field
u.name := "Robert"
print(u)                // [name: "Robert", age: 30]

n := [1, 2, 3]
n += 4                  // append to the end: [1, 2, 3, 4]
n -= 2                  // remove 2 elements from the end: [1, 2]
gone = n.remove(0)      // remove by index; returns 1
print(n, gone)          // [2] 1
print(n + 9, n - 1, n)  // [2, 9] [] [2]: without := the original array does not change
```

`.pop()` removes the last element of a mutable array and returns it. `.pop(i)` removes the element at index `i`. On an empty array, or with a bad index, it is an error.

```
st := [1, 2, 3, 4]
top = st.pop()
print(top, st)          // 4 [1, 2, 3]
second = st.pop(0)
print(second, st)       // 1 [2, 3]
```

### References and copies

Arrays and objects are passed by reference. So that two names do not silently change the same thing, `b = a` for a mutable `a` is an error. You need an explicit copy or an explicit reference.

```
a := [[1], [2]]
s := a.copy()       // shallow copy: the inner arrays are shared
d := a.dcopy()      // deep copy: nothing is shared
a[0][0] := 99
print(a, s, d)      // [[99], [2]] [[99], [2]] [[1], [2]]

c := a              // reference: both names change the same array
c[1] := [7]
print(a)            // [[99], [7]]
```

`[x] * n` repeats an array and copies the elements deeply. So `[[0] * 3] * 2` gives two independent rows, not two references to one row.

```
grid := [[0] * 3] * 2
grid[0][1] := 7
print(grid)    // [[0, 7, 0], [0, 0, 0]]
```

### Destructuring

A pattern to the left of `=` spreads a value over variables. For an object it is `[name:, name:]`, and for an array it is `[a, b, ...rest]`. To rename a field, write `[field: new_name]`.

```
user = [name: "Anna", age: 20]
[age:, name:] = user       // variables age and name
[age: years] = user        // years = 20
[first, ...rest] = [1, 2, 3]
print(name, age, years, first, rest)    // Anna 20 20 1 [2, 3]
print([...[1, 2], 3], [...user, city: "Paris"])   // spread: [1, 2, 3] [name: "Anna", ...]
```

### Checking and traversing

```
user = [name: "Anna"]
print("name" in user, "x" in user, 2 in [1, 2])   // true false true
print(user.keys(), user.values())               // ["name"] ["Anna"]
[a: 1, b: 2]::k,v -> print(k, v)                // a 1 / b 2
```

### Schemas: a table into objects

An array of arrays turns into an array of objects with the given fields. Rows of different lengths are an error.

```
users = [
    [18, "Kirill"]
    [19, "Alena"]
] as [age, name]
print(users[0].age, users[1].name)    // 18 Alena
```

## Conditions

Instead of `if`, Prismflow has the expression `is condition -> value`. It returns a value, so it also works for assignment.

```
n = 7
print(is n > 5 -> "big" else -> "small")     // big

// several conditions: consecutive is lines work like else if
size = is n > 100 -> "huge"
       is n > 5 -> "big"
       else -> "small"
print(size)                                     // big
```

If you do not need the value of the expression (a statement on its own line), you can leave out `else`. If you need the value but there is no `else`, it is a syntax error.

```
is n > 3 -> print("greater than three")
```

A branch can be a block. After `->`, start a new line and write the body with an indent.

```
is n > 3 ->
    print("block")
    print("of two lines")
else ->
    print("otherwise")
```

If a branch needs only a `return`, you can write it on the same line: `is condition -> return value`. A `return` in a branch exits the whole function (more in the section "Without while: next, stop, return").

```python
f n = n match
    is n < 2 -> return 5
    else -> 7
print(f(1), f(9))     // 5 7
```

### Logic and comparisons

A condition must be a `bool`: a number or a string in place of a condition is an error. The operators are `and`, `or`, `not`, `in`. Comparisons can be chained.

```
print(0 <= n < 10, 5 in [1, 5], not (n == 7))    // true true false
print(n > 1 and n < 9, n < 1 or n > 6)           // true true
```

Arrays are compared element by element, like words in a dictionary: `[1, 2] < [1, 3]`. If one array is the beginning of another, the shorter one is smaller. The elements must be comparable with each other (numbers, strings or arrays), otherwise it is an error.

```
print([1, 2] < [1, 3])   // true
print([2] < [1, 9])      // false
print([1] < [1, 0])      // true
```

### Deferred call and when

`f:(arguments)` binds the arguments to a function and defers the call until `()`. `function when condition` calls the function with no arguments if the condition is true. If the condition is false, the left side is not evaluated.

```
job = print:("deferred")
print("before")           // before
job()                  // deferred

a = [10, 20]
i = 5
print:(a[i].name) when i < 2      // will not run
print:("i is big") when i >= 2   // i is big
```

### Safe access

`?.` gives `none` instead of an error, and `??` substitutes a fallback value for `none` and `error`.

```
user = none
print(user?.name ?? "anonymous")     // anonymous
```

## Functions

A function is declared with a header: the name, the parameters separated by spaces, `=`, and an expression. There can be no parameters: `f() = ...`. The parentheses in a call are required.

```
square n = n ** 2
add a b = a + b
print(square(7), add(1, 2))     // 49 3
```

If the body is several lines, leave the `=` at the end of the line and write the body with an indent. The result is the value of the last expression in the block. An early exit from an `is` branch is `return` (see "Without while").

```
greet name =
    text = `Hello, {name}!`
    print(text)
    return text.len()
print(greet("Anna"))             // Hello, Anna! then 11
```

A line `name =` with a body below it and no parameters is a value that is computed once. If the body contains `_`, you get a function of one parameter, and `k = _` at the start of the body gives the parameter a name. The block value is its last expression.

```
total =
    a = 2
    b = 3
    a * b
print(total)       // 6

twice =
    k = _
    k * 2
print(twice(5))    // 10: k is the function parameter

area(w, h) =
    w * h
print(area(3, 4))  // 12: the result is the last expression
```

### Cases by pattern

You can declare one function on several lines. From top to bottom, the first one whose patterns match is chosen.

```
fact 0 = 1
fact n = n * fact(n - 1)
print(fact(10))                 // 3628800

describe([age: 18]) = "eighteen"
describe([first, ...rest]) = "array, first is " + first
describe(v) = "something else"
print(describe([age: 18, x: 1]), describe([7, 8]), describe(5))
// eighteen array, first is 7 something else
```

A case can test a condition on the argument: `name < 0 = …`. The same operators work as in `is` (`<`, `>`, `<=`, `>=`, `==`, `!=`, `%` and others). You can also write the general case with `_`: a line `name = _ * …` right after the specific cases is one more case of the same function. But `_` inside a call argument is a separate function, so in recursion `fact(_ - 1)` is not the same as `fact(n - 1)`. For recursion, it is better to name the parameter.

```
sign < 0 = -1
sign 0 = 0
sign > 0 = 1
print(sign(-5), sign(0), sign(7))   // -1 0 1

fact 0 = 1
fact < 0 = none
fact n = n * fact(n - 1)
print(fact(5), fact(-3))            // 120 none
```

### Anonymous functions

There are three ways to write a function without a name. The arrow `x -> expression` names the parameter. The hole `_` is one parameter, and `_1`, `_2` are several.

```
sq = x -> x * x
inc = _ + 1
sum = _1 + _2
print(sq(6), inc(4), sum(2, 3))        // 36 5 5
print([1, 2, 3].map(sq))               // [1, 4, 9]
```

In an argument of a regular call, a whole expression with `_` becomes a function, and a bare `_` becomes a partial call.

```
twice(f, x) = f(f(x))
print(twice(_ * 2, 5))                 // 20
add10 = add(10, _)
print(add10(5))                        // 15
```

Nested holes. When `_` is in an argument inside another method argument, it refers to the nearest function by default. To refer to an outer one, write the level number: `1_` is the outermost implicit function, `2_` is the next one, and so on. `N_k` is its parameter number k. Only implicit functions in method arguments and in `::` bodies are counted. The `x -> ...` form does not take up a level. A nonexistent level is an error.

```python
xs = [1, 2]
ys = [10, 20]
print(xs.map(ys.map(1_ + _)))      // [[11, 21], [12, 22]]
print(xs.map(xs.map(xs.map(1_ * 100 + 2_ * 10 + _))))
// [[[111, 112], [121, 122]], [[211, 212], [221, 222]]]
m = [[1, 2], [3, 4]]
print(m.map(_.map(_ * 2)))         // [[2, 4], [6, 8]]: _ is the nearest one
```

A function is an ordinary value: you can pass and return its name, for example `range(1, 5).reduce(0, add)`.

## Streams instead of loops

Prismflow has no loops. A stream processes the data, and the foreach operator `::` sets the action for each element.

```
range(5)::print                 // 0 1 2 3 4 (one per line)
range(1, 10, 3)::print          // 1 4 7
range(3)::x -> print(x * 10)    // 0 10 20
["a", "b"]::i,x -> print(i, x)  // 0 a / 1 b: index and value
```

The forms are `range(end)`, `range(start, end)` and `range(start, end, step)`. The end is not included. `range` is a stream, and no array is created. For a multiline body, write it under `::`, with the element name after it. Without a name, `_` works.

```
range(3)::n
    k = n + 1
    print(`{n} → {k}`)
```

### Filtering and mapping with words

`where` keeps the matching elements, and `select` transforms each one. A leading dot `.name` means `_.name`, and `["name"]` means `_["name"]`. `obj.x` and `obj["x"]` are the same field. Brackets with one line at the start of an expression after `where`/`select` are read as field access, not as an array.

```
users = [[name: "Anna", age: 20], [name: "Bob", age: 15], [name: "Vera", age: 31]]
users where .age > 17 select .name :: print    // Anna / Vera
adults = users where _.age > 17
print(adults.len())                             // 2

names = users
    where .age > 17
    select .name.upper()
```

The short form equals a chain of methods: `users.filter(_.age > 17).map(_.name)`. To the left of `where` and `select` is a whole expression, and they themselves rank below `and` and `or` in precedence.

### Stream methods

Methods apply to an array, a string or a stream, and they form a chain. You can break a chain onto the next line if it starts with a dot.

```
print(range(1, 6).map(_ * _).to_list())       // [1, 4, 9, 16, 25]
print(range(1, 11).reduce(0, _1 + _2))        // 55
print([3, 1, 2].sort(), [3, 1, 2].sort(-_))   // [1, 2, 3] [3, 2, 1]
print([1, 2, 3, 4].sum(), [1, 2, 3, 4].avg()) // 10 2.5
print(range(7).chunk(3).to_list())            // [[0, 1, 2], [3, 4, 5], [6]]
print(range(10).any(_ > 8), range(10).all(_ > 8))   // true false
print(range(10).count(_ % 2 == 0), range(5).find(_ > 2))   // 5 3
```

A method without a call is a function with the receiver already filled in. You can pass `s.upper` along like any function. The exception is `.len`, which gives a number for a string and an array.

```python
up = "ab".upper
print(up())                          // AB
print(["a", "b"].map("x".upper))      // ["X", "X"]
print("a-b".split("-").len)          // 2
```

You can leave out the dot entirely. If a name is not defined as a variable but matches a method, it works as a "method of the first argument" function: `.map(split)` is the same as `.map(x -> x.split())`, and `split("a b")` is the same as `"a b".split()`. A variable is looked up first (your function with the same name wins), then a method, otherwise it is an error. Methods with extra arguments need a lambda: `.map(x -> x.split(","))`.

```
print(["a b", "c d e"].map(split).to_list())    // [["a", "b"], ["c", "d", "e"]]
print(["ab", "cd"].map(upper).to_list())        // ["AB", "CD"]
```

A stream is lazy: values are computed as needed, so it can be infinite. `.from(start)` turns a function into a stream: each next value is the result of the function applied to the previous one. You limit such a stream with `take` and `take_while`.

```
double i = i * 2
double.from(1).take(6)::print          // 1 2 4 8 16 32
double.from(1).take_while(_ < 50)::print
```

### Tree traversal

`.expand(children)` builds a traversal stream. The function receives an element and returns its children (an array, a stream or `none`). `.expand` goes depth-first, and `.expand_bfs` goes breadth-first. The second function in `.expand(children, exit)` adds `exit(node)` to the stream after the children of the node have been traversed. This gives an Euler tour. The stream is lazy: `.take(n)` stops the traversal.

```
adj = [[], [2, 3], [4], [], []]
print([1].expand(adj[_]).to_list())          // [1, 2, 4, 3]
print([1].expand_bfs(adj[_]).to_list())      // [1, 2, 3, 4]
print([1].expand(adj[_], -_).to_list())      // [1, 2, 4, -4, -2, 3, -3, -1]
```

The traversal does not remember visited nodes: in a graph with cycles it will not stop. For a tree given by edges, keep a `[node, parent]` pair in the element (see the example in the section "Practice"). Start a chain with an infinite traversal from `range(...)`, not from an array: over an array, the chain is evaluated element by element in full.

### Sorting by a composite key

`.sort(key)` also accepts an array: the first element is compared first, then the second. A minus reverses a numeric field.

```
words = ["elephant", "cat", "owl", "fox", "lynx"]
print(words.sort([len(_), _]))     // ["cat", "fox", "owl", "lynx", "elephant"]
people = [[name: "Anna", age: 20], [name: "Boris", age: 30], [name: "Vera", age: 20]]
print(people.sort([-_.age, _.name]).map(_.name))   // ["Boris", "Anna", "Vera"]
```

### In parallel: ::par

`xs::par x -> ...` processes the elements in parallel, so you cannot change outer variables inside it. `par` on a stream with state (`.from(...)`, `request.lines()`) is an error with an explanation.

## Without while: next, stop, return

The language will not get a `while`: repetition is set by streams and `::`. For what is usually written as a loop with `break`, `continue` and an early `return`, there are three tools.

### next and stop

`next` skips the current element, and `stop` ends the traversal. They work in the body of `::` and in the stream stages `map`, `filter`, `flat_map` (and in `select`/`where`). In `map`, `next` drops the element from the result, and `stop` cuts the stream off. They are reserved words, and outside a loop or a stream stage they are an error. You cannot use them in `::par`.

```
s := 0
range(10):: i
    is i % 2 == 0 -> next
    is i > 7 -> stop
    s += i
print(s)                                                   // 16: 1 + 3 + 5 + 7
print(range(10).map(x -> is x % 3 == 0 -> next else -> x * 10).to_list())
// [10, 20, 40, 50, 70, 80]
```

### return in branches

A `return` inside an `is`/`else`/`match` branch, including a one-line one, exits the whole function, even from nested `::`. This is how you write "find the first match". In an ordinary block (not in a branch), `return` still just sets the block value, as before. In `map`/`filter` and other stages, `return` exits the lambda of that stage.

```
find_first xs t =
    xs:: x
        is x > t -> return x
    none
print(find_first([1, 5, 9], 4), find_first([1], 4))        // 5 none

pair n =
    range(n):: i
        range(n):: j
            is i * j == 6 -> return [i, j]
    none
print(pair(5), pair(2))                                    // [2, 3] none
```

### A worklist instead of while

The body of `::` over a mutable array sees the elements appended during the traversal. This is how you write graph traversals, queues and simulations where the number of steps is not known in advance.

```
g = [[1, 2], [3], [3], []]
dist := [-1, -1, -1, -1]
dist[0] := 0
queue := [0]
queue:: v
    g[v] where dist[_] < 0 :: u
        dist[u] := dist[v] + 1
        queue += u
print(dist)                                                // [0, 1, 1, 2]
```

The Josephus problem also needs no `while` loop: there are exactly `n` people, so `range(n)` is enough.

```
josephus n k =
    ring := range(1, n + 1).to_list()
    pos := 0
    out := []
    range(n):: _t
        pos := (pos + k - 1) % len(ring)
        out += ring.remove(pos)
    out
print(josephus(7, 3))                                      // [3, 6, 2, 7, 5, 1, 4]
```

If you do not know the number of steps in advance, build a stream of steps and cut it off: `step.from(x).take_while(...)`, `until(condition)` or `unfold`.

```
print(range(1, 100).until(x -> x * x > 50).to_list())      // [1, 2, 3, 4, 5, 6, 7, 8]: including the first match
print(unfold(s -> is s > 50 -> none else -> [s, s * 2], 1).to_list())   // [1, 2, 4, 8, 16, 32]
```

If the last expression of a block is a stream, the function returns that stream and does not run it silently. The stream is "eaten up" by whoever discards the value: a standalone statement, a loop body or the top level of the program.

### Multiple assignment

All right-hand sides are evaluated before the assignment, so a swap and a shift fit on one line. The targets can be names, elements and fields. Both `=` (constants) and `:=` work.

```
a := 1
b := 2
a, b := b, a
xs := [1, 2, 3, 4]
xs[0], xs[3] := xs[3], xs[0]
print(a, b, xs)                                            // 2 1 [4, 2, 3, 1]
f0 := 0
f1 := 1
range(10):: _k
    f0, f1 := f1, f0 + f1
print(f0)                                                  // 55
```

## Standard library

Part of the library is written in Prismflow itself (the file `lib/prelude.pr`) and is available without importing. You can shadow built-in and library names with your own variable: `sum := 0` or `comb := 1` creates a new variable.

| What | Functions |
| --- | --- |
| Numbers | `lcm`, `factorial`, `comb`, `prod`, `bit_count`, `isqrt` (an error value for a negative number) |
| Streams | `until(condition)` — a stream up to and including the first match, `iterate(f, x)` (same as `f.from(x)`), `unfold(f, start)` — `f` returns `[value, next]` or `none` |
| Sorted arrays | `bisect_left(xs, x)`, `bisect_right(xs, x)`, `partition_point(xs, condition)` |
| Heap (min) | `heap_push(h, x)`, `heap_pop(h)`, `heap_peek(h)`; `h` is a mutable array, and the elements are compared as numbers, strings or arrays |
| Arrays | `transpose`, `rotate(xs, k)` |
| Combinatorics | `product(xs, ys)`, `combinations(xs, k)`, `permutations(xs)`; the result is an array of arrays |

```
print(lcm(4, 6), isqrt(99), bit_count(255), comb(5, 2))    // 12 9 8 10
h := []
[5, 1, 4].to_list():: x
    heap_push(h, x)
print(heap_pop(h), heap_pop(h), heap_peek(h))              // 1 4 5
print(permutations([1, 2, 3]).len(), combinations([1, 2, 3, 4], 2).len())   // 6 6
print(bisect_left([1, 3, 3, 5], 3), bisect_right([1, 3, 3, 5], 3))          // 1 3
```

The heap works well with `[priority, value]` pairs: arrays are compared element by element. A sorted array is already a valid heap.

Strings are immutable, so `s.remove(i)` and `s.insert(i, text)` return a new string (the index counts characters, and a negative index counts from the end): `"abcde".remove(1)` gives `"acde"`, and `"ac".insert(1, "b")` gives `"abc"`.

You can call your own functions as methods on streams too, including infinite ones: `nr.from(n).until(...)`.

## match and patterns

`value match` checks the branches from top to bottom and runs the first one that fits. Branches are written with an indent, and an `else` branch is required. In a condition you can leave out the left part: `is % 2 == 0` means "the value being tested % 2 == 0".

```
l = 4
l match
    is % 2 == 0 -> print("even")
    else -> print("odd")

x = 2
x match
    is x > 2 -> print("greater than two")
    is x > 1 ->
        print("two")       // block branch
        print("and a block")
    else -> print("less")
```

`match` returns a value, so you can assign it. A branch with a value compares it with the tested value:

```
r = x match
    is 1 -> "one"
    is 2 -> "two"
    else -> "many"
print(r)       // two
```

### Patterns

A branch can check a type (`is int`, `is error`, `is Point`) and a structure (`is [age: 18]` for an object whose `age` field equals 18, `is [first, ...rest]` for an array). The names from the pattern are available in the body of the branch.

```
describe v = v match
    is none -> "empty"
    is error -> "error: " + v.message
    is [age: 18] -> "eighteen"
    is [first, ...rest] -> "array, first is " + first
    is int -> "integer"
    is float -> "fractional"
    else -> "other"
print(describe(none), describe(10 / 0), describe([age: 18, a: 1]))
// empty error: division by zero eighteen
print(describe([7, 8]), describe(42), describe(4.5), describe("t"))
// array, first is 7 integer fractional other
```

`is number` matches any number, while `is int` and `is float` match only their own type. The same pattern works in function parameters (see "Cases by pattern").

## Types and methods

Prismflow has no classes and no inheritance. A type is a name and a list of fields, and a method is an ordinary function whose first parameter has the right type.

```
type Point [x, y]
type User [name, age = 18]      // default value

p = Point(1, 2)                 // create a value by calling the type
print(p, type(p), p is Point, p.x)
// Point[x: 1, y: 2] Point true 1
print(User("Kirill"), User("Alena", 19))
// User[name: "Kirill", age: 18] User[name: "Alena", age: 19]
```

The constructor checks only the number of arguments. Extra or missing ones give an error (`Point(1)` → `missing argument "y" in Point`).

### Methods

The type of the first parameter selects the function case, so one name works for different types. The call `p.f(a)` is the same as `f(p, a)` (UFCS, uniform call syntax). This lets you call methods on any values, including numbers and strings.

```
type Circle [r]
type Rect [w, h]
area(Circle c) = 3 * c.r ** 2
area(Rect [w:, h:]) = w * h        // the pattern unpacks the fields
print(area(Circle(2)), area(Rect(3, 4)))     // 12 12
print([Circle(1), Rect(2, 5)].map(area))     // [3, 10]

dist(Point a, Point b) = (a.x - b.x) ** 2 + (a.y - b.y) ** 2
print(p.dist(Point(4, 6)))                   // 25

double n = n * 2
shout s = s.upper() + "!"
print(5.double(), "hello".shout())     // 10 HELLO!
```

### Changing things inside a method

A function can change the object passed to it only if the object is in a `:=` variable. If a constant (`=`) is passed, it is an error: the protection depends on where the argument came from.

```
move(Point p, dx, dy) =
    p.x += dx
    p.y += dy
q := Point(0, 0)
q.move(2, 3)
print(q)               // Point[x: 2, y: 3]
// p.move(1, 1)        // Error: p is a constant

c := p.copy()          // a new copy instead of changing
c.x := 100
print(p, c)            // Point[x: 1, y: 2] Point[x: 100, y: 2]
```

## Errors as values

Division by zero, reading a number from a bad string and the like do not stop the program. They return a value of type `error`. You can store it, pass it around, check it with `is error`, and replace it using `??`.

```
r = 10 / 0
print(r, type(r), r ?? "default")
// error("division by zero") error default
print("abc".to_int() ?? 0)     // 0
print(error("my error").message)   // my error

safe n = is n < 0 -> error("negative") else -> n * 2
print(safe(4), safe(-1))         // 8 error("negative")
print(safe(-1) is error)         // true
```

A search that finds nothing returns `none`: `[1, 2, 3].find(_ > 5) ?? "not found"`. Syntax errors and violations of the language rules (changing a constant, `b = a` for a mutable `a`, a wrong type) are no longer values. They stop the program with a message and a line number.

## Input, output and modules

`print(a, b, ...)` prints the values separated by spaces and ends the line. Input goes through the `request` object. `request.line()` reads one line, and `request.lines()` gives a stream of all the remaining lines (it is a stream with state, so `par` cannot be applied to it).

```
print("What is your name?")
name = request.line()
print(`Hello, {name}!`)

// the sum of numbers up to the word stop
total = request.lines()
    .take_while(_ != "stop")
    .map(_.to_int())
    .reduce(0, _1 + _2)
print("sum: " + total)
```

`request.ints()` and `request.words()` are streams of all the integers and all the words of the input. Lines are read whole: if you take only some of the numbers from a line, the rest of that line is lost. A token that is not an integer is an error in `request.ints()`.

```
// input: 3 / 10 20 30 / hello world
[n] = request.ints().take(1).to_list()
print(request.ints().take(n).sum())     // 60
print(request.words().to_list())        // ["hello", "world"]
```

In the playground, you can fill in the input beforehand in the "Prepared input" field, or type lines right under the output window. The end of input is the button or Ctrl+D.

### Modules

Every file is a module. `from("name")` loads a file and returns an object with its top-level names. Names that start with `_` stay private. In the playground, another file is created with a new tab.

```
// file my_lib
pi = 3.14159
_secret = 42                      // private
greet name = "Hello, " + name + "!"
square n = n ** 2

// main file
lib = from("my_lib")
print(lib.greet("Kirill"), lib.square(7), lib.pi)
// Hello, Kirill! 49 3.14159
```

## Files, network and JSON

Files and network are off by default. The program that runs the language (the host) turns them on. In the console these are the flags `--files DIR` and `--http` (curl is required). In the embedded version, they are `prism_set_file_root(dir)` and `prism_set_http_handler(fn, user)` in the host code. In the playground, files are virtual (they live for the duration of one run), and HTTP goes through the browser with its CORS rules. If access is not allowed, the call returns `error("file access is disabled")` or `error("http is disabled")`.

All errors are ordinary `error` values, as in the rest of the language: no `try` is needed.

### Files

Paths are relative and only inside the allowed folder: absolute paths and `..` are rejected.

```
file.write("a.txt", "one\ntwo\n")      // overwrite
file.append("a.txt", "three\n")
text = file.read("a.txt")
print(file.lines("a.txt") where _.len > 3 select _.upper())
file.write_lines("b.txt", [1, "x", 2.5])   // each item is a line of the file
print(file.exists("a.txt"), file.list("."))
file.delete("b.txt")

r = file.read("missing.txt")
r match
    is error -> print(r.message)       // file.read: No such file or directory ("missing.txt")
    else -> print(r.len)
```

`file.lines` reads the whole file and returns an array of lines. Streaming reads for very large files are not implemented yet.

### HTTP

The response is an object `[status:, ok:, body:, headers:]`. Statuses 400 and above are returned as `error("http.get: HTTP 404")`. Header names are written with an underscore instead of a hyphen, so `content_type` is `Content-Type`. The response works the same way: `r.headers.content_type`.

```
r = http.get("https://example.com/api")
r.status                                  // 200
items = r.body.json()                     // parse JSON

http.post(url, json_str([name: "a", n: 1]), [content_type: "application/json"])
http.put(url, "text")
http.delete(url)
http.request([method: "POST", url: url, body: s, headers: [authorization: "Bearer t"], timeout: 5000])
```

The request body is a string only. You turn an object into JSON explicitly with `json_str`. Requests run one after another. There are no parallel or asynchronous requests (`::par`, `await`) yet.

### JSON

`string.json()` parses JSON into arrays, objects, numbers, strings, bool and `none`. Integers stay `int`, and very large ones become big `int`s. Fractional numbers are `float`, or decimal under `precision(n)`. `json_str(x)` builds a JSON string from a value. Parsing invalid JSON returns `error("invalid JSON at position N: …")`.

```
d = "[a: 1]".json()                      // error(...): in JSON, keys are in quotes
d = "{\"a\": 1, \"b\": [1, 2.5]}".json()
print(d.a, d.b[1], json_str(d))           // 1 2.5 {"a":1,"b":[1,2.5]}
```

## Time and concurrency

`time.wait(ms)` pauses the program or a function. Inside a function, only that function waits and the rest keeps running. Calling such a function immediately returns a *task*, and `await` waits for its result. The program finishes when the main code and all the tasks are done.

```
worker name ms =
    time.wait(ms)
    print(name + " is ready")
    return ms

a = worker("slow", 200)    // returned a task right away
b = worker("fast", 50)
print("started")                  // started
print("total:", await a + await b)    // fast is ready / slow is ready / total: 250
```

`await` on a value that is not a task returns it as is. `await [a, b]` waits for all the tasks in the array. An error inside a task is propagated to the place of `await`, or, if nobody awaited the task, to the end of the program.

### Loops and waiting

The body of a `::` operator with `time.wait` is part of the loop: the iterations go one after another. If you pass `::` the name of a function that waits, each element starts its own task, and they all run in parallel.

```
[1, 2, 3]::
    time.wait(30)
    print("step", _)        // step 1 / step 2 / step 3 — one after another

tick n =
    time.wait(90 - n * 30)
    print("tick", n)
await [1, 2, 3].map(tick)       // tick 3 / tick 2 / tick 1 — in parallel
print("done")
```

### Clocks and benchmarks

| Call | What it returns |
| --- | --- |
| `time.program()` | milliseconds since the start of the program, `float` |
| `time.time()` | the system (local) time of the device, as an object |
| `time.world()` | world time UTC+0, as an object |
| `time.test(fn, repeats)` | the average time of one run of `fn` in ms, `float`; with no second argument there is one run |

A time object has the fields `year`, `month`, `day`, `hour`, `minute`, `second`, `ms`, `unix` (ms since 1970) and `text` (`YYYY-MM-DD HH:MM:SS`).

```
t = time.world()
print(t.text, t.unix)
print(time.test(() -> range(1000).reduce(0, _1 + _2), 20), "ms")    // average over 20 runs
```

In the playground, waiting time does not count toward the ten-second run limit, and after input from the console, wait time that has already passed is not repeated.

## Reference

### Syntax at a glance

| What | Notation |
| --- | --- |
| Constant / mutable | `x = 1` / `x := 1` |
| Comment | `// text` |
| Array / object | `[1, 2]` / `[k: v]`; empty `[]` / `[:]` |
| Destructuring | `[a:, b:] = obj`, `[a: x] = obj`, `[h, ...t] = arr` |
| Slice | `s[1..4]`, `s[..3]`, `s[-3..]` |
| Schema | `rows as [age, name]` |
| Condition | `is c -> a else -> b`; a chain of `is` lines |
| Callback on a condition | `f when c` |
| Deferred call | `f:(arguments)` |
| Function | `f a b = a + b`, `f(a, b) = ...`, block: `f a =` + an indented body |
| Anonymous | `x -> x + 1`, `_ + 1`, `_1 + _2` |
| Partial call | `add(1, _)` |
| Iteration | `xs::f`, `xs::x -> ...`, `xs::i,x -> ...`, `xs::par x -> ...` |
| Filter / map | `xs where .a > 1 select .b` |
| Type | `type P [x, y = 0]` |
| Branching by pattern | `v match` + `is ... ->` branches + `else ->` |
| Fallback / safe access | `a ?? b`, `a?.b` |
| Waiting | `time.wait(ms)`, `await x` |
| Skip / end the traversal | `next`, `stop` (in `::` and stream stages) |
| Exit a function | `is c -> return x` |
| Swap / multiple assignment | `a, b := b, a` |

A block is opened by a line that ends with `=` (a function header with parameters), `->`, `::` (with or without names) or `match`. The body is written with a larger indent. Line breaks inside brackets do not matter, and a line that starts with `.`, `::`, `where` or `select` continues the previous expression. The value of a block is its last expression. An early exit and an explicit result use `return`. A block with no expressions is `none`.

### Built-in functions

| Function | What it does |
| --- | --- |
| `print(…)` | print separated by spaces |
| `range(end)`, `range(s, e)`, `range(s, e, step)` | a stream of integers, the end is not included |
| `len(x)` | the length of a string, array or object |
| `str(x)`, `type(x)` | the text of a value; the type name |
| `int(x)`, `float(x)`, `div(a, b)` | conversions; integer division |
| `abs`, `sqrt`, `floor`, `ceil`, `round`, `min`, `max` | numeric functions |
| `random()` | a number from 0 to 1 |
| `error(text)` | create an error value |
| `from("module")` | load a module |
| `request.line()`, `request.lines()` | read a line / a stream of lines |
| `time.…` | time, waiting, benchmarks |

### Methods

| On | Methods |
| --- | --- |
| Arrays and streams | `filter`, `map`, `find`, `any`, `all`, `count`, `reduce(init, f)`, `sum`, `avg`, `min`, `max`, `take(n)`, `skip(n)`, `take_while`, `skip_while`, `sort`, `sort(-_.k)`, `sort([_.a, -_.b])`, `reverse`, `zip`, `flat`, `enumerate`, `unique`, `chunk(n)`, `window(n)`, `group_by(f)`, `expand(children)`, `expand(children, exit)`, `expand_bfs(children)`, `first`, `last`, `index_of`, `join(str)`, `to_list`, `par` |
| Strings | `remove(i)`, `insert(i, text)` (return a new string), `len`, `upper`, `lower`, `trim`, `split(sep)`, `lines`, `chars`, `replace(a, b)`, `contains`, `starts_with`, `ends_with`, `to_int`, `to_float`, `interpret` |
| Objects | `keys`, `values`, `copy`, `dcopy` |
| Mutable arrays (`:=`) | `pop()`, `pop(index)`, `remove(index)`, `insert(index, x)`, `+= x`, `-= k` |
| Functions | `.from(start)` — an infinite stream of repetition |
| Input | `request.line()`, `request.lines()`, `request.words()`, `request.ints()` |

Any function of yours can be called as a method (`x.f(a)` is `f(x, a)`).

## Additions: bits, math, streams, errors

### Bitwise operations

`&`, `|`, `^`, `<<`, `>>` work on integers. The precedence is as in Python: lower than `+ -`, higher than comparisons, so `6 & 3 == 2` is `(6 & 3) == 2`. A left shift does not overflow: `1 << 70` gives a big number.

```
print(5 & 3, 5 | 3, 5 ^ 3)   // 1 7 6
print(1 << 10, 1024 >> 3)    // 1024 128
```

### Math and text

The functions are `pow(a, b)`, `pow(a, b, mod)`, `gcd(a, b)`, `divmod(a, b)` (rounding down), `sin`, `cos`, `tan`, `atan`, `atan2`, `exp`, `log(x)`, `log(x, base)`, `log2`, `log10`, the constant `pi`, `sum(xs)`, `ord(c)`, `chr(n)`, `round(x, n)` (n from 0 to 15). Number bases: `int("ff", 16)` and `(255).to_string(2)`. Strings: `.repeat(n)`, `.pad_left(n, c)`, `.pad_right(n, c)`.

```
print(pow(3, 200, 1000), gcd(84, 36), divmod(-7, 2))   // 1 12 [-4, 1]
print(round(3.14159, 2), int("-101", 2), (255).to_string(16))
print("7".pad_left(3, "0"), "ab".repeat(3))            // 007 ababab
```

### New stream and array methods

- `.scan(init, f)` and `.scan(f)` — a running total: `range(1, 6).scan(_1 + _2)` → `[1, 3, 6, 10, 15]`. The initial value is not part of the result.
- `.flat_map(f)` — `map` plus flattening one level (an array or stream result is expanded, anything else stays as is).
- `.partition(condition)` — a pair of arrays `[matching, not matching]`.
- `.min_by(f)` and `.max_by(f)` — the element with the smallest or largest key (the first one on a tie, `none` for an empty one).
- `.cycle()` — endless repetition, `.repeat(n)` — n repetitions.
- `.expand_once(children)` and `.expand_bfs_once(children)` — graph traversal with memory: a node that was already seen is not emitted and not expanded a second time. Good for graphs with cycles.

### Assignment returns a value

The last statement of a function body, if it is an assignment, gives the assigned value: `inc = i -> i += 1` returns `i + 1`. `(i -> i += 1).from(0)` works the same way.

### Arrays in a function header

In a header you can write an array pattern, including a nested one and together with ordinary parameters. A space is required before `[`, otherwise it is an index.

```
swap [a, b] = [b, a]
f [a, [b, c]] n = a + b + c + n
step [a, b] = is a % b == 0 -> [b, a / b]
    else -> [b, a % b]
```

### The `_` rule in calls

Inside an argument of a method (`.map(...)`), a `::` body or a definition `f = ...`, the `_` sign refers to that outer function, even if it sits in an argument of an ordinary call. In `f = _ * int(_ / 2)`, both `_` are the same parameter of `f`, and `int` receives a number. So `fact = _ * fact(_ - 1)` is ordinary recursion.

### Specific cases with an ordinary value

After the specific cases of a function you can write `name = value`. This is a general case with no parameters: `parity 0 = "even"`, then `parity = "odd"`.

### Error texts

Errors are printed in a technical format in English: `SyntaxError (line 3): unexpected token "}"`, `RuntimeError (line 5): cannot compare none and int with <`, `RecursionError: maximum recursion depth exceeded`. For unknown names and methods, a Python-style hint is added: `undefined variable "lenx". Did you mean: "lenn"?`, `array has no method .mapp. Did you mean: .map?` (based on edit distance; there are no hints for names shorter than three characters). The call depth limit is about 100,000 in the C version and about 600 in the playground (a browser stack limit).

## Practice

Eight small complete programs. Each one has been checked in the playground and in the C version, and the result is given in the comments.

### FizzBuzz

```
range(1, 16) :: i ->
    out = is i % 15 == 0 -> "FizzBuzz"
          is i % 3 == 0 -> "Fizz"
          is i % 5 == 0 -> "Buzz"
          else -> str(i)
    print(out)
// 1, 2, Fizz, 4, Buzz, Fizz, 7, 8, Fizz, Buzz, 11, Fizz, 13, 14, FizzBuzz
```

### Prime numbers

A condition function with `_` inside `all`: a number is prime if none of `2..n-1` divides it.

```
is_prime n = n > 1 and range(2, n).all(n % _ != 0)
print(range(1, 50).filter(is_prime).to_list())
// [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47]
```

Trial division checks every candidate against all smaller numbers. When you need many answers, build the primes once with the sieve of Eratosthenes and then only look numbers up: `n in primes` is several times faster, and asking the sieve directly (`sieve[n]`, one array access) is hundreds of times faster on large ranges.

```
limit = 5000
sieve := [true] * (limit + 1)
sieve[0] := false
sieve[1] := false
range(2, limit + 1):: i
    is sieve[i] and i * i <= limit ->
        range(i * i, limit + 1, i):: j
            sieve[j] := false
primes = range(limit + 1).filter(i -> sieve[i]).to_list()
is_prime_fast n = n in primes
print(primes.len(), primes.take(5).to_list())      // 669 [2, 3, 5, 7, 11]
print(is_prime_fast(4999), sieve[4999])             // true true
```

### Word count

```
text = "to be or not to be that is the question to be"
counts := [:]
text.split(" ") :: w ->
    counts[w] := (counts[w] ?? 0) + 1
print(counts)
// [to: 3, be: 3, or: 1, not: 1, that: 1, is: 1, the: 1, question: 1]
```

### A bank account

A type, methods with dot syntax, changes only on an object from `:=`, and an error as a value.

```
type Account [owner, balance]

deposit(Account a, sum) =
    a.balance += sum

withdraw(Account a, sum) =
    is a.balance < sum -> error("insufficient funds")
    else ->
        a.balance -= sum
        a.balance

acc := Account("Anna", 100)
acc.deposit(50)
print(acc)                  // Account[owner: "Anna", balance: 150]
print(acc.withdraw(30))     // 120
r = acc.withdraw(500)
print(r)                    // error("insufficient funds")
print(r ?? "denied")        // denied
```

### A student table

```
students = [
    [name: "Anna", grade: 5],
    [name: "Boris", grade: 3],
    [name: "Vera", grade: 4],
    [name: "Gleb", grade: 5]
]
print(students where .grade >= 4 select .name)       // ["Anna", "Vera", "Gleb"]
print(students.sort(-_.grade).map(_.name))           // ["Anna", "Gleb", "Vera", "Boris"]
print(students.map(_.grade).avg())                   // 4.25
```

### Fibonacci numbers as a stream

The state is a pair `[a, b]`, the step produces the next pair, and `.from` repeats the step endlessly.

```
step = p -> [p[1], p[0] + p[1]]
print(step.from([0, 1]).take(10).map(_[0]).to_list())
// [0, 1, 1, 2, 3, 5, 8, 13, 21, 34]
```

### The Collatz conjecture

There is no loop: `take_while` cuts off the stream of steps.

```
step n = is n % 2 == 0 -> div(n, 2)
         else -> 3 * n + 1
print(step.from(6).take_while(_ != 1).to_list())       // [6, 3, 10, 5, 16, 8, 4, 2]
print(len(step.from(27).take_while(_ != 1).to_list())) // 111
```

### Traversing a tree from input

The input is the number of nodes `n` and `n - 1` edges. A traversal element holds a `[node, parent]` pair, so that it does not go back.

```
// input: 5 / 1 2 / 1 3 / 2 4 / 3 5
[n] = request.ints().take(1).to_list()
adj := [[]] * (n + 1)
request.ints().take(2 * (n - 1)).chunk(2).to_list() :: e ->
    adj[e[0]] += e[1]
    adj[e[1]] += e[0]
kids = p -> adj[p[0]] where _ != p[1] select [_, p[0]]
print([[1, 0]].expand(kids).map(_[0]).to_list())       // [1, 2, 4, 3, 5]
print([[1, 0]].expand_bfs(kids).map(_[0]).to_list())   // [1, 2, 3, 4, 5]
```
