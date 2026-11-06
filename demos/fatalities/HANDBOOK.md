# Fatalities 투어 핸드북 — 패널 계량경제학 한 바퀴

> **데이터 출처(provenance).** 미국 교통사망 패널(US traffic fatalities panel), 본토 48개 주 × 1982–1988년(연 336 관측). Ruhm, C. (1996), "Alcohol Policies and Highway Vehicle Fatalities," *Journal of Health Economics* 에서 쓰인 자료이며, Stock & Watson, *Introduction to Econometrics* 교재의 실습 추출본으로 널리 배포되어 있다. 핵심 변수: `beertax`(맥주 한 상자에 붙는 실질세, 1988년 달러), `fatal`(교통사망자 수), `pop`(인구), `unemp`(실업률), `income`(1인당 실질소득), `breath`(음주 예비호흡측정법 시행 여부, yes/no). 이 커널 폴더의 `fatalities.csv` 로 함께 배포된다.
>
> **검증(verified).** 이 문서의 모든 숫자는 출하 앱 바이너리 `piet-2.0.4+cobra-0.19.2` 로 `fatalities_tour.do` 를 헤드리스 실행한 **실제 결과**다. 재검증: 저장소 루트에서 `demos/run_demos.sh` (전 블록 GREEN, 에러 블록 0).

이 핸드북의 독자는 학부 계량경제학을 성실히 마친 사람이다. 그래서 "회귀계수는 기울기다" 같은 이야기는 생략하고, **왜 이 추정량인가 · 왜 이 표준오차(standard error)인가 · 이 데이터에서 식별(identification) 가정이 얼마나 믿을 만한가 · 실제로 나온 숫자를 어떻게 읽는가**에 집중한다. `fatalities_tour.do` 는 "팬그램(pangram)"이다. 출하된 PIET/cobra 엔진의 핵심 능력을 이 한 데이터셋 위에서 한 번씩 훑는다.

## 출하 범위와 대체(substitution) 원칙 — 정직한 고지

`fatalities_tour.do` 는 **출하된 do-executor가 실제로 지원하는 명령만** 쓴다. 엔진(cobra)은 더 많은 추정량을 구현하고 있지만, PIET 2.0.4의 do-실행 계층에 아직 배선(wiring)되지 않은 것들이 있다. 아래 표의 로드맵 공백(roadmap gap)들은 **지원되는 대체물로 바꾸고, do-파일 주석과 해당 절에서 명시**했다. 이것은 은폐가 아니라 교육적 정직성이다 — 오히려 대체물이 "그 추정량이 사실 무엇인지"를 더 잘 드러내는 경우가 많다.

| 절 | 정석(로드맵 공백) | 이 데모의 대체물 |
|----|------------------|------------------|
| §4 | `boottest` 와일드 클러스터 부트스트랩 | iid → HC1 → 클러스터 SE 위계(hierarchy)를 해석적으로 나란히 비교 |
| §5 | `ivregress`/`ivreghdfe` 2SLS | 정확식별(just-identified) IV = 축약형/1단계 비율(간접최소제곱, ILS)을 OLS 벽돌로 손수 조립 |
| §6 | `poisson … , exposure(pop)` | 로그선형 유사모형 `reghdfe ln(fatal) … ln(pop)` (노출항 계수를 자유롭게 추정) |
| §9 | `newey` HAC / 동적패널 | 지원되는 시계열 연산자 `L.`/`D.` 로 동태를 예시 |

---

## 목차

- [§1 데이터: 구성과 패널 구조](#1-데이터-구성과-패널-구조)
- [§2 합동 OLS와 표준오차가 거짓말하는 이유](#2-합동-ols와-표준오차가-거짓말하는-이유)
- [§3 패널 고정효과: within 추정량](#3-패널-고정효과-within-추정량)
- [§4 추론의 위계: iid · HC1 · 클러스터, 그리고 48개 클러스터 문제](#4-추론의-위계-iid--hc1--클러스터-그리고-48개-클러스터-문제)
- [§5 IV/2SLS를 손으로: 간접최소제곱과 약한 도구변수](#5-iv2sls를-손으로-간접최소제곱과-약한-도구변수)
- [§6 카운트와 노출: 포아송의 로그선형 유사모형](#6-카운트와-노출-포아송의-로그선형-유사모형)
- [§7 시차적 도입 이중차분: Callaway–Sant'Anna](#7-시차적-도입-이중차분-callawaysantanna)
- [§8 그래픽](#8-그래픽)
- [§9 시계열 연산자로 본 동태](#9-시계열-연산자로-본-동태)
- [부록: 검증 원장(전 블록 GREEN)](#부록-검증-원장)

---

## §1 데이터: 구성과 패널 구조

`use "fatalities.csv"` 로 336개 관측(48개 주 × 7개 연도)을 읽는다. 핵심 종속변수는 **인구 만 명당 교통사망률**이다.

$$\texttt{frate}_{it} = \frac{\texttt{fatal}_{it}}{\texttt{pop}_{it}} \times 10{,}000$$

`summarize` 결과: `frate` 평균 **2.040**, 표준편차 0.570, 범위 [0.821, 4.218]. `beertax` 평균 **0.513** 달러, 표준편차 0.478. 실업률 평균 7.35%, 1인당 소득 평균 \$13,880. 회귀에서 소득은 로그로 들어간다(`lincome = log(income)`) — 소득 효과가 비율적(proportional)이라고 보는 표준적 처리다.

`xtset statenum year` 로 패널 구조(개체 $i$ = 주, 시간 $t$ = 연도)를 선언한다. 이 선언은 이후 §7의 within 변환과 §9의 시차 연산자 `L.`/`D.` 의 전제다. 처치변수는 음주 예비호흡측정법이다: `drinkd = (breath=="yes")`. `tabulate breath` 로 보면 법이 있는 주-연도가 **155**, 없는 것이 **181**이다. 이 시차적 시행이 §7 이중차분의 재료가 된다.

> 교육적 주의: `frate` 는 이미 `pop` 으로 나눈 **비율**이다. 그래서 규모(scale)가 표준화돼 있어 주 간 비교가 바로 가능하다. 반면 §6에서는 일부러 **원자료 카운트** `fatal` 로 돌아가 노출(exposure)을 명시적으로 다룬다.

---

## §2 합동 OLS와 표준오차가 거짓말하는 이유

### 모형과 추정량

가장 단순한 명세는 패널 구조를 무시한 **합동(pooled) OLS** 다:

$$\texttt{frate}_{it} = \beta_0 + \beta_1\,\texttt{beertax}_{it} + \varepsilon_{it}$$

`regress frate beertax` 결과:

$$\hat\beta_1 = +0.3646 \quad (\text{iid SE } 0.0622,\ t=5.86,\ p=1.1\times10^{-8}),\qquad R^2=0.093.$$

**부호가 이상하다.** 맥주세가 높을수록 사망률이 *높다*? 세금은 음주운전을 줄여 사망을 낮춰야 하는데, 단순 상관은 정반대다. 이것이 누락변수편의(omitted variable bias, OVB)의 교과서적 사례다 — 세금이 높은 주는 다른 면에서도 다르다(농촌/고속도로 비중, 소득, 운전 문화).

### 왜 iid 표준오차는 틀렸는가

`estat hettest`(Breusch–Pagan/Cook–Weisberg): $\chi^2 = 9.48,\ p=0.0023$. 등분산(homoskedasticity) 귀무가설을 기각한다. 따라서 iid 가정에 기댄 고전적 SE는 **원리적으로 신뢰할 수 없다**. `vce(robust)` 로 HC1 이분산-강건 SE를 계산하면 점추정치는 그대로 0.3646, SE는 **0.0529**(t=6.90)로 오히려 조금 *작아졌다*. 강건 SE가 항상 더 크다는 통념은 틀렸다 — 오차분산이 회귀변수와 어떻게 상관되느냐에 따라 커질 수도 작아질 수도 있다. 요점은 크기가 아니라, **가정을 만족하지 못하는 SE는 그 t값과 p값이 무의미**하다는 것이다.

### 통제변수를 넣으면

`regress frate beertax unemp lincome`:

| 변수 | 계수 | SE(iid) | t | p |
|------|------|---------|---|---|
| beertax | **+0.1103** | 0.0627 | 1.76 | 0.080 |
| unemp | −0.0302 | 0.0131 | −2.31 | 0.021 |
| lincome | −1.9486 | 0.2288 | −8.52 | 6×10⁻¹⁶ |

$R^2$ 는 0.093 → **0.276** 으로 올랐다. 맥주세 계수는 0.365 → **0.110** 으로 3분의 1로 쪼그라들고 유의성이 사라진다(p=0.080). 로그소득이 강하게 음(−1.95): 부유한 주일수록 사망률이 낮다. 즉 §2 첫 회귀의 "양의 효과"는 상당 부분 소득 교란이었다. `estat vif` 는 VIF가 beertax 1.27, unemp 1.55, lincome 1.85 로 모두 낮아 다중공선성(multicollinearity) 문제는 아님을 확인해 준다. `estat ovtest`(Ramsey RESET) $F=0.82$ 로 함수형태 오설정 증거도 약하다.

**그러나** 통제변수를 아무리 넣어도 관측되지 않는 주 고유의 시간불변 요인(문화, 지형)은 남는다. 이것이 §3으로 가는 이유다.

---

## §3 패널 고정효과: within 추정량

### 모형

주 고정효과 $\alpha_i$ 와 연도 고정효과 $\lambda_t$ 를 넣은 **이원(two-way) 고정효과** 모형:

$$\texttt{frate}_{it} = \beta_1\,\texttt{beertax}_{it} + \gamma'\mathbf{x}_{it} + \alpha_i + \lambda_t + \varepsilon_{it}$$

$\alpha_i$ 는 주별 평균 격차(도로망, 지형, 기저 음주문화 등 **시간에 따라 변하지 않는 모든 것**)를 흡수하고, $\lambda_t$ 는 전국 공통 충격(경기, 유가, 연방 안전규제, CAFE 연비기준 등 **주에 공통인 모든 것**)을 흡수한다. 남는 변동은 "어떤 주가 다른 주 대비, 어떤 해에 세금을 올렸을 때 사망률이 어떻게 움직였나"뿐이다. 이것이 within 추정량이다.

### 결과 — 부호가 뒤집힌다

`xtreg frate beertax unemp lincome, fe vce(cluster statenum)` (주 FE만):

$$\hat\beta_{\texttt{beertax}} = -0.3696 \quad (\text{cluster SE } 0.2665,\ t=-1.39,\ p=0.17).$$

`reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)` (주 + 연도 FE):

| 변수 | 계수 | cluster SE | t | p |
|------|------|-----------|---|---|
| beertax | **−0.4604** | 0.3081 | −1.49 | 0.142 |
| lincome | +1.8103 | 0.6400 | +2.83 | 0.007 |
| unemp | −0.0630 | 0.0131 | −4.80 | 2×10⁻⁵ |

합동 OLS의 **+0.365** 가 주 FE에서 **−0.370**, 이원 FE에서 **−0.460** 으로 부호가 뒤집힌다. 이것이 Stock–Watson이 이 데이터로 가르치는 핵심 교훈이다: 주 고유의 시간불변 이질성을 걷어내자 맥주세의 부호가 이론과 맞는 방향(세금↑ → 사망↓)으로 바뀐다. `test beertax unemp lincome` 은 세 기울기가 동시에 0이라는 귀무가설을 $F=50.8\ (p=3\times10^{-26})$ 로 강하게 기각한다.

### 왜 주별로 클러스터(cluster by state)하는가

within 잔차는 같은 주 안에서 **연도 간 양의 계열상관(serial correlation)**을 가진다 — 어느 주의 사망률이 높으면 그 다음 해에도 높은 경향. Bertrand, Duflo & Mullainathan(2004)이 보였듯, 이 계열상관을 무시하면 SE가 심하게 과소추정되어 "유의"가 남발된다. 해법은 **주 단위 클러스터-강건 SE(cluster-robust, CRVE)**: 같은 주 안의 임의의 상관구조를 허용한다. 여기 클러스터는 48개(주)다. 그 대가와 위험은 §4에서 정면으로 다룬다.

### 확률효과(RE)와 Hausman의 직관

`xtreg frate beertax unemp lincome, re` 는 $\hat\beta_{\texttt{beertax}} = +0.055$ 로 다시 합동 OLS 쪽으로 끌려간다. RE-GLS는 $\alpha_i$ 가 회귀변수와 **무상관**이라고 가정하기 때문이다. 맥주세 정책이 주 고유의 음주문화($\alpha_i$)와 상관없다는 가정은 이 맥락에서 믿기 어렵다 — 술에 관대한 주가 세금도 낮게 매길 것이다. Hausman 검정이 정확히 이 가정을 검정하지만, 이 데모 빌드에는 `hausman` 이 배선돼 있지 않다. 그래도 **논리는 명확하다**: FE와 RE 추정치가 이렇게(−0.46 대 +0.055) 크게 벌어지면 그 자체가 RE 직교성 가정에 대한 강한 반증이며, FE를 택해야 한다.

---

## §4 추론의 위계: iid · HC1 · 클러스터, 그리고 48개 클러스터 문제

같은 이원 FE 점추정치 $\hat\beta_{\texttt{beertax}} = -0.4604$ 를 고정하고, 표준오차 방식만 세 단계로 바꿔 본다. `do` 파일은 정확히 같은 명세를 세 번 돌린다.

| vce | SE | t | p | 함의 |
|-----|----|---|---|------|
| `(기본, iid)` | 0.1633 | −2.82 | **0.005** | 등분산·무자기상관 가정 (비현실적) |
| `vce(robust)` (HC1) | 0.2250 | −2.05 | **0.042** | 이분산 허용, 계열상관 무시 |
| `vce(cluster statenum)` | 0.3081 | −1.49 | 0.142 | 주 내 임의 상관 허용 (48 클러스터) |

**단조 증가한다**: 0.163 → 0.225 → 0.308. t는 2.82 → 2.05 → **1.49** 로 떨어지고, "5% 유의"가 클러스터링과 함께 사라진다. 이것이 정직한 그림이다 — 맥주세의 이원 FE 효과는 **가장 방어 가능한 SE(클러스터) 하에서 통계적으로 유의하지 않다.** 계열상관을 정직하게 반영하면 결론이 약해진다.

### 48개 클러스터는 왜 아슬아슬한가

CRVE는 클러스터 수 $G \to \infty$ 에서 일치추정량이다. 유한한 $G$ 에서는 클러스터 분산행렬이 **하향 편의(downward bias)**를 갖고, 정규분포가 아니라 자유도 $G-1$ 의 $t$ 분포, 즉 여기서는 $t(47)$ 을 기준분포로 써야 한다. $G=48$ 은 "충분히 크다"와 "너무 작다"의 경계에 있다 — 정책평가 문헌의 표준 상황이다.

### 와일드 클러스터 부트스트랩이 사주는 것 (로드맵 공백)

Cameron, Gelbach & Miller(2008)의 **와일드 클러스터 부트스트랩(wild cluster bootstrap)**은 소수 클러스터에서 CRVE보다 훨씬 정확한 검정 크기(size)를 준다. 귀무가설을 부과한 뒤 클러스터 단위로 Rademacher(±1) 가중치를 재추출해 $t$ 통계량의 유한표본 분포를 직접 근사한다. 정석이라면 여기서 `boottest beertax, reps(9999) seed(...) weighttype(rademacher)` 로 계산할 것이다. **PIET 2.0.4에는 `boottest` 가 아직 배선돼 있지 않다**(로드맵 공백). 그래서 이 절은 해석적 위계로 대체했다. 실무적 결론은 바뀌지 않는다: 클러스터 SE 하에서 이미 경계선(p=0.14)이므로, 소표본 보정을 더 하면 유의성은 더 약해지면 약해졌지 강해지지 않는다.

---

## §5 IV/2SLS를 손으로: 간접최소제곱과 약한 도구변수

### 왜 IV인가, 그리고 정확식별 IV의 정체

맥주세는 내생적(endogenous)일 수 있다 — 사망률이 높은 주가 정치적으로 세금을 올릴 수도 있고(역인과), 관측 안 되는 교란이 세금과 사망 모두를 움직일 수도 있다. 도구변수(instrument) $z$ 가 (i) 세금과 상관되고(**적합성 relevance**, 검정 가능), (ii) 오차와 무상관이며 오직 세금을 통해서만 사망에 영향을 준다면(**배제제약 exclusion**, 검정 불가) 인과효과를 식별한다.

**정확식별(도구 1개 = 내생변수 1개)**의 경우, 2SLS는 간단한 비율과 정확히 같다 — 간접최소제곱(indirect least squares, ILS):

$$\hat\beta_{\text{IV}} = \frac{\text{축약형 계수(reduced form)}}{\text{1단계 계수(first stage)}} = \frac{\partial \texttt{frate}/\partial z}{\partial \texttt{beertax}/\partial z}.$$

`ivregress`/`ivreghdfe` 는 이 데모 빌드에 배선돼 있지 않다(로드맵 공백). 그래서 **OLS 벽돌로 IV를 손수 조립한다** — 이것이 오히려 "2SLS가 사실 무엇인지"를 드러낸다.

### 1단계: 적합성은 검정 가능하다

도구 후보로 주의 **침례교인 비중(`baptist`)**을 쓴다. 금주(temperance) 정서가 강한 주가 술에 세금을 무겁게 매긴다는, 문헌에 실제로 등장하는 발상이다.

`regress beertax baptist unemp lincome`:

$$\hat\pi_{\texttt{baptist}} = +0.02771\quad(\text{SE }0.00229,\ t=12.1),\qquad \texttt{test baptist}:\ F = 146.3.$$

1단계 $F = 146 \gg 10$ (Staiger–Stock 경험칙). **적합성은 강하다.** 대조군으로 **금주(dry) 카운티 인구비중(`dry`)**을 도구로 쓰면:

$$\hat\pi_{\texttt{dry}} = +0.00229\quad(\text{SE }0.00264,\ t=0.87),\qquad \texttt{test dry}:\ F = 0.75.$$

$F=0.75 \ll 10$ — **교과서적 약한 도구변수(weak instrument)**다. 약한 도구로 ILS를 하면 분모가 0에 가까워 추정치가 폭발하고, 2SLS는 OLS 편의 쪽으로 심하게 쏠린다. 적합성이 검정 가능하다는 것, 그리고 그 검정을 반드시 봐야 한다는 것이 첫 교훈이다.

### 축약형과 IV 추정치

강한 도구 `baptist` 로 축약형을 돌린다. `regress frate baptist unemp lincome`:

$$\hat\gamma_{\texttt{baptist}} = +0.01459\quad(\text{SE }0.00305,\ t=4.78).$$

간접최소제곱:

$$\hat\beta_{\text{IV}} = \frac{\hat\gamma_{\texttt{baptist}}}{\hat\pi_{\texttt{baptist}}} = \frac{0.01459}{0.02771} = \boxed{+0.527}.$$

### 정직한 해석 — 이건 신뢰할 설계가 아니다

IV 추정치가 **+0.527**, 즉 OLS보다도 훨씬 큰 양수다. 세금이 사망을 늘린다는, 이론과 정반대의 큰 효과. 왜인가? **적합성은 강했지만(F=146) 배제제약이 명백히 깨졌기 때문**이다. 침례교인 비중은 맥주세를 통해서만 사망에 영향을 주는 게 아니라, 농촌성·종교문화·주행거리·운전행태를 통해 **사망률에 직접** 영향을 준다. 강한 도구가 곧 타당한 도구는 아니다 — 1단계 F는 (i)만 검정하고 (ii)는 결코 검정하지 못한다.

> **이 절은 메커니즘과 진단을 가르치기 위한 것이지, 신뢰할 만한 식별 설계가 아니다.** `baptist` 도 `dry` 도 배제제약이 방어되지 않는다. 데모의 목적은 (a) 정확식별 IV = 축약형/1단계라는 대수를 손으로 보이고, (b) 약한 도구(F=0.75)와 강하지만 무효한 도구(F=146, 그래도 큰 편의)를 대비시키는 것이다.

---

## §6 카운트와 노출: 포아송의 로그선형 유사모형

### 왜 카운트 모형인가

`fatal` 은 음이 아닌 **정수 카운트(count)**다. 주마다 인구(노출, exposure)가 크게 다르므로, 정석은 노출을 명시한 **포아송 회귀**다:

$$\mathbb{E}[\texttt{fatal}_{it}\mid \mathbf{x}] = \exp\!\big(\ln \texttt{pop}_{it} + \mathbf{x}_{it}'\beta\big),\quad\text{즉}\quad \ln \mathbb{E}[\texttt{fatal}_{it}] = \underbrace{\ln \texttt{pop}_{it}}_{\text{offset, 계수}=1\text{ 고정}} + \mathbf{x}_{it}'\beta.$$

노출항 `exposure(pop)` 은 $\ln\texttt{pop}$ 의 계수를 정확히 **1로 제약**한다 — 즉 종속변수를 사실상 발생률(rate)로 만든다. 포아송은 카운트의 이분산과 0값(여기선 없지만)을 자연스럽게 다루고, `poisson … , exposure(pop)` 한 줄이면 된다.

**PIET 2.0.4에는 `poisson`/`nbreg`/`ppmlhdfe` 가 아직 배선돼 있지 않다**(로드맵 공백). 그래서 **로그선형 유사모형**으로 대체한다 — $\ln\texttt{pop}$ 을 offset(계수 1 고정)이 아니라 **자유 회귀변수**로 넣어, 노출탄력성을 데이터가 정하게 한다.

`reghdfe lfatal beertax unemp lincome lpop, absorb(statenum year) vce(cluster statenum)`:

| 변수 | 계수 | cluster SE | t | p |
|------|------|-----------|---|---|
| beertax | −0.1324 | 0.1202 | −1.10 | 0.276 |
| lpop (노출) | +0.5939 | 0.3660 | +1.62 | 0.111 |
| lincome | +0.8287 | 0.3071 | +2.70 | 0.010 |
| unemp | −0.0277 | 0.0067 | −4.13 | 0.0001 |

### 읽는 법과 주의

`beertax` 계수 **−0.132** 는 준탄력성(semi-elasticity)이다: 맥주세 \$1 인상당 사망 카운트가 약 **13% 감소**(단, cluster SE 하에서 유의하지 않음, p=0.276). 부호는 §3의 이원 FE와 정합적이다. 노출항 `lpop` 계수는 **0.594** 로 포아송-노출이 강제하는 **1과 다르다** — 다만 주·연도 FE가 이미 주 규모의 대부분을 흡수한 뒤라 노출항은 약하게 식별되며, 그 검정은 이 대체 명세의 한계를 보여 준다.

정직한 한계: 로그선형은 (i) $\mathbb{E}[\ln y]\neq \ln \mathbb{E}[y]$ (Jensen 부등식)이라 포아송과 다른 모수를 추정하고, (ii) $y=0$ 인 관측을 로그가 버린다(이 데이터엔 없지만 일반적으로 치명적). Silva & Tenreyro(2006)의 결론대로 카운트/노출 문제의 정석은 (유사)포아송 최대가능도이며, 로그선형은 그 근사일 뿐이다.

---

## §7 시차적 도입 이중차분: Callaway–Sant'Anna

### 처치 코호트 구성

음주 호흡측정법은 주마다 **다른 해에** 도입됐다(시차적 도입, staggered adoption). 각 주의 최초 처치연도로 코호트 변수 `gvar` 를 만든다:

```
gen treatyr = year if drinkd==1
egen gvar   = min(treatyr), by(statenum)   // 주별 최초 처치연도
replace gvar = 0 if missing(gvar)          // 한 번도 처치 안 된 주 = 0
```

`tabulate gvar` 코호트 분포:

| 코호트 | 관측수 | 성격 |
|--------|--------|------|
| 0 (never-treated) | 154 | 통제 후보 |
| 1982 | 126 | **항상 처치(always-treated)** → 사전기간 없음 → CS가 탈락시킴 |
| 1983 | 35 | |
| 1984 | 7 | |
| 1986 | 7 | |
| 1987 | 7 | |

1982년 코호트는 표본 첫 해부터 이미 처치돼 있어 **사전(pre) 기간이 없다.** Callaway–Sant'Anna는 사전-사후 비교가 불가능한 이 항상-처치군을 자동으로 **탈락**시킨다. 그래서 `csdid … N=205` (336 − 126(1982 코호트) − …)로 표본이 줄어든다. **데모는 이 탈락을 숨기지 않는다** — 코호트 표를 먼저 찍는 이유다.

### 추정량과 표준오차

`csdid frate unemp lincome, ivar(statenum) time(year) gvar(gvar) method(dripw) cluster(statenum)`.

Callaway & Sant'Anna(2021)의 추정 대상은 코호트 $g$·시점 $t$ 별 처치효과 **ATT$(g,t)$** 의 집합이며, 이를 목적에 맞게 가중평균한다. `method(dripw)` 는 **이중강건(doubly-robust) IPW**(Sant'Anna & Zhao 2020): 결과회귀(outcome regression)와 성향점수(propensity score) 중 하나만 옳아도 일치추정이 된다. 통제군은 아직-처치 안 됨(not-yet-treated)/전혀-처치 안 됨(never-treated) 집단이다. SE는 주 단위 클러스터.

### 결과 — 그리고 사전추세가 깨진다

`estat simple`(전체 가중평균 ATT):

$$\widehat{\text{ATT}} = -0.0788\quad(\text{SE }0.1242,\ t=-0.63,\ p=0.53).$$

전체 효과는 음(사망률 낮춤)이나 유의하지 않다. 그런데 **핵심은 event study다.** `estat event` (처치까지의 상대시간 $e$ 별 동태 ATT):

| $e$ (상대연도) | ATT | SE | p |
|:---:|---:|---:|---:|
| −4 | +0.017 | 0.026 | 0.52 |
| **−3** | **+0.139** | 0.031 | **9×10⁻⁶** |
| **−1** | **−0.140** | 0.051 | **0.006** |
| 0 | +0.026 | 0.096 | 0.78 |
| 1 | −0.084 | 0.114 | 0.46 |
| 2 | −0.072 | 0.121 | 0.55 |
| 3 | −0.176 | 0.148 | 0.23 |
| 4 | −0.059 | 0.161 | 0.72 |
| 5 | −0.135 | 0.188 | 0.47 |

**사전추세(pre-trend)가 유의하게 깨진다**: 처치 이전인 $e=-3$ 에서 +0.139 (p=9×10⁻⁶), $e=-1$ 에서 −0.140 (p=0.006)로 0에서 유의하게 벗어난다. 처치 *전*에 이미 처치군과 통제군의 사망률 궤적이 다르게 움직였다는 뜻이다.

### 평행추세(parallel trends)와 정직한 결론

이중차분의 식별 가정은 **평행추세**다: 처치가 없었다면 두 집단의 결과가 평행하게 움직였을 것. Event study의 사전기간 계수들이 그 반증가능한(testable) 함의다 — 여기서 그것이 **기각된다.** 따라서 이 데이터에서 breath law의 이중차분 설계는 **신뢰할 수 없다.** (사후 계수들은 대체로 음이지만 개별적으로 유의하지 않고, 사전추세가 깨진 이상 인과 해석은 방어되지 않는다.)

`estat calendar` 는 달력연도별 ATT(1983–1988, 모두 유의하지 않음)를 준다.

> **`estat group` 은 왜 투어에 없나(주석 처리).** 2.0.4 출하 바이너리의 `estat group` 은 코호트별 ATT 대신 **calendar 표를 그대로 반환하는 버그**(Doctrine ① 위반: rc=0 무오류로 틀린 숫자)가 있어 투어에서 뺐다. cobra 0.20.0(계약 1.10.0)에서 엔진에 코호트별 집계(`groups`)를 추가하고 PIET 매핑을 수정해 Stata `estat group` 과 대조 검증했다(G1983 −0.084, G1984 −0.077, G1986 −0.032; att 상대오차 ≤1e-12). 수정 엔진이 패키징되어 출하되면 투어의 주석을 해제한다. 단, 알려진 잔여 격차: 1987 코호트(처치 주 1개)는 성향점수 로짓이 **완전분리(perfect separation)** 되어 현재 엔진은 해당 (g,t) 셀들을 탈락시키므로 G1987 행이 나오지 않는다(Stata는 준분리 적합으로 값을 내놓음; 별도 엔진 과제로 추적).

> **왜 그냥 TWFE(이원 FE) 이중차분을 안 쓰나?** Goodman-Bacon(2021)이 보였듯, 처치효과가 코호트/시점에 따라 이질적일 때 시차적 도입의 표준 TWFE 추정량은 **이미 처치된 집단을 통제군으로 쓰는 "금지된 비교(forbidden comparison)"**를 포함해 음의 가중치가 끼고 편의가 생긴다. Callaway–Sant'Anna는 아직/전혀 처치 안 된 집단만 통제군으로 써서 이를 피한다. 다만 그 대가로 평행추세가 필요하고 — 이 데이터는 그것을 만족하지 못한다.

---

## §8 그래픽

세 그림 모두 **SVG로 내보낸다**(PNG/PDF export는 로드맵 공백이므로 `graph export … .svg`).

- **`fig_scatter.svg`** — `twoway (scatter frate beertax) (lfit frate beertax)`. 원자료 산점도 + OLS 적합선. §2의 오해를 부르는 **양의** 횡단면 기울기를 눈으로 보여 준다. 이 그림이 "왜 순진한 회귀가 위험한가"의 시각적 서두다.
- **`fig_did.svg`** — `collapse (mean) frate, by(year evertreat)` 로 연도×(처치경험 여부) 평균을 만든 뒤 두 선을 겹쳐 그린다(`twoway line … if evertreat==1 || … ==0`). 이것이 이중차분의 **날것 평행추세 점검**이다: 두 집단 평균 사망률의 시간 궤적이 처치 전에 평행한가? §7의 event-study가 수치로 기각한 것을 이 그림이 시각적으로 뒷받침한다.
- **coefplot(인라인)** — `eststo m_fe` 로 이원 FE 모형을 저장한 뒤 `coefplot m_fe, keep(beertax)` 로 맥주세 계수와 신뢰구간을 콘솔에 렌더한다. (참고: 이 빌드에서 `graph export` 는 coefplot이 아니라 직전의 twoway 캔버스를 내보내므로, coefplot은 **인라인 렌더로만** 시연하고 SVG로 내보내지 않는다.)

---

## §9 시계열 연산자로 본 동태

패널 선언(`xtset`) 위에서 시차(`L.`)·차분(`D.`) 연산자를 쓸 수 있다. 정석의 HAC(newey)나 동적패널 GMM은 배선돼 있지 않으므로(로드맵 공백), 여기서는 연산자 자체를 예시한다.

- `regress frate L.beertax unemp lincome` (N=288): $\hat\beta_{\texttt{L.beertax}} = +0.156\ (t=2.50,\ p=0.013)$. 1년 시차 세금이 "유의"해 보이지만, 이건 **FE 없는** 합동 회귀라 §2와 같은 교란을 그대로 안고 있다.
- `regress frate beertax L.beertax unemp lincome`: 동시기와 시차를 함께 넣으면 둘 다 부정확해진다(beertax +0.403, L.beertax −0.232, 둘 다 유의하지 않음). 세금은 강하게 지속적(persistent)이라 두 항이 공선적이기 때문이다.
- `regress frate D.beertax unemp lincome`: 세금의 1차 차분을 넣으면 $\hat\beta = -0.284\ (p=0.67)$ 로 유의성이 사라진다 — 차분 역시 within 변환의 일종이라 §3처럼 횡단면 교란을 상당 부분 걷어낸다.
- `regress frate L.frate beertax`: 시차종속변수(lagged dependent variable). $\hat\beta_{\texttt{L.frate}} = 0.879\ (t=44)$ 로 사망률은 극히 지속적이고, $R^2=0.888$. 하지만 패널에서 시차종속변수 + 개체효과는 **Nickell 편의(Nickell 1981)**를 낳는다 — 여기선 FE를 뺐지만, 넣더라도 within 변환이 $L.\texttt{frate}$ 와 오차를 상관시켜 OLS/FE가 편의를 갖는다. 정석은 Arellano–Bond류 동적패널 GMM이며(로드맵 공백), 이 절은 그 함정을 상기시키는 예시다.

---

## 부록: 검증 원장

`fatalities_tour.do` 전 블록을 `piet-2.0.4+cobra-0.19.2` 로 헤드리스 실행 → **에러 블록 0, GREEN**. 재현:

```sh
demos/run_demos.sh
# 또는 바이너리 지정:
demos/run_demos.sh /path/to/PIET.app/Contents/MacOS/piet_native
```

| 절 | 대표 명령 | 핵심 수치 |
|----|-----------|-----------|
| §1 | `use` · `summarize` · `tabulate` | N=336, 48주×7년; frate 평균 2.040; breath yes=155/no=181 |
| §2 | `regress frate beertax` (+robust, 통제) | b=+0.365 (iid SE .062 / HC1 .053); +통제 b=+0.110 (p=.080); hettest χ²=9.48 (p=.002) |
| §3 | `xtreg fe` · `reghdfe absorb(state year)` | 주FE b=−0.370; 이원FE b=−0.460 (cluster, 48); RE b=+0.055 |
| §4 | 같은 이원FE, vce 3종 | SE 0.163→0.225→0.308; p 0.005→0.042→0.142 |
| §5 | `regress`(1단계·축약형) + `test` | baptist: 1단계 F=146.3, ILS b_IV=+0.527; dry: 1단계 F=0.75 (약함) |
| §6 | `reghdfe lfatal … lpop` | beertax −0.132 (p=.28); lpop(노출) 0.594 |
| §7 | `csdid method(dripw)` + `estat event` | N=205; 전체 ATT −0.079 (p=.53); 사전추세 e=−3 p=9e−6, e=−1 p=.006 (기각) |
| §8 | `twoway`+`lfit` · `collapse`+`line` · `coefplot` | SVG 2종(fig_scatter, fig_did) 내보냄 |
| §9 | `L.` · `D.` 연산자 회귀 | L.beertax +0.156 (p=.013); L.frate 0.879 (t=44) |
