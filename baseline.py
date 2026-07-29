#!/usr/bin/env python3
# Bot mẫu HEXUDON (format BTC gốc) — HTTP polling, chỉ dùng thư viện chuẩn Python.
# Chạy:  python bot.py <BASE_URL> <MATCH_ID> <TOKEN>
import sys, json, time, urllib.request, urllib.error
from collections import deque

BASE, MATCH, TOKEN = sys.argv[1], sys.argv[2], sys.argv[3]
API = BASE + "/api/v1/matches/" + MATCH

def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, method=method,
        headers={"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or "null")
    except urllib.error.HTTPError as e:
        return e.code, None

# Hướng BTC gốc: 0 trên-trái,1 trên-phải,2 phải,3 dưới-phải,4 dưới-trái,5 trái.
# Hình học EVEN-R (hàng CHẴN lệch phải — khớp BTC Q1); bù (dcol,drow) theo hàng chẵn/lẻ.
DIRS_EVEN = [(0,-1),(1,-1),(1,0),(1,1),(0,1),(-1,0)]
DIRS_ODD  = [(-1,-1),(0,-1),(1,0),(0,1),(-1,1),(-1,0)]

# 1) Lấy setup (425 = chưa mở bản đồ -> thử lại; >=200ms tránh 429)
while True:
    code, setup = call("GET", "/setup")
    if code == 200: break
    time.sleep(0.2)
W, H  = setup["map"]["width"], setup["map"]["height"]
cells = setup["map"]["cells"]          # 2D: 0 đất, 1 đường, 2 núi, 3 ao
spots = setup["spots"]
n     = len(setup["agents"])
day_steps = setup["daySteps"]

def terrain(pos): return cells[pos // W][pos % W]

def neighbor(pos, d):                  # ô kề theo hướng d, -1 nếu ra ngoài
    row, col = pos // W, pos % W
    dc, dr = (DIRS_ODD if row % 2 else DIRS_EVEN)[d]
    nc, nr = col + dc, row + dr
    if nc < 0 or nc >= W or nr < 0 or nr >= H: return -1
    return nr * W + nc

def move_cost(pos, status):            # Bảng 1 CỐ ĐỊNH: (bước, nhiên liệu) rời ô
    t = terrain(pos)
    if t == 0: return 2, 1             # đất
    if t == 2: return 3, 2             # núi
    if t == 1: return (2,2) if status==1 else (4,2) if status==2 else (1,2)  # đường theo lưu lượng
    return None                        # ao: không đi được

# 2) Gán loại: xe cuối = tiếp tế (1), còn lại tuần tra (0) -> MẢNG PHẲNG [0,..,1]
kinds = [0]*n
if n > 1: kinds[-1] = 1
call("POST", "/assignment", kinds)

def bfs(src, targets):
    prev = {src: -1}; q = deque([src])
    while q:
        c = q.popleft()
        if c != src and c in targets:
            path = []
            while c != src: path.append(c); c = prev[c]
            return path[::-1]
        for d in range(6):
            nb = neighbor(c, d)
            if nb < 0 or nb in prev or terrain(nb) == 3: continue
            prev[nb] = c; q.append(nb)
    return []

def plan_day(state):                   # trả MẢNG-CỦA-MẢNG: mỗi agent 1 dãy hướng/đứng-yên
    day = state["day"]
    steps = day_steps[day] if 0 <= day < len(day_steps) else 30
    status = {t["pos"]: t["status"] for t in state.get("traffics", [])}
    open_pts = {s["pos"] for s in spots}     # day_state không có kho -> nhắm mọi spot
    plan = []
    for a in state["agents"]:
        if a["kind"] != 0:                   # xe tiếp tế: đứng yên cả ngày
            plan.append([-steps]); continue
        path = bfs(a["pos"], open_pts)
        seq, cur, used = [], a["pos"], 0
        fuel = a["fuel"] if a["fuel"] is not None else 10**9
        for nxt in path:
            c = move_cost(cur, status.get(cur, 0))
            if c is None or used + c[0] > steps or fuel < c[1]: break
            d = next((k for k in range(6) if neighbor(cur, k) == nxt), -1)
            if d < 0: break
            seq.append(d); used += c[0]; fuel -= c[1]; cur = nxt
        if used < steps: seq.append(-(steps - used))   # đệm đứng yên cho đủ daySteps
        plan.append(seq if seq else [-steps])
    return plan

# 3) Vòng ngày: mỗi ngày mới gửi kế hoạch [[..],..]; thoát khi có kết quả
cur_day = -1
while True:
    code, state = call("GET", "/state")
    if code == 200 and state["day"] != cur_day:
        r = call("POST", "/actions", plan_day(state))
        if r[0] == 200: cur_day = state["day"]
    elif code != 200:
        rc, res = call("GET", "/result")
        if rc == 200:
            print("Ket qua:", json.dumps(res.get("standings"))); break  # ASCII: an toàn mọi console
    time.sleep(0.2)  # >=200ms tránh HTTP 429
