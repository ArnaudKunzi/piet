> **⚠ SUPERSEDED (2026-07-07).** This pre-Constitution engine roadmap is retained for reference
> only: the Doctrine and the engine feature plan (waves W0–W6, cobra 0.19–0.24) now live in the internal
> planning tree. Do NOT plan from this file.

# CoBrA Engine — Roadmap

Engine binary: `cobra`. Eval harness plan → [EVAL.md](EVAL.md).

---

## Doctrine

**Priority order (strict — each gate holds before optimizing the next):**
1. **숫자가 Stata와 맞아야 한다.** Correctness/parity는 절대 타협 없음. 항상 authoritative `.ado` +
   Stata golden으로 검증. 맞지 않는 숫자는 절대 출하하지 않는다.
2. **로컬 맥에서 최대한 빠르게.** 이 8 GB M-series Mac + Accelerate를 1순위 속도 타깃으로.
3. **최대한 가볍게.** 1·2가 충족된 후에만 — 메모리, 바이너리 크기, 의존성, 할당 최소화.
4. **그 다음 Cowork Linux 포팅.** 크로스플랫폼/many-core 튜닝은 마지막.

**Stata parity 워크플로우 (피처 추가 때마다 의무):**
- `.ado` 소스를 `/stata` 스킬로 읽는다 (`findfile x.ado` 또는 Stata의 사용자 ado 디렉터리).
  기억이나 논문에서 공식을 추측하지 않는다.
- `StataBE -b do <name>_golden.do`로 golden 생성 (`.log` + `.csv` to cwd).
- `tests/unit/test_*.cpp`에서 ~1e-6 이내로 검증.
- dof/small-sample 컨벤션 **정확히** 맞춤 (점추정만이 아니라).

참고 자산 (내부 문서, 공개 트리에는 없음):
- M&F 노트 14개 (~3,950줄): 수치 parity용 추정량 공식
- 언어 노트 8개 (~2,620줄): PIET .do 파서/인터프리터 언어 스펙

---

## 현황 — v0.10.0 (2026-06-18; factor-var `##` 포함)

**45개 서브커맨드 완성:** OLS/FE/IV/LIML · FE-Poisson/quasi-Poisson · xtreg(RE)/xtpois/xtlogit/nbreg ·
logit/probit/tobit/heckman(2step)/reg3 · csdid(+pretrend/calendar) · qreg · mediate ·
lincom/nlcom/margins/predict/hausman/test/chow · TS 진단 9개 · newey/prais · dfuller/pperron ·
iv-diag · eval/collapse/tsfill/tsops/convert/grid.

주요 달성: 구조화 에러+exit code · provenance{} · 크로스-코어 결정론 · `--format tidy` ·
native CSV · multi-y batch(feols/iv/fepois) · **KP rk Wald F ~1e-14**.

---

## 백로그

### Tier 1 — 구조적 paste-&-run 차단요소 (최우선)

- ~~**factor-var 상호작용 `c.x##c.z` / `i.t##i.p`**~~ — ✅ **구현됨** (2026-06-18, commits `2a73ff5` + `7c284f4` collinear omission). DID `i.treat##i.post` 동작. `i./c./#/##` in `--x`, base level + 제곱 + 교차항 전개.
- **merge / append** — `_merge` 1/2/3, 1:1/m:1, 충돌 규칙.
- **`[aw=]/[pw=]/[fw=]` 가중치** — 추정량 전반.

### Tier 2 — 없는 추정량

- **mlogit / ologit / oprobit** — 다항/순위형, 학부 수업 단골.
- **Cox PH (stcox) + competing risks** — 사망 결과 직결 (Wind IV).
- **intreg / truncreg** — tobit 완료; 같은 4-case LL 프레임워크.
- **IV-quantile (ivqreg)**, **GMM (gmm, 2-step/CUE)**, **RD/RDD (rdrobust)**, **sdid/synth**.
- 낮은 우선: mixed/xtmixed (다수준), arima/var/vecm.

### Tier 3 — VCE / 진단

- **Conley spatial HAC** — 오프라인 차단 (`ssc install acreg` 필요; 시군구 지리).
  `--vcov conley --coords lat,lon --cutoff km`.
- Driscoll-Kraay (xtscc), dfgls, White-IM hettest, Shea's R²/Cragg-Donald in iv-diag.

### Tier 4 — 싸고 가치 큰 통계/데이터 ("에러리포트→명령추가 루프" 1순위 타깃)

교과서 공식 / python-native에 가까워 빠르게 추가+golden 가능:
- **corr/pwcorr, ttest, sdtest, anova, vif**
- **tab/tabulate(+χ²), tab1/tab2, tabstat by(), codebook**
- **recode, rename, label**
- egen 롱테일: **group/rank/tag/std**

### Tier 5 — 기존 커맨드 서브피처

heckman full MLE · reg3 iterated/dfk/robust-VCE · `eval` 파서 내 ts-operators + numlist `L(1/3)` ·
**`cobra convert x.dta`** native (readstat 기반; to_hbin.py 대체, PIET 번들 경량화).

---

## 사용자 실용 우선순위 (연구 프로젝트 기준)

1. ~~factor-var `##` 상호작용 (모든 DID)~~ ✅ 완료 (2026-06-18)
2. Cox PH + competing risks (사망 결과)
3. Conley spatial HAC (시군구; 네트워크 필요)
4. corr/ttest/tab/tabstat 번들 (싸고 어디서나 쓰임)
5. mlogit/ologit/oprobit

---

## 설계 경계 (엔진이 아닌 레이어에서 할 일)

- esttab/estout/outreg2 테이블 — LLM이 tidy JSON에서 빌드.
- `.do` 인터프리터 (macros/foreach/program/loops) — PIET.
- 그래픽 (scatter/twoway/hist/kdensity) — 엔진은 데이터 출력; 렌더링은 클라이언트.

---

## PIET가 요청하는 엔진 작업

- `cobra version --json` → `{cobra_version, contract_version}` 노출 (패치 핸드셰이크용; 현재 미구현).
  현재 버전은 estimator JSON의 `provenance.cobra_version`에만 있음 → 커널은 부팅 시 하드체크 못 하고 경고만. [D2 확인]
- **`ols --tidy` se/t = null 수정** — `ols`는 SE를 `se_classic`/`se_hc1` dict에 저장하는데 `tidy_coeftable`는 `se` 키를
  찾음 → coeftable의 se/t가 null (b/p는 정상). `feols`처럼 `se`/`tstat` 키를 emit하거나 tidy builder가 `se_classic`
  fallback. 현재 PIET 백엔드는 `regress`→`feols --vcov iid`로 우회 (b 비트동일, 고전 SE 정확). 낮은 우선. [D2, PIET 백엔드 보고]
- `cobra convert x.dta x.hbin` native (Tier 5).
- `--keepif` `/tmp` 하드코딩 → Windows 경로 우회 (현재 PIET가 eval --out 로 우회).
