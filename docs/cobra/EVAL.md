# CoBrA Evaluation Harness + Autonomous Patch Loop

Engine roadmap → [ROADMAP.md](ROADMAP.md). Status: **PLAN ONLY** — not yet built.

---

## 배경

CoBrA는 43개 CLI 커맨드를 노출하고, 정확도 검증은 현재 세 레이어로 분산돼 있다:
~45개 C++ `ctest`, ~30개 Stata golden CSV + `.do` 생성기, 7개 수동 Python 검증기.
**모든 커맨드를 자동으로 exercising + ground truth 대조 + fix 루프**를 도는 하니스가 없다.

목표: 모든 커맨드를 정확도·속도 양 축으로 자동 벤치마크 + **자율 백그라운드 패치 루프**
(격리된 브랜치에서 수정 → beta → PR, 인간이 한 번 리뷰·머지).

---

## 확정 결정 (사용자, 2026-06-17)

1. **자율성 = 자율 백그라운드 루프, PR에서 인간 게이트.** `auto-patch/<UTC>` 브랜치에서 무인 실행
   — **절대 `main` 아님** — 트리아지 → 엔진 소스 패치 → 빌드 → 재실행 → 커밋 → **beta 버전 bump**.
   완료 시 PR 오픈; 사용자가 한 번에 리뷰·머지. **온디맨드 + 야간 스케줄** 양쪽 실행.
2. **데이터셋 = 공개 데이터만.** Stata 예제 (`sysuse`/`webuse`) + **Wooldridge 교과서** 데이터 +
   `.do` 안에서 **seeded-synthetic** 생성. KOWEPS/Wind 사설 데이터 없음 → eval 스위트 공유 가능.
3. **Ground truth = 모든 커맨드에 Stata golden** (정확도 = **hard gate**) + numpy oracle + property/metamorphic 검사.
4. **속도 = 1등급 측정 축** (엔진 #2 우선순위): cobra vs **pyfixest** (4개 커맨드, venv `미시계량경제/.venv`)
   vs **Stata** (golden `.do`의 `timer on/off`). 리포트 + 셀프-회귀 플래그 + **경쟁 알람**
   (pyfixest/Stata보다 느리면 큰 소리로) — 단 **hard gate는 아님** (fit≈pyfixest 예상; 진짜 승부는
   로드+invocation+bootstrap/DID).
5. **커버리지 = canonical + existing-golden 축** (~60–80 케이스); manifest는 나중에 성장.
6. **Safe-git:** run-per-branch, 절대 `main` 없음, **force-push 금지**; `git push -u` (새 ref) +
   `gh pr create` (인간/CI 머지). **비공개** GitHub remote.

---

## 카디널 룰 — no-gaming 가드레일 (자율이기 때문에 특히 중요)

숫자는 **코드가 고정 tolerance로 판정**, float을 눈으로 보지 않음. 루프는 **엔진 소스만 편집 가능**
(`src/`, `apps/`) — **golden·tolerance·manifest expected value 수정 금지**. 이를 변경해야 할 실패는
**자동 해결 불가 → PR에 ESCALATE** ("인간 판단 필요"). 브랜치 격리 + src-only = 무인 패칭이 안전한 이유.

**에스컬레이션 빈도는 낮고 클러스터됨.** 흔한 실패 = "엔진 버그 → `src/` 패치"(루프의 본래 일). 에스컬레이션은
두 비정상 구간에만: (a) **커맨드 golden 최초 작성** — 야간 루프가 아니라 `/stata`로 상류에서 처리;
(b) **Stata-컨벤션 모호성 또는 spec 변경**. 안정된 스위트의 야간 루프는 대부분 실제 엔진 회귀 → 자동 수정 가능.
**안전 주의:** 루프는 *의심스러운* golden도 에스컬레이트 — golden을 맞추려 엔진을 깨는 것은 역게이밍.
어느 경우든 run은 자동 수정 + 짧은 ESCALATE 목록을 PR에 넣고 종료; 전체를 멈추지 않음.

---

## 설계 — `tests/eval/` 아티팩트

### 1. Manifest — `manifest.json`

선언적 진실 소스; 케이스당 하나의 항목 (데이터셋은 **공개** 플레이스홀더):
```json
{
  "id": "iv_mroz",
  "command": "iv",
  "args": ["--data","{mroz}","--y","lwage","--x","exper,expersq",
           "--endog","educ","--instruments","fatheduc,motheduc"],
  "oracle": {"kind":"golden","golden":"iv_mroz_golden.csv","do":"iv_mroz_golden.do"},
  "checks": [{"q":"coef.educ","rtol":1e-7},{"q":"se.educ","rtol":1e-6}],
  "also": ["schema","smoke"]
}
```
`{mroz}`/`{auto}`/`{wagepan}`… 는 공개 `.hbin` 경로로 해석. `oracle.kind ∈ {golden, property, smoke}`.

### 2. Runner — `run_eval.py`

- 각 케이스: `cobra <cmd> <args> --format tidy` (단일 `coeftable[]` → 추정량별 분기 없음).
- 검사: **golden** (`validate_feols.py:116-127` rel-err gate + `read_csv` 재사용), **property** (`properties.py`),
  **schema/smoke** (exit 0? valid JSON? 필수 필드? 플래그 sanity: `converged`/`demean_converged`, p∈[0,1], se>0, r2∈[0,1]).
- `--bench`: 핫 추정량 (ols, feols, iv, fepois, csdid, wildboot, xtreg) 타이밍 — warm median N reps —
  **pure-fit** (`timing_ms`) + **per-invocation** (process + `.hbin` load), vs pyfixest vs Stata.
  `report.md` 속도표; **셀프-회귀 플래그** vs `perf_baseline.json` + **경쟁 알람**.
- 출력: `report.json` (케이스별 status/kind/deltas/`suspected_cause`) + `report.md` (커맨드×{golden,property,smoke} 매트릭스 + 속도표).
- `--only/--group/--rebuild` for 루프. `--with-ctest` = C++ exact-answer 테스트 포함 (**로컬 전용** — 일부가 gitignored 사설 데이터 읽음).

### 3. Property library — `properties.py`

Oracle-free 불변 검사 (저렴한 광범위 버그 탐지):
`offset_invariance`, `iv_equals_ols_when_Z_is_X`, `lincom_single_coef_equals_se`,
`predict_xb_plus_resid_equals_y`, `reg3_sure_identical_regressors_equals_ols`,
`wildboot_t0_matches_analytic_t`, `cluster_se_ge_iid`, `pvalue_in_unit_interval`,
`se_positive`, `r2_in_unit_interval`, `converged_true`.
`convert` 오라클 (라운드트립), `run`/`grid` (== 단일커맨드 golden 동치), `info` (smoke).

### 4. 자율 루프 드라이버 — `autopatch.py`

Claude의 트리아지/패치 추론 주위를 결정론적으로 오케스트레이션:
1. `git switch -c auto-patch/$(date -u +%Y-%m-%d-%H%M)` (**절대 `main` 없음**).
2. 루프: `run_eval` → **확인된 엔진 버그**마다 Claude가 **`src/` 만** 패치 → `cmake --build build -j4`
   → `--only <cmd>` 재실행 → **수정 1건당 커밋** (명확한 메시지).
3. 수정이 쌓이면 **beta 버전 bump** (`COBRA_VERSION → X.Y.Z-beta.N`).
4. **가드레일 강제:** `tests/eval/golden/`, tolerance 필드, manifest expected value 아래의 변경 → 거부/스킵 → **ESCALATE** 기록.
5. 완료: `git push -u origin <branch>`; `gh pr create` (수정별 changelog + ESCALATE 섹션 + 속도표). **절대 `main`에 머지 안 함.**

**온디맨드** = 내가 드라이버 실행. **스케줄** = 야간 루틴 (`schedule` 스킬/cron)이 같은 드라이버 실행 → PR 생성/갱신.

### 5. Doctrine 스킬 — `~/.claude/skills/cobra-eval/SKILL.md`

`cobra` SKILL.md 레이아웃 미러: 카디널 룰 · **tolerance 표** (OLS 1e-10; FE/IV β 1e-7/SE 1e-6;
csdid ATT 5e-6/SE 5e-4; Hansen-J 1e-8; data-layer 1e-10; tsma 1e-14) · **실패 트리아지 트리**
(실행되나? → golden 신뢰할 수 있나? → invocation 맞나? → 엔진 버그 vs 수치 방법 차이) ·
**자율 루프 프로토콜** · **`/stata`로 공개 데이터 golden 작성** · 속도 방법론 · 공개 데이터셋 맵.

### 6. Goldens — `golden/*.do` + `make_goldens.sh`

공개/합성 `.do` 생성기 (템플릿 `tests/validate/demean3fe_golden.do`: `set seed`, `_b`/`_se`/`e()`
를 `%21.15e` 로 dump, `timer on/off`로 cmd 감싸기). `make_goldens.sh`로 `stata -b do`로 전체 재생성.

---

## 공개 데이터셋 맵

| 커맨드 그룹 | 데이터셋 |
|---|---|
| 횡단면 OLS / het-reset / serial-corr / logit / probit / tobit | `sysuse auto`, Wooldridge `wage1` |
| IV / 2SLS / LIML / iv-diag | Wooldridge `mroz` (educ instrumented) or `card` |
| Heckman selection | `mroz` (canonical selection dataset) |
| 패널 FE / xtreg / xtpois / xtlogit / fepois | Wooldridge `wagepan`, `webuse nlswork`/`grunfeld` |
| csdid staggered DID | **seeded-synthetic** staggered panel in `.do` |
| Count / nbreg | Wooldridge `crime1`, `webuse ships` |
| Time series (newey/prais/dfuller/pperron/archlm/wntestq) | Wooldridge `phillips`, `barium`, `intdef` |
| reg3 / systems | seeded-synthetic 2-eq system |

**재사용 가능 (이미 seeded-synthetic 또는 공개):** ols, feols (demean3fe), hettest (auto), reg3,
unitroot, neweyprais, ts_diag, glm/glmqp, lincom, nlcom, margins, mkspline, rowegen, qreg, collapse.

**공개/합성으로 재작성 필요 (현재 사설 .dta):** csdid, iv(+iv-diag/hansen), mediate, tobit, heckman,
fepois, nbreg, liml, eval/d2.

**신규 golden 필요 (현재 없음):** xtreg, xtpois, xtlogit, chow, test, hausman, predict, tsfill, tsops.

---

## 파일 목록

**생성:** `tests/eval/{manifest.json, run_eval.py, properties.py, autopatch.py, perf_baseline.json,
make_goldens.sh, README.md, golden/*.do}`; `~/.claude/skills/cobra-eval/SKILL.md`; repo `.gitignore`.

**재사용:** `validate_feols.py:116-127` (rel-err gate + `read_csv`); `demean3fe_golden.do` (템플릿);
`--format tidy`; `cmake --build build -j4`; `ctest`.

**루프가 절대 수정 불가:** `tests/eval/golden/` 아래 모든 것, tolerance 값, manifest expected value.

---

## 실행 단계

1. **Git 부트스트랩** — `git init` + `.gitignore` (`build/`, 사설 `tests/validate/*.{hbin,dta,parquet,csv}` 제외)
   + base `main` 커밋 + **비공개** GitHub repo (`gh repo create --private`).
   *외부 노출* → repo-create/첫 push 전 확인; `git ls-files`에서 데이터 파일 0개 검증.
2. **스캐폴드** — `tests/eval/` + `manifest.json` (공개 데이터셋) + `run_eval.py` + `properties.py`.
3. **Goldens** — 합성 재사용; 사설-데이터 건 재작성 + 누락 건 신규 작성, 전부 공개 데이터로 `/stata` 경유; `make_goldens.sh`.
4. **속도** — `--bench` + `perf_baseline.json` seed.
5. **Doctrine 스킬** — `cobra-eval/SKILL.md`.
6. **자율 루프** — `autopatch.py`; 첫 온디맨드 실행 → PR (beta) → 리뷰. 이후 야간 루틴 선택적 스케줄.

---

## 검증

```bash
cmake --build build -j4
python tests/eval/run_eval.py --with-ctest --bench
```

- `report.md`에 **43개 커맨드** 커버리지 매트릭스 + 속도표. 정확도 게이트 통과 시만 exit 0.
- 스팟체크: golden 케이스 (`--only iv`), property 케이스 (`--only fepois`), smoke 케이스 (`--only info`).
- **자율 드라이버:** `src/`에 한 줄 버그 의도적 삽입 → 자동 탐지·수정·beta-bump·PR 오픈, `main` 무변.
- **가드레일:** golden/tolerance 변경 필요 실패 → ESCALATE (자동 편집 없음).
- **데이터 위생:** `git ls-files | grep -E '\.(hbin|dta|parquet)$'` 첫 push 전 공집합.
