const BT = '`';
const EX_RU = [
  {
    id: 'basics', name: 'Основы и схемы',
    files: { main: String.raw`// переменные: = константа, := изменяемая
x = 0
y = "Hello"
print(x + y) // 0Hello

// объект - именованный массив: [ключ: значение]
p = [age: 18, name: "Kirill"]

// схема: массивы -> объекты
a = [
    [18, "Kirill"]
    [19, "Alena"]
] as [age, name]

b = [
    [1.99, "Yogurt"]
    [0.99, "Bread"]
]
b2 = b as [price, name]

print(p.age)        // 18
print(a[0].age)     // 18
print(b2[1].name)   // Bread
print(a)

// _ - параметр безымянной функции
adults = a.filter(_.age > 18)
print(adults)
print(a.find(_.name == "Kirill"))
print(a.any(_.age > 18))
` }
  },
  {
    id: 'stopnext', name: 'stop и next',
    files: { main: String.raw`// stop завершает цикл, next переходит к следующему элементу
range(100)::i
    is i % 2 == 0 -> next
    is i > 9 -> stop
    print(i)

// то же внутри стадии потока: next отбрасывает элемент, stop обрывает поток
squares = range(1, 100).map(n ->
    is n % 3 == 0 -> next
    is n > 10 -> stop
    n * n)
print(squares.to_list())
` }
  },
  {
    id: 'bfs', name: 'Поиск в ширину (очередь)',
    files: { main: String.raw`// g[v] - список соседей вершины v
g = [[1, 2], [3], [3, 4], [5], [5], []]
dist := [-1] * 6
dist[0] := 0
queue := [0]

// очередь растёт прямо во время обхода: :: берёт и новые элементы
queue:: v
    g[v] where dist[_] < 0 :: u
        dist[u] := dist[v] + 1
        queue += u
print(dist)
` }
  },
  {
    id: 'early', name: 'Ранний return из ветки is',
    files: { main: String.raw`// return внутри ветки is выходит из всей функции, даже из вложенных ::
find_pair target =
    range(1, 10):: a
        range(a, 10):: b
            is a * b == target -> return [a, b]
    return none

print(find_pair(6))     // первая пара с произведением 6
print(find_pair(97))    // none: такой пары нет
` }
  },
  {
    id: 'swap', name: 'Обмен a, b := b, a и ссылки на методы',
    files: { main: String.raw`// множественное присваивание: правая часть вычисляется целиком
a := 1
b := 2
a, b := b, a
print(a, b)

// Фибоначчи без временной переменной
x := 0
y := 1
range(10)::
    print(x)
    x, y := y, x + y

// имя метода без вызова - ссылка на функцию
print(["a b", "c d e"].map(split))
print(["  hi ", " yo"].map(trim))
` }
  },
  {
    id: 'objects', name: 'Массивы и объекты [ ]',
    files: { main: String.raw`// массив: [1, 2, 3]; объект: [ключ: значение]; пустые: [] и [:]
nums = [1, 2, 3]
user = [name: "Аня", age: 20, tags: ["a", "b"]]
empty = [:]
print(nums, user, empty, [])
print(user.name, user.tags[1], nums[0])

// изменяемые - только через :=
u := [name: "Боря"]
u.age := 30          // поле добавляется на лету
u.name := "Борис"
print(u)

// распаковка: [имя:, имя:] для объекта, [a, b, ...rest] для массива
[age:, name:] = user
print(name, age)
[first, ...rest] = nums
print(first, rest)
[age: years] = user       // переименовать при распаковке
print(years)

// раскрытие
print([...nums, 4], [...user, age: 21])

// проверка структуры
print("age" in user, 2 in nums)
` }
  },
  {
    id: 'words', name: 'where и select',
    files: { main: String.raw`users = [
    [name: "Аня", age: 20]
    [name: "Боря", age: 15]
    [name: "Вера", age: 31]
]

// where - фильтр, select - преобразование; ведущая .имя = _.имя
users where .age > 17 select .name :: print

adults = users where _.age > 17
print(adults.len())

// цепочку можно переносить
names = users
    where .age > 17
    select .name.upper()
print(names)

// то же методами
print(users.filter(_.age > 17).map(_.name))
` }
  },
  {
    id: 'funcs', name: 'Функции: заголовки и _',
    files: { main: String.raw`// заголовок: имя параметры = выражение
square n = n ** 2
add a b = a + b
print(square(7), add(1, 2))

// блок: после "=" пустая строка-конец, тело с отступом
greet name =
    text = "Привет, {name}!"
    print(text.interpret())
greet("Kirill")

// безымянные функции: _ , _1, _2
inc = _ + 1
sum = _1 + _2
print(inc(4), sum(2, 3))
print(range(1, 6).reduce(0, _1 + _2))

// _ в обычном вызове: целое выражение - функция, голый _ - частичный вызов
twice(f, x) = f(f(x))
print(twice(_ * 2, 5))        // 20
add10 = add(10, _)
print(add10(5))               // 15

// функция - значение: имя можно передать
print(range(1, 5).reduce(0, add))

// функция по случаям: шаблоны сверху вниз
fact 0 = 1
fact n = n * fact(n - 1)
print(fact(10))

describe([age: 18]) = "ровно восемнадцать"
describe([first, ...rest]) = "массив, первый " + first
describe(v) = "что-то другое"
print(describe([age: 18, x: 1]), describe([7, 8]), describe(5))

// оценка: is-цепочка как тело
grade s = is s >= 90 -> "A"
          is s >= 80 -> "B"
          else -> "C"
print(grade(95), grade(85), grade(10))
` }
  },
  {
    id: 'ranges', name: 'range, in и срезы',
    files: { main: String.raw`// range(конец), range(начало, конец), range(начало, конец, шаг); конец не включается
range(5)::print                  // 0 1 2 3 4
range(1, 10, 3)::print           // 1 4 7
range(10, 0, -1)::print          // 10 ... 1
range(0, 100, 2)::print          // чётные до 98

// диапазон - поток: массив не создаётся
print(range(1, 6).map(_ * _).to_list())

// in работает с диапазоном, строкой, массивом, объектом
print(5 in range(1, 10), 10 in range(1, 10), 4 in range(0, 10, 2))
print("an" in "banana", 2 in [1, 2, 3], "age" in [age: 18])

// цепочка сравнений
x = 7
print(0 <= x < 10, 0 <= x < 5)

// срезы [от..до]: конец не включается, минус считает с конца
s = "привет"
print(s[1..4], s[..3], s[-3..])
print([1, 2, 3, 4, 5, 6][1..3])

// два имени после :: - индекс и значение (ключ и значение у объекта)
["a", "b"]::i,x -> print(i, x)
[name: "Kirill", age: 18]::k,v -> print(k, v)
` }
  },
  {
    id: 'isexpr', name: 'is, when и f:(...)',
    files: { main: String.raw`// is-выражение: условие без if
n = 5
size = is n > 100 -> "много" else -> "мало"
print(size)

// цепочка: строки is подряд работают как else if
score = 85
grade = is score >= 90 -> "A"
        is score >= 80 -> "B"
        else -> "C"
print(grade)

// ветка может быть блоком: тело с отступом
is n > 3 ->
    print("n больше трёх")
    print("и это блок")

// в позиции оператора else можно опустить
is n > 3 -> print("n больше трёх")

// f:(...) связывает аргументы и откладывает вызов
job = print:("отложено")
print("до вызова")
job()

// when: колбэк без аргументов вызывается, если условие истинно
a = [10, 20]
i = 5
print:(a[i].name) when i < 2      // не выполнится: a[i] не вычисляется
print:("i >= 2") when i >= 2
(() -> print("лямбда")) when true

// безопасный доступ: none вместо ошибки
user = none
print(user?.name ?? "аноним")
` }
  },
  {
    id: 'types', name: 'Типы и методы',
    files: { main: String.raw`// тип: имя и поля (можно значения по умолчанию)
type Point [x, y]
type User [name, age = 18]

p = Point(1, 2)
print(p, type(p), p is Point)
print(User("Kirill"), User("Alena", 19))

// метод - обычная функция; тип первого параметра выбирает случай
type Circle [r]
type Rect [w, h]
area(Circle c) = 3 * c.r ** 2
area(Rect [w:, h:]) = w * h
print(area(Circle(2)), area(Rect(3, 4)))
print([Circle(1), Rect(2, 5)].map(area))

// вызов через точку: p.f(a) - то же, что f(p, a)
dist(Point a, Point b) = (a.x - b.x) ** 2 + (a.y - b.y) ** 2
print(p.dist(Point(4, 6)))

// методы для встроенных типов
double n = n * 2
shout s = s.upper() + "!"
print(5.double(), "привет".shout())

// match по типу
describe v = v match
    is Point -> "точка"
    is Circle -> "круг"
    is int -> "целое"
    else -> "что-то"
print(describe(p), describe(Circle(1)), describe(3), describe("s"))

// менять на месте можно только объект из := ; константу функция менять не может
move(Point p, dx, dy) =
    p.x += dx
    p.y += dy
q := Point(0, 0)
q.move(2, 3)
print(q)
// p.move(1, 1)   // ошибка: p - константа (объявлена через =)

// новая копия вместо изменения
c := p.copy()
c.x := 100
print(p, c)
` }
  },
  {
    id: 'match', name: 'match вместо if',
    files: { main: String.raw`l = 4
// без значения слева: проверяемое значение подставляется само
l match
    is % 2 == 0 -> print("Yes")
    else -> print("No")

// условие можно писать полностью
l match
    is l % 2 == 0 -> print("Yes")
    else -> print("No")

// несколько условий, ветка-блок
x = 2
x match
    is x > 2 -> print("3")
    is x > 1 ->
        print("2")
    else -> print("0")

// match возвращает значение
is_even n = n match
    is % 2 == 0 -> true
    else -> false
print(range(10).filter(is_even).to_list())

// проверка по типу и по структуре
describe v = v match
    is none -> "пусто"
    is error -> "ошибка: " + v.message
    is [age: 18] -> "ровно восемнадцать"
    is [first, ...rest] -> "массив, первый " + first + ", ещё " + rest.len()
    is int -> "целое число"
    is float -> "дробное число"
    else -> "что-то другое"
print(describe(none))
print(describe(10 / 0))
print(describe([age: 18, name: "Kirill"]))
print(describe([7, 8, 9]))
print(describe(42), describe(4.5))
print(describe("текст"))
` }
  },
  {
    id: 'streams', name: 'Потоки: range, from, take',
    files: { main: String.raw`// :: выполняет действие для каждого элемента
range(5)::x -> print(x ** 2)

// диапазон - поток: массив не создаётся
print(range(10).filter(_ % 2 == 0).map(_ * _).to_list())

// функция превращается в бесконечный поток через .from(старт)
double i = i * 2
double.from(1).take(8)::print

// пока не выполнится условие
double.from(1).take_while(_ < 100)::print

// функция с побочным эффектом: поток сам вызывает её
l i =
    print(i)
    return i + 1
l.from(1).take_while(_ < 10)

// reduce: свернуть поток в одно значение
print(range(1, 11).reduce(0, _1 + _2))

// enumerate, zip, sort, chunk, window
["a", "b", "c"]::i,s -> print(i, s)
print([3, 1, 2].sort(), [3, 1, 2].sort(-_))
print(range(3).zip(["x", "y", "z"]))
print(range(7).chunk(3).to_list(), range(4).window(2).to_list())
print([1, 2, 3, 4].avg(), [1, 1, 2, 2].unique())
` }
  },
  {
    id: 'factorial', name: 'Факториал и Фибоначчи',
    files: { main: String.raw`// 1. по случаям
fact 0 = 1
fact n = n * fact(n - 1)
print(fact(5))

// 2. без состояния: reduce
fact2 n = range(1, n + 1).reduce(1, _1 * _2)
print(fact2(10))

// 3. через изменяемое состояние
fact3 num =
    result := 1
    range(1, num + 1)::k -> result *= k
    return result
print(fact3(5))

// Фибоначчи через := и ::
x := 0
y := 1
range(10)::
    print(x)
    nx = x + y
    x := y
    y := nx

print("--- поток ---")
// Фибоначчи как поток пар
step([a, b]) = [b, a + b]
step.from([0, 1]).take(10).map(_[0])::print
` }
  },
  {
    id: 'fizz', name: 'FizzBuzz и простые числа',
    files: { main: String.raw`fizz_buzz n = is n % 15 == 0 -> "FizzBuzz"
               is n % 3 == 0 -> "Fizz"
               is n % 5 == 0 -> "Buzz"
               else -> n
range(1, 21).map(fizz_buzz)::print

limit = 5000

// простая версия: перебор делителей для каждого числа
is_prime_slow n = is n < 2 -> false
                  else -> not range(2, n).any(n % _ == 0)
print(range(100).filter(is_prime_slow).to_list())

// быстрее: один раз строим решето Эратосфена, дальше только проверяем
sieve := [true] * (limit + 1)
sieve[0] := false
sieve[1] := false
range(2, limit + 1):: i
    is sieve[i] and i * i <= limit ->
        range(i * i, limit + 1, i):: j
            sieve[j] := false
primes = range(limit + 1).filter(i -> sieve[i]).to_list()
is_prime n = n in primes
print(primes.len(), primes.take(10).to_list())

// ещё быстрее: спрашиваем решето напрямую (одно обращение к массиву)
is_prime_fast n = sieve[n]
print(range(100).filter(is_prime_fast).to_list() == range(100).filter(is_prime_slow).to_list())

// time.test(f, повторы) = среднее время одного запуска в мс
print(round(time.test(() -> range(limit).filter(is_prime_slow).to_list(), 3), 1), "мс: перебор делителей")
print(round(time.test(() -> range(limit).filter(is_prime).to_list(), 3), 1), "мс: n in primes")
print(round(time.test(() -> range(limit).filter(is_prime_fast).to_list(), 3), 1), "мс: sieve[n]")
` }
  },
  {
    id: 'console', name: 'Консольный ввод',
    files: { main: String.raw`// поле «Заготовленный ввод» пустое: программа ждёт строки в окне вывода
print("Как вас зовут?")
name = request.line()
print(${BT}Привет, {name}!${BT})

print("Вводите строки (stop или Ctrl+D - конец):")
request.lines()
    .take_while(_ != "stop")
    ::x -> print("получено: " + x.upper())
print("ввод закончен")
` }
  },
  {
    id: 'input', name: 'Ввод: request.lines',
    stdin: '10\n20\n30\nstop\nэто уже не читается',
    files: { main: String.raw`// сумма чисел до слова stop
total = request.lines()
    .take_while(_ != "stop")
    .map(_.to_int())
    .reduce(0, _1 + _2)
print("сумма: " + total)

// одна следующая строка
rest = request.line()
print("после stop: " + rest)
` }
  },
  {
    id: 'arrays', name: 'Массивы, строки и *',
    files: { main: String.raw`arr := [1, 2, 3, 4, 5]
arr += 6            // добавить элемент в конец
print(arr)
arr -= 2            // убрать 2 элемента с конца
print(arr)
gone = arr.remove(0) // убрать по индексу
print(arr, "убрали", gone)

// без := исходный массив не меняется: получается новый
print(arr - 1, arr + 9, arr)
print([...arr, 0, ...arr])    // раскрытие

print("ab" * 3)     // ababab
print([0] * 5)      // [0, 0, 0, 0, 0]

// деструктуризация
[age:, name:] = [age: 18, name: "Kirill"]
print(name, age)
[q, w] = [1, 2]
print(q + w)

users = [[18, "Kirill"], [19, "Alena"]] as [age, name]
users::u -> print(u.name + " " + u.age)
users::u -> print(u.name)
print([...users[0], age: 20])

// строки
s = "  Hello, Prism  ".trim()
print(s.upper(), s.len(), s.split(", "))
print("a\nb".lines())

// split() без аргументов делит по пробельным символам
print("  много   пробелов  ".split())

// копии
x := [[1], [2]]
shallow := x.copy()
deep := x.dcopy()
x[0][0] := 99
print(shallow, deep)
` }
  },
  {
    id: 'interpret', name: 'Строки и .interpret()',
    files: { main: String.raw`x = 0
l = "{x} = 0"
print(l)                 // {x} = 0
print(l.interpret())     // 0 = 0
print("\{x\} = 0".interpret())   // {x} = 0

// обратные кавычки: подстановка сразу (то же, что "...".interpret())
name = "Alena"
age = 19
print(${BT}{name} - {age + 1} через год${BT})
print(${BT}{range(4).map(_ * _).to_list()}${BT})

// многострочная строка
text = """первая
вторая"""
print(text.lines())
` }
  },
  {
    id: 'numbers', name: 'int и float',
    files: { main: String.raw`print(5, 5.0, type(5), type(5.0))
print(7 / 2, 10 / 2)         // / всегда float
print(div(7, 2), div(-7, 2)) // целое деление
print(7 % 3, 2 ** 10, 2 ** 0.5)
print(1 == 1.0)               // true

// int не ограничен: большие числа точные
print(range(1, 26).reduce(1, _1 * _2))
print(2 ** 100)

// преобразования
print(int(3.9), float(2), "12".to_int(), "1.5".to_float())
print("abc".to_int() ?? "не int")

// индексы и диапазоны требуют int
a = [10, 20, 30]
print(a[1])
// print(a[1.0])   // ошибка: индекс - int; используйте a[int(1.0)]
` }
  },
  {
    id: 'time', name: 'Время и ожидание',
    files: { main: String.raw`// time.wait(мс) приостанавливает программу или функцию.
// Внутри функции ждёт только она, остальное продолжает работать.
worker name ms =
    time.wait(ms)
    print(name + " готов")
    return ms

a = worker("медленный", 300)   // сразу вернул задачу
b = worker("быстрый", 100)
print("запущено, ждём...")

// await ждёт завершения функции и даёт её результат
print("итого:", await a + await b)

// тело ::-блока с ожиданием — часть цикла: итерации идут по очереди
[1, 2, 3]::
    time.wait(50)
    print("шаг", _)

// именованная функция запускается параллельно для каждого элемента;
// await [..] ждёт их все
tick n =
    time.wait(90 - n * 30)
    print("tick", n)
await [1, 2, 3].map(tick)

// часы
print("с начала программы, мс:", int(time.program()))
t = time.world()
print("мировое время:", t.text, "| unix:", t.unix)
print("местное время:", time.time().text)

// бенчмарк: среднее время по повторам (в мс), по умолчанию 1 повтор
print("сумма 1000 чисел:", time.test(() -> range(1000).reduce(0, _1 + _2), 20), "мс")
print("ожидание 40 мс:", int(time.test(() -> time.wait(40))))
` }
  },
  {
    id: 'errors', name: 'Ошибки как значения',
    stdin: '12\nabc\n7\nstop',
    files: { main: String.raw`// деление на ноль, неверное число и т.п. возвращают значение типа error
r = 10 / 0
print(r)
print(r ?? "по умолчанию")   // ?? подменяет none и error

parsed = request.lines()
    .take_while(_ != "stop")
    .map(_.to_int())

parsed::v
    v match
        is error -> print("пропуск: " + v.message)
        else -> print("число " + v)

// find ничего не нашёл -> none
print([1, 2, 3].find(_ > 5) ?? "не найдено")
` }
  },
  {
    id: 'modules', name: 'Модули: from("my_lib")',
    files: {
      main: String.raw`lib = from("my_lib")
print(lib.greet("Kirill"))
print(lib.square(7))
print(lib.pi)
`,
      my_lib: String.raw`// всё, что объявлено на верхнем уровне, доступно снаружи
// (имена с _ в начале остаются приватными)
pi = 3.14159
_secret = 42

greet name = "Привет, " + name + "!"
square n = n ** 2
`
    }
  },
  {
    id: 'graph', name: 'Обход дерева: expand, pop',
    stdin: '5 4\n1 2\n1 3\n2 4\n3 5',
    files: { main: String.raw`// ввод: n m, затем m рёбер
[n, m] = request.ints().take(2).to_list()
edges = request.ints().take(2 * m).chunk(2).to_list()
adj := [[]] * (n + 1)          // [x] * n копирует элементы: строки независимы
edges :: e ->
    adj[e[0]] += e[1]
    adj[e[1]] += e[0]

// узел - пара [вершина, родитель]: так обход дерева не возвращается назад
kids = p -> adj[p[0]] where _ != p[1] select [_, p[0]]
print("вглубь: ", [[1, 0]].expand(kids).map(_[0]).to_list())
print("вширь:  ", [[1, 0]].expand_bfs(kids).map(_[0]).to_list())
print("вход/выход (выход со знаком -):", [[1, 0]].expand(kids, p -> [-p[0], 0]).map(_[0]).to_list())

// сортировка по составному ключу: сначала по длине, потом по алфавиту
words = ["слон", "кот", "ёж", "лиса", "рысь"]
print(words.sort([len(_), _]))

// стек
st := [1, 2, 3]
top = st.pop()
print(top, st)
` }
  },
  {
    id: 'pascal', name: 'Треугольник Паскаля',
    files: { main: String.raw`fact n = range(1, n + 1).reduce(1, _1 * _2)
choose n k = div(fact(n), fact(k) * fact(n - k))

// после :: имя элемента, затем блок с отступом
range(8)::n
    row = range(n + 1)
        .map(choose(n, _))
        .join(" ")
    print(" " * (8 - n) + row)
` }
  },
  {
    id: 'rules', name: 'Правила и ошибки',
    files: { main: String.raw`// Раскомментируйте строки по одной и посмотрите на сообщения

x = 1
// x += 1                // ошибка: x - константа

y := 1
y += 1                   // можно: y изменяемая
print(y)

// y = 5                 // ошибка: y уже изменяемая, нужно :=

// a := [1]
// b = a                 // ошибка: два имени на одно значение; пишите b = a.copy()

// range(3)::par i -> y += i   // ошибка: в ::par нельзя менять внешнее

// {a: 1}                // ошибка: фигурные скобки не используются: объект - [a: 1]

// [[1, 2], [3]] as [a, b]      // ошибка: неравная длина строк схемы

// 5 match
//     is 5 -> print("5")       // ошибка: нет else

// print(_)                     // ошибка: у _ нет функции вокруг

print("всё в порядке")
` }
  },
  {
    id: 'numbers2', name: 'Биты, математика, потоки',
    files: { main: String.raw`// побитовые операции: & | ^ << >>
print(5 & 3, 5 | 3, 5 ^ 3, 1 << 10, 1024 >> 3)

// математика
print(pow(2, 100), pow(3, 200, 1000), gcd(84, 36), divmod(-7, 2))
print(sin(pi / 2), log2(1024), round(3.14159, 2))
print((255).to_string(2), int("ff", 16), ord("a"), chr(100))

// строки
print("ab".repeat(3), "7".pad_left(3, "0"), "x".pad_right(4, "."))

// новые методы потоков
print(range(1, 6).scan(_1 + _2).to_list())            // нарастающий итог
print(range(3).flat_map(n -> [n, n * 10]).to_list())
print(range(10).partition(_ % 3 == 0))
print(["ccc", "a", "bb"].min_by(len), ["ccc", "a", "bb"].max_by(len))
print([1, 2, 3].cycle().take(7).to_list())

// граф с циклами: expand_once не заходит в вершину дважды
nbrs = n -> [n % 5 + 1, (n * 2) % 5 + 1]
print([1].expand_once(nbrs).to_list())

// присваивание возвращает значение
inc = i -> i += 1
print(inc(41))

// массив в заголовке функции
swap [a, b] = [b, a]
print(swap([1, 2]))
` }
  }
];

const EX_EN = [
  {
    id: 'basics', name: 'Basics and schemas',
    files: { main: String.raw`// variables: = constant, := mutable
x = 0
y = "Hello"
print(x + y) // 0Hello

// an object is a named array: [key: value]
p = [age: 18, name: "Kirill"]

// schema: arrays -> objects
a = [
    [18, "Kirill"]
    [19, "Alena"]
] as [age, name]

b = [
    [1.99, "Yogurt"]
    [0.99, "Bread"]
]
b2 = b as [price, name]

print(p.age)        // 18
print(a[0].age)     // 18
print(b2[1].name)   // Bread
print(a)

// _ is the parameter of an anonymous function
adults = a.filter(_.age > 18)
print(adults)
print(a.find(_.name == "Kirill"))
print(a.any(_.age > 18))
` }
  },
  {
    id: 'stopnext', name: 'stop and next',
    files: { main: String.raw`// stop ends the loop, next skips to the following element
range(100)::i
    is i % 2 == 0 -> next
    is i > 9 -> stop
    print(i)

// the same inside a stream stage: next drops the element, stop ends the stream
squares = range(1, 100).map(n ->
    is n % 3 == 0 -> next
    is n > 10 -> stop
    n * n)
print(squares.to_list())
` }
  },
  {
    id: 'bfs', name: 'Breadth-first search (queue)',
    files: { main: String.raw`// g[v] is the list of neighbours of vertex v
g = [[1, 2], [3], [3, 4], [5], [5], []]
dist := [-1] * 6
dist[0] := 0
queue := [0]

// the queue grows during the walk: :: also picks up new elements
queue:: v
    g[v] where dist[_] < 0 :: u
        dist[u] := dist[v] + 1
        queue += u
print(dist)
` }
  },
  {
    id: 'early', name: 'Early return from an is branch',
    files: { main: String.raw`// return inside an is branch leaves the whole function, even from nested ::
find_pair target =
    range(1, 10):: a
        range(a, 10):: b
            is a * b == target -> return [a, b]
    return none

print(find_pair(6))     // the first pair with product 6
print(find_pair(97))    // none: there is no such pair
` }
  },
  {
    id: 'swap', name: 'Swap a, b := b, a and method references',
    files: { main: String.raw`// multiple assignment: the right side is evaluated first
a := 1
b := 2
a, b := b, a
print(a, b)

// Fibonacci without a temporary variable
x := 0
y := 1
range(10)::
    print(x)
    x, y := y, x + y

// a method name without a call is a function reference
print(["a b", "c d e"].map(split))
print(["  hi ", " yo"].map(trim))
` }
  },
  {
    id: 'objects', name: 'Arrays and objects [ ]',
    files: { main: String.raw`// array: [1, 2, 3]; object: [key: value]; empty ones: [] and [:]
nums = [1, 2, 3]
user = [name: "Anna", age: 20, tags: ["a", "b"]]
empty = [:]
print(nums, user, empty, [])
print(user.name, user.tags[1], nums[0])

// mutable only through :=
u := [name: "Boris"]
u.age := 30          // a field is added on the fly
u.name := "Bob"
print(u)

// unpacking: [name:, name:] for an object, [a, b, ...rest] for an array
[age:, name:] = user
print(name, age)
[first, ...rest] = nums
print(first, rest)
[age: years] = user       // rename while unpacking
print(years)

// spread
print([...nums, 4], [...user, age: 21])

// structure check
print("age" in user, 2 in nums)
` }
  },
  {
    id: 'words', name: 'where and select',
    files: { main: String.raw`users = [
    [name: "Anna", age: 20]
    [name: "Boris", age: 15]
    [name: "Vera", age: 31]
]

// where filters, select transforms; a leading .name means _.name
users where .age > 17 select .name :: print

adults = users where _.age > 17
print(adults.len())

// a chain can be wrapped onto new lines
names = users
    where .age > 17
    select .name.upper()
print(names)

// the same with methods
print(users.filter(_.age > 17).map(_.name))
` }
  },
  {
    id: 'funcs', name: 'Functions: headers and _',
    files: { main: String.raw`// header: name parameters = expression
square n = n ** 2
add a b = a + b
print(square(7), add(1, 2))

// block: "=" ends the line, the body is indented
greet name =
    text = "Hello, {name}!"
    print(text.interpret())
greet("Kirill")

// anonymous functions: _ , _1, _2
inc = _ + 1
sum = _1 + _2
print(inc(4), sum(2, 3))
print(range(1, 6).reduce(0, _1 + _2))

// _ in an ordinary call: a whole expression is a function, a bare _ is a partial call
twice(f, x) = f(f(x))
print(twice(_ * 2, 5))        // 20
add10 = add(10, _)
print(add10(5))               // 15

// a function is a value: pass its name
print(range(1, 5).reduce(0, add))

// a function by cases: patterns from top to bottom
fact 0 = 1
fact n = n * fact(n - 1)
print(fact(10))

describe([age: 18]) = "exactly eighteen"
describe([first, ...rest]) = "an array, first is " + first
describe(v) = "something else"
print(describe([age: 18, x: 1]), describe([7, 8]), describe(5))

// grade: an is chain as the body
grade s = is s >= 90 -> "A"
          is s >= 80 -> "B"
          else -> "C"
print(grade(95), grade(85), grade(10))
` }
  },
  {
    id: 'ranges', name: 'range, in and slices',
    files: { main: String.raw`// range(end), range(start, end), range(start, end, step); end is excluded
range(5)::print                  // 0 1 2 3 4
range(1, 10, 3)::print           // 1 4 7
range(10, 0, -1)::print          // 10 ... 1
range(0, 100, 2)::print          // evens up to 98

// a range is a stream: no array is created
print(range(1, 6).map(_ * _).to_list())

// in works with a range, a string, an array, an object
print(5 in range(1, 10), 10 in range(1, 10), 4 in range(0, 10, 2))
print("an" in "banana", 2 in [1, 2, 3], "age" in [age: 18])

// chained comparison
x = 7
print(0 <= x < 10, 0 <= x < 5)

// slices [from..to]: end is excluded, a minus counts from the end
s = "hello"
print(s[1..4], s[..3], s[-3..])
print([1, 2, 3, 4, 5, 6][1..3])

// two names after :: are index and value (key and value for an object)
["a", "b"]::i,x -> print(i, x)
[name: "Kirill", age: 18]::k,v -> print(k, v)
` }
  },
  {
    id: 'isexpr', name: 'is, when and f:(...)',
    files: { main: String.raw`// an is expression: a condition without if
n = 5
size = is n > 100 -> "big" else -> "small"
print(size)

// a chain: consecutive is lines work like else if
score = 85
grade = is score >= 90 -> "A"
        is score >= 80 -> "B"
        else -> "C"
print(grade)

// a branch can be a block: an indented body
is n > 3 ->
    print("n is greater than three")
    print("and this is a block")

// in statement position else may be omitted
is n > 3 -> print("n is greater than three")

// f:(...) binds the arguments and defers the call
job = print:("deferred")
print("before the call")
job()

// when: a callback without arguments is called if the condition is true
a = [10, 20]
i = 5
print:(a[i].name) when i < 2      // not executed: a[i] is not evaluated
print:("i >= 2") when i >= 2
(() -> print("lambda")) when true

// safe access: none instead of an error
user = none
print(user?.name ?? "anonymous")
` }
  },
  {
    id: 'types', name: 'Types and methods',
    files: { main: String.raw`// a type: a name and fields (default values are allowed)
type Point [x, y]
type User [name, age = 18]

p = Point(1, 2)
print(p, type(p), p is Point)
print(User("Kirill"), User("Alena", 19))

// a method is an ordinary function; the type of the first parameter picks the case
type Circle [r]
type Rect [w, h]
area(Circle c) = 3 * c.r ** 2
area(Rect [w:, h:]) = w * h
print(area(Circle(2)), area(Rect(3, 4)))
print([Circle(1), Rect(2, 5)].map(area))

// dot call: p.f(a) is the same as f(p, a)
dist(Point a, Point b) = (a.x - b.x) ** 2 + (a.y - b.y) ** 2
print(p.dist(Point(4, 6)))

// methods for built-in types
double n = n * 2
shout s = s.upper() + "!"
print(5.double(), "hello".shout())

// match by type
describe v = v match
    is Point -> "point"
    is Circle -> "circle"
    is int -> "integer"
    else -> "something"
print(describe(p), describe(Circle(1)), describe(3), describe("s"))

// only an object from := can be changed in place; a function cannot change a constant
move(Point p, dx, dy) =
    p.x += dx
    p.y += dy
q := Point(0, 0)
q.move(2, 3)
print(q)
// p.move(1, 1)   // error: p is a constant (declared with =)

// a new copy instead of a change
c := p.copy()
c.x := 100
print(p, c)
` }
  },
  {
    id: 'match', name: 'match instead of if',
    files: { main: String.raw`l = 4
// no value on the left: the tested value is filled in automatically
l match
    is % 2 == 0 -> print("Yes")
    else -> print("No")

// the condition can be written in full
l match
    is l % 2 == 0 -> print("Yes")
    else -> print("No")

// several conditions, a block branch
x = 2
x match
    is x > 2 -> print("3")
    is x > 1 ->
        print("2")
    else -> print("0")

// match returns a value
is_even n = n match
    is % 2 == 0 -> true
    else -> false
print(range(10).filter(is_even).to_list())

// checks by type and by structure
describe v = v match
    is none -> "empty"
    is error -> "error: " + v.message
    is [age: 18] -> "exactly eighteen"
    is [first, ...rest] -> "an array, first is " + first + ", " + rest.len() + " more"
    is int -> "an integer"
    is float -> "a float"
    else -> "something else"
print(describe(none))
print(describe(10 / 0))
print(describe([age: 18, name: "Kirill"]))
print(describe([7, 8, 9]))
print(describe(42), describe(4.5))
print(describe("text"))
` }
  },
  {
    id: 'streams', name: 'Streams: range, from, take',
    files: { main: String.raw`// :: runs an action for every element
range(5)::x -> print(x ** 2)

// a range is a stream: no array is created
print(range(10).filter(_ % 2 == 0).map(_ * _).to_list())

// a function becomes an infinite stream through .from(start)
double i = i * 2
double.from(1).take(8)::print

// until a condition holds
double.from(1).take_while(_ < 100)::print

// a function with a side effect: the stream calls it itself
l i =
    print(i)
    return i + 1
l.from(1).take_while(_ < 10)

// reduce: fold a stream into one value
print(range(1, 11).reduce(0, _1 + _2))

// enumerate, zip, sort, chunk, window
["a", "b", "c"]::i,s -> print(i, s)
print([3, 1, 2].sort(), [3, 1, 2].sort(-_))
print(range(3).zip(["x", "y", "z"]))
print(range(7).chunk(3).to_list(), range(4).window(2).to_list())
print([1, 2, 3, 4].avg(), [1, 1, 2, 2].unique())
` }
  },
  {
    id: 'factorial', name: 'Factorial and Fibonacci',
    files: { main: String.raw`// 1. by cases
fact 0 = 1
fact n = n * fact(n - 1)
print(fact(5))

// 2. stateless: reduce
fact2 n = range(1, n + 1).reduce(1, _1 * _2)
print(fact2(10))

// 3. with mutable state
fact3 num =
    result := 1
    range(1, num + 1)::k -> result *= k
    return result
print(fact3(5))

// Fibonacci with := and ::
x := 0
y := 1
range(10)::
    print(x)
    nx = x + y
    x := y
    y := nx

print("--- stream ---")
// Fibonacci as a stream of pairs
step([a, b]) = [b, a + b]
step.from([0, 1]).take(10).map(_[0])::print
` }
  },
  {
    id: 'fizz', name: 'FizzBuzz and primes',
    files: { main: String.raw`fizz_buzz n = is n % 15 == 0 -> "FizzBuzz"
               is n % 3 == 0 -> "Fizz"
               is n % 5 == 0 -> "Buzz"
               else -> n
range(1, 21).map(fizz_buzz)::print

limit = 5000

// simple version: trial division of every candidate
is_prime_slow n = is n < 2 -> false
                  else -> not range(2, n).any(n % _ == 0)
print(range(100).filter(is_prime_slow).to_list())

// faster: build the sieve of Eratosthenes once, then only look numbers up
sieve := [true] * (limit + 1)
sieve[0] := false
sieve[1] := false
range(2, limit + 1):: i
    is sieve[i] and i * i <= limit ->
        range(i * i, limit + 1, i):: j
            sieve[j] := false
primes = range(limit + 1).filter(i -> sieve[i]).to_list()
is_prime n = n in primes
print(primes.len(), primes.take(10).to_list())

// even faster: ask the sieve directly (one array access)
is_prime_fast n = sieve[n]
print(range(100).filter(is_prime_fast).to_list() == range(100).filter(is_prime_slow).to_list())

// time.test(f, repeats) = average time of one run in ms
print(round(time.test(() -> range(limit).filter(is_prime_slow).to_list(), 3), 1), "ms for trial division")
print(round(time.test(() -> range(limit).filter(is_prime).to_list(), 3), 1), "ms for n in primes")
print(round(time.test(() -> range(limit).filter(is_prime_fast).to_list(), 3), 1), "ms for sieve[n]")
` }
  },
  {
    id: 'console', name: 'Console input',
    files: { main: String.raw`// the "Prepared input" field is empty: the program waits for lines in the output window
print("What is your name?")
name = request.line()
print(${BT}Hello, {name}!${BT})

print("Type lines (stop or Ctrl+D ends the input):")
request.lines()
    .take_while(_ != "stop")
    ::x -> print("received: " + x.upper())
print("input finished")
` }
  },
  {
    id: 'input', name: 'Input: request.lines',
    stdin: '10\n20\n30\nstop\nthis is no longer read',
    files: { main: String.raw`// the sum of numbers up to the word stop
total = request.lines()
    .take_while(_ != "stop")
    .map(_.to_int())
    .reduce(0, _1 + _2)
print("sum: " + total)

// one more line
rest = request.line()
print("after stop: " + rest)
` }
  },
  {
    id: 'arrays', name: 'Arrays, strings and *',
    files: { main: String.raw`arr := [1, 2, 3, 4, 5]
arr += 6            // append an element
print(arr)
arr -= 2            // remove 2 elements from the end
print(arr)
gone = arr.remove(0) // remove by index
print(arr, "removed", gone)

// without := the original array is not changed: a new one is made
print(arr - 1, arr + 9, arr)
print([...arr, 0, ...arr])    // spread

print("ab" * 3)     // ababab
print([0] * 5)      // [0, 0, 0, 0, 0]

// destructuring
[age:, name:] = [age: 18, name: "Kirill"]
print(name, age)
[q, w] = [1, 2]
print(q + w)

users = [[18, "Kirill"], [19, "Alena"]] as [age, name]
users::u -> print(u.name + " " + u.age)
users::u -> print(u.name)
print([...users[0], age: 20])

// strings
s = "  Hello, Prism  ".trim()
print(s.upper(), s.len(), s.split(", "))
print("a\nb".lines())

// split() without arguments splits on whitespace
print("  many   spaces  ".split())

// copies
x := [[1], [2]]
shallow := x.copy()
deep := x.dcopy()
x[0][0] := 99
print(shallow, deep)
` }
  },
  {
    id: 'interpret', name: 'Strings and .interpret()',
    files: { main: String.raw`x = 0
l = "{x} = 0"
print(l)                 // {x} = 0
print(l.interpret())     // 0 = 0
print("\{x\} = 0".interpret())   // {x} = 0

// backticks: substitution right away (same as "...".interpret())
name = "Alena"
age = 19
print(${BT}{name} - {age + 1} next year${BT})
print(${BT}{range(4).map(_ * _).to_list()}${BT})

// a multi-line string
text = """first
second"""
print(text.lines())
` }
  },
  {
    id: 'numbers', name: 'int and float',
    files: { main: String.raw`print(5, 5.0, type(5), type(5.0))
print(7 / 2, 10 / 2)         // / is always float
print(div(7, 2), div(-7, 2)) // integer division
print(7 % 3, 2 ** 10, 2 ** 0.5)
print(1 == 1.0)               // true

// int is unbounded: big numbers are exact
print(range(1, 26).reduce(1, _1 * _2))
print(2 ** 100)

// conversions
print(int(3.9), float(2), "12".to_int(), "1.5".to_float())
print("abc".to_int() ?? "not an int")

// indexes and ranges require int
a = [10, 20, 30]
print(a[1])
// print(a[1.0])   // error: an index is an int; use a[int(1.0)]
` }
  },
  {
    id: 'time', name: 'Time and waiting',
    files: { main: String.raw`// time.wait(ms) pauses the program or a function.
// Inside a function only that function waits, the rest keeps running.
worker name ms =
    time.wait(ms)
    print(name + " is ready")
    return ms

a = worker("slow", 300)   // returned a task at once
b = worker("fast", 100)
print("started, waiting...")

// await waits for a function to finish and gives its result
print("total:", await a + await b)

// the body of a ::-block with waiting is part of the loop: iterations go one by one
[1, 2, 3]::
    time.wait(50)
    print("step", _)

// a named function runs in parallel for every element;
// await [..] waits for all of them
tick n =
    time.wait(90 - n * 30)
    print("tick", n)
await [1, 2, 3].map(tick)

// clocks
print("since program start, ms:", int(time.program()))
t = time.world()
print("world time:", t.text, "| unix:", t.unix)
print("local time:", time.time().text)

// benchmark: average time over repeats (in ms), 1 repeat by default
print("sum of 1000 numbers:", time.test(() -> range(1000).reduce(0, _1 + _2), 20), "ms")
print("waiting 40 ms:", int(time.test(() -> time.wait(40))))
` }
  },
  {
    id: 'errors', name: 'Errors as values',
    stdin: '12\nabc\n7\nstop',
    files: { main: String.raw`// division by zero, an invalid number etc. return a value of type error
r = 10 / 0
print(r)
print(r ?? "default")   // ?? replaces none and error

parsed = request.lines()
    .take_while(_ != "stop")
    .map(_.to_int())

parsed::v
    v match
        is error -> print("skipped: " + v.message)
        else -> print("number " + v)

// find found nothing -> none
print([1, 2, 3].find(_ > 5) ?? "not found")
` }
  },
  {
    id: 'modules', name: 'Modules: from("my_lib")',
    files: {
      main: String.raw`lib = from("my_lib")
print(lib.greet("Kirill"))
print(lib.square(7))
print(lib.pi)
`,
      my_lib: String.raw`// everything declared at the top level is visible outside
// (names starting with _ stay private)
pi = 3.14159
_secret = 42

greet name = "Hello, " + name + "!"
square n = n ** 2
`
    }
  },
  {
    id: 'graph', name: 'Tree walk: expand, pop',
    stdin: '5 4\n1 2\n1 3\n2 4\n3 5',
    files: { main: String.raw`// input: n m, then m edges
[n, m] = request.ints().take(2).to_list()
edges = request.ints().take(2 * m).chunk(2).to_list()
adj := [[]] * (n + 1)          // [x] * n copies elements: the rows are independent
edges :: e ->
    adj[e[0]] += e[1]
    adj[e[1]] += e[0]

// a node is a [vertex, parent] pair: so the tree walk never goes back
kids = p -> adj[p[0]] where _ != p[1] select [_, p[0]]
print("depth-first: ", [[1, 0]].expand(kids).map(_[0]).to_list())
print("breadth-first:", [[1, 0]].expand_bfs(kids).map(_[0]).to_list())
print("enter/exit (exit is negative):", [[1, 0]].expand(kids, p -> [-p[0], 0]).map(_[0]).to_list())

// sorting by a composite key: first by length, then alphabetically
words = ["moose", "cat", "ox", "lynx", "fox"]
print(words.sort([len(_), _]))

// stack
st := [1, 2, 3]
top = st.pop()
print(top, st)
` }
  },
  {
    id: 'pascal', name: "Pascal's triangle",
    files: { main: String.raw`fact n = range(1, n + 1).reduce(1, _1 * _2)
choose n k = div(fact(n), fact(k) * fact(n - k))

// after :: comes the element name, then an indented block
range(8)::n
    row = range(n + 1)
        .map(choose(n, _))
        .join(" ")
    print(" " * (8 - n) + row)
` }
  },
  {
    id: 'rules', name: 'Rules and errors',
    files: { main: String.raw`// Uncomment the lines one by one and look at the messages

x = 1
// x += 1                // error: x is a constant

y := 1
y += 1                   // fine: y is mutable
print(y)

// y = 5                 // error: y is already mutable, use :=

// a := [1]
// b = a                 // error: two names for one value; write b = a.copy()

// range(3)::par i -> y += i   // error: ::par cannot change outer variables

// {a: 1}                // error: braces are not used: an object is [a: 1]

// [[1, 2], [3]] as [a, b]      // error: rows of the schema have unequal length

// 5 match
//     is 5 -> print("5")       // error: no else

// print(_)                     // error: _ has no function around it

print("everything is fine")
` }
  },
  {
    id: 'numbers2', name: 'Bits, math, streams',
    files: { main: String.raw`// bitwise operations: & | ^ << >>
print(5 & 3, 5 | 3, 5 ^ 3, 1 << 10, 1024 >> 3)

// math
print(pow(2, 100), pow(3, 200, 1000), gcd(84, 36), divmod(-7, 2))
print(sin(pi / 2), log2(1024), round(3.14159, 2))
print((255).to_string(2), int("ff", 16), ord("a"), chr(100))

// strings
print("ab".repeat(3), "7".pad_left(3, "0"), "x".pad_right(4, "."))

// newer stream methods
print(range(1, 6).scan(_1 + _2).to_list())            // running total
print(range(3).flat_map(n -> [n, n * 10]).to_list())
print(range(10).partition(_ % 3 == 0))
print(["ccc", "a", "bb"].min_by(len), ["ccc", "a", "bb"].max_by(len))
print([1, 2, 3].cycle().take(7).to_list())

// a graph with cycles: expand_once never enters a vertex twice
nbrs = n -> [n % 5 + 1, (n * 2) % 5 + 1]
print([1].expand_once(nbrs).to_list())

// assignment returns a value
inc = i -> i += 1
print(inc(41))

// an array in a function header
swap [a, b] = [b, a]
print(swap([1, 2]))
` }
  }
];

const EXAMPLES = { ru: EX_RU, en: EX_EN };
