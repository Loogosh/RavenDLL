# RavenEra API — аддон-API для WoW 1.14.2 (RavenCraft)

Реализация отсутствующего в 1.14 функционала SuperWoW / UnitXP_SP3 / ClassicAPI
для аддонов, целиком на стороне DLL.

- **Клиент:** `WowClassic.exe` 1.14.2.42597 (RavenCraft 1.14)
- **Без модификации EXE.** Бинарник клиента не патчится.
- **Без автоматизации.** API только читает/отдаёт данные и выполняет действия,
  инициированные игроком — он не играет за вас.
- Всё, что помечено ✅, проверено в игре.

---

## 1. Что это

В 1.14 убрали большую часть аддон-API, на которую опирается экосистема 1.12/Turtle:
позиции юнитов, дистанции, линию видимости, таргет по GUID, фокус, доступ к слотам
лута и прочее. Аддоны, использующие эти функции, молча ломаются.

RavenEra возвращает их: вызывает движок из нативной DLL и отдаёт результат в Lua
через тонкий документированный шим.

| Компонент | Роль |
|---|---|
| `version.dll` | proxy-DLL, грузит стек до создания Lua-состояния |
| `SlotPrep.dll` | резервирует слоты FrameScript для гейтов |
| `SuperWoW.dll` | движковый слой: чтение памяти, лучи, лут, фокус, гейт |
| `HearthDB.dll` | база предметов (SQLite) |
| `!RavenEraAPI` (аддон) | Lua-шим: публичные функции `RavenEra_*` |

---

## 2. Установка

```
_classic_era_\
├─ version.dll
├─ SlotPrep.dll
├─ SuperWoW.dll
├─ HearthDB.dll
└─ Interface\AddOns\!RavenEraAPI\      (core.lua, SuperWoW.lua, …)
```

Запускай игру как обычно. При успешной загрузке в чат выводится:

```
RavenEra API ready
HearthDB ready
```

Статус гейтов также пишется в `Logs\superwow.log` и `Logs\slotprep.log`.

> DLL грузятся только при старте процесса. `/reload` перезагружает Lua-аддон,
> но **не** DLL — обновление DLL требует полного перезапуска клиента.

---

## 3. Архитектура

```
version.dll (proxy, рано)
  └─ SlotPrep.dll      резервирует 3 слота FrameScript (таблица name→fn)
       ├─ SuperWoW.dll клеймит слот 1 (гейт RavenEra) и слот 2 (protected proxy)
       └─ HearthDB.dll клеймит слот 0
```

Клиент хранит FrameScript-функции как пары `[name_ptr, fn_ptr]` в статической
таблице. SlotPrep берёт три неиспользуемых имени, направляет их `fn` на гаджет
`jmp [slot]` **внутри модуля** (движок отвергает указатели функций вне образа
модуля фатальной ошибкой `#134`) и раздаёт слоты через
`RavenEra_ClaimSlot(index, fn)`.

Protected-действия разрешаются на время вызова установкой бита `0x40` по адресу
`base + 0x3171B80` с последующим восстановлением. Попапы «blocked from an action»
подавляются аддоном.

---

## 4. Как вызывать

Высокоуровневые функции (рекомендуется):

```lua
RavenEra_UnitDistance("player", "target")
```

Низкоуровневый гейт (для диагностики и новых интеграций):

```lua
RavenEra("Cmd", "payload")
```

---

## 5. Lua API

### 5.1 Позиции, дистанции, видимость

| Функция | Возврат | Статус |
|---|---|---|
| `RavenEra_UnitPosition(unit)` | `x, y, z, o` | ✅ |
| `RavenEra_UnitFacing(unit)` | `o` (радианы) | ✅ |
| `RavenEra_UnitDistance(a, b)` | число, с вычетом combat reach | ✅ |
| `RavenEra_UnitInSight(a, b)` | `visible, distance, traceFactor` | ✅ |
| `RavenEra_UnitBehind(unit, target)` | `bool` | ✅ |
| `RavenEra_UnitReach(unit)` | `reach, radius` | ✅ |

```lua
/run print(RavenEra_UnitPosition("target"))
/run print(RavenEra_UnitDistance("player", "target"))
/run print(RavenEra_UnitInSight("player", "target"))
```

Источник: объект-менеджер (`std::vector<Object*>` по `base + 0x2F90000`).
Позиция/ориентация `obj + 0x1644`, GUID `obj + 0x58`, дескриптор `obj + 0x10`
(combat reach `+0x1A0`, bounding radius `+0x19C`).
Линия видимости — движковый луч `CWorld_Intersect` (`0x1A4EAA0`, flags `0x100111`).

### 5.2 Таргет

| Функция | Возврат | Статус |
|---|---|---|
| `RavenEra_TargetUnit(unit)` | `bool` | ✅ |
| `RavenEra_TargetLastTarget()` | `bool` | ✅ |
| `RavenEra_AssistUnit(unit)` | `bool` | ✅ |
| `RavenEra_TargetUnitByGUID(guid)` | имя цели или `nil, "no token"` | ✅ |
| `RavenEra_TokenForGUID(guid)` | токен или `nil` | ✅ |
| `RavenEra("TargetByGuid", guid)` | `bool` | ✅ |

```lua
/run local g = UnitGUID("nameplate1"); print(RavenEra("TargetByGuid", g), UnitGUID("target"))
```

GUID-строки резолвятся через стандартные токены юнитов, поэтому таргет работает
для любого юнита с токеном (target, pet, party, raid, boss, неймплейты).

### 5.3 Фокус

| Функция | Возврат | Статус |
|---|---|---|
| `RavenEra_SetFocus(unit)` | новый `UnitGUID("focus")` | ✅ |
| `RavenEra_SetFocus(nil)` | `nil` (сброс фокуса) | ✅ |
| `RavenEra("FocusByGuid", guid)` | `bool` | ✅ |
| `RavenEra("FocusDesc")` | `lo=… hi=…` | ✅ |

```lua
/run print(RavenEra_SetFocus("pet"), RavenEra("FocusDesc"), UnitGUID("focus"))
/run print(RavenEra_SetFocus(nil), RavenEra("FocusDesc"))
```

Фокус пишется напрямую в дескриптор клиента (`base + 0x2D24768`).

### 5.4 Токены, метки, владельцы

| Функция | Возврат | Статус |
|---|---|---|
| `RavenEra_UnitCandidates()` | массив ~60 токенов юнитов | ✅ |
| `RavenEra_UnitTokenForMark(n)` | токен с рейд-меткой `n` (1–8) | ✅ при неймплейтах |
| `RavenEra_UnitOwner(unit)` | токен владельца или `nil` | ✅ |
| `RavenEra_UnitOwnerGUIDLow(unit)` | hex GUID владельца | ✅ |
| `RavenEra_CanLootUnit(unit)` | `bool` | ✅ |

```lua
/run print(#RavenEra_UnitCandidates())
/run print(RavenEra_UnitOwner("pet"))
/run print(RavenEra_UnitTokenForMark(8))
```

Рейд-метки: `1` star, `2` circle, `3` diamond, `4` triangle, `5` moon,
`6` square, `7` cross, `8` skull.

### 5.5 Лут

| Функция | Возврат | Статус |
|---|---|---|
| `RavenEra_LootSlot(n)` | `bool` (нумерация с 1) | ✅ |
| `RavenEra_LootAll()` | `taken, total` | ✅ |
| автосбор по `LOOT_OPENED` | `RAVENERA_AUTOLOOT_ON_OPEN = true` | ✅ |

```lua
/run print(RavenEra_LootSlot(1))
/run print(RavenEra_LootAll())
```

Точка входа движка: `0x16FDFB0`. Лут **закрытого** трупа не поддерживается
(см. «Ограничения»).

### 5.6 Файлы, CVar, служебное

| Функция | Возврат |
|---|---|
| `RavenEra("ImportFile", path)` | содержимое файла |
| `RavenEra("ExportFile", path, data)` | `true` |
| `RavenEra_GetCVar(name)` / `RavenEra_SetCVar(name, value)` | значение |
| `RavenEra("GetVersion")` | `0, 1` |
| `RavenEra("Ping")` | `true` |
| `RavenEra("Caps")` | битовая маска возможностей |
| `RavenEra("ObjCount")` | число объектов |
| `RavenEra("ObjMgr")` | `vec=… end=… cnt=…` |
| `RavenEra("GetLocalPlayer")` | адрес объекта игрока |

### 5.7 HearthDB (база предметов)

`HearthDB.dll` — движок SQLite 3, доступный из Lua. Это независимый гейт
(слот 0) со своим пространством `HDB_*`; можно держать открытыми несколько баз
одновременно, каждая адресуется целочисленным handle.

| Функция | Возврат | Примечание |
|---|---|---|
| `HDB_GetVersion()` | `major, minor, patch` | версия DLL |
| `HDB_Open(filename)` | `handle` | открывает (или создаёт) файл БД; при ошибке — исключение |
| `HDB_OpenAddon(addon, path)` | `handle` | открывает БД внутри аддона, только для чтения |
| `HDB_Close(handle)` | — | |
| `HDB_Execute(handle, sql)` | — | DDL/DML; при ошибке — исключение |
| `HDB_Query(handle, sql)` | `rows[]` с ключами по именам колонок | |
| `HDB_QueryRaw(handle, sql)` | `cols[], rows[][]` | сырые массивы |
| `HDB_ExecuteAsync(handle, sql, cb)` | `ticket` | выполняется в рабочем потоке |
| `HDB_QueryAsync(handle, sql, cb)` | `ticket` | |
| `HDB_QueryRawAsync(handle, sql, cb)` | `ticket` | |
| `HDB_GetResult(ticket)` | `rows` / `value, err` | `nil`, пока ticket не готов |
| `HDB_ClearPoison(handle)` | — | сбрасывает handle, зависший после ошибки |
| `HDB_Pump()` | — | вызывается автоматически каждый кадр (callback-фрейм) |

Синхронный пример:

```lua
local h = HDB_Open("hdb_smoke.db")
HDB_Execute(h, "CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT)")
HDB_Execute(h, "INSERT INTO kv VALUES ('hello','world')")
local rows = HDB_Query(h, "SELECT * FROM kv")
print(rows[1].k, rows[1].v)      -- hello world
```

Асинхронный пример:

```lua
local t = HDB_QueryAsync(h, "SELECT * FROM kv", function(rows) print(rows[1].v) end)
print(HDB_GetResult(t))          -- nil, пока не готово; rows после завершения
```

Примечания:

- результат пакуется DLL и распаковывается аддоном; `HDB_Query` отдаёт строки
  с ключами по именам колонок, `HDB_QueryRaw` — список колонок + строки-массивы;
- асинхронные ticket'ы вычитываются `HDB_Pump()`, который зарегистрирован в
  `OnUpdate` фрейма — вручную опрашивать не нужно;
- `HDB_OpenAddon` по умолчанию только для чтения (поставляемые справочные данные);
- низкоуровневые команды гейта: `GetVersion`, `Open`, `OpenAddon`, `Close`,
  `Execute`, `Query`, `QueryRaw`, `ExecuteAsync`, `QueryAsync`, `QueryRawAsync`,
  `GetResult`, `ClearPoison`, `Pump`.

---

## 6. Низкоуровневые команды гейта

Для диагностики и собственных интеграций. `payload` — всегда строка.

| Команда | Payload | Возврат |
|---|---|---|
| `UnitPos` | hex GUID | `r=1 x y z o` |
| `UnitDist` | `hexA,hexB` | число |
| `UnitReach` | hex GUID | `ok=… reach=… radius=…` |
| `UnitOwner` | hex GUID | `ok=… lo=… hi=…` |
| `Los` | — | `ctx=… open=… ground=…` (самотест луча) |
| `LosUnits` | `hexA,hexB` | `insight=… hit=… d=… dist=…` |
| `Loot` | номер слота (с 1) | `1` / `-1` |
| `SetTargetToken` | токен | `bool` |
| `SetFocusToken` / `ClearFocus` | токен / — | `bool` |
| `TargetByGuid` / `FocusByGuid` | GUID-строка | `bool` |
| `FocusDesc` | — | `lo=… hi=…` |
| `ArmNextTarget` | `target` / `focus` / … | `true` |

---

## 7. Ограничения

1. **Нет запроса лута (`CMSG_LOOT_UNIT`).**
   Поэтому недоступны `LootAllCorpses`, `LootUnit`, `LootUnitItem`.
   - `InteractUnit` виртуализирован/обфусцирован протектором клиента и падает
     (`INT_DIVIDE_BY_ZERO`) при вызове вне контекста;
   - трафик клиента — TLS, опкоды не наблюдаются;
   - аппаратные точки останова анти-чит отвергает (краш клиента).
2. **`CastSpellByName`** — та же виртуализация; каст только через secure-кнопки.
3. **Глобалы `TargetUnit` / `FocusUnit` не патчатся.**
   - патч таблицы FrameScript даёт `#134 Invalid function pointer`
     (движок требует, чтобы указатель функции был внутри образа модуля);
   - патч Lua-замыканий в куче вешает клиент.
   Используйте `TargetByGuid` / `FocusByGuid`.
4. **Имена, метки и ауры требуют токена юнита** (то есть включённых неймплейтов).
   Позиция/HP/GUID читаются для любого GUID, присутствующего в объект-менеджере.

---

## 8. Тестирование

Каждую функцию можно проверить одной строкой `/run` в чате — без кода аддона:

```lua
/run print("ver", RavenEra("GetVersion"), "cnt", RavenEra("ObjCount"))
/run print(RavenEra_UnitPosition("player"))
/run local g = UnitGUID("target"); print(RavenEra("TargetByGuid", g), UnitGUID("target"))
/run local g = UnitGUID("pet"); print(RavenEra("FocusByGuid", g), RavenEra("FocusDesc"), UnitGUID("focus"))
```

---

## 9. Сборка

Требуется Visual Studio (MSVC x64) и CMake.

```
call VsDevCmd.bat -arch=amd64 -host_arch=amd64
cd build
nmake SuperWoW      # также: SlotPrep, HearthDB
```

Результат — в `build\out\`. Скопируй DLL в `_classic_era_\` и перезапусти клиент.

---

## 10. Защита и реверс

| Объект | Состояние |
|---|---|
| `WowClassic.exe` | упакован (ACG/VP), `.text`/`.rdata` только на чтение, часть Lua-API виртуализирована |
| DLL RavenEra | обычный x64 PE, **не шифрованы**, без обфускации |
| Экспорты | `SlotPrep.dll`: `Load`, `RavenEra_ClaimSlot`, `RavenEra_SlotCount`; `HearthDB.dll`: `Load`; `SuperWoW.dll`: нет |

Наши DLL можно дизассемблировать/декомпилировать (Ghidra / IDA / Binary Ninja)
без исходников; без символов результат читаем, но восстанавливать долго. Ключей
и секретов в бинарниках нет.

---

## 11. FAQ

**Почему нет `LootAllCorpses`?**
Потому что открытие трупа требует отправки `CMSG_LOOT_UNIT`, а путь запроса лута
нельзя вызвать извне (виртуализированная функция, TLS-трафик, анти-чит блокирует
отладку). Лут **открытого** окна работает полностью.

**Нужны ли неймплейты для токенов `nameplateN`?**
Да. API на токенах (имена, метки, ауры) видят только юнитов, у которых есть токен.

**Это модифицирует клиент игры?**
Нет. Добавляются только внешние DLL и обычный аддон.

**Переживёт ли патч клиента?**
Стек опирается на несколько RVA клиента и на раскладку таблицы FrameScript.
Обновление клиента скорее всего сломает первым `SlotPrep.dll` — после каждого
патча проверяйте `READY slots=3` в `Logs\slotprep.log`.
