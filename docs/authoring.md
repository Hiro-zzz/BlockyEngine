# Написание сцен

[← README](../README.md)

Сцена — обычный `.cpp` в `scenes/`. CMake находит его сам и собирает в
`scene_<имя>.exe`. Никакого формата сцены, парсера и редактора нет намеренно:
раз сцена это код, доступны циклы, функции, отладчик и весь C++. Цена —
пересборка, но она занимает секунды. Таймлайн Блендера для этого тоже не нужен:
дубль — функция времени, а не дорожка, по которой ползут ключи.

> Этот документ — про **кадр**: описать мир, поставить свет, посчитать
> картинку. Второй режим движка — мир, который идёт, — устроен иначе, и
> пересборка там как раз неприемлема: см.
> [физику](api-physics.md), [скрипт](api-script.md) и
> [шов вьюпорта](viewport.md#шов-превращающий-смотрелку-в-хост).

---

## Рабочий цикл

1. **Набросать мир** — блоками, примитивами, генерацией.
2. **Найти ракурс** — запустить [вьюпорт](viewport.md), долететь, нажать `F`.
   Он напечатает `scene.camera.lookAt(...)` готовым C++.
3. **Подобрать свет** в режиме `draft` — уменьшенный прогон за секунду-две.
4. **Финальный рендер** с шумоподавителем и постобработкой.
5. **Если это не кадр, а дубль** — мир один раз, поза и камера — функция
   `Frame`. На двойках второй кадр пары ничего не стоит.

Все сцены движка принимают `draft`; вот скелет:

```cpp
bool draft = argc > 1 && std::strcmp(argv[1], "draft") == 0;

PathSettings settings;
settings.width  = draft ? 800 : 1600;
settings.height = draft ? 500 : 1000;
settings.samplesPerPixel = draft ? 16 : 64;
settings.maxBounces      = draft ? 4  : 8;
```

`build.cmd` аргументы не передаёт — запускай экземпляр напрямую:
`build\RelWithDebInfo\scenes\scene_demo.exe draft`.

---

## Моделирование

`fillWhere` принимает предикат по точке и ставит блоки везде, где предикат
согласен. Чего нет среди готовых форм — пишется здесь же:

```cpp
// Волна вместо пола
shape::fillWhere(world, {-30, 0, -30}, {30, 20, 30}, palette::Stone, [](Vec3 p) {
    float h = 8.0f + std::sin(p.x * 0.25f) * 2.0f + std::cos(p.z * 0.2f) * 2.0f;
    return p.y < h;
});
```

Готовые тела: `ellipsoid`, `cylinder`, `cone`, `line`, `torus`, `disc`. Плюс
`World::fillBox`, `fillHollowBox`, `fillSphere`.

### Буфер обмена

Построил один раз — размножил:

```cpp
shape::Clipboard arch = shape::copy(world, {0, 0, 0}, {6, 8, 2});

for (int i = 1; i < 5; ++i) {
    shape::paste(world, arch, {i * 8, 0, 0});
}
shape::paste(world, shape::rotatedY(arch, 1), {0, 0, 12});
shape::paste(world, shape::mirrored(arch, 0), {0, 0, 24});
```

`paste` по умолчанию не трогает то, где в буфере воздух, — так штампуют форму
на готовый рельеф.

### Смягчение

```cpp
shape::erode(world, min, max, 3, 2);   // убрать блоки с < 3 соседями, два прохода
```

Пара проходов сбивает острые края процедурной формы: цилиндр становится руиной.

---

## Генерация

### Рельеф

```cpp
int groundHeight(int x, int z) {
    float base  = noise::fbm2(x * 0.021f, z * 0.021f, 5, kSeed) * 0.5f + 0.5f;
    float ridge = noise::ridged2(x * 0.05f, z * 0.05f, 3, kSeed + 31u);

    // Радиальный спад превращает бесконечный ландшафт в остров или холм
    float d = std::sqrt(float(x*x + z*z)) / 40.0f;
    float falloff = saturate(1.0f - d * d);
    falloff *= falloff;

    return int(base * 7.0f + falloff * 22.0f + ridge * 5.0f * falloff) + 4;
}
```

Дальше по колонке раскладываются слои: верхний блок — трава или песок у воды,
три ниже — земля, глубже камень; от `height + 1` до уровня моря — вода.

### Лес

Распределение Пуассона вместо сетки с хешем — точки не ближе заданного шага,
без слипания и без выравнивания по решётке:

```cpp
std::vector<Vec2> spots = poissonDisk({-50, -50}, {50, 50}, 5.4f, kSeed);

// Форма — движка, породы — твои: пресет берёт бревно и листву аргументами.
const TreeParams species[] = {tree::oak(palette::OakLog, palette::OakLeaves),
                              tree::birch(palette::BirchLog, palette::BirchLeaves),
                              tree::spruce(palette::SpruceLog, palette::SpruceLeaves)};

for (size_t i = 0; i < spots.size(); ++i) {
    int x = int(spots[i].x), z = int(spots[i].y);

    shape::SurfacePoint surface = shape::findSurface(world, x, z, 60);
    if (!surface.found || surface.id != palette::GrassBlock) continue;
    if (shape::surfaceRoughness(world, x, z, 60, 1) > 2) continue;   // не на обрыве

    int pick = surface.block.y > 22 ? 2 : (surface.block.y > 15 ? 1 : 0);   // порода по высоте
    growTree(world, surface.block, species[pick], kSeed + uint32_t(i) * 101u);
}
```

Порода по высоте даёт полосы; чтобы они не читались как полосы, немного
перемешай через `noise::hashToFloat`.

---

## Персонажи

```cpp
// Держать за пределами цикла: набор хранит только указатели
std::vector<Skin> skins(count);
std::vector<EntityModel> models(count);
EntitySet entities;

for (size_t i = 0; i < count; ++i) {
    if (!skins[i].loadFromSource(source, paths[i], &error)) continue;
    models[i] = buildPlayerModel(skins[i]);

    Entity e;
    e.model = &models[i];
    e.skin  = &skins[i];
    e.position = {float(i) * 2.2f, groundY, 0.0f};   // ступни на этой высоте
    e.yawDegrees = 12.0f;
    e.pose = Pose::striding(24.0f);
    entities.add(e);
}
scene.entities = &entities;
```

Поставить персонажа на землю помогает `shape::findSurface`.

---

## Частицы и текст

Оба — спрайты: плоские квады с альфа-вырезом. Набор собирается один раз и
кладётся в сцену указателем, как ассеты и сущности.

```cpp
SpriteSet sprites;

// Пыль в столбе света: цилиндр вдоль направления солнца от дыры в крыше.
scatter::ParticleStyle dust;
dust.size = {0.075f, 0.075f};
dust.tint = {1.0f, 0.95f, 0.84f};
dust.randomYaw = false;                       // развернём сами, ниже

std::vector<Sprite> motes = scatter::inBeam(hole, -sunDirection, 26.0f, 2.6f, 1800, kSeed, dust);
scatter::removeInsideSolid(motes, world);     // выбросить замурованные в стену
aimAt(motes, eye);                            // к камере — и заморозить
sprites.add(motes);

// Угли над лавой. Светятся, но сцену не освещают.
scatter::ParticleStyle ember;
ember.tint = {1.0f, 0.52f, 0.16f};
ember.emission = {11.0f, 3.8f, 0.8f};
sprites.add(scatter::above(world, {-15, 0, -13}, {-4, 0, -9}, palette::Lava, 7.0f, 5, kSeed + 1u, ember));

// Надпись, развёрнутая к камере один раз и замороженная.
TextStyle style;
style.height = 1.4f;
style.color = srgbToLinear(Vec3{0.98f, 0.92f, 0.78f});
style.emission = {1.1f, 0.92f, 0.62f};
sprites.add(text::facing(font, "EMBER HALL", {-3, 12.5f, 0}, eye, style));

sprites.build();                              // без этого набор невидим
scene.sprites = &sprites;                     // обязан пережить сцену
```

Шрифт берётся из игры, а без неё — встроенный:

```cpp
Font font;
if (!font.loadFromSource(source, Font::kMinecraftAscii, nullptr)) font.useBuiltin();
```

> **Разворачивай пыль к камере.** Под случайными углами частицы показывают лучу
> то ребро, то теневую сторону, и столб света не собирается в столб. Это
> единственный не самоочевидный момент во всей подсистеме.

> **Столб виден только на тёмном.** На дневном небе шахта света не читается —
> её не с чем сравнить. Нужна крыша, дыра в ней и сумерки.

Подробности и остальные россыпи — в [api-sprite.md](api-sprite.md).

---

## Предметы и пропы

Маленькие воксельные сетки, размещаемые преобразованием, а не координатами
решётки: свободный поворот и любой масштаб, включая мельче блока.

### Из игры

```cpp
item::ItemOptions gold;
gold.metallic = 1.0f;          // в текстуре не написано, какой предмет металл
gold.roughness = 0.18f;

VoxelModel ingot;
item::loadByName(source, "gold_ingot", ingot, gold);

Prop prop;
prop.model = &ingot;           // набор хранит указатель: модель обязана пережить его
prop.position = {4.0f, groundY, 0.0f};
prop.voxelSize = 1.0f / 9.0f;  // чуть меньше двух блоков ростом
prop.yawDegrees = 17.0f;
props.add(prop);
```

### Захват из мира — главный способ для своего

Всё, что умеет `shape::`, доступно пропу через черновой мир:

```cpp
World scratch(palette::registry());
shape::cylinder(scratch, {12, 0, 12}, {0, 1, 0}, 7.0f, 18.0f, palette::OakPlanks);
shape::cylinder(scratch, {12, 3, 12}, {0, 1, 0}, 5.6f, 16.0f, block::Air);
shape::torus(scratch, {12, 3.5f, 12}, {0, 1, 0}, 7.0f, 0.9f, palette::IronBlock);

VoxelModel barrel = voxelize::fromWorld(scratch, {0, 0, 0}, {24, 20, 24});
```

Дальше бочку можно положить на бок — чего решётка не позволила бы:

```cpp
Prop tipped;
tipped.model = &barrel;
tipped.voxelSize = 0.165f;
tipped.rollDegrees = 90.0f;
tipped.yawDegrees = -34.0f;
props.add(tipped);
```

### Рисунком и формулой

```cpp
VoxelModel lantern = voxelize::fromLayers({
    {".###.", "#####", "#####", "#####", ".###."},   // y = 0
    {".....", ".#~#.", ".~~~.", ".#~#.", "....."},   // y = 1
    {".###.", "#####", "#####", "#####", ".###."},   // y = 2
}, {{'#', iron}, {'~', glass}});
```

`layers[y][z][x]`. А совсем произвольную форму пишет цикл прямо в сетку —
`model.set(...)` и всё.

```cpp
props.build();                 // без этого набор невидим
scene.props = &props;
```

> **Одну модель можно красить.** `tint` и `emissionScale` умножают палитру на
> размещении, так что десяток разноцветных кристаллов — одна сетка и десять
> матриц.

> **Не запечатывай светящееся.** Стекло фонаря, обнесённое железом со всех
> четырёх сторон, герметично и черно: свету некуда выйти. Оставь проёмы.

> **Проп не освещает сцену.** Кристалл светится и виден в отражениях, но в
> список источников не попадает. Нужен настоящий свет — поставь рядом
> `palette::Glowstone`, как это делает `scene_props`.

Подробности — в [api-prop.md](api-prop.md).

---

### Предмет в руке

Предмет — обычный `VoxelModel`, а рука — сустав. Связывает их
`VoxelAttachment`: он живёт в пространстве сустава, где один воксель равен
одному модельному пикселю, поэтому вещь из игры выходит того размера, каким её
рисует игра.

```cpp
VoxelModel pickaxe;
item::loadByName(source, "diamond_pickaxe", pickaxe, {}, nullptr);

VoxelAttachment held;
held.model = &pickaxe;
held.joint = rightElbow;                      // предплечье, если рука разрезана
held.offset = jointTip(model, rightElbow);    // кулак: низ коробки предплечья
held.anchor = gripVoxel(pickaxe);             // хват, а не центр габаритов
held.rotationDegrees = {180.0f, 90.0f, 18.0f};

PropSet props;
addAttachment(props, figure, held);
props.build();
scene.props = &props;
```

Углы почти всегда нужны, и вот почему. Предмет выдавлен из спрайта, у которого
верх это `+Y`, а конечность висит вниз по `-Y`: 180° вокруг X кладёт рукоять
вдоль предплечья. 90° вокруг Y разворачивает плоскую грань к камере — иначе в
кадре окажется двухтексельное ребро и предмет исчезнет тёмной щепкой. Всё
остальное — наклон кисти, дело вкуса.

Смещать `offset` на десятые доли пикселя нормально: `jointTip` даёт нижнюю
грань коробки, а кулак чуть выше и чуть в стороне.

Поза меняется — привязка едет с рукой сама, потому что берёт ту же матрицу
сустава, которой рендерер уплощает коробки. В дубле её, как и весь `PropSet`,
надо пересобирать каждый кадр внутри `shot`.

> Во вьюпорте предмета не будет: пропы там не рисуются вовсе.

---

## Риггинг

Скелет уже есть у каждой модели игрока: торс на поясе, голова и руки на торсе,
ноги на корне. Поэтому сгибание в поясе — одна строчка, и голова с руками идут
следом:

```cpp
Pose pose;
pose[joint::Body].rotationDegrees = {-38.0f, -12.0f, 0.0f};   // сложиться вперёд
pose[joint::Head].rotationDegrees = {32.0f, 20.0f, 0.0f};     // и посмотреть вверх
entity.pose = pose;
```

> **Знак зависит от того, с какой стороны от оси часть.** Конечность висит
> **ниже** оси, поэтому положительный поворот вокруг X уводит её вперёд. Торс
> поднимается **выше** пояса, поэтому тот же знак кладёт его назад. Челюсть
> ниже своего шарнира — значит открывается отрицательным углом.

### Тонкие детали

```cpp
EntityModel model = buildPlayerModel(skin);

int elbow = rigging::addHinge(model, joint::RightArm, "rightElbow");
int knee  = rigging::addHinge(model, joint::RightLeg, "rightKnee");
int jaw   = rigging::addJaw(model, joint::Head, 3.0f, "jaw");
int hat   = rigging::detachLayer(model, PartHead, LayerOuter, "hat");

// Бровь из нескольких текселей той грани, над которой она сидит.
SkinRect face = skin.faceRect(PartHead, LayerBase, SkinFaceFront);
int brow = rigging::attach(model,
    rigging::attachmentFrom("brow", joint::Head, {-3, 29.5f, -4.6f}, {6, 1, 1},
                            rigging::subRect(face, 1, 2, 6, 1)));

Pose pose;
pose[joint::RightArm].rotationDegrees = {64.0f, 0.0f, 26.0f};
pose[elbow].rotationDegrees = {76.0f, 0.0f, 0.0f};
pose[jaw].rotationDegrees   = {-20.0f, 0.0f, 0.0f};
pose[hat].rotationDegrees   = {0.0f, 47.0f, 0.0f};
```

Разрез режет прямоугольники скина вместе с коробкой, поэтому в покое картинка
не меняется вовсе — это проверяется побайтово.

> **Наклонять шляпу почти нельзя.** Внешний слой головы — та же коробка,
> раздутая на полпикселя, так что заметный наклон уводит козырёк на глаза.
> Разворот вокруг вертикали показывает отдельный сустав ничуть не хуже.

> **Ось можно подвинуть.** `model.skeleton.mutableJoint(j).pivot = ...` — если
> шарнир после разреза оказался не там, где нужно.

### Составные пропы

```cpp
PropRig rig;
int base = rig.addPart("post", -1, {1, 0, 1}, &postModel);
int arm  = rig.addPart("arm", base, {1, 20, 1}, &armModel);
int head = rig.addPart("lantern", arm, {1, 29, 1}, &lantern, {-1.5f, 25, -1.5f});

Pose pose;
pose[arm].rotationDegrees  = {0, 0, -62};
pose[head].rotationDegrees = {0, 0,  62};   // фонарь висит ровно

PropPlacement placement;
placement.position = {-5.2f, 2.0f, 5.0f};
placement.voxelSize = 0.155f;
addRigged(props, rig, pose, placement);
props.build();
```

Подробности — в [api-rig.md](api-rig.md).

---

## Анимация

Риг позирует, `anim/` считает кадры. Ниже этого слоя секунд не существует:
трассировщик по-прежнему рисует одну сцену. Дубль — цикл вокруг него.

Два способа написать движение, и они мешаются в одном `shot`:

- **Формула.** Качание бёдер — синус. Ключи сделали бы ту же кривую грубее.
  Так устроен `scene_sway`.
- **Трек.** Три позы по кругу, каждая стоит и потом прыгает в следующую —
  это `ease::hold`. Так устроен `scene_turntable`. Камера там всё равно
  формула: полный оборот за дубль — одна строчка тригонометрии.

```cpp
Take take;
take.name = draft ? "sway_draft" : "sway";
take.scene = &scene;
take.timing.duration = 4.0f;
take.timing.fps = 24;
take.timing.stepEvery = ones ? 1 : 2;    // на двойках — половина рендера

take.settings = settings;
take.shot = [&](Scene& s, const Frame& f) {
    const float rock = std::sin(f.time * kTwoPi / take.timing.duration);
    figure.pose = swayPose(arms, rock);
    entities.clear();
    entities.add(figure);                // add() запекает позу; без него — первый кадр вечно
    s.entities = &entities;
};
take.finish = [&](RenderTargets& targets, const Frame&) {
    return celShade(targets, cel);
};

renderTake(take);
```

> **`shot` — чистая функция `Frame`.** Часы стены, глобальный счётчик или
> незасеянный ГПСЧ внутри дубля ломают hold молча: движок копирует первый
> кадр пары, а второй должен был совпасть и не совпал.

> **Зерно одно на весь дубль.** Менять его с кадром — инстинкт, который
> превращает остаточный шум в кипящую кашу. Фиксированный сид держит спекл
> на месте.

> **Не пересобирай мир каждый кадр.** Можно, `shot` видит всю сцену. Тогда
> «дорогое, построенное один раз» перестаёт быть построенным один раз, и
> дубль на двойках внезапно стоит как на единицах, только с генерацией сверху.

Кадры пишутся в `out/<name>/0000.png`. Кадр, который уже на диске, не
считается снова; один плохой удаляется и пересчитывается. Рядом `take.txt` —
подпись настроек: поднять разрешение, не стерев каталог, — это способ получить
вчерашние кадры и отчёт об успехе.

APNG собирается сам. MP4 — по запросу (`take.writeMp4 = true`): квантование
исходников — решение, которое дубль не принимает за автора.

Подробности — [api-anim.md](api-anim.md), контейнеры — [api-video.md](api-video.md).

---

## Камера

```cpp
scene.frameAll(1.05f, 35.0f);        // изометрия: поля 5%, поворот 35°

// или вручную
scene.camera = Camera::isometric({6, 16, 6}, 26.0f, 256.0f, 35.0f);

// или перспектива с глубиной резкости
scene.camera.projection = Camera::Projection::Perspective;
scene.camera.fovY = radians(34.0f);
scene.camera.lookAt({50, 39, 56}, {-12, 31, -2});
scene.camera.aperture = 0.5f;
scene.camera.focusOn({-12, 34, -4});
```

> У ортокамеры все лучи параллельны, поэтому промах даёт ровно один цвет.
> Задай `overrideBackground` и `background`, иначе получишь плоскую заливку.

---

## Свет

```cpp
scene.sun.direction = normalize(Vec3{-0.52f, 0.66f, 0.36f});   // направление К солнцу
scene.sun.color = Vec3{1.0f, 0.90f, 0.74f};
scene.sun.intensity = 13.0f;
scene.sun.angularRadiusDegrees = 1.4f;    // больше — мягче тени

scene.sky.zenith  = Vec3{0.14f, 0.28f, 0.60f};
scene.sky.horizon = Vec3{0.68f, 0.76f, 0.90f};
scene.sky.intensity = 1.05f;
scene.ambientStrength = 0.9f;             // ниже — контрастнее и мрачнее
```

Светящиеся блоки работают источниками сами: `palette::Glowstone`, `palette::Lava`.
Замурованная в камень грань в список источников не попадает.

---

## Финальный рендер

```cpp
RenderStats stats;
RenderTargets targets;                       // вспомогательные буферы
Image raw = renderPath(scene, settings, &stats, &targets);

Image frame = denoise(targets, {});          // 1. шум

BloomSettings bloom;                         // 2. свечение
bloom.threshold = 1.4f;
bloom.intensity = 0.055f;
applyBloom(frame, bloom);

GradeSettings grade;                         // 3. цвет
grade.contrast = 1.06f;
grade.saturation = 1.08f;
grade.temperature = 0.12f;
applyGrade(frame, grade);

applyVignette(frame, {});                    // 4. виньетка
applyGrain(frame, 0.012f);                   // 5. зерно

ToneParams tone;
tone.curve = Tonemap::ACES;
pngSave("out/scene.png", frame, tone);       // 6. кривая и запись
```

**Порядок не обсуждается.** Всё до `pngSave` — в линейном свете. Любой из этих
эффектов после тонмаппинга даёт мутный вид эффекта, наложенного на уже
обрезанные значения.

Шумоподавитель экономит примерно вчетверо по сэмплам: 64 spp с ним дают то, за
чем раньше приходилось идти на 220–300.

Чтобы увидеть, что именно он делает, сохрани оба кадра — как это делает
`scene_diorama raw`.

---

## Свои материалы

```cpp
BlockRegistry registry;                       // уже содержит палитру по умолчанию

BlockDef lamp;
lamp.name = "cornell_lamp";
lamp.albedo = srgbToLinear({1.0f, 1.0f, 1.0f});
lamp.emission = {22.0f, 17.6f, 12.8f};        // линейное, выше единицы
BlockId lampId = registry.add(lamp);

scene.world = World(&registry);               // registry обязана пережить мир
```

Прозрачная среда задаётся так:

```cpp
BlockDef ice = /* ... */;
ice.opaque = false;
ice.transmission = 1.0f;
ice.ior = 1.31f;
ice.absorption = Vec3{0.05f, 0.02f, 0.01f};   // на блок пути
```

---

## Запросы к миру

Обход доступен напрямую — полезно для процедурной расстановки:

```cpp
RayHit hit;
if (raycast(world, {{x, 200.0f, z}, {0, -1, 0}}, 400.0f, hit)) {
    // hit.block.y — верхний блок колонки, hit.id — что это
}
```

Или короче — `shape::findSurface(world, x, z, topY)`.

---

## Стили рендера

Пять взглядов на одну сцену. Демонстрация — `scenes/styles.cpp`, подробности —
[api-render.md](api-render.md#стили-рендера).

### Простой — без шейдеров

Одна яркость на направление грани. Ни теневых лучей, ни неба, ни AO — самое
дешёвое, что движок умеет нарисовать, и при этом законченный вид, а не
ухудшенный предпросмотр.

```cpp
DirectSettings direct;
direct.flatLighting = true;
Image frame = renderDirect(scene, direct);

ToneParams tone;
tone.curve = Tonemap::None;      // в плоском кадре нечего сворачивать
pngSave("out/flat.png", frame, tone);
```

### Plastic

```cpp
scene.materialStyle = MaterialStyle::plastic();
```

И всё: дальше обычный `renderPath`. Блик трассируется наравне со всем
остальным, поэтому попадает в отражения и в тени. Металлы намеренно перестают
быть металлами — цветной блик и есть главный признак металла.

> **Воду стиль не трогает.** Преломляющая среда пропускается: пластиковый океан
> перестал бы быть океаном, а отслеживание среды у трассировщика завязано на
> `ior`.

### Cel

```cpp
scene.materialStyle = MaterialStyle::matte();   // 1. убрать блики

RenderTargets targets;
renderPath(scene, settings, nullptr, &targets);
targets.color = denoise(targets, {});           // 2. убрать шум

CelSettings cel;                                // 3. полосы и контур
cel.bands = 4;
cel.outlineWidth = 2;
Image frame = celShade(targets, cel);
```

Три шага, и каждый обязателен.

> **Matte — не украшение.** Блик это гладкий градиент шириной в несколько
> пикселей; квантование превращает его в лестницу жёстких колец.

> **Шумоподавитель до, а не после.** Полоса выбирается по каждому пикселю
> отдельно, поэтому остаточный шум разводит соседей по разным ступеням.
> Спекл с резкими границами заметно хуже шума, из которого он вырос.

Мало полос — резкий мультфильм, пять-шесть — мягкий комикс. `bands = 0`
оставляет один контур, и это тоже рабочий вид.

### Pixelated

```cpp
Image frame = denoise(targets, {});

PixelateSettings pixel;
pixel.factor = 8;      // сторона ячейки в пикселях
pixel.levels = 12;     // ступеней на канал, 0 — не трогать цвет
pixelate(frame, pixel);
```

Ячейка усредняется, а не берётся точечно: точечная выборка тащит шум одного
пикселя на всю ячейку, и кадр кишит светляками ровно того размера, на который
теперь смотрит глаз.

> Разрешение кадра не меняется — меняется то, сколько деталей он несёт. Чтобы
> получить настоящую маленькую картинку, уменьшай `settings.width/height`.
