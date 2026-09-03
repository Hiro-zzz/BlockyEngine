# API: engine/core

[← README](../README.md) · [core](api-core.md) · [world](api-world.md) · [scene и render](api-render.md) · [gpu](api-gpu.md) · [assets и entity](api-assets.md) · [sprite](api-sprite.md) · [prop](api-prop.md) · [rig](api-rig.md) · [anim](api-anim.md) · [physics](api-physics.md) · [script](api-script.md) · [video](api-video.md)

---

## math.hpp

Собственная линейная алгебра. Почти всё `constexpr`. `Vec3` и `IVec3`
поддерживают `operator[]` — это активно используется в обходах, где ось
выбирается по индексу.

```cpp
struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct IVec3 { int32_t x, y, z; };   // блок, чанк, размер
struct Mat4;                          // column-major
```

**Векторы.** Полный набор операторов, плюс `dot`, `cross`, `normalize`,
`length`, `lengthSq`, `minv`, `maxv`, `absv`, `lerp`, `reflect`,
`maxComponent`, `minComponent`.

**Между целым и вещественным.** `toVec3(IVec3)`, `floorToInt(Vec3)`.

**Скаляры.** `radians`, `degrees`, `lerp`, `saturate`, константы `kPi`,
`kInvPi`, `kTwoPi`, `kEps`.

| Функция | Смысл |
|---|---|
| `transformPoint(m, p)` | Преобразовать точку (w = 1) |
| `transformDir(m, v)` | Преобразовать направление (w = 0) |
| `translate(t)`, `scale(s)`, `rotateAxis(axis, rad)` | Базовые преобразования |
| `lookAt(eye, target, up)` | Мировая матрица → видовая, правая система |
| `perspective(fovY, aspect, near, far)` | Проекция в диапазон OpenGL `z ∈ [−1, 1]` |
| `orthographic(halfW, halfH, near, far)` | То же для ортографии |
| `inverseAffine(m)` | Обращение произвольного аффинного преобразования |
| `orthonormalBasis(n, t, b)` | Ортонормированный базис вокруг нормали, без ветвлений |

`inverseAffine` нужен, чтобы загнать луч в локальное пространство коробки
сущности; `orthonormalBasis` — везде, где сэмплируется полусфера.

---

## bvh.hpp

Иерархия ограничивающих объёмов над осевыми коробками, намеренно не знающая,
что внутри. Хранит только коробки и индексы; чем является индекс и как луч
встречается с примитивом — целиком дело вызывающего.

```cpp
struct Aabb { Vec3 lo, hi; void expand(Vec3); void expand(const Aabb&);
              Vec3 centroid() const; float surfaceArea() const; };

void build(const std::vector<Aabb>& boxes);
bool empty() const;
int  maxDepth() const;
size_t nodeCount() const;

template <bool AnyHit = false, class Fn>
bool traverse(Vec3 origin, Vec3 invDirection, float maxDistance, Fn&& test) const;
```

Воксельному миру такое не нужно — сетка чанков **уже** ускоряющая структура, и
двухуровневый DDA едет по ней. Нужно противоположному случаю: спрайты,
разбросанные в открытом воздухе, не лежат ни на какой сетке, и линейный перебор
десяти тысяч стоит дороже луча, на который он отвечает.

**Обход.** `test(primitiveIndex, tMax)` принимает `tMax` **по ссылке**: вернуть
`true`, приняв попадание ближе `tMax`, и самому его подтянуть. Это та же
бухгалтерия, которую линейный перебор ведёт вручную, — просто отданная дереву,
чтобы поддеревья за текущим лучшим можно было отбросить целиком. Флаг `AnyHit`
останавливает обход на первом принятии: ровно то, что всегда было нужно
теневому лучу.

Разбиение — SAH по двенадцати корзинам, с откатом на медиану, когда центроид
садится точно на границу корзины. Левый ребёнок всегда следующий выделенный
узел, поэтому обходу хватает `parent + 1` и одного явного индекса на правого.

Направление передаётся **обратным**: бесконечность там намеренна и верна для
луча, параллельного плите.

Сейчас на нём живёт `SpriteSet`. `EntitySet` пока сканирует список линейно —
шестидесяти коробок хватает, — но заменить перебор этим деревом можно, не
трогая ничего снаружи.

---

## image.hpp

Два типа, разделённые намеренно.

```cpp
class Image;     // линейный HDR, Vec3 на пиксель — то, во что рендерит движок
class ImageU8;   // плотный RGBA8 — то, что лежит на диске и в текстурах
```

`Image`: `at(x, y)`, `width()`, `height()`, `data()`, `empty()`. Без гаммы и
без ограничения сверху — значения выше единицы законны и именно они дают лаву
и глоустоун.

`ImageU8`: `get`/`set` через вложенный `RGBA`, `resize`, `data()`, `storage()`.
Альфа значима: вырезы листвы, прозрачные области скина.

**Цветовые переходы.** `srgbToLinear` / `linearToSrgb` для `float` и для
`Vec3`. Текстуры Minecraft закодированы в sRGB и обязаны пройти через это до
любой математики света.

**Тонмаппинг.**

```cpp
ImageU8 tonemapToU8(const Image& hdr, const ToneParams& params = {});
```

| `ToneParams` | По умолчанию | Смысл |
|---|---|---|
| `exposure` | `1.0` | Множитель перед кривой |
| `curve` | `ACES` | `None` — точные значения для отладки; `Reinhard` — мягко, сохраняет плоскую палитру; `ACES` — киношно, с откатом светов |
| `gamma` | `1.0` | 1.0 = чистый sRGB. Поднять — приподнять тени |

---

## random.hpp

PCG32. Каждый поток рендера владеет своим, засеянным номером строки — поэтому
кадр не зависит от того, как задачи разошлись по потокам, и воспроизводится
побитово.

```cpp
Rng(uint64_t seed, uint64_t sequence = 1);

uint32_t nextUint();
float    nextFloat();          // равномерно в [0, 1)
Vec2     nextVec2();

Vec3 cosineHemisphere(Vec3 n);              // косинусный вес — то, что нужно диффузной поверхности
Vec3 uniformCone(Vec3 axis, float cosMax);  // угловой размер солнца → мягкие тени
```

---

## file.hpp

Все пути — UTF-8. На Windows узкие функции CRT идут через ANSI-кодовую
страницу и калечат любой не-ASCII путь, а проект уже лежит под таким. Поэтому
каждое обращение к файлу переводится в UTF-16.

```cpp
bool readFileBytes(path, std::vector<uint8_t>& out, std::string* error = nullptr);
bool writeFileBytes(path, const uint8_t* bytes, size_t size, std::string* error = nullptr);

bool fileExists(path);
bool isDirectory(path);
bool listDirectory(path, std::vector<DirEntry>& out);   // DirEntry: name, isDirectory
bool removeFile(path);                                  // true и тогда, когда файла не было

bool createDirectories(path);
bool createParentDirectories(filePath);
std::string parentPath(path);
```

`removeFile` появился вместе с дублями: плохой кадр пересчитывается тем, что
его стирают. Функция, которая не умеет стереть один PNG из последовательности,
оставляла бы автора в диалоге проводника — неловко после всего остального.

---

## deflate.hpp

DEFLATE (RFC 1951) и zlib (RFC 1950), написанные с нуля. Декодер понимает все
три типа блоков: stored, фиксированные и динамические коды Хаффмана. Кодер —
LZ77 по хеш-цепочкам плюс фиксированные коды.

```cpp
bool rawInflate(bytes, size, std::vector<uint8_t>& out, error);   // без обёртки — это ZIP
std::vector<uint8_t> rawDeflate(bytes, size);

bool zlibInflate(bytes, size, std::vector<uint8_t>& out, error);  // с заголовком и Adler-32 — это PNG
std::vector<uint8_t> zlibDeflate(bytes, size);

uint32_t crc32Bytes(bytes, size, seed = 0);
uint32_t adler32Bytes(bytes, size);
```

Потребителей двое, отсюда разделение: PNG заворачивает deflate в zlib-поток,
запись ZIP хранит сырой deflate без обёртки.

---

## png.hpp

Свой кодек: ни zlib, ни stb, ни libpng.

Чтение покрывает всё, что может встретиться в ресурспаке или скине — типы
цвета 0/2/3/4/6, битность 1/2/4/8/16, ключ прозрачности `tRNS` и палитровую
альфу. Чересстрочность Adam7 честно отвергается, а не поддерживается наполовину.
Запись — 8-битный RGBA с адаптивной фильтрацией строк.

```cpp
bool pngDecode(const uint8_t* bytes, size_t size, ImageU8& out, error);
bool pngLoad(const std::string& path, ImageU8& out, error);

std::vector<uint8_t> pngEncode(const ImageU8& image);
bool pngSave(path, const ImageU8& image, error);
bool pngSave(path, const Image& hdr, const ToneParams& = {}, error);   // тонмаппинг и запись
```

Родительские каталоги создаются сами.

### APNG

Тот же файл, три новых типа чанков. Единственный контейнер анимации, который
проект может писать, не заводя зависимости: данные кадра — побайтно то, что
уже даёт `pngEncode`, компрессор не тронут. Задержка кадра — рациональное
число, поэтому 24 fps это ровно `1/24`, а кадр на двойках — один кадр с
задержкой `2/24`, а не две копии. На двойках это половина рендера **и**
половина файла.

```cpp
class ApngWriter {
    bool begin(int width, int height, int plays = 0, error);   // 0 — крутить вечно
    bool addFrame(const ImageU8&, int delayNum, int delayDen, error);
    bool addEncodedFrame(const uint8_t* pngBytes, size_t size, int delayNum, int delayDen, error);
    bool save(path, error);
};
```

`addEncodedFrame` поднимает сжатые IDAT как есть: сборка дубля из готовой
последовательности стоит чтений файлов и ничего больше — ни декодирования, ни
повторного deflate. Обычный декодер неподвижных PNG всё равно видит первый
кадр: это всё обещание формата, и `test_anim` его проверяет парсером, который
не разделяет с писателем ни одной функции.

Дубль вызывает это через `assembleApng`. См. [api-anim.md](api-anim.md).

---

## zip.hpp

Ридер поверх собственного inflate. Именно он позволяет открывать клиентский
`.jar` или запакованный ресурспак **напрямую, без распаковки**.

```cpp
bool openFile(const std::string& path, error);
bool read(const std::string& name, std::vector<uint8_t>& out, error) const;

bool contains(name) const;
uint64_t sizeOf(name) const;
const std::vector<std::string>& names() const;
size_t entryCount() const;
```

Поддержаны два метода, которые реально встречаются в игровых архивах: stored и
deflate. CRC-32 каждой записи проверяется. Zip64 разбирается, если поля
переполнены.

`openFile` читает архив в память целиком: для клиентского jar это около 25 МБ,
что дешевле тысяч системных вызовов при вытаскивании мелких текстур.
