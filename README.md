# HEXUDON Bot V1 - C++ sandbox

Bot giao tiep bang che do sandbox: doc JSON Lines tu stdin va in JSON Lines ra stdout.
Log/diagnostic ghi ra stderr.

## Build

```sh
g++ -std=c++17 -O2 -Wall -o bot main.cpp
```

Hoac:

```sh
make
```

## Chay smoke test

```sh
./bot < tests/smoke_input.jsonl
```

Output mong doi co 2 dong JSON:

1. Assignment agent, dang `[0,0,...,1]`.
2. Plan ngay, dang `[[actions...], ...]`.

## Planner hien tai

- Data model noi bo cho agent, spot, score, route va game state.
- Luat even-r neighbor va move cost theo o nguon.
- Dijkstra theo step cost hien tai, co tinh traffic status.
- Baseline thi dau duoc khoi phuc theo hanh vi m-0558..m-0564.
- Luon tao fast baseline, chi chay strong planner khi profile/deadline cho phep.
- Planner stock-aware diversity-first: uu tien brand chua lay trong ngay, sau do farm portions theo quota `stocks`.
- Cho phep nhieu patrol ghe cung spot trong ngay toi gioi han `stocks`, tranh de patrol dung yen khi con stock.
- Route pool + global combiner nhe: sinh nhieu route theo mode khac nhau roi chon theo stock con lai, slack va road penalty.
- Validator/simulator truoc khi output; neu candidate invalid thi fallback ve safe plan dung yen.
- Refuel dung heuristic service-zone/route endpoint cua baseline; exact timeline scheduler dang nam trong snapshot thu nghiem va khong tham gia binary thi dau.
- Assignment mac dinh toi da mot tanker; khong tu dong bat hai tanker.

## Ghi chu

`baseline.py` duoc giu lai lam ban HTTP polling tham chieu. Bot chinh de nop la `main.cpp`.

## Chay C++ bot tren tran HTTP

Bot C++ la sandbox stdin/stdout, nen khi dau HTTP can wrapper.
Khuyen nghi dung `run_cpp_http.py` cho tran that; wrapper chi lam transport, logic van nam trong `bot.exe`, khong dung `baseline.py`.

```powershell
.\build_cpp.ps1
python .\run_cpp_http.py --match m-0461
```

Co the truyen token truc tiep neu can:

```powershell
python .\run_cpp_http.py --match m-0461 --token TOKEN_DOI
```

`run_cpp_http.ps1` van duoc giu lai de debug PowerShell.

## Replay/debug

Moi tran HTTP se tao thu muc `matches/<match-id>/` gom:

- `setup.json`
- `day-N-state.json`
- `day-N-actions.raw.json`
- `http.log`
- `bot.stderr.log`
- `replay.jsonl`

Phan tich nhanh tran da luu:

```powershell
python .\analyze_match.py m-0467
```

Chay ma tran replay de so sanh profile:

```powershell
.\tests\budget_matrix.ps1 -MatchId m-0563 -Budgets 20,50,100,200,500
```
