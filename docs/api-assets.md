# API: engine/assets и engine/entity

[← README](../README.md) · [core](api-core.md) · [world](api-world.md) · [scene и render](api-render.md) · [gpu](api-gpu.md) · [assets и entity](api-assets.md) · [sprite](api-sprite.md) · [prop](api-prop.md) · [rig](api-rig.md) · [anim](api-anim.md) · [physics](api-physics.md) · [script](api-script.md) · [video](api-video.md)

Игровые ассеты и скины сущностей — **две отдельные подсистемы**, делящие
только `Texture` и `AssetSource`. Почему так — в
[архитектуре](architecture.md#два-асимметричных-пути-ассетов).

Ассеты необязательны: без них всё рендерится плоской палитрой.

---

## texture.hpp

Хранится в линейном свете, выбирается по ближайшему текселю. Это не срезание
угла, а верный выбор: искусство Minecraft пиксельное, и билинейная фильтрация
размазала бы каждый тексель 16×16 в кашу.

```cpp
bool loadFromPng(bytes, size, error);
void fromImage(const ImageU8&);

void cropToFirstSquareFrame();                   // анимации — вертикальная полоса кадров
Texture subRegion(int x, int y, int w, int h) const;   // так часть тела получает свою текстуру

Vec3  sample(Vec2 uv) const;
float sampleAlpha(Vec2 uv) const;
Vec3  texel(int x, int y) const;
float alphaAt(int x, int y) const;

Vec3 averageColor() const;          // запасное альбедо; у пустой текстуры пурпурный
bool hasAnyTransparency() const;
bool hasPartialAlpha() const;
```

---

## asset_source.hpp

Одна абстракция и два совершенно отдельных потребителя над ней. Источник —
либо распакованный каталог, либо zip/jar; вызывающему знать какой именно не
нужно.

```cpp
bool open(const std::string& path, error);        // решает по пути сам
bool openDirectory(path, error);
bool openArchive(path, error);

bool exists(const std::string& path) const;
bool read(const std::string& path, std::vector<uint8_t>& out, error) const;
std::vector<std::string> list(const std::string& prefix) const;

static std::string findClientJar();
```

Путь всегда относительный к паку, со слэшами:
`assets/minecraft/textures/block/stone.png`.

`findClientJar` ищет установку в обычных местах, сравнивает версии **численно**
(чтобы 1.9 не оказалась старше 1.20) и пропускает профили OptiFine, Forge и
Fabric. Возвращает пустую строку, если ничего нет.

---

## game/block_textures.hpp

Игровая ветка. Множество мелких независимых картинок, адресуемых блоком и
гранью, часть анимирована полосой, часть требует биомного тинта.

```cpp
AssetSource source;
source.open(AssetSource::findClientJar());

BlockTextureLibrary textures;
if (textures.load(source, scene.world.registry(), palette::minecraftRules())) {
    scene.blockTextures = &textures;   // обязана пережить сцену
}
```

```cpp
bool load(const AssetSource&, const BlockRegistry&,
          const std::vector<BlockTextureRule>& rules, error);
bool bind(const AssetSource&, BlockId, const char* top, const char* side,
          const char* bottom, Vec3 tint = {1,1,1}, error);
Vec3 sampleAlbedo(BlockId, int face, Vec2 uv, Vec3 fallback) const;
Vec3 averageAlbedo(BlockId, int face, Vec3 fallback) const;
bool hasTexture(BlockId, int face) const;
size_t textureCount() const;
size_t texturedBlockCount() const;
const std::vector<std::string>& missing() const;
```

Блоки без текстур сохраняют плоское альбедо, поэтому неполный или экзотический
пак деградирует, а не ломает рендер. Индекс грани — из
`blockFaceIndex(normal)`, порядок `−X, +X, −Y, +Y, −Z, +Z`.

> Соответствие «блок → текстуры граней» задано плоской таблицей в
> `block_textures.cpp`. Настоящий Minecraft выводит его из дерева JSON
> блоксостояний и моделей — с наследованием и мультипарт-правилами, — и это
> отдельная крупная подсистема. Таблица покрывает нашу палитру точно, а
> заменить её загрузчиком моделей можно, не трогая ничего выше.

`bind` — тот самый шов. Таблица знает только двадцать семь блоков палитры по
умолчанию; `bind` привязывает **любое** имя из `textures/block/` к любому
идентификатору, в том числе к блоку, который сцена добавила в свой реестр:

```cpp
BlockId embers = registry.add(emberDef);
// ...
blockTextures.bind(source, embers, "campfire_log_lit", "campfire_log", "campfire_log");
```

Загрузчик моделей, когда он появится, будет вызывать именно это в цикле:
разбор блоксостояний и моделей сводится к вопросу «какая текстура на какой
грани», а это ровно то, что `bind` принимает.

Трава собирается как в ванили: серый верх умножается на биомный тинт, а на
незакрашенную землю бока композитится по альфе подкрашенное наложение
`grass_block_side_overlay`.

---

## entity/skin.hpp

Сущностная ветка. Скин — одна картинка 64×64, смысл в которой задаётся тем,
**где** что лежит: каждая часть тела это коробка, каждая её грань —
фиксированный прямоугольник.

```cpp
bool loadFromSource(const AssetSource&, const std::string& path, error);
bool loadFromPng(bytes, size, error);

SkinModel model() const;              // Classic или Slim, определяется сам
void      setModel(SkinModel);        // переопределить вручную
bool      wasLegacy() const;          // файл был 64x32 и мы его развернули

SkinRect faceRect(SkinPart, SkinLayer, SkinFace) const;
Texture  faceTexture(SkinPart, SkinLayer, SkinFace) const;
IVec3    partSize(SkinPart) const;
bool     hasLayer(SkinPart, SkinLayer) const;
```

Разбираются: современная раскладка 64×64, устаревшая 64×32 (разворачивается при
загрузке зеркальным копированием правых конечностей в левые слоты) и обе ширины
рук — classic четыре пикселя, slim три. Ширина определяется автоматически: у
slim два правых столбца блока руки прозрачны.

`faceRect` — единственный дом этой раскладки: трассировщик, вьюпорт и GPU берут
прямоугольники из `ModelBox::faces`, которые заполняются отсюда. Полоса боковых
граней идёт `[right][front][left][back]` и является **раскатом** — соседние в
файле прямоугольники соседствуют и на модели. Имена сторон — собственные для
персонажа, не для зрителя.

Перечисления, раскладка и ловушка с зеркалом — в
[соглашениях](conventions.md#раскладка-скина).

---

## entity/model.hpp

Модель — список коробок, каждая со своими шестью прямоугольниками скина. Всё в
модельных пикселях, шестнадцать на блок.

```cpp
EntityModel buildPlayerModel(const Skin&);

struct ModelBox {
    SkinPart part; SkinLayer layer;
    Vec3 origin, size, pivot;
    float inflate;                // внешние слои — оболочка вокруг базовой
    SkinRect faces[SkinFaceCount];
    bool cutout;                  // альфа-тест
};

Vec2 boxFaceUv(Vec3 local, Vec3 size, SkinFace);
Vec3 skinFaceNormal(SkinFace);
```

Двенадцать коробок: шесть частей по два слоя. Внешние раздуты на четверть
пикселя (шляпа — на половину) и помечены `cutout`: прозрачный тексель
пропускает луч дальше, к базовому слою. У устаревшего скина лишние внешние слои
отбрасываются.

> **Расширение под мобов.** `EntityModel` — просто список коробок, ничего
> игрового в нём нет. Рядом с `buildPlayerModel` встаёт `buildCreeperModel` и
> любая другая, а весь код выше даже не заметит.

---

## entity/entity.hpp

Сущность — модель плюс скин плюс размещение плюс поза. Добавление в набор один
раз уплощает её коробки в мировое пространство; дальше сцена для рендера —
плоский список ориентированных коробок.

```cpp
Skin skin;
skin.loadFromSource(source, ".../entity/player/wide/steve.png");

EntityModel model = buildPlayerModel(skin);

Entity player;
player.model = &model;          // набор хранит только указатели —
player.skin  = &skin;           // и модель, и скин обязаны его пережить
player.position = {0.0f, 0.0f, 0.0f};   // ступни на этой высоте
player.yawDegrees = 20.0f;
player.pose = Pose::waving();

EntitySet entities;
entities.add(player);
scene.entities = &entities;
```

```cpp
Pose::standing();
Pose::striding(float degrees);
Pose::waving(float degrees = 150);
Pose::tPose();
// своя поза — pose[joint::Head].rotationDegrees и так далее; суставы и их
// иерархия описаны в api-rig.md, там же инструменты для локтей и челюсти.
// Чтобы поза менялась со временем — api-anim.md: риг секунд не знает.

void EntitySet::add(const Entity&);
bool EntitySet::intersect(const Ray&, float maxDistance, EntityHit&) const;
bool EntitySet::occluded(const Ray&, float maxDistance) const;
bool EntitySet::bounds(Vec3& lo, Vec3& hi) const;
const std::vector<Entity>& EntitySet::entities() const;   // для меширования во вьюпорте
```

`EntityHit` сообщает `part` и `layer` помимо геометрии, поэтому по попаданию
видно, во что именно попал луч.

Поза — значение, не клип. Чтобы она менялась от кадра к кадру, см.
[api-anim.md](api-anim.md): `EntitySet` пересобирается внутри `shot`, потому
что `add()` запекает коробки сразу.

> **Время жизни.** `EntitySet` хранит указатели на модель и скин. Если держишь
> их в `std::vector`, задай размер **заранее** — перевыделение оставит набор с
> висячими указателями.

Знаки поворотов и оси — в [соглашениях](conventions.md#ориентация-сущностей).

---

## entity/attach.hpp

Воксельные элементы на суставах сущности: инструмент в кулаке, фонарь на
поясе, рог на шлеме.

Оба конца этого существовали и не встречались. `rigging::attach` вешает на
сустав **коробку**, но коробка одевается в скин — это ещё персонаж, а не
отдельный предмет. `PropRig` двигает целые `VoxelModel`, но по своему
скелету, а не по скелету персонажа.

```cpp
struct VoxelAttachment {
    const VoxelModel* model = nullptr;
    int   joint = -1;              // сустав скелета самой сущности

    Vec3  offset{};                // где на суставе, в модельных пикселях
    Vec3  anchor{};                // какой воксель модели садится на offset
    Vec3  rotationDegrees{};       // вокруг anchor, в порядке X, Y, Z
    float voxelScale = 1.0f;       // модельных пикселей на воксель

    Vec3  tint{1, 1, 1};
    Vec3  emissionScale{1, 1, 1};
};

bool attachmentToWorld(const Entity&, const VoxelAttachment&, Mat4& out);
bool addAttachment(PropSet&, const Entity&, const VoxelAttachment&);
int  addAttachments(PropSet&, const Entity&, const std::vector<VoxelAttachment>&);

Vec3 jointTip(const EntityModel&, int joint);              // кулак: низ коробок сустава
Vec3 gripVoxel(const VoxelModel&, float fraction = 0.25f); // хват: центр масс нижней части
```

**Единицы.** Привязка живёт в пространстве своего сустава, а это модельные
пиксели, и по умолчанию **один воксель равен одному модельному пикселю**. Это
не выбрано для красоты: предмет Minecraft ровно шестнадцать вокселей в высоту,
а блок — шестнадцать модельных пикселей, поэтому предмет из игры в кулаке
выходит того размера, каким его рисует игра. `Entity::scale` учитывается сам —
он уже сидит в матрице сустава.

**Куда это попадает.** В `PropSet`, как любой другой проп, а не в `EntitySet`.
К моменту трассировки фонарь в руке — это проп, и `intersectScene` сохраняет
свои четыре ветки вместо того, чтобы узнать, что сущность бывает ещё и
воксельной сеткой. Набор по-прежнему нужно `build()`, а дубль пересобирает и
его, и `EntitySet` каждый кадр.

```cpp
VoxelModel lantern = buildLantern();

VoxelAttachment held;
held.model = &lantern;
held.joint = joint::RightArm;
held.offset = jointTip(model, joint::RightArm);   // кулак
held.anchor = gripVoxel(lantern);                 // ручка, а не центр габаритов
held.rotationDegrees = {180.0f, 90.0f, 0.0f};

PropSet props;
addAttachment(props, figure, held);
props.build();
scene.props = &props;
```

`jointTip` даёт низ базовых коробок сустава — конечность висит **ниже** своей
оси, поэтому её дальний конец это низ самой нижней коробки. `gripVoxel` берёт
центр масс сплошных вокселей в нижней четверти модели: инструмент держат у
основания, а ручка почти никогда не в середине габаритного ящика — у кирки не
близко.

Привязка без модели, с пустой моделью или с несуществующим суставом
**пропускается**, а не считается ошибкой: сцена, не сумевшая загрузить предмет,
должна потерять предмет, а не персонажа. `addAttachments` возвращает, сколько
поставлено.

> **Во вьюпорте их не видно.** `buildPropMesh` не существует, поэтому предмет
> в руке появляется только в трассировке — как и любой другой проп.

### Где находится сустав

Привязки построены на трёх функциях, которые полезны и сами по себе: ими
`EntitySet::add` уплощает коробки, и ими же можно поставить у руки что угодно —
спрайт, источник света, вторую фигуру.

```cpp
Mat4 entityToWorld(const Entity&);
void resolveEntityJoints(const Entity&, std::vector<Mat4>& jointToWorld);
bool entityJointToWorld(const Entity&, int joint, Mat4& out);
```

Они публичные ровно затем, чтобы ответ был **один**. Сцена, собирающая это
размещение руками, совпадает с рендерером ровно до того дня, когда одна из
копий изменится: `strike.cpp` и `meet.cpp` несли по копии одних и тех же
пятнадцати строк, пока эти функции не появились. `test_rig` проверяет
совпадение точно — матрица из `entityJointToWorld` обязана быть той же самой,
которой рендерер уплощил коробку руки.

Перебор коробок линейный: на шестидесяти коробках это ничто, на тысяче
персонажей понадобится BVH.
