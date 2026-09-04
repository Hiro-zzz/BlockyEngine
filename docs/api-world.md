# API: engine/world

[← README](../README.ru.md) · [core](api-core.md) · [world](api-world.md) · [scene и render](api-render.md) · [gpu](api-gpu.md) · [assets и entity](api-assets.md) · [sprite](api-sprite.md) · [prop](api-prop.md) · [rig](api-rig.md) · [anim](api-anim.md) · [physics](api-physics.md) · [script](api-script.md) · [video](api-video.md)

---

## block.hpp

Блок — это имя плюс материал плюс то, что о нём нужно знать физике. **Каких
блоков не бывает: их не задаёт движок.** Реестр приходит с одним воздухом,
а что положить дальше — вопрос к тому, кто строит мир: сцены отвечают на него
в [`scenes/common/palette.hpp`](../scenes/common/palette.hpp), Konstruct —
в [`game/blocks.hpp`](../game/blocks.hpp), тест — локальным `BlockRegistry`
на два блока.

Идентификаторы там заданы константами времени компиляции, поэтому в сцене
пишешь `palette::Glowstone`, а не строку. Идентификатор — это индекс, поэтому
порядок констант обязан совпадать с порядком регистрации.

| Поле `BlockDef` | По умолчанию | Смысл |
|---|---|---|
| `name` | — | Имя в реестре |
| `albedo` | `0.8³` | Линейный цвет. Никогда не sRGB |
| `emission` | `0` | Излучение, может быть больше единицы |
| `roughness` | `1.0` | 1 — полностью диффузно, 0 — зеркало |
| `metallic` | `0.0` | `> 0.5` переключает на проводник GGX |
| `opaque` | `true` | Останавливает и камерные, и теневые лучи |
| `solid` | `true` | Останавливает тело. Это спрашивает физика |
| `fluid` | `false` | Жидкость: не останавливает и давит на то, что внутри |
| `transmission` | `0.0` | `> 0` делает блок преломляющей средой |
| `ior` | `1.5` | Показатель преломления |
| `absorption` | `0` | Коэффициент поглощения **на блок пути** |
| `tintTop` / `topTint` | `false` | Отдельный цвет верхней грани — трава |

Помощники: `emissive()`, `transmissive()`.

`solid` и `fluid` — не одно и то же с двух сторон. Воздух тоже проходим, и
ровно одна из двух вещей держит пловца на плаву; поэтому это два флага, а не
перечисление из трёх значений, притворяющееся, что третьего не бывает.

Раньше на оба вопроса отвечали сравнением идентификатора с водой и лавой
встроенной палитры. Это было честно ровно для неё: блок, который сцена
зарегистрировала сама, выходил твёрдым, хотел он того или нет.

### Палитра сцен

Живёт в `scenes/common/palette.hpp`, общая для всего каталога `scenes/`:

```
 0  Air           7  OakLog       14  Snow         21  GoldBlock
 1  Stone         8  OakPlanks    15  Obsidian     22  Netherrack
 2  Cobblestone   9  OakLeaves    16  WhiteWool    23  BirchLog
 3  Dirt         10  Water        17  RedWool      24  BirchLeaves
 4  GrassBlock   11  Glass        18  BlackWool    25  SpruceLog
 5  Sand         12  Glowstone    19  Bricks       26  SpruceLeaves
 6  Gravel       13  Lava         20  IronBlock
```

У Konstruct своя, с теми же именами и в том же порядке, и они вправе
разойтись: игра, подбирающая свой камень, не должна править то, что рендерит
`scene_cornell`.

### Реестр

```cpp
BlockRegistry();                                   // приходит с одним воздухом
BlockId add(BlockDef def);
BlockId find(const std::string& name) const;       // block::Air, если имя неизвестно
bool    isSolid(BlockId id) const;                 // то, что спрашивает физика
bool    isFluid(BlockId id) const;
size_t  size() const;
```

Палитру передают в конструктор `World` и `Scene`; она обязана пережить мир.
Умолчания нет намеренно — «какие блоки» это вопрос, на который движку нечего
ответить.

```cpp
Scene scene(palette::registry());                  // сцена
World scratch(palette::registry());                // отдельный мир под захват
BlockRegistry own;                                 // свой, на два блока
BlockId white = own.add(matte("white", {0.73f, 0.73f, 0.73f}));
```

Сцена, которой нужны и общие блоки, и свои, копирует палитру и дописывает
конец — так делают `scene_campfire` и `scene_strike`. Дописывать в общую
нельзя: это выдало бы каждой второй сцене блок, для которого у неё нет
текстуры.

> **Вода и стекло** обрабатываются как преломляющие среды, а не как
> поверхности. Цвет воды берётся из поглощения по длине пути, а не из
> альбедо — поэтому отмель читается прозрачной, а глубина синей.

---

## world.hpp

Разреженное хранилище: хеш-карта плотных чанков 16×16×16. Пустые области не
стоят ничего, а сетка чанков заодно работает ускоряющей структурой.

```cpp
explicit World(BlockRegistry& registry);   // умолчания нет: движок не знает, какие блоки

BlockId get(IVec3 p) const;
void    set(IVec3 p, BlockId id);
bool    isOpaque(IVec3 p) const;
bool    collides(IVec3 p) const;          // BlockDef::solid через палитру мира
bool    isFluid(IVec3 p) const;           // BlockDef::fluid

void fillBox(IVec3 min, IVec3 max, BlockId);                    // границы включительно
void fillHollowBox(IVec3 min, IVec3 max, BlockId, int thickness = 1);
void fillSphere(IVec3 centre, float radius, BlockId);
void clear();

bool  hasBlocks() const;
IVec3 minBlock() const;    // габариты включительные
IVec3 maxBlock() const;
uint64_t blockCount() const;
size_t   chunkCount() const;

template <class Fn> void forEachChunk(Fn&&) const;   // так собирается список источников света
```

Запись воздуха в отсутствующий чанк не создаёт его. Отрицательные координаты
работают: перевод в координату чанка — арифметический сдвиг, который округляет
вниз корректно.

Границы при удалении блоков **не сужаются**: пересчёт был бы квадратичным, а
слегка широкая граница стоит обходу лишь пары пустых шагов.

---

## raycast.hpp

Два уровня DDA Аманатидеса — Ву. Внешний идёт по ячейкам чанков и стоит одного
обращения к хеш-карте на ячейку; внутренний идёт по блокам и работает только
внутри существующих чанков.

```cpp
struct Ray  { Vec3 origin, direction; };   // direction нормирован
struct RayHit {
    float   t;          // расстояние вдоль луча
    Vec3    position;   // точка на грани
    IVec3   block;      // координата блока
    IVec3   normal;     // одна компонента ±1
    BlockId id;
    int     axis;       // 0 = грань X, 1 = Y, 2 = Z
    Vec2    uv;         // положение на грани, обе в [0, 1)
};
```

| Запрос | Что делает |
|---|---|
| `raycast(world, ray, maxDistance, hit, filter = {})` | Первый блок, который фильтр не пропускает |
| `raycastOccluded(world, ray, maxDistance)` | Дешевле: первый непрозрачный, без данных поверхности |
| `rayTransmittance(world, ray, maxDistance)` | Доля дошедшего света по каналам |

`rayTransmittance` — ноль, если путь перекрыт непрозрачным; иначе накопленное
поглощение по Бугеру — Ламберту с учётом длины внутри каждой среды. Именно это
нужно теневому лучу со дна: солнце уже прошло сквозь толщу воды.

### RayFilter

| Поле | По умолчанию | Смысл |
|---|---|---|
| `passThrough` | `Air` | Блоки с этим id пропускаются насквозь |
| `opaqueOnly` | `false` | Игнорировать всё, что не задерживает свет |

Поставь `passThrough = palette::Water`, пока луч **внутри** воды — и обход
сообщит, где вода кончается, причём воздух там считается попаданием.

---

## noise.hpp

Градиентный шум Перлина на целочисленном хеше вместо таблицы перестановок —
любой seed работает без подготовки, а функции остаются чистыми. Результаты в
`[−1, 1]`, если не сказано иное.

```cpp
float perlin2(float x, float y, uint32_t seed = 0);
float perlin3(Vec3 p, uint32_t seed = 0);

float fbm2(x, y, int octaves, seed = 0, lacunarity = 2, gain = 0.5);
float fbm3(Vec3 p, int octaves, seed = 0, lacunarity = 2, gain = 0.5);

float ridged2(x, y, int octaves, seed = 0, lacunarity = 2, gain = 0.5);   // [0, 1], гребни гор
float hashToFloat(int32_t x, int32_t y, int32_t z, seed = 0);             // [0, 1), без Rng
```

---

## shapes.hpp

Инструменты моделирования. Всё в пространстве имён `shape`.

### Ядро

```cpp
template <class Fn>
void fillWhere(World&, IVec3 min, IVec3 max, BlockId, Fn&& accept);
```

Отдаёшь предикат по точке — он ставит блоки везде, где предикат согласен.
`accept` получает **центр блока** в мировых единицах, потому что этого хочет
геометрия. Всё остальное в заголовке — эта же функция с уже написанной формой,
а чего в ней нет, пишется в три строки без правки движка.

```cpp
shape::fillWhere(world, {-20, 0, -20}, {20, 30, 20}, palette::Stone, [](Vec3 p) {
    return std::sin(p.x * 0.3f) * 4.0f + 12.0f > p.y;
});
```

`fillWhereBlock` — то же, но предикат видит координату блока, а не точку.

### Тела

```cpp
void ellipsoid(World&, Vec3 centre, Vec3 radii, BlockId, bool hollow = false);
void cylinder(World&, Vec3 base, Vec3 axis, float radius, float height, BlockId);
void cone(World&, Vec3 base, Vec3 axis, float baseRadius, float topRadius, float height, BlockId);
void line(World&, Vec3 from, Vec3 to, float radius, BlockId);     // балки, ветви, тропы
void torus(World&, Vec3 centre, Vec3 axis, float major, float minor, BlockId);
void disc(World&, Vec3 centre, Vec3 axis, float radius, BlockId);
```

`axis` нормировать не нужно; его длина игнорируется, используется `height`.

### Правки

```cpp
void replace(World&, IVec3 min, IVec3 max, BlockId from, BlockId to);
void erode(World&, IVec3 min, IVec3 max, int minNeighbours, int passes = 1);
```

`erode` убирает блоки, у которых меньше `minNeighbours` соседей. Пара проходов
сбивает острые края процедурной формы — так башня становится руиной. Решение
принимается по снимку и применяется потом, иначе одно удаление каскадом
потянуло бы остальные.

### Буфер обмена

Это то, что превращает «расставить блоки» в моделирование: построил вещь один
раз — отзеркалил, повернул, повторил.

```cpp
struct Clipboard { IVec3 size; std::vector<BlockId> blocks; BlockId at(x,y,z); };

Clipboard copy(const World&, IVec3 min, IVec3 max);
void      paste(World&, const Clipboard&, IVec3 at, bool skipAir = true);
Clipboard mirrored(const Clipboard&, int axis);
Clipboard rotatedY(const Clipboard&, int quarterTurns);
```

`skipAir` оставляет на месте то, что уже есть, — это нужно, когда штампуешь
форму на рельеф.

### Поверхность

```cpp
struct SurfacePoint { IVec3 block; BlockId id; bool found; };

SurfacePoint findSurface(const World&, int x, int z, int topY, int bottomY = 0);
int surfaceRoughness(const World&, int x, int z, int topY, int radius = 1);
```

`surfaceRoughness` — разброс высот вокруг колонки, в блоках. Полезно, чтобы
отказаться сажать дерево на обрыве.

---

## vegetation.hpp

Дерево здесь — набор параметров, а не жёстко зашитый блоб: ствол, ветви и
крона из перекрывающихся эллипсоидов либо из сложенных конусов.

```cpp
struct TreeParams {
    BlockId log, leaves;

    int   minHeight, maxHeight;    // высота выбирается равномерно из диапазона
    float trunkRadius, lean;       // lean — снос ствола вбок по высоте

    int   branches;                // ветви растут из верхней части ствола
    float branchStart;             // доля высоты, где они начинаются
    float branchLength, branchTiltDegrees, branchRadius;

    float crownRadius, crownHeight;
    float crownDroop;              // тянет нижнюю крону наружу и вниз
    float raggedness;              // 0 — гладкая оболочка, 1 — сильно рваная

    bool  conifer;                 // хвойные строятся стопкой конусов
    int   coniferTiers;
};

namespace tree { TreeParams oak(), birch(), spruce(), bush(); }

void growTree(World&, IVec3 base, const TreeParams&, uint32_t seed);
```

`base` — блок земли; ствол начинается на один выше. Листья пишутся только туда,
где пусто, поэтому крона никогда не съедает ствол или соседнее дерево.

### Распределение

```cpp
std::vector<Vec2> poissonDisk(Vec2 min, Vec2 max, float minSpacing,
                              uint32_t seed, int attemptsPerPoint = 24);
```

Алгоритм Бридсона: точки не ближе `minSpacing` друг к другу, без слипания и
без выравнивания по решётке, которые дают случайная расстановка и сетка с
джиттером. Это и делает разбросанный лес похожим на лес.
